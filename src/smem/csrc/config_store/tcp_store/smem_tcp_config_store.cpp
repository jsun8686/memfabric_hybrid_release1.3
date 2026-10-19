/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include <pthread.h>
#include "smem_tcp_config_store.h"
#include "smem_config_store_logger.h"
#include "smem_message_packer.h"
#include "smem_group_manager_client.h"
#include "smem_tcp_config_store_ssl_helper.h"
#include "mf_str_util.h"

namespace ock {
namespace smem {
constexpr auto CONNECT_RETRY_MAX_TIMES = 60;
constexpr int DECIMAL_BASE = 10;
constexpr int WORLD_SIZE_SHIFT = 32;
constexpr uint32_t LINK_SEND_QUEUE_SIZE = 4096;
// requestId layout: high 32 bits carry the rank id, low 32 bits carry the message sequence
constexpr uint32_t REQUEST_ID_RANK_SHIFT = 32;

class ClientWaitContext : public ClientCommonContext {
public:
    ClientWaitContext(std::mutex &mtx, std::condition_variable &cond) noexcept
        : waitMutex_{mtx}, waitCond_{cond}, finished_{false}
    {}

    std::shared_ptr<ock::acc::AccTcpRequestContext> WaitFinished() noexcept override
    {
        constexpr auto WAIT_TIMEOUT = std::chrono::seconds(600);
        return WaitFinishedFor(WAIT_TIMEOUT);
    }

    std::shared_ptr<ock::acc::AccTcpRequestContext> WaitFinishedFor(std::chrono::seconds waitTimeout) noexcept
    {
        std::unique_lock<std::mutex> locker{waitMutex_};
        if (!waitCond_.wait_for(locker, waitTimeout, [this]() { return finished_; })) {
            STORE_LOG_ERROR("ClientWaitContext::WaitFinished timeout after " << waitTimeout.count() << "s");
            return nullptr;
        }
        auto copy = responseInfo_;
        locker.unlock();

        return copy;
    }

    void SetFinished(const ock::acc::AccTcpRequestContext &response) noexcept override
    {
        std::unique_lock<std::mutex> locker{waitMutex_};
        responseInfo_ = std::make_shared<ock::acc::AccTcpRequestContext>(response);
        finished_ = true;
        locker.unlock();

        waitCond_.notify_one();
    }

    void SetFailedFinish() noexcept override
    {
        std::unique_lock<std::mutex> locker{waitMutex_};
        finished_ = true;
        responseInfo_ = nullptr;
        locker.unlock();

        waitCond_.notify_one();
    }
    bool Blocking() const noexcept override
    {
        return true;
    }

private:
    std::mutex &waitMutex_;
    std::condition_variable &waitCond_;
    bool finished_;
    std::shared_ptr<ock::acc::AccTcpRequestContext> responseInfo_;
};

class ClientWatchContext : public ClientCommonContext {
public:
    explicit ClientWatchContext(std::function<void(int, const std::vector<uint8_t> &)> nfy, bool oneTime = true,
                                std::string key = {}) noexcept
        : notify_{std::move(nfy)}, onlyOneTime_{oneTime}, key_{std::move(key)}
    {}

    std::shared_ptr<ock::acc::AccTcpRequestContext> WaitFinished() noexcept override
    {
        return nullptr;
    }

    void SetFinished(const ock::acc::AccTcpRequestContext &response) noexcept override
    {
        auto data = reinterpret_cast<const uint8_t *>(response.DataPtr());
        if (data == nullptr) {
            STORE_LOG_ERROR("data is nullptr.");
            return;
        }

        SmemMessage responseBody;
        auto ret = SmemMessagePacker::Unpack(data, response.DataLen(), responseBody);
        if (ret < 0) {
            STORE_LOG_ERROR("unpack response body failed, result: " << ret);
            notify_(IO_ERROR, std::vector<uint8_t>{});
            return;
        }

        if (responseBody.values.empty()) {
            STORE_LOG_ERROR("response body has no value");
            notify_(IO_ERROR, std::vector<uint8_t>{});
            return;
        }

        STORE_LOG_DEBUG("watch end, id: " << response.SeqNo());
        notify_(SUCCESS, responseBody.values[0]);
    }

    void SetFailedFinish() noexcept override
    {
        notify_(IO_ERROR, std::vector<uint8_t>{});
    }

    bool Blocking() const noexcept override
    {
        return false;
    }

    bool OnlyOneTime() const noexcept override
    {
        return onlyOneTime_;
    }

    const std::string &GetKey() const noexcept
    {
        return key_;
    }

private:
    const std::function<void(int result, const std::vector<uint8_t> &)> notify_;
    const bool onlyOneTime_;
    const std::string key_;
};

std::atomic<uint32_t> TcpConfigStore::reqSeqGen_{0};
static std::atomic<uint32_t> g_clientMsgSeq{0};

TcpConfigStore::TcpConfigStore(StoreBackendPtr backend, std::string ip, uint16_t port, uint16_t model, bool skipRecover,
                               uint32_t worldSize, int32_t rankId, SmemGroupManagerClientPtr tc) noexcept
    : SmemGroupManager(), serverIp_{std::move(ip)}, serverPort_{port}, startupModel_{model}, skipRecover_{skipRecover},
      rankId_{rankId}, worldSize_{worldSize}, backend_(std::move(backend))
{
    if (tc.Get() != nullptr) {
        SetExecutor(std::move(tc));
    } else {
        SetExecutor(new SmemGroupManagerClient());
    }
    if (rankId >= 0) {
        SetLocalRankId(static_cast<uint32_t>(rankId));
        InitAsyncDispatcher(static_cast<uint32_t>(rankId));
    } else {
        SetLocalRankId(UINT32_MAX);
    }
}

TcpConfigStore::~TcpConfigStore() noexcept
{
    Shutdown();
}

int TcpConfigStore::Join(const RankFullInfo &info) noexcept
{
    STORE_LOG_INFO("TcpConfigStore.Join: info=" << info);
    localRankInfo_ = info;
    SmemMessage request = SmemMessage::PackJoin(info);
    request.requestId = (static_cast<uint64_t>(info.rankId + 1) << REQUEST_ID_RANK_SHIFT) | g_clientMsgSeq.fetch_add(1);
    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("Join: send join request failed, get null response");
        return -1;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("Join: send join request failed, response code: " << responseCode);
        return responseCode;
    }

    return 0;
}

int TcpConfigStore::ExtendMemory(const MultiBytes &additionalSlices) noexcept
{
    STORE_LOG_INFO("TcpConfigStore.ExtendMemory: rankId=" << localRankId_
                                                          << " additionalSlices=" << additionalSlices.size());
    SmemMessage request = SmemMessage::PackExtendMemory(localRankId_, additionalSlices);
    request.requestId =
        (static_cast<uint64_t>(localRankId_ + 1) << REQUEST_ID_RANK_SHIFT) | g_clientMsgSeq.fetch_add(1);
    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("ExtendMemory: send request failed, get null response");
        return -1;
    }
    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("ExtendMemory: request failed, response code: " << responseCode);
        return responseCode;
    }
    return 0;
}

int TcpConfigStore::Leave() noexcept
{
    SmemMessage request = SmemMessage::PackLeave(localRankId_);
    request.requestId =
        (static_cast<uint64_t>(localRankId_ + 1) << REQUEST_ID_RANK_SHIFT) | g_clientMsgSeq.fetch_add(1);
    STORE_LOG_DEBUG("Leave: sending LEAVREQ rank=" << localRankId_ << " reqId=" << request.requestId);
    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("Leave: send leave request failed, get null response");
        return -1;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("Leave: send leave request failed, response code: " << responseCode);
        return responseCode;
    }
    STORE_LOG_INFO("rank=" << localRankId_ << " Leave success");

    return 0;
}

void TcpConfigStore::InitAsyncDispatcher(uint32_t localRankId) noexcept
{
    auto *groupMgr = dynamic_cast<SmemGroupManager *>(this);
    if (groupMgr == nullptr) {
        STORE_LOG_ERROR("InitAsyncDispatcher: failed to cast to SmemGroupManager");
        return;
    }
    asyncDispatcher_ = std::make_unique<SmemGroupCommandAsyncDispatcher>(groupMgr);
    asyncDispatcher_->SetLocalRankId(localRankId);
    asyncDispatcher_->SetSendAckBatchFunc(
        [this](ControlOp ackOp, uint32_t senderRankId, const std::vector<uint32_t> &targetRankIds,
               const std::vector<int32_t> &results,
               uint64_t requestId) { SendControlAckBatch(ackOp, senderRankId, targetRankIds, results, requestId); });
    if (!asyncDispatcher_->Start()) {
        STORE_LOG_ERROR("InitAsyncDispatcher: failed to start async manager");
        asyncDispatcher_.reset();
    }
}

void TcpConfigStore::SendControlAckBatch(ControlOp ackOp, uint32_t senderRankId,
                                         const std::vector<uint32_t> &targetRankIds,
                                         const std::vector<int32_t> &results, uint64_t requestId) noexcept
{
    static const char *typeTag[] = {"ADDWACK", "REMWACK", "ESTCACK"};
    auto idx = (ackOp >= CONTROL_ADD_TO_WHITELIST_ACK && ackOp <= CONTROL_ESTABLISH_CONNECTION_ACK)
                   ? (ackOp - CONTROL_ADD_TO_WHITELIST_ACK)
                   : -1;
    const char *typeName = (idx >= 0) ? typeTag[idx] : "?";
    SmemMessage ackMsg = SmemMessage::PackAckBatch(ackOp, senderRankId, targetRankIds, results);
    ackMsg.requestId = requestId;
    auto packed = SmemMessagePacker::Pack(ackMsg);
    auto buf = ock::acc::AccDataBuffer::Create(packed.data(), packed.size());
    if (buf != nullptr && accClientLink_ != nullptr) {
        int ackRet = LocalNonBlockSend(0, 0, buf, nullptr);
        if (ackRet != 0) {
            STORE_LOG_WARN("[GM][Client][Ack] rank=" << localRankId_ << " type=" << typeName << " reqId=" << requestId
                                                     << " cnt=" << targetRankIds.size() << " fail: " << ackRet);
        } else {
            STORE_LOG_INFO("[GM][Client][Ack] rank=" << localRankId_ << " type=" << typeName << " reqId=" << requestId
                                                     << " cnt=" << targetRankIds.size() << " ok");
        }
    }
}

Result TcpConfigStore::Startup(const smem_tls_config &tlsConfig, int reconnectRetryTimes) noexcept
{
    Result result = SM_OK;
    if (startupModel_ != ConfigStoreModel::CSM_CLIENT) {
        result = ServerStart(tlsConfig, reconnectRetryTimes);
        if (result != 0) {
            STORE_LOG_ERROR("Failed to start config store server ret:" << result);
            return result;
        }
        STORE_LOG_INFO("start store server success, ip:" << serverIp_ << ":" << serverPort_);
    }

    if (startupModel_ != ConfigStoreModel::CSM_SERVER) {
        result = ClientStart(tlsConfig, reconnectRetryTimes);
        if (result != 0) {
            STORE_LOG_ERROR("Failed to start config store client ret:" << result);
            return result;
        }
        STORE_LOG_INFO("connect store success, ip:" << serverIp_ << ":" << serverPort_);
    }
    isConnect_.store(true);
    return result;
}

Result TcpConfigStore::ClientStart(const smem_tls_config &tlsConfig, int reconnectRetryTimes) noexcept
{
    Result result = SM_OK;
    auto retryMaxTimes = reconnectRetryTimes < 0 ? CONNECT_RETRY_MAX_TIMES : reconnectRetryTimes;

    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (accClient_ != nullptr) {
        STORE_LOG_WARN("TcpConfigStore already startup");
        return SM_OK;
    }

    accClient_ = ock::acc::AccTcpServer::Create();
    if (accClient_ == nullptr) {
        STORE_LOG_ERROR("create acc tcp client failed");
        return SM_ERROR;
    }
    if (tlsConfig.tlsEnable && PrepareTlsForAccTcpServer(accClient_, tlsConfig) != SM_OK) {
        STORE_LOG_ERROR("Failed to prepare TLS for acc client");
    }

    accClient_->RegisterNewRequestHandler(
        0, [this](const ock::acc::AccTcpRequestContext &context) { return ReceiveResponseHandler(context); });
    accClient_->RegisterLinkBrokenHandler(
        [this](const ock::acc::AccTcpLinkComplexPtr &link) { return LinkBrokenHandler(link); });

    ock::acc::AccTcpServerOptions options;
    options.linkSendQueueSize = LINK_SEND_QUEUE_SIZE;
    if ((result = accClient_->Start(options, GetAccTlsOption(tlsConfig))) != SM_OK) {
        STORE_LOG_ERROR("start acc client failed, result: " << result);
        Shutdown();
        return result;
    }

    ock::acc::AccConnReq connReq;
    connReq.reconnect = 0;
    connReq.rankId =
        rankId_ >= 0 ? ((static_cast<uint64_t>(worldSize_) << WORLD_SIZE_SHIFT) | static_cast<uint64_t>(rankId_))
                     : ((static_cast<uint64_t>(worldSize_) << WORLD_SIZE_SHIFT) | std::numeric_limits<uint32_t>::max());
    result = accClient_->ConnectToPeerServer(serverIp_, serverPort_, connReq, retryMaxTimes, accClientLink_);
    if (result != 0) {
        STORE_LOG_ERROR("connect to server failed, ip: " << serverIp_ << " port: " << serverPort_
                                                         << " result: " << result);
        Shutdown();
        return result;
    }
    isRunning_.store(true);
    isConnect_.store(true);
    clientStarted_ = true;
    heartBeatThread_ = std::thread{[this]() { HeartBeat(); }};
    return result;
}

Result TcpConfigStore::ServerStart(const smem_tls_config &tlsConfig, int reconnectRetryTimes) noexcept
{
    Result result = SM_OK;
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    accServer_ = SmMakeRef<AccStoreServer>(serverIp_, serverPort_, worldSize_, backend_, skipRecover_);
    if (accServer_ == nullptr) {
        STORE_LOG_ERROR("create acc store server failed, ip: " << serverIp_ << " port: " << serverPort_
                                                               << " worldSize: " << worldSize_);
        Shutdown();
        return SM_NEW_OBJECT_FAILED;
    }

    if ((result = accServer_->Startup(tlsConfig)) != SM_OK) {
        Shutdown();
        return result;
    }
    clientStarted_ = true;
    return result;
}

void TcpConfigStore::Shutdown(bool afterFork) noexcept
{
    isRunning_.store(false);
    SetConnectStatus(false);
    if (heartBeatThread_.joinable()) {
        heartBeatThread_.join();
    }
    accClientLink_ = nullptr;

    if (accClient_ != nullptr) {
        if (afterFork) {
            accClient_->StopAfterFork();
        } else {
            accClient_->Stop();
        }
        accClient_ = nullptr;
    }

    if (accServer_ != nullptr) {
        accServer_->Shutdown(afterFork);
        accServer_ = nullptr;
    }
    clientStarted_ = false;
}

Result TcpConfigStore::PrefixGet(const std::string &key, std::unordered_map<std::string, std::string> &value) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::PREFIX};
    request.keys.push_back(key);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send get for key: " << key << ", get null response");
        return IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0 && responseCode != RESTORE) {
        if (responseCode != NOT_EXIST) {
            STORE_LOG_WARN("send prefix_get for key: " << key << ", resp code: " << responseCode);
        }
        return responseCode;
    }

    auto data = reinterpret_cast<const uint8_t *>(response->DataPtr());
    STORE_ASSERT_RETURN(data != nullptr, SM_MALLOC_FAILED);
    SmemMessage responseBody;
    auto ret = SmemMessagePacker::Unpack(data, response->DataLen(), responseBody);
    if (ret < 0) {
        STORE_LOG_ERROR("unpack response body failed, result: " << ret);
        return -1;
    }

    if (responseBody.values.size() != responseBody.keys.size()) {
        STORE_LOG_ERROR("response body has not matched");
        return -1;
    }

    uint32_t len = responseBody.keys.size();
    for (uint32_t i = 0; i < len; i++) {
        value[responseBody.keys[i]] = std::string(responseBody.values[i].begin(), responseBody.values[i].end());
    }
    return static_cast<Result>(responseCode);
}

Result TcpConfigStore::Set(const std::string &key, const std::vector<uint8_t> &value) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::SET};
    request.keys.push_back(key);
    request.values.push_back(value);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send set for key: " << key << ", get null response");
        return IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send set for key: " << key << ", get response code: " << responseCode);
    }

    return responseCode;
}

Result TcpConfigStore::GetReal(const std::string &key, std::vector<uint8_t> &value, int64_t timeoutMs) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::GET};
    request.keys.push_back(key);
    request.userDef = timeoutMs;

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send get for key: " << key << ", get null response");
        return IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0 && responseCode != RESTORE) {
        if (responseCode != NOT_EXIST && response->DataPtr() != nullptr) {
            std::string data(reinterpret_cast<char *>(response->DataPtr()), response->DataLen());
            STORE_LOG_WARN("send get for key: " << key << ", resp code: " << responseCode << ", timeout:" << timeoutMs
                                                << ", reason:" << data);
        }
        return responseCode;
    }

    auto data = reinterpret_cast<const uint8_t *>(response->DataPtr());
    STORE_ASSERT_RETURN(data != nullptr, SM_MALLOC_FAILED);
    SmemMessage responseBody;
    auto ret = SmemMessagePacker::Unpack(data, response->DataLen(), responseBody);
    if (ret < 0) {
        STORE_LOG_ERROR("unpack response body failed, result: " << ret);
        return -1;
    }

    if (responseBody.values.empty()) {
        STORE_LOG_ERROR("response body has no value");
        return -1;
    }

    value = std::move(responseBody.values[0]);
    return static_cast<Result>(responseCode);
}

Result TcpConfigStore::Add(const std::string &key, int64_t increment, int64_t &value) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::ADD};
    request.keys.push_back(key);
    std::string inc = std::to_string(increment);
    request.values.push_back(std::vector<uint8_t>(inc.begin(), inc.end()));

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send add for key: " << key << ", get null response");
        return StoreErrorCode::IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send add for key: " << key << ", get response code: " << responseCode);
        return responseCode;
    }
    STORE_ASSERT_RETURN(response->DataPtr() != nullptr, IO_ERROR);
    std::string data(reinterpret_cast<char *>(response->DataPtr()), response->DataLen());

    value = 0;
    auto ret = mf::StrUtil::String2Int<int64_t>(data, value);
    STORE_ASSERT_RETURN(errno != ERANGE, IO_ERROR);
    if ((value == 0 && data != "0") || !ret) {
        STORE_LOG_ERROR("data=" << data);
        return StoreErrorCode::ERROR;
    }
    return StoreErrorCode::SUCCESS;
}

Result TcpConfigStore::Remove(const std::string &key, bool printKeyNotExist) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::REMOVE};
    request.keys.push_back(key);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send remove for key: " << key << ", get null response");
        return StoreErrorCode::IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode == StoreErrorCode::NOT_EXIST) {
        if (printKeyNotExist) {
            // Do not need to print ERROR message when sending remove key which is already not exist.
            STORE_LOG_INFO("send remove for key: " << key << ", is already not exist");
        }
    } else if (responseCode != 0) {
        STORE_LOG_ERROR("send remove for key: " << key << ", get response code: " << responseCode);
    }

    return responseCode;
}

Result TcpConfigStore::Append(const std::string &key, const std::vector<uint8_t> &value, uint64_t &newSize) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::APPEND};
    request.keys.push_back(key);
    request.values.push_back(value);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send append for key: " << key << ", get null response");
        return StoreErrorCode::IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send append for key: " << key << ", get response code: " << responseCode);
        return responseCode;
    }
    STORE_ASSERT_RETURN(response->DataPtr() != nullptr, IO_ERROR);
    std::string data(reinterpret_cast<char *>(response->DataPtr()), response->DataLen());

    long tmpValue = 0;
    STORE_VALIDATE_RETURN(mf::StrUtil::String2Int<long>(data, tmpValue),
                          "convert string to long failed, data: " << data, StoreErrorCode::ERROR);
    newSize = static_cast<uint64_t>(tmpValue);

    return StoreErrorCode::SUCCESS;
}

Result TcpConfigStore::Write(const std::string &key, const std::vector<uint8_t> &value, const uint32_t offset) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::WRITE};
    std::vector<uint8_t> sendValue(reinterpret_cast<const uint8_t *>(&offset),
                                   reinterpret_cast<const uint8_t *>(&offset) + sizeof(offset));
    sendValue.insert(sendValue.end(), value.begin(), value.end());
    request.keys.push_back(key);
    request.values.push_back(sendValue);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send set for key: " << key << ", get null response");
        return IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send set for key: " << key << ", get response code: " << responseCode);
    }

    return responseCode;
}

Result TcpConfigStore::QueryAlive(uint32_t rank, uint32_t &alive) noexcept
{
    SmemMessage request{MessageType::QUERY_ALIVE};
    request.keys.push_back(std::to_string(rank));
    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send query alive for rank: " << rank << ", get null response");
        return StoreErrorCode::IO_ERROR;
    }
    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send query alive for rank: " << rank << ", get response code: " << responseCode);
        return responseCode;
    }

    auto data = reinterpret_cast<const uint8_t *>(response->DataPtr());
    STORE_ASSERT_RETURN(data != nullptr, SM_MALLOC_FAILED);
    SmemMessage responseBody;
    auto ret = SmemMessagePacker::Unpack(data, response->DataLen(), responseBody);
    if (ret < 0) {
        STORE_LOG_ERROR("unpack response body failed, result: " << ret);
        return -1;
    }

    alive = !responseBody.values.empty();
    STORE_LOG_DEBUG("query rank:" << rank << " alive:" << alive);
    return 0;
}

Result TcpConfigStore::Cas(const std::string &key, const std::vector<uint8_t> &expect,
                           const std::vector<uint8_t> &value, std::vector<uint8_t> &exists) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::CAS};
    request.keys.push_back(key);
    request.values.push_back(expect);
    request.values.push_back(value);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto response = SendMessageBlocked(packedRequest);
    if (response == nullptr) {
        STORE_LOG_ERROR("send CAS for key: " << key << ", get null response");
        return StoreErrorCode::IO_ERROR;
    }

    auto responseCode = response->Header().result;
    if (responseCode != 0) {
        STORE_LOG_ERROR("send CAS for key: " << key << ", get response code: " << responseCode);
        return responseCode;
    }

    auto data = reinterpret_cast<const uint8_t *>(response->DataPtr());
    STORE_ASSERT_RETURN(data != nullptr, SM_MALLOC_FAILED);
    SmemMessage responseBody;
    auto ret = SmemMessagePacker::Unpack(data, response->DataLen(), responseBody);
    if (ret < 0) {
        STORE_LOG_ERROR("unpack response body failed, result: " << ret);
        return -1;
    }

    if (responseBody.values.empty()) {
        STORE_LOG_ERROR("response body has no value");
        return -1;
    }

    exists = std::move(responseBody.values[0]);
    if (responseBody.values.size() > 1) { // cas failed
        return StoreErrorCode::RESTORE;
    } else {
        return 0;
    }
}

Result
TcpConfigStore::Watch(const std::string &key,
                      const std::function<void(int result, const std::string &, const std::vector<uint8_t> &)> &notify,
                      uint32_t &wid) noexcept
{
    if (key.empty() || key.length() > MAX_KEY_LEN_CLIENT) {
        STORE_LOG_ERROR("key length is invalid, keyLen: " << key.length() << ", maxLen: " << MAX_KEY_LEN_CLIENT);
        return StoreErrorCode::INVALID_KEY;
    }

    SmemMessage request{MessageType::WATCH};
    request.keys.push_back(key);

    auto packedRequest = SmemMessagePacker::Pack(request);
    auto ret = SendWatchRequest(
        packedRequest, [key, notify](int res, const std::vector<uint8_t> &value) { notify(res, key, value); }, wid,
        key);
    if (ret != SM_OK) {
        STORE_LOG_ERROR_LIMIT("send get for key: " << key << ", get null response");
        return ret;
    }

    STORE_LOG_DEBUG("watch for key: " << key << ", id: " << wid);
    return SM_OK;
}

Result TcpConfigStore::Watch(WatchRankType type, const std::function<void(WatchRankType, uint32_t, Result)> &notify,
                             uint32_t &wid) noexcept
{
    if (type != WATCH_RANK_LINK_DOWN) {
        STORE_LOG_ERROR("invalid watch rank type: " << type);
        return SM_INVALID_PARAM;
    }

    SmemMessage request{MessageType::WATCH_RANK_STATE};
    request.keys.emplace_back(WATCH_RANK_DOWN_KEY);
    auto packedRequest = SmemMessagePacker::Pack(request);
    auto ret = SendWatchRequest(
        packedRequest,
        [notify](int res, const std::vector<uint8_t> &value) {
            if (res == SM_OK && value.size() == sizeof(uint32_t)) {
                notify(WATCH_RANK_LINK_DOWN, *static_cast<const uint32_t *>(static_cast<const void *>(value.data())),
                       SM_OK);
            } else {
                notify(WATCH_RANK_LINK_DOWN, 0, SM_ERROR);
            }
        },
        wid, WATCH_RANK_DOWN_KEY);
    if (ret != SM_OK) {
        STORE_LOG_ERROR("send watch for rank down get null response");
        return ret;
    }

    STORE_LOG_DEBUG("send watch for rank down success, id: " << wid);
    return SM_OK;
}

Result TcpConfigStore::Unwatch(uint32_t wid) noexcept
{
    std::shared_ptr<ClientCommonContext> watchContext;
    std::string key;

    std::unique_lock<std::mutex> msgCtxLocker{msgCtxMutex_};
    auto pos = msgClientContext_.find(wid);
    if (pos != msgClientContext_.end() && !pos->second->Blocking()) {
        auto *wctx = dynamic_cast<ClientWatchContext *>(pos->second.get());
        if (wctx != nullptr) {
            key = wctx->GetKey();
        }
        watchContext = std::move(pos->second);
        msgClientContext_.erase(pos);
    }
    msgCtxLocker.unlock();

    if (watchContext == nullptr) {
        STORE_LOG_DEBUG("unwatch for id: " << wid << ", not exist.");
        return NOT_EXIST;
    }

    if (!key.empty() && accClientLink_ != nullptr && accClientLink_->Established()) {
        SmemMessage unwatchReq{MessageType::UNWATCH};
        unwatchReq.keys.push_back(key);
        auto packed = SmemMessagePacker::Pack(unwatchReq);
        auto dataBuf = ock::acc::AccDataBuffer::Create(packed.data(), packed.size());
        LocalNonBlockSend(0, reqSeqGen_.fetch_add(1U), dataBuf, nullptr);
    }

    return SM_OK;
}

std::shared_ptr<ock::acc::AccTcpRequestContext> TcpConfigStore::SendMessageBlocked(const std::vector<uint8_t> &reqBody,
                                                                                   int waitTimeoutSec) noexcept
{
    auto seqNo = reqSeqGen_.fetch_add(1U);
    auto dataBuf = ock::acc::AccDataBuffer::Create(reqBody.data(), reqBody.size());
    STORE_ASSERT_RETURN(accClientLink_ != nullptr, nullptr);
    std::mutex waitRespMutex;
    std::condition_variable waitRespCond;
    auto waitContext = std::make_shared<ClientWaitContext>(waitRespMutex, waitRespCond);
    STORE_ASSERT_RETURN(waitContext != nullptr, nullptr);
    std::unique_lock<std::mutex> msgCtxLocker{msgCtxMutex_};
    msgClientContext_.emplace(seqNo, waitContext);
    msgCtxLocker.unlock();
    auto ret = LocalNonBlockSend(0, seqNo, dataBuf, nullptr);
    if (ret != SM_OK) {
        std::unique_lock<std::mutex> msgCtxLocker0{msgCtxMutex_};
        msgClientContext_.erase(seqNo);
        msgCtxLocker0.unlock();
        STORE_LOG_ERROR("send message failed, result: " << ret);
        return nullptr;
    }

    auto response = waitContext->WaitFinishedFor(std::chrono::seconds(waitTimeoutSec));
    if (response == nullptr) {
        // 等待超时：移除挂起上下文，避免迟到的响应占用表项（响应本身会被丢弃）
        std::unique_lock<std::mutex> msgCtxLocker1{msgCtxMutex_};
        msgClientContext_.erase(seqNo);
    }
    return response;
}

Result TcpConfigStore::SendMessageNonBlock(const std::vector<uint8_t> &reqBody) noexcept
{
    STORE_ASSERT_RETURN(accClientLink_ != nullptr, SM_ERROR);
    auto seqNo = reqSeqGen_.fetch_add(1U);
    auto dataBuf = ock::acc::AccDataBuffer::Create(reqBody.data(), reqBody.size());
    if (dataBuf.Get() == nullptr) {
        STORE_LOG_ERROR("create data buffer failed, seqNo: " << seqNo);
        return SM_ERROR;
    }
    auto ret = LocalNonBlockSend(0, seqNo, dataBuf, nullptr);
    return ret;
}

void TcpConfigStore::SetRankId(const int32_t &rankId) noexcept
{
    rankId_ = rankId;
    if (rankId >= 0) {
        SetLocalRankId(static_cast<uint32_t>(rankId));
        if (asyncDispatcher_ == nullptr) {
            InitAsyncDispatcher(static_cast<uint32_t>(rankId));
        }
    } else {
        SetLocalRankId(UINT32_MAX);
    }
    STORE_LOG_INFO("Set rankId: " << rankId_);
}

Result TcpConfigStore::ReConnectAfterBroken(int reconnectRetryTimes) noexcept
{
    auto retryMaxTimes = reconnectRetryTimes < 0 ? CONNECT_RETRY_MAX_TIMES : reconnectRetryTimes;
    ock::acc::AccConnReq connReq;
    connReq.reconnect = 1; // reconnection
    connReq.rankId =
        rankId_ >= 0 ? ((static_cast<uint64_t>(worldSize_) << WORLD_SIZE_SHIFT) | static_cast<uint64_t>(rankId_))
                     : ((static_cast<uint64_t>(worldSize_) << WORLD_SIZE_SHIFT) | std::numeric_limits<uint32_t>::max());
    auto result = accClient_->ConnectToPeerServer(serverIp_, serverPort_, connReq, retryMaxTimes, accClientLink_);
    if (result != 0) {
        STORE_LOG_ERROR_LIMIT("Reconnect to server failed, ip: " << serverIp_ << " port: " << serverPort_
                                                                 << " result: " << result);
        return result;
    }
    STORE_LOG_INFO("Reconnect to server successful, rankId: " << rankId_);
    if (reconnectHandler) {
        (void)reconnectHandler();
    }
    SetConnectStatus(true);
    return SM_OK;
}

bool TcpConfigStore::ProbeLeaderAlive(int timeoutMs) noexcept
{
    if (accClientLink_ == nullptr || !GetConnectStatus()) {
        return false;
    }
    // 探测不存在的 key：只要服务端返回（含 NOT_EXIST）即证明链路与服务端均可用
    constexpr const char *PROBE_KEY = "__leader_link_probe__";
    SmemMessage request{MessageType::GET};
    request.keys.emplace_back(PROBE_KEY);
    request.userDef = timeoutMs;
    auto packedRequest = SmemMessagePacker::Pack(request);
    // 等待上限略大于服务端处理超时，保证正常情况一定能等到响应
    auto response = SendMessageBlocked(packedRequest, timeoutMs / 1000 + 1);
    if (response == nullptr) {
        STORE_LOG_WARN("leader link probe failed, link considered not alive");
        return false;
    }
    STORE_LOG_DEBUG("leader link probe succeeded, link alive");
    return true;
}

bool TcpConfigStore::GetConnectStatus() noexcept
{
    return isConnect_.load();
}

void TcpConfigStore::SetConnectStatus(bool status) noexcept
{
    isConnect_.store(status);
}

void TcpConfigStore::RegisterClientBrokenHandler(const ConfigStoreClientBrokenHandler &handler) noexcept
{
    std::lock_guard<std::mutex> guard(brokenHandlerMutex_);
    brokenHandler_.push_back(handler);
}

void TcpConfigStore::RegisterServerBrokenHandler(const ConfigStoreServerBrokenHandler &handler) noexcept
{
    if (accServer_ == nullptr) {
        STORE_LOG_INFO("accServer_ is null, cannot register server broken handler");
        return;
    }
    accServer_->RegisterBrokenLinkCHandler(handler);
}

Result TcpConfigStore::LinkBrokenHandler(const ock::acc::AccTcpLinkComplexPtr &link) noexcept
{
    STORE_LOG_WARN("link broken, linkId: " << link->Id());
    SetConnectStatus(false);
    std::vector<ConfigStoreClientBrokenHandler> handlers;
    {
        std::lock_guard<std::mutex> guard(brokenHandlerMutex_);
        handlers = brokenHandler_;
    }
    for (auto &handler : handlers) {
        STORE_LOG_INFO("link broken, linkId: " << link->Id() << ", call extern handler.");
        (void)handler();
    }
    std::unordered_map<uint32_t, std::shared_ptr<ClientCommonContext>> tempContext;
    std::unique_lock<std::mutex> msgCtxLocker{msgCtxMutex_};
    tempContext.swap(msgClientContext_);
    msgCtxLocker.unlock();

    for (auto &it : tempContext) {
        it.second->SetFailedFinish();
    }

    return SM_OK;
}

Result TcpConfigStore::ReceiveResponseHandler(const ock::acc::AccTcpRequestContext &context) noexcept
{
    STORE_LOG_DEBUG("client received message: " << context.SeqNo());
    std::shared_ptr<ClientCommonContext> clientContext;

    std::unique_lock<std::mutex> msgCtxLocker{msgCtxMutex_};
    auto pos = msgClientContext_.find(context.SeqNo());
    if (pos != msgClientContext_.end()) {
        if (pos->second->OnlyOneTime()) {
            clientContext = std::move(pos->second);
            msgClientContext_.erase(pos);
        } else {
            clientContext = pos->second;
        }
    }
    msgCtxLocker.unlock();

    if (clientContext != nullptr) {
        clientContext->SetFinished(context);
        return SM_OK;
    }

    auto data = reinterpret_cast<const uint8_t *>(context.DataPtr());
    auto dataLen = context.DataLen();
    if (data == nullptr || dataLen == 0) {
        STORE_LOG_WARN("receive response(" << context.SeqNo() << ") not sent request and no data.");
        return SM_ERROR;
    }

    SmemMessage msg;
    auto unpackRet = SmemMessagePacker::Unpack(data, dataLen, msg);
    if (unpackRet < 0 || msg.mt != MessageType::CONTROL) {
        STORE_LOG_WARN("rsp(" << context.SeqNo() << ") unpack ret=" << unpackRet << " mt=" << static_cast<int>(msg.mt));
        return SM_ERROR;
    }

    return HandleControlMessage(msg);
}

Result TcpConfigStore::SendWatchRequest(const std::vector<uint8_t> &reqBody,
                                        const std::function<void(int result, const std::vector<uint8_t> &)> &notify,
                                        uint32_t &id, const std::string &key) noexcept
{
    auto seqNo = reqSeqGen_.fetch_add(1U);
    auto dataBuf = ock::acc::AccDataBuffer::Create(reqBody.data(), reqBody.size());
    STORE_ASSERT_RETURN(accClientLink_ != nullptr, SM_NOT_INITIALIZED);
    auto watchContext = std::make_shared<ClientWatchContext>(notify, false, key);
    STORE_ASSERT_RETURN(watchContext != nullptr, SM_MALLOC_FAILED);
    std::unique_lock<std::mutex> msgCtxLocker{msgCtxMutex_};
    msgClientContext_.emplace(seqNo, std::move(watchContext));
    msgCtxLocker.unlock();
    auto ret = LocalNonBlockSend(0, seqNo, dataBuf, nullptr);
    if (ret != SM_OK) {
        msgCtxLocker.lock();
        msgClientContext_.erase(seqNo);
        msgCtxLocker.unlock();
        STORE_LOG_ERROR_LIMIT("send msg failed, result: " << ret << ", Established: " << accClientLink_->Established());
        return ret;
    }

    id = seqNo;
    return SM_OK;
}

void TcpConfigStore::HeartBeat() noexcept
{
    pthread_setname_np(pthread_self(), "config_store_hb");
    while (isRunning_.load()) {
        if (isConnect_.load()) {
            SmemMessage request{MessageType::HEARTBEAT};
            auto packedRequest = SmemMessagePacker::Pack(request);
            auto dataBuf = ock::acc::AccDataBuffer::Create(packedRequest.data(), packedRequest.size());
            if (dataBuf == nullptr) {
                STORE_LOG_ERROR("create data buffer failed, no enough mem");
                continue;
            }
            auto ret = LocalNonBlockSend(0, 0, dataBuf, nullptr);
            if (ret != SM_OK) {
                STORE_LOG_ERROR("send message failed, result: " << ret);
            }
        } else {
            TryReconnectByHeartbeat();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL));
    }

    STORE_LOG_INFO("TcpConfigStore heart beat thread exit.");
}

void TcpConfigStore::TryReconnectByHeartbeat() noexcept
{
    // 有外部断链接管方（HA 选举、组引擎等注册的 broken handler）时，主动权交回上层，
    // 避免心跳线程与上层并发重连同一个 accClient_。
    if (HasExternalBrokenHandler()) {
        return;
    }
    ReconnectWithDedup();
}

void TcpConfigStore::ReconnectWithDedup() noexcept
{
    // 单飞去重：send 触发与心跳看门狗共用此入口，保证并发时只有一个
    // ReConnectAfterBroken 在跑，避免两个 ConnectToPeerServer 同时操作 accClient_。
    bool expected = false;
    if (!reconnecting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    // 每次只做一次有界重连尝试（acc 层 1 次 connect），失败则等下一个触发源再试。
    (void)ReConnectAfterBroken(1);
    reconnecting_.store(false, std::memory_order_release);
}

bool TcpConfigStore::HasExternalBrokenHandler() noexcept
{
    std::lock_guard<std::mutex> guard(brokenHandlerMutex_);
    return !brokenHandler_.empty();
}

Result TcpConfigStore::HandleAddToWhitelist(SmemMessage &msg) noexcept
{
    uint32_t rankId = 0;
    std::vector<RankFullInfo> others;
    if (SmemMessage::UnpackAddToWhitelist(msg, rankId, others) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: AddToWhitelist unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        auto ret = executor_->AddToWhitelist(rankId, others, msg.requestId);
        if (ret != 0) {
            STORE_LOG_ERROR("HandleControlMessage: AddToWhitelist failed, rankId: " << rankId << " ret: " << ret);
        }
    }
    STORE_LOG_INFO("[GM][Client][Recv] rank=" << localRankId_ << " type=ADDWLST src=" << rankId << " reqId="
                                              << msg.requestId << " peers=[" << JoinRankIds(others) << "]");
    return SM_OK;
}

Result TcpConfigStore::HandleRemoveFromWhitelist(SmemMessage &msg) noexcept
{
    uint32_t rankId = 0;
    std::vector<RankBaseInfo> others;
    if (SmemMessage::UnpackRemoveFromWhitelist(msg, rankId, others) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: RemoveFromWhitelist unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        auto ret = executor_->RemoveFromWhitelist(rankId, others, msg.requestId);
        if (ret != 0) {
            STORE_LOG_ERROR("HandleControlMessage: RemoveFromWhitelist failed, rankId: " << rankId << " ret: " << ret);
        }
    }
    STORE_LOG_INFO("[GM][Client][Recv] rank=" << localRankId_ << " type=REMWLST src=" << rankId << " reqId="
                                              << msg.requestId << " peers=[" << JoinRankIds(others) << "]");
    return SM_OK;
}

Result TcpConfigStore::HandleEstablishConnection(SmemMessage &msg) noexcept
{
    uint32_t rankId = 0;
    std::vector<RankFullInfo> others;
    if (SmemMessage::UnpackEstablishConnection(msg, rankId, others) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: EstablishConnection unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        auto ret = executor_->EstablishConnection(rankId, others, msg.requestId);
        if (ret != 0) {
            STORE_LOG_ERROR("HandleControlMessage: EstablishConnection failed, rankId: " << rankId << " ret: " << ret);
        }
    }
    STORE_LOG_INFO("[GM][Client][Recv] rank=" << localRankId_ << " type=ESTCONN src=" << rankId << " reqId="
                                              << msg.requestId << " peers=[" << JoinRankIds(others) << "]");
    return SM_OK;
}

Result TcpConfigStore::HandleQueryLinkState(SmemMessage &msg) noexcept
{
    uint32_t rankId = 0;
    if (SmemMessage::UnpackQueryLinkState(msg, rankId) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: QueryLinkState unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        executor_->QueryLinkState(rankId);
    }
    std::vector<LinkStateEntry> entries;
    auto *executor = static_cast<SmemGroupManagerClient *>(executor_.Get());
    if (executor != nullptr) {
        entries = executor->GetLastQueryLinkStateEntries();
    }
    SmemMessage response = SmemMessage::PackLinkStateResponse(localRankInfo_, entries);
    auto packedResponse = SmemMessagePacker::Pack(response);
    STORE_LOG_INFO("[GM][Client][Send] rank=" << localRankId_ << " type=LNKSRSP reqId=" << msg.requestId << " entries="
                                              << entries.size() << ", role=" << RankRoleName(localRankInfo_.role));
    auto sendRet = SendMessageNonBlock(packedResponse);
    STORE_LOG_INFO("[GM][Client][Send] rank=" << localRankId_ << " type=LNKSRSP return=" << sendRet);
    if (sendRet != SM_OK) {
        STORE_LOG_ERROR("HandleControlMessage: QueryLinkState response send failed: " << sendRet);
        return sendRet;
    }
    return SM_OK;
}

Result TcpConfigStore::HandlePromoteToActive(SmemMessage &msg) noexcept
{
    uint32_t rankId = 0;
    if (SmemMessage::UnpackPromoteToActive(msg, rankId) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: PromoteToActive unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        auto ret = executor_->PromoteToActive(rankId);
        if (ret != 0) {
            STORE_LOG_ERROR("HandleControlMessage: PromoteToActive failed, rankId: " << rankId << " ret: " << ret);
        }
    }
    STORE_LOG_INFO("[GM][Client][Recv] rank=" << localRankId_ << " type=PROMOTE_TO_ACTIVE"
                                              << " promotedRank=" << rankId);
    return SM_OK;
}

Result TcpConfigStore::HandleAddSlices(SmemMessage &msg) noexcept
{
    uint32_t extendingRankId = 0;
    MultiBytes newSlices;
    if (SmemMessage::UnpackAddSlices(msg, extendingRankId, newSlices) < 0) {
        STORE_LOG_ERROR("HandleControlMessage: AddSlices unpack failed");
        return SM_ERROR;
    }
    if (executor_ != nullptr) {
        executor_->AddSlices(extendingRankId, newSlices, msg.requestId);
    }
    STORE_LOG_INFO("[GM][Client][Recv] rank=" << localRankId_ << " type=ADD_SLICES"
                                              << " extendingRank=" << extendingRankId
                                              << " sliceCount=" << newSlices.size());
    return SM_OK;
}

Result TcpConfigStore::HandleControlMessage(SmemMessage &msg) noexcept
{
    int8_t op = SmemMessage::GetControlOp(msg);
    if (op < 0) {
        STORE_LOG_ERROR("HandleControlMessage: invalid or missing ControlOp");
        return SM_ERROR;
    }

    switch (static_cast<ControlOp>(op)) {
        case ControlOp::CONTROL_ADD_TO_WHITELIST:
            return HandleAddToWhitelist(msg);
        case ControlOp::CONTROL_REMOVE_FROM_WHITELIST:
            return HandleRemoveFromWhitelist(msg);
        case ControlOp::CONTROL_ESTABLISH_CONNECTION:
            return HandleEstablishConnection(msg);
        case ControlOp::CONTROL_QUERY_LINK_STATE:
            return HandleQueryLinkState(msg);
        case ControlOp::CONTROL_PROMOTE_TO_ACTIVE:
            return HandlePromoteToActive(msg);
        case ControlOp::CONTROL_ADD_SLICES:
            return HandleAddSlices(msg);
        default:
            STORE_LOG_ERROR("HandleControlMessage: unhandled ControlOp: " << static_cast<int>(op));
            return SM_ERROR;
    }
}

void TcpConfigStore::GetClusterStatusJson(std::string &out) noexcept
{
    out.append("{\"type\":\"tcp\"");
    out.append(",\"model\":")
        .append(std::to_string(startupModel_))
        .append(",\"connect\":")
        .append(GetConnectStatus() ? "true" : "false");
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (accServer_ == nullptr) {
        out.push_back('}');
        return;
    }
    out.append(",\"server\":");
    accServer_->GetClusterStatusJson(out);
    out.push_back('}');
}

} // namespace smem
} // namespace ock
