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
#include <sys/stat.h>

#include <pthread.h>
#include <algorithm>
#include <climits>
#include <iomanip>
#include <sstream>
#include <map>
#include <cstring>
#include <utility>
#include "acc_tcp_server.h"
#include "smem_config_store_logger.h"
#include "smem_message_packer.h"
#include "smem_config_store.h"
#include "smem_tcp_config_store_ssl_helper.h"
#include "mf_str_util.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_monotonic_time.h"
#include "smem_tcp_config_store_server.h"

namespace ock {
namespace smem {

std::atomic<uint64_t> StoreWaitContext::idGen_{1UL};
std::atomic<uint32_t> g_ctrlReqSeq{0};
constexpr uint16_t MAX_U16_INDEX = 65535;
constexpr uint64_t SERVER_RECOVER_TIME = 60 * 1000 * 1000; // 60s
constexpr uint64_t RECOVER_PERIOD_TIME = 10;               // 10s
constexpr uint32_t DEFAULT_HEARTBEAT_TIMEOUT_S = 60;
constexpr uint32_t MAX_HEARTBEAT_TIMEOUT_S = 3600; // 1 hour
constexpr uint32_t DEFAULT_LINK_RETRY_TIMEOUT_S = 2;
constexpr uint32_t MIN_LINK_RETRY_TIMEOUT_S = 1;
constexpr uint32_t MAX_LINK_RETRY_TIMEOUT_S = 600; // 10 minutes
constexpr int32_t EPHEMERAL_KEY_TTL_SEC = 5;
constexpr int32_t PERSISTENT_KEY_TTL_SEC = 0;
constexpr size_t MAX_WRITE_TOTAL_SIZE = MAX_VALUE_SIZE * 16ULL;
constexpr uint32_t LINK_SEND_QUEUE_SIZE = 4096;
constexpr uint32_t TIMER_POLL_MS = 1;

AccStoreServer::AccStoreServer(std::string ip, uint16_t port, uint32_t worldSize, StoreBackendPtr backend,
                               bool skipRecover) noexcept
    : requestHandlers_{{MessageType::SET, &AccStoreServer::SetHandler},

                       {MessageType::GET, &AccStoreServer::GetHandler},
                       {MessageType::PREFIX, &AccStoreServer::PrefixGetHandler},
                       {MessageType::WATCH, &AccStoreServer::WatchHandler},
                       {MessageType::ADD, &AccStoreServer::AddHandler},
                       {MessageType::REMOVE, &AccStoreServer::RemoveHandler},
                       {MessageType::APPEND, &AccStoreServer::AppendHandler},
                       {MessageType::CAS, &AccStoreServer::CasHandler},
                       {MessageType::WRITE, &AccStoreServer::WriteHandler},
                       {MessageType::QUERY_ALIVE, &AccStoreServer::QueryAliveHandler},
                       {MessageType::WATCH_RANK_STATE, &AccStoreServer::WatchRankStateHandler},
                       {MessageType::HEARTBEAT, &AccStoreServer::HeartbeatHandler},
                       {MessageType::UNWATCH, &AccStoreServer::UnwatchHandler},
                       {MessageType::CONTROL, &AccStoreServer::ControlHandler}},
      backend_(std::move(backend)), listenIp_{std::move(ip)}, listenPort_{port}, worldSize_{worldSize},
      skipRecover_{skipRecover},
      heartBeatTimeoutS_{ParseHeartbeatTimeoutS(mf::env::MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S)}
{
    auto sendFunc = [this](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
        return SendControlToRank(targetRankId, data);
    };
    sender_ = sendFunc;
    groupManager_ = SmMakeRef<SmemGroupManagerServer>(
        sendFunc, worldSize_, ParseLinkRetryTimeoutS(mf::env::MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S));
    if (backend_ != nullptr && backend_->IsDistributed()) {
        etcdStore_ = std::make_unique<EtcdStateStore>(backend_);
        groupManager_->SetOnStateChangeCallback([this]() {
            if (etcdStore_) {
                etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
                etcdStore_->FlushLinks(groupManager_->GetLinks(), groupManager_->GetMaxRanks());
            }
        });
    }
}

AccStoreServer::AccStoreServer(std::string ip, uint16_t port, SmemGroupManagerServerPtr groupManager,
                               StoreBackendPtr backend, bool skipRecover) noexcept
    : requestHandlers_{{MessageType::SET, &AccStoreServer::SetHandler},
                       {MessageType::GET, &AccStoreServer::GetHandler},
                       {MessageType::PREFIX, &AccStoreServer::PrefixGetHandler},
                       {MessageType::WATCH, &AccStoreServer::WatchHandler},
                       {MessageType::ADD, &AccStoreServer::AddHandler},
                       {MessageType::REMOVE, &AccStoreServer::RemoveHandler},
                       {MessageType::APPEND, &AccStoreServer::AppendHandler},
                       {MessageType::CAS, &AccStoreServer::CasHandler},
                       {MessageType::WRITE, &AccStoreServer::WriteHandler},
                       {MessageType::QUERY_ALIVE, &AccStoreServer::QueryAliveHandler},
                       {MessageType::WATCH_RANK_STATE, &AccStoreServer::WatchRankStateHandler},
                       {MessageType::HEARTBEAT, &AccStoreServer::HeartbeatHandler},
                       {MessageType::UNWATCH, &AccStoreServer::UnwatchHandler},
                       {MessageType::CONTROL, &AccStoreServer::ControlHandler}},
      backend_(std::move(backend)), groupManager_(std::move(groupManager)), listenIp_{std::move(ip)}, listenPort_{port},
      worldSize_{UINT32_MAX}, skipRecover_{skipRecover},
      heartBeatTimeoutS_{ParseHeartbeatTimeoutS(mf::env::MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S)}
{
    auto sendFunc = [this](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
        return SendControlToRank(targetRankId, data);
    };
    sender_ = sendFunc;
    groupManager_->SetSendFunc(sender_);
    if (backend_ != nullptr && backend_->IsDistributed()) {
        etcdStore_ = std::make_unique<EtcdStateStore>(backend_);
        groupManager_->SetOnStateChangeCallback([this]() {
            if (etcdStore_) {
                etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
                etcdStore_->FlushLinks(groupManager_->GetLinks(), groupManager_->GetMaxRanks());
            }
        });
    }
}

Result AccStoreServer::Startup(const smem_tls_config &tlsConfig) noexcept
{
    std::unique_lock<std::mutex> guard(storeMutex_);
    if (accTcpServer_ != nullptr) {
        STORE_LOG_WARN("tcp store server already startup");
        return SM_OK;
    }

    accTcpServer_ = acc::AccTcpServer::Create();
    if (accTcpServer_ == nullptr) {
        STORE_LOG_ERROR("create acc tcp server failed");
        return SM_NEW_OBJECT_FAILED;
    }

    accTcpServer_->RegisterNewRequestHandler(
        0, [this](const ock::acc::AccTcpRequestContext &context) { return ReceiveMessageHandler(context); });
    accTcpServer_->RegisterNewLinkHandler(
        [this](const ock::acc::AccConnReq &req, const ock::acc::AccTcpLinkComplexPtr &link) {
            return LinkConnectedHandler(req, link);
        });
    accTcpServer_->RegisterLinkBrokenHandler(
        [this](const ock::acc::AccTcpLinkComplexPtr &link) { return LinkBrokenHandler(link); });

    acc::AccTcpServerOptions options{};
    options.listenIp = listenIp_;
    options.listenPort = listenPort_;
    options.enableListener = true;
    options.linkSendQueueSize = LINK_SEND_QUEUE_SIZE;
    acc::AccTlsOption tlsOption = GetAccTlsOption(tlsConfig);
    if (tlsOption.enableTls) {
        if (PrepareTlsForAccTcpServer(accTcpServer_, tlsConfig) != SM_OK) {
            STORE_LOG_ERROR("Failed to prepare TLS for AccTcpServer");
            return SM_ERROR;
        }
    }

    auto result = accTcpServer_->Start(options, tlsOption);
    if (result == ock::acc::ACC_LINK_ADDRESS_IN_USE) {
        STORE_LOG_TRACE("startup acc tcp server on port: " << listenPort_ << " already in use.");
        return SM_RESOURCE_IN_USE;
    }
    if (result != SM_OK) {
        STORE_LOG_ERROR("startup acc tcp server on port: " << listenPort_ << " failed: " << result);
        return SM_ERROR;
    }

    startupTimestamp_ = mf::MonotonicTime::TimeUs();
    state_.store(SS_INITED);

    timerThread_ = std::thread{[this]() { TimerThreadTask(); }};
    rankStateThread_ = std::thread{[this]() { RankStateTask(); }};
    checkerThread_ = std::thread{[this]() { CheckerThreadTask(); }};
    groupManager_->Start();
    // RestoreFromEtcdIfNeeded 内部需再次获取 storeMutex_（非递归锁），先释放避免自死锁。
    guard.unlock();
    RestoreFromEtcdIfNeeded();
    guard.lock();
    STORE_LOG_DEBUG("startup acc tcp server on port: " << listenPort_);
    if (!backend_->IsDistributed()) {
        return SM_OK;
    }
    if (LaunchCleanupThread() != SM_OK) {
        STORE_LOG_ERROR("LaunchCleanupThread failed");
        guard.unlock();
        Shutdown(false);
        return SM_ERROR;
    }
    return SM_OK;
}

void AccStoreServer::Shutdown(bool afterFork) noexcept
{
    STORE_LOG_TRACE("start to shutdown Acc Store Server");
    {
        if (accTcpServer_ == nullptr) {
            return;
        }
        if (afterFork) {
            accTcpServer_->StopAfterFork();
            if (timerThread_.joinable()) {
                timerThread_.detach();
            }
        } else {
            accTcpServer_->Stop();
        }
        std::unique_lock<std::mutex> lockGuard{storeMutex_};
        state_.store(SS_EXITED);
        shouldStop_.store(true);
        storeCond_.notify_all();
        recoveryCond_.notify_all();
        accTcpServer_ = nullptr;
    }

    groupManager_->Stop();

    if (timerThread_.joinable() && !afterFork) {
        try {
            timerThread_.join();
        } catch (const std::system_error &e) {
            STORE_LOG_ERROR("thread join failed: " << e.what());
        }
    }
    if (rankStateThread_.joinable()) {
        rankStateThread_.join();
    }
    if (checkerThread_.joinable()) {
        checkerThread_.join();
    }
    shouldStop_.store(true);
    if (cleanupThread_.joinable()) {
        cleanupThread_.join();
    }
    STORE_LOG_TRACE("finished shutdown Acc Store Server");
}

void AccStoreServer::RegisterBrokenLinkCHandler(const ConfigStoreServerBrokenHandler &handler) noexcept
{
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    externalBrokenHandler_ = handler;
}

Result AccStoreServer::ReceiveMessageHandler(const ock::acc::AccTcpRequestContext &context) noexcept
{
    auto data = reinterpret_cast<const uint8_t *>(context.DataPtr());
    if (data == nullptr) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle get null request body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "request no body");
        return SM_INVALID_PARAM;
    }

    SmemMessage requestMessage;
    auto size = SmemMessagePacker::Unpack(data, context.DataLen(), requestMessage);
    if (size < 0) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") ptr:" << context.DataPtr() << " len:" << context.DataLen());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request");
        return SM_ERROR;
    }

    auto pos = requestHandlers_.find(requestMessage.mt);
    if (pos == requestHandlers_.end()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid message type: " << requestMessage.mt);
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request message type");
        return SM_ERROR;
    }

    return (this->*(pos->second))(context, requestMessage);
}

// call in storeMutex_
// 准入判定：无旧 rank（后端空集）时立即退出恢复并接受新连接；其余场景只读。
bool AccStoreServer::CanReceiveNewLink()
{
    uint32_t state = state_.load();
    if (state == SS_INITED) {
        const uint32_t dstState = skipRecover_ ? SS_NORMAL : SS_RECOVERING;
        if (state_.compare_exchange_strong(state, dstState)) {
            STORE_LOG_TRACE("change server state from INITED to " << (skipRecover_ ? "NORMAL" : "RECOVER"));
            if (dstState == SS_NORMAL) {
                recoveryCond_.notify_all();
            }
        }
        // CAS 成功/失败都不会保证把最新值写回 state，需重新加载再继续处理。
        state = state_.load();
    }
    if (state == SS_RECOVERING && aliveRankFromBackend_.empty()) {
        // 无旧 rank（全新部署/二任 leader 且后端无持久化存活 rank）时立即退出恢复：
        // 该场景 cleanup 线程被跳过（LaunchCleanupThread isFirstUpdate 直返），
        // 且 reconnect==0 首条连接在入口被拒、到不了 LinkConnectedHandler 末尾的
        // 迁移点——此处若不迁移，状态将永久卡在 RECOVERING 拒绝所有新连接。
        uint32_t srcState = state;
        UpdateRecoverState(srcState);
        state = state_.load();
    }
    // SS_RECOVERED 表示恢复完成，必须接受新连接（无旧 rank 时 cleanupThread 被跳过）。
    return (state == SS_NORMAL || state == SS_RECOVERED);
}

void AccStoreServer::UpdateRecoverState(uint32_t &srcState)
{
    uint64_t nowT = mf::MonotonicTime::TimeUs();
    // 无旧 rank 时立即退出恢复，避免第二任 leader 停在 RECOVERING 拒绝新连接。
    if (aliveRankFromBackend_.empty()) {
        if (state_.compare_exchange_strong(srcState, SS_RECOVERED)) {
            STORE_LOG_INFO("state RECOVERED (no old ranks to recover)");
            recoveryCond_.notify_all();
        }
        return;
    }
    // Exit recovery when:
    // 1. All old ranks (aliveRankFromBackend_) have reconnected (in reconnectedRankSet_), OR
    // 2. SERVER_RECOVER_TIME (60s) timeout kicks in
    bool allReconnected = std::all_of(aliveRankFromBackend_.begin(), aliveRankFromBackend_.end(),
                                      [this](uint32_t rk) { return reconnectedRankSet_.count(rk) > 0; });
    if (!allReconnected && nowT <= startupTimestamp_ + SERVER_RECOVER_TIME) {
        return;
    }
    if (state_.compare_exchange_strong(srcState, SS_RECOVERED)) {
        STORE_LOG_INFO("state RECOVERED" << (allReconnected ? " (all ranks reconnected)" : " (timeout)"));
        recoveryCond_.notify_all();
    }
}

Result AccStoreServer::LinkConnectedHandler(const ock::acc::AccConnReq &req,
                                            const ock::acc::AccTcpLinkComplexPtr &link) noexcept
{
    uint32_t worldSize = static_cast<uint32_t>(req.rankId >> 32);
    uint32_t rankId = static_cast<uint32_t>(req.rankId & 0xFFFFFFFF);
    STORE_LOG_TRACE("l:" << link->Id() << " ws:" << worldSize << " r:" << rankId << " rec:" << (int)req.reconnect);
    if (worldSize_ == std::numeric_limits<uint32_t>::max()) {
        // UINT32_MAX 表示未指定（如 follower），保持 worldSize_ 未固定，等待携带 world size 的 rank 连接。
        if (worldSize != std::numeric_limits<uint32_t>::max()) {
            STORE_ASSERT_RETURN(PersistWorldSize(worldSize) == SUCCESS, SM_ERROR);
            worldSize_ = worldSize;
            STORE_LOG_TRACE("Success to fix world size:" << worldSize_);
        } else {
            STORE_LOG_INFO("Connector world size unspecified, keep world size unfixed");
        }
    } else if (worldSize_ != worldSize && worldSize != std::numeric_limits<uint32_t>::max()) {
        // 未指定 world size 的连接跟随已固定值，避免误拒后续 follower/rank。
        STORE_LOG_ERROR("record world size: " << worldSize_ << " receive: " << worldSize);
        return SM_INVALID_PARAM;
    }

    std::unique_lock<std::mutex> lockGuard{storeMutex_};

    if (!CanReceiveNewLink() && req.reconnect == 0) {
        STORE_LOG_ERROR("[RECOVER] id:" << link->Id() << " s:" << state_.load() << " re:" << (int)req.reconnect);
        return SM_RECONNECT;
    }

    if (rankId >= std::numeric_limits<uint32_t>::max()) { // auto_rank
        return SM_OK;
    }

    // If this is a new connection (not reconnect) and the rank is already actively connected, reject.
    if (req.reconnect == 0 && reconnectedRankSet_.count(rankId) > 0) {
        STORE_LOG_ERROR("rankId:" << rankId << " has connected!");
        return SM_ERROR;
    }

    // A reconnect claiming a rankId that already has an active link would hijack the slot
    // from the rank currently holding it (e.g. a new client grabbed 0-3 while the old rank
    // was down). Reject the reconnect so the client falls back to auto_rank re-allocation.
    if (req.reconnect == 1 && rankLinks_.count(rankId) > 0) {
        STORE_LOG_ERROR("reconnect rank:" << rankId << " already active, reject, fallback to auto_rank");
        return SM_ERROR;
    }
    aliveRankSet_.insert(rankId);
    rankLinks_[rankId] = link;
    reconnectedRankSet_.insert(rankId);
    linkRankMap_[link->Id()] = rankId;
    if (groupManager_ != nullptr && req.reconnect == 1) {
        groupManager_->MarkRankReconnected(rankId);
    }
    STORE_ASSERT_RETURN(PersistAliveRankIds(aliveRankSet_) == SUCCESS, SM_ERROR);
    // 注册完成后评估恢复退出：本次接入的 rank 立即参与 allReconnected 判定，
    // "aliveRankFromBackend_ 中最后一个旧 rank 重连" 即时置位 SS_RECOVERED，
    // 无需等待下一条连接事件或 60s 超时。
    uint32_t srcState = state_.load();
    if (srcState == SS_RECOVERING) {
        UpdateRecoverState(srcState);
    }
    return SM_OK;
}

Result AccStoreServer::LinkBrokenHandler(const ock::acc::AccTcpLinkComplexPtr &link) noexcept
{
    return LinkBrokenHandler(link->Id());
}

Result AccStoreServer::LinkBrokenHandler(const uint32_t linkId) noexcept
{
    STORE_LOG_DEBUG("link broken, linkId: " << linkId);
    uint32_t rankId = std::numeric_limits<uint32_t>::max();
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    if (externalBrokenHandler_ != nullptr) {
        externalBrokenHandler_(linkId, backend_);
    }
    auto it = linkRankMap_.find(linkId);
    if (it != linkRankMap_.end()) {
        rankId = it->second;
        linkRankMap_.erase(it);
        aliveRankSet_.erase(rankId);
        reconnectedRankSet_.erase(rankId);
        rankLinks_.erase(rankId);
        PersistAliveRankIds(aliveRankSet_);
        STORE_LOG_TRACE("link broken, linkId: " << linkId << " remove rankId: " << rankId);
    }
    heartBeatMap_.erase(linkId);
    if (aliveRankSet_.empty()) {
        STORE_LOG_TRACE("all client link broken, will clear data");
        rankIndex_ = 0;
        backend_->Clear();
        waitCtx_.clear();
        keyWaiters_.clear();
        timedWaiters_.clear();
        rankStateWaiters_.clear();
        linkWatchList_.clear();
        watchWaiters_.clear();
        rankStateTaskQueue_ = {};
        linkRankMap_.clear();
        (void)backend_->Delete(KEY_ALIVE_RANK_LIST);
    }
    rankStateWaiters_.erase(linkId);
    for (auto &key : linkWatchList_[linkId]) {
        watchWaiters_[key].erase(linkId);
    }
    linkWatchList_.erase(linkId);
    if (rankId == std::numeric_limits<uint32_t>::max()) {
        STORE_LOG_WARN("broken link id: " << linkId << ", cannot find rank id.");
        return SM_OK;
    }
    if (groupManager_ != nullptr) {
        groupManager_->OnLinkBroken(rankId);
    }
    // 断开需如实持久化：alive_rank_list 已更新（去掉该 rank），此处同步写回
    // groupManager states 为 IDLE，避免新 leader 恢复时残留 ACTIVE，导致同 rankId
    // 重启的客户端 CheckIn 被拒（无法 Join）。锁内先取快照，etcd 写放到锁外，
    // 避免故障时同步重试写入阻塞持 storeMutex_ 的全部配置存储操作。
    std::vector<RankState> statesSnapshot;
    uint32_t maxRanks = 0;
    if (etcdStore_ != nullptr && groupManager_ != nullptr) {
        statesSnapshot = groupManager_->GetStates();
        maxRanks = groupManager_->GetMaxRanks();
    }
    rankStateTaskQueue_.push(rankId);
    storeCond_.notify_all();
    lockGuard.unlock();
    if (etcdStore_ != nullptr && !statesSnapshot.empty()) {
        auto ret = etcdStore_->PersistStates(statesSnapshot, maxRanks);
        if (ret != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("link broken, persist states failed, rankId: " << rankId
                                                                           << ", ret: " << static_cast<int>(ret));
        }
    }
    return SM_OK;
}

void AccStoreServer::GetWakeupList(const std::string &key, std::list<ock::acc::AccTcpRequestContext> &waiters,
                                   std::list<ock::acc::AccTcpRequestContext> &watchers) noexcept
{
    auto wPos = keyWaiters_.find(key);
    if (wPos != keyWaiters_.end()) {
        waiters = GetOutWaitersInLock(wPos->second);
        keyWaiters_.erase(wPos);
    }
    auto pos = watchWaiters_.find(key);
    if (pos != watchWaiters_.end()) {
        for (auto &watch : pos->second) {
            watchers.push_back(watch.second.ReqCtx());
        }
    }
}

Result AccStoreServer::SetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || request.values.size() != 1) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key value should be one");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    auto &value = request.values[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    STORE_LOG_DEBUG("SET REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    std::list<ock::acc::AccTcpRequestContext> wakeupWaiters;
    std::list<ock::acc::AccTcpRequestContext> wakeupWatchers;
    std::vector<uint8_t> reqVal;
    std::vector<uint8_t> oldVal;
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    auto ret = backend_->Get(key, oldVal);
    if (ret != SUCCESS || oldVal != value) { // not exist or update need to wake up waiter
        reqVal = value;
        GetWakeupList(key, wakeupWaiters, wakeupWatchers);
    }
    ret = backend_->Put(key, std::move(value), EPHEMERAL_KEY_TTL_SEC);
    lockGuard.unlock();

    ReplyWithMessage(context, ret, ret == SUCCESS ? "success" : "error");
    if (!wakeupWaiters.empty() || !wakeupWatchers.empty()) {
        WakeupWaiters(wakeupWaiters, wakeupWatchers, reqVal);
    }

    return SM_OK;
}

Result AccStoreServer::FindOrInsertRank(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    STORE_ASSERT_RETURN(context.Link() != nullptr, SM_INVALID_PARAM);
    auto linkId = context.Link()->Id();
    STORE_LOG_DEBUG("FindOrInsertRank start, linkId: " << linkId);

    SmemMessage responseMessage{request.mt};
    std::unique_lock<std::mutex> lockGuard{storeMutex_};

    // AutoRanking is in-memory only. Look up the rank assigned to this link.
    auto it = linkRankMap_.find(linkId);
    if (it != linkRankMap_.end()) {
        // Rank already assigned to this link → return it.
        union Transfer {
            uint32_t rankId;
            uint8_t date[4];
        } trans{};
        trans.rankId = it->second;
        responseMessage.values.emplace_back(trans.date, trans.date + sizeof(trans.date));
        lockGuard.unlock();
        auto response = SmemMessagePacker::Pack(responseMessage);
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
        return SM_OK;
    }

    // No rank assigned yet — allocate a new one.
    return AllocateAndReplyRank(context, responseMessage, linkId, lockGuard);
}

Result AccStoreServer::AllocateAndReplyRank(const ock::acc::AccTcpRequestContext &context, SmemMessage &responseMessage,
                                            uint32_t linkId, std::unique_lock<std::mutex> &lockGuard) noexcept
{
    if (aliveRankSet_.size() >= worldSize_) {
        lockGuard.unlock();
        STORE_LOG_ERROR("aliveRankSet_ full, sz=" << aliveRankSet_.size() << " worldSize=" << worldSize_);
        ReplyWithMessage(context, StoreErrorCode::ERROR, "error: worldSize rankSize bigger than worldSize.");
        return SM_ERROR;
    }

    // dump aliveRankSet_ before allocation
    {
        std::string aliveStr;
        for (auto r : aliveRankSet_) {
            aliveStr += std::to_string(r) + ",";
        }
        STORE_LOG_INFO("AllocAndReply: idx=" << rankIndex_ << " ws=" << worldSize_ << " alive=[" << aliveStr << "]");
    }

    uint32_t allocatedRank = FindFreeRankIndex(context, linkId);
    if (allocatedRank == UINT32_MAX) {
        lockGuard.unlock();
        ReplyWithMessage(context, StoreErrorCode::ERROR, "no available rank");
        return SM_ERROR;
    }

    union Transfer {
        uint32_t rankId;
        uint8_t date[4];
    } trans{};
    trans.rankId = allocatedRank;
    responseMessage.values.emplace_back(trans.date, trans.date + sizeof(trans.date));
    lockGuard.unlock();
    STORE_LOG_INFO("FindOrInsertRank ok link:" << linkId << " rank:" << trans.rankId << " ws:" << worldSize_);
    auto response = SmemMessagePacker::Pack(responseMessage);
    ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
    return 0;
}

uint32_t AccStoreServer::FindFreeRankIndex(const ock::acc::AccTcpRequestContext &context, uint32_t linkId) noexcept
{
    uint32_t scanCount = 0;
    for (; scanCount <= worldSize_; ++scanCount) {
        rankIndex_ %= worldSize_;
        if (aliveRankSet_.find(rankIndex_) == aliveRankSet_.end()) {
            aliveRankSet_.insert(rankIndex_);
            reconnectedRankSet_.insert(rankIndex_);
            linkRankMap_[linkId] = rankIndex_;
            rankLinks_[rankIndex_] = context.Link();
            STORE_LOG_INFO("AllocAndReply prePersist idx=" << rankIndex_ << " alive=" << aliveRankSet_.size());
            if (PersistAliveRankIds(aliveRankSet_) != SUCCESS) {
                aliveRankSet_.erase(rankIndex_);
                reconnectedRankSet_.erase(rankIndex_);
                linkRankMap_.erase(linkId);
                STORE_LOG_ERROR("persist alive rank failed");
                return UINT32_MAX;
            }
            STORE_LOG_INFO("PersistAliveRanks OK idx=" << rankIndex_ << " aSize=" << aliveRankSet_.size());
            return rankIndex_;
        }
        rankIndex_++;
    }
    return UINT32_MAX;
}

Result AccStoreServer::GetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || !request.values.empty()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key should be one and no values.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    if (key.compare(0, autoRankingStr_.size(), autoRankingStr_) == 0) {
        if (!GetStatus()) {
            STORE_LOG_WARN("GetStatus failed for AutoRanking request, key: " << key << ", seqNo: " << context.SeqNo()
                                                                             << ", replying leader status inactive");
            ReplyWithMessage(context, StoreErrorCode::ERROR, "leader status inactive");
            return SM_ERROR;
        }
        return FindOrInsertRank(context, request);
    }

    STORE_LOG_DEBUG("GET REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    SmemMessage responseMessage{request.mt};
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);
    if (ret == SUCCESS) {
        responseMessage.values.push_back(oldValue);
        lockGuard.unlock();

        STORE_LOG_DEBUG("GET REQUEST(" << context.SeqNo() << ") for key(" << key << ") success.");
        auto response = SmemMessagePacker::Pack(responseMessage);
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
        return SM_OK;
    }

    std::vector<uint8_t> outValue;
    if (request.userDef == 0) {
        lockGuard.unlock();
        STORE_LOG_DEBUG("GET REQUEST(" << context.SeqNo() << ") for key(" << key << ") not exist.");
        ReplyWithMessage(context, StoreErrorCode::NOT_EXIST, "<not exist>");
        return SM_ERROR;
    }

    STORE_LOG_DEBUG("GET REQUEST(" << context.SeqNo() << ") for key(" << key << ") timeout=" << request.userDef);
    auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.userDef);
    auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout.time_since_epoch()).count();
    STORE_LOG_DEBUG("GET REQUEST(" << context.SeqNo() << ") for key(" << key << ") waiting timeout=" << timeoutMs);
    StoreWaitContext waitContext{timeoutMs, key, context};
    auto pair = waitCtx_.emplace(waitContext.Id(), std::move(waitContext));
    auto wPos = keyWaiters_.find(key);
    if (wPos != keyWaiters_.end()) {
        wPos->second.emplace(pair.first->first);
    } else {
        keyWaiters_.emplace(key, std::unordered_set<uint64_t>{pair.first->first});
    }

    if (request.userDef > 0) {
        auto timerPos = timedWaiters_.find(timeoutMs);
        if (timerPos == timedWaiters_.end()) {
            timedWaiters_.emplace(timeoutMs, std::unordered_set<uint64_t>{pair.first->first});
        } else {
            timerPos->second.emplace(pair.first->first);
        }
    }
    return SM_OK;
}

Result AccStoreServer::PrefixGetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || !request.values.empty()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key should be one and no values.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    STORE_LOG_DEBUG("PREFIX REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    SmemMessage responseMessage{request.mt};
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    PrefixGetMap retValue;
    auto ret = backend_->PrefixGet(key, retValue);
    if (ret == SUCCESS) {
        for (auto &it : retValue) {
            responseMessage.keys.push_back(it.first);
            responseMessage.values.push_back(it.second);
        }
        lockGuard.unlock();

        STORE_LOG_DEBUG("PREFIX REQUEST(" << context.SeqNo() << ") for key(" << key << ") success.");
        auto response = SmemMessagePacker::Pack(responseMessage);
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
        return SM_OK;
    } else {
        auto response = SmemMessagePacker::Pack(responseMessage);
        ReplyWithMessage(context, StoreErrorCode::ERROR, response);
    }
    return SM_OK;
}

Result AccStoreServer::WatchHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || !request.values.empty()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE,
                         "invalid request: key should be one and has values.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    STORE_LOG_DEBUG("WATCH REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    SmemMessage responseMessage{request.mt};
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);

    StoreWaitContext waitContext{-1L, key, context};
    auto linkId = context.Link()->Id();
    watchWaiters_[key].emplace(linkId, waitContext);
    linkWatchList_[linkId].emplace_back(key);
    if (ret == SUCCESS) {
        responseMessage.values.push_back(oldValue);
        lockGuard.unlock();

        STORE_LOG_DEBUG("WATCH REQUEST(" << context.SeqNo() << ") for key(" << key << ") reply.");
        auto response = SmemMessagePacker::Pack(responseMessage);
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
    }
    return SM_OK;
}

Result AccStoreServer::AddHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || request.values.size() != 1) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key value should be one.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    auto &value = request.values[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    std::string valueStr{value.begin(), value.end()};
    STORE_LOG_DEBUG("ADD REQUEST(" << context.SeqNo() << ") for key(" << key << ") value(" << valueStr << ") start.");

    long valueNum;
    STORE_VALIDATE_RETURN(mf::StrUtil::String2Int<long>(valueStr, valueNum), "convert string to long failed.",
                          SM_ERROR);
    if (valueStr != std::to_string(valueNum)) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") add for key(" << key << ") value is not a number");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: value should be a number.");
        return SM_ERROR;
    }

    auto responseValue = valueNum;
    std::list<ock::acc::AccTcpRequestContext> wakeupWaiters;
    std::list<ock::acc::AccTcpRequestContext> wakeupWatchers;
    std::vector<uint8_t> reqVal;
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);
    if (ret != SUCCESS) {
        reqVal = value;
        ret = backend_->Put(key, std::move(value), EPHEMERAL_KEY_TTL_SEC);
    } else {
        std::string oldValueStr{oldValue.begin(), oldValue.end()};
        long storedValueNum = 0;
        auto ret = mf::StrUtil::String2Int<long>(oldValueStr, storedValueNum);
        if ((storedValueNum == 0 && oldValueStr != "0") || !ret) {
            lockGuard.unlock();
            STORE_LOG_ERROR("oldValueStr is " << oldValueStr);
            ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "oldValueStr should be a number.");
            return SM_ERROR;
        }

        if ((valueNum > 0 && storedValueNum > LONG_MAX - valueNum) ||
            (valueNum < 0 && storedValueNum < LONG_MIN - valueNum)) {
            lockGuard.unlock();
            STORE_LOG_ERROR("ADD overflow: storedValueNum=" << storedValueNum << " valueNum=" << valueNum);
            ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "add result overflow.");
            return SM_ERROR;
        }
        storedValueNum += valueNum;
        auto storedValueStr = std::to_string(storedValueNum);
        reqVal = std::vector<uint8_t>(storedValueStr.begin(), storedValueStr.end());
        ret = backend_->Put(key, reqVal, EPHEMERAL_KEY_TTL_SEC);
        responseValue = storedValueNum;
    }
    GetWakeupList(key, wakeupWaiters, wakeupWatchers);
    lockGuard.unlock();
    STORE_LOG_DEBUG("ADD(" << context.SeqNo() << ") key(" << key << ") val(" << responseValue << ") end.");
    ReplyWithMessage(context, ret, std::to_string(responseValue));
    if (!wakeupWaiters.empty() || !wakeupWatchers.empty()) {
        WakeupWaiters(wakeupWaiters, wakeupWatchers, reqVal);
    }
    return SM_OK;
}

Result AccStoreServer::RemoveHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || !request.values.empty()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key should be one and no values.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    STORE_LOG_DEBUG("REMOVE REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    bool removed = false;
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    auto ret = backend_->Exist(key);
    if (ret == SUCCESS) {
        StoreErrorCode delRet = backend_->Delete(key);
        if (delRet != StoreErrorCode::SUCCESS) {
            STORE_LOG_WARN("RemoveHandler: Delete for key=" << key << " failed: " << static_cast<int>(delRet));
        }
        removed = true;
    }
    lockGuard.unlock();
    ReplyWithMessage(context, removed ? StoreErrorCode::SUCCESS : StoreErrorCode::NOT_EXIST,
                     removed ? "success" : "not exist");

    return SM_OK;
}

Result AccStoreServer::AppendHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || request.values.size() != 1) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key & value should be one.");
        return SM_INVALID_PARAM;
    }

    auto &key = request.keys[0];
    auto &value = request.values[0];
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }

    STORE_LOG_DEBUG("APPEND REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    uint64_t newSize;
    std::list<ock::acc::AccTcpRequestContext> wakeupWaiters;
    std::list<ock::acc::AccTcpRequestContext> wakeupWatchers;
    std::vector<uint8_t> reqVal;
    std::vector<uint8_t> appendValue = value;
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);
    if (ret == SUCCESS) {
        oldValue.insert(oldValue.end(), value.begin(), value.end());
        newSize = oldValue.size();
        reqVal = oldValue;
        ret = backend_->Put(key, oldValue, EPHEMERAL_KEY_TTL_SEC);
    } else {
        newSize = value.size();
        reqVal = value;
        ret = backend_->Put(key, std::move(value), EPHEMERAL_KEY_TTL_SEC);
    }
    GetWakeupList(key, wakeupWaiters, wakeupWatchers);
    lockGuard.unlock();
    ReplyWithMessage(context, ret, std::to_string(newSize));
    if (!wakeupWaiters.empty() || !wakeupWatchers.empty()) {
        WakeupWaiters(wakeupWaiters, wakeupWatchers, value);
    }

    return SM_OK;
}

Result AccStoreServer::WriteHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || request.values.size() != 1) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key & value should be one.");
        return SM_INVALID_PARAM;
    }
    auto &key = request.keys[0];
    auto &value = request.values[0];

    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }
    STORE_LOG_INFO("WRITE REQUEST(" << context.SeqNo() << ") for key(" << key << ") start.");
    if (value.size() < sizeof(uint32_t)) {
        STORE_LOG_ERROR("WRITE value size too small: " << value.size());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "value size too small.");
        return SM_INVALID_PARAM;
    }
    uint32_t offset = *(reinterpret_cast<uint32_t *>(value.data()));
    size_t realValSize = value.size() - sizeof(uint32_t);
    if (realValSize > SIZE_MAX / static_cast<size_t>(MAX_U16_INDEX)) {
        STORE_LOG_ERROR("WRITE realValSize too large: " << realValSize);
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "value size too large.");
        return SM_INVALID_PARAM;
    }
    STORE_VALIDATE_RETURN(offset <= MAX_U16_INDEX * realValSize, "offset too large, offset:" << offset,
                          StoreErrorCode::INVALID_KEY);

    size_t totalSize = static_cast<size_t>(offset) + realValSize;
    if (totalSize < realValSize) {
        STORE_LOG_ERROR("WRITE offset+realValSize overflow: offset=" << offset << " realValSize=" << realValSize);
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "offset plus value size overflow.");
        return SM_INVALID_PARAM;
    }

    if (totalSize > MAX_WRITE_TOTAL_SIZE) { // Avoid remote large offset causing multi-gigabyte allocation, trigger OOM
        STORE_LOG_ERROR("WRITE total size exceeds limit, totalSize: " << totalSize << " limit: " << MAX_WRITE_TOTAL_SIZE
                                                                      << " offset: " << offset
                                                                      << " realValSize: " << realValSize);
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "write total size exceeds limit.");
        return SM_INVALID_PARAM;
    }

    STORE_LOG_INFO("WRITE(" << context.SeqNo() << ") k:" << key << " o:" << offset << " vs:" << realValSize << ")");
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);
    if (ret != SUCCESS) {
        STORE_LOG_INFO("write: not find key:" << key << ", new alloc mem: " << totalSize);
        ret = backend_->Put(key, std::vector<uint8_t>(totalSize, 0), EPHEMERAL_KEY_TTL_SEC);
        if (ret != SUCCESS) {
            STORE_LOG_ERROR("put failed, key=" << key << ", ret: " << ret);
            ReplyWithMessage(context, StoreErrorCode::ERROR, "failed");
            return StoreErrorCode::ERROR;
        }
    }
    ret = backend_->Get(key, oldValue);
    if (ret != SUCCESS) {
        oldValue.resize(totalSize, 0);
        STORE_LOG_INFO("write: re-get failed, allocate zero buffer size: " << totalSize);
    }
    auto &curValue = oldValue;
    if (totalSize > curValue.size()) {
        curValue.resize(totalSize, 0);
        STORE_LOG_INFO("write: not enough kvStore room, expansion size: " << totalSize);
    }
    std::copy_n(value.data() + sizeof(uint32_t), realValSize, curValue.data() + offset);
    ret = backend_->Put(key, curValue, EPHEMERAL_KEY_TTL_SEC);
    if (ret != SUCCESS) {
        lockGuard.unlock();
        STORE_LOG_ERROR("WRITE(" << context.SeqNo() << ") k:" << key << " fail:" << ret);
        ReplyWithMessage(context, StoreErrorCode::ERROR, "failed");
        return StoreErrorCode::ERROR;
    }
    lockGuard.unlock();
    ReplyWithMessage(context, StoreErrorCode::SUCCESS, "success");
    return SM_OK;
}

Result AccStoreServer::CasHandler(const ock::acc::AccTcpRequestContext &context,
                                  ock::smem::SmemMessage &request) noexcept
{
    const size_t EXPECTDE_KEY = 1;
    const size_t EXPECTED_VAL = 2;

    if (request.keys.size() != EXPECTDE_KEY || request.values.size() != EXPECTED_VAL) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: count(key)=1 & count(value)=2");
        return SM_INVALID_PARAM;
    }
    auto &key = request.keys[0];
    auto &expected = request.values[0];
    auto &exchange = request.values[1];
    auto newValue = exchange;
    if (key.length() > MAX_KEY_LEN_SERVER) {
        STORE_LOG_ERROR("key length too large, length: " << key.length() << ", max: " << MAX_KEY_LEN_SERVER);
        return StoreErrorCode::INVALID_KEY;
    }
    std::string newValueStr = std::string{newValue.begin(), newValue.end()};
    std::vector<uint8_t> exists;
    SmemMessage responseMessage{request.mt};
    std::list<ock::acc::AccTcpRequestContext> wakeupWaiters;
    std::list<ock::acc::AccTcpRequestContext> wakeupWatchers;
    STORE_LOG_DEBUG("CAS(" << context.SeqNo() << ") for key(" << key << ") start, newValueStr: " << newValueStr);
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    std::vector<uint8_t> oldValue;
    auto ret = backend_->Get(key, oldValue);
    if (ret == SUCCESS) {
        if (expected == oldValue) {
            ret = backend_->Put(key, exchange, EPHEMERAL_KEY_TTL_SEC);
            if (ret == SUCCESS) {
                GetWakeupList(key, wakeupWaiters, wakeupWatchers);
                exists = exchange;
            }
        } else {
            responseMessage.values.push_back(oldValue);
            exists = oldValue;
        }
    } else if (ret == NOT_EXIST) {
        ret = backend_->Put(key, exchange, EPHEMERAL_KEY_TTL_SEC);
        if (ret == SUCCESS) {
            GetWakeupList(key, wakeupWaiters, wakeupWatchers);
            exists = exchange;
        }
    }
    lockGuard.unlock();
    STORE_LOG_DEBUG("CAS(" << context.SeqNo() << ") k:" << key << " r:" << ret
                           << " f:" << responseMessage.values.size());

    responseMessage.values.push_back(exists);
    auto response = SmemMessagePacker::Pack(responseMessage);
    ReplyWithMessage(context, ret, response);
    if (!wakeupWaiters.empty() || !wakeupWatchers.empty()) {
        WakeupWaiters(wakeupWaiters, wakeupWatchers, exists);
    }
    return SM_OK;
}

Result AccStoreServer::QueryAliveHandler(const ock::acc::AccTcpRequestContext &context,
                                         ock::smem::SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || !request.values.empty()) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key should be one and no values.");
        return SM_INVALID_PARAM;
    }
    auto &key = request.keys[0];
    uint32_t rank;
    if (!mf::StrUtil::String2Uint<uint32_t>(key, rank)) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") receive invalid rank:" << key);
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid input");
        return SM_INVALID_PARAM;
    }

    SmemMessage responseMessage{request.mt};
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    if (reconnectedRankSet_.count(rank)) {
        responseMessage.values.push_back(std::vector<uint8_t>(1, 1));
    }

    STORE_LOG_DEBUG("QUERY_ALIVE rank:" << rank << " alive: " << (responseMessage.values.empty() ? "false" : "true"));
    auto response = SmemMessagePacker::Pack(responseMessage);
    ReplyWithMessage(context, 0, response);
    return SM_OK;
}

Result AccStoreServer::WatchRankStateHandler(const acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1 || request.keys[0] != WATCH_RANK_DOWN_KEY) {
        STORE_LOG_ERROR("request(" << context.SeqNo() << ") handle invalid body");
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid request: key should be");
        return SM_INVALID_PARAM;
    }
    STORE_ASSERT_RETURN(context.Link() != nullptr, SM_INVALID_PARAM);
    auto linkId = context.Link()->Id();
    StoreWaitContext waitContext{-1L, WATCH_RANK_DOWN_KEY, context};
    std::unique_lock<std::mutex> uniqueLock{storeMutex_};
    auto pair = rankStateWaiters_.emplace(linkId, waitContext);
    if (!pair.second) {
        uniqueLock.unlock();
        STORE_LOG_ERROR("link id : " << linkId << ", already watched for rank state.");
        return SM_REPEAT_CALL;
    }
    STORE_LOG_DEBUG("WATCH REQ(" << context.SeqNo() << ") key=" << WATCH_RANK_DOWN_KEY << " done link=" << linkId);
    return SM_OK;
}

Result AccStoreServer::UnwatchHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 1) {
        STORE_LOG_ERROR("UNWATCH request(" << context.SeqNo() << ") invalid body");
        return SM_INVALID_PARAM;
    }

    const auto &key = request.keys[0];
    STORE_ASSERT_RETURN(context.Link() != nullptr, SM_INVALID_PARAM);
    auto linkId = context.Link()->Id();

    std::unique_lock<std::mutex> uniqueLock{storeMutex_};
    if (key == WATCH_RANK_DOWN_KEY) {
        rankStateWaiters_.erase(linkId);
        STORE_LOG_DEBUG("UNWATCH rank state for linkId: " << linkId);
    } else {
        watchWaiters_[key].erase(linkId);
        auto &watchKeys = linkWatchList_[linkId];
        watchKeys.erase(std::remove(watchKeys.begin(), watchKeys.end(), key), watchKeys.end());
        STORE_LOG_DEBUG("UNWATCH key: " << key << " for linkId: " << linkId);
    }
    return SM_OK;
}

Result AccStoreServer::HeartbeatHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    if (request.keys.size() != 0 || request.values.size() != 0) {
        STORE_LOG_ERROR("heart beat request(" << context.SeqNo() << ") handle invalid body");
        return SM_INVALID_PARAM;
    }
    STORE_ASSERT_RETURN(context.Link() != nullptr, SM_INVALID_PARAM);
    uint32_t linkId = context.Link()->Id();
    std::unique_lock<std::mutex> lockGuard{storeMutex_};
    heartBeatMap_[linkId] = mf::StrUtil::GetNowTime();
    return SM_OK;
}

std::list<ock::acc::AccTcpRequestContext>
AccStoreServer::GetOutWaitersInLock(const std::unordered_set<uint64_t> &ids) noexcept
{
    std::list<ock::acc::AccTcpRequestContext> reqCtx;
    for (auto id : ids) {
        auto it = waitCtx_.find(id);
        if (it != waitCtx_.end()) {
            reqCtx.emplace_back(std::move(it->second.ReqCtx()));
            auto wit = timedWaiters_.find(it->second.TimeoutMs());
            if (wit != timedWaiters_.end()) {
                wit->second.erase(it->second.Id());
                if (wit->second.empty()) {
                    timedWaiters_.erase(wit);
                }
            }
            waitCtx_.erase(it);
        }
    }
    return std::move(reqCtx);
}

void AccStoreServer::WakeupWaiters(const std::list<ock::acc::AccTcpRequestContext> &waiters,
                                   const std::list<ock::acc::AccTcpRequestContext> &watchers,
                                   const std::vector<uint8_t> &value) noexcept
{
    SmemMessage responseMessage{MessageType::GET};
    responseMessage.values.push_back(value);
    auto response = SmemMessagePacker::Pack(responseMessage);
    for (auto &context : waiters) {
        STORE_LOG_DEBUG("WAKEUP REQUEST(" << context.SeqNo() << ").");
        if (!context.Link()->Established()) {
            continue;
        }
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
    }

    responseMessage.mt = MessageType::WATCH;
    response = SmemMessagePacker::Pack(responseMessage);
    for (auto &context : watchers) {
        STORE_LOG_DEBUG("WAKEUP REQUEST(" << context.SeqNo() << ").");
        if (!context.Link()->Established()) {
            continue;
        }
        ReplyWithMessage(context, StoreErrorCode::SUCCESS, response);
    }
}

void AccStoreServer::ReplyWithMessage(const ock::acc::AccTcpRequestContext &ctx, int16_t code,
                                      const std::string &message) noexcept
{
    auto response = ock::acc::AccDataBuffer::Create(message.c_str(), message.size());
    if (response == nullptr) {
        STORE_LOG_ERROR("create response message failed");
        return;
    }

    ctx.Reply(code, response);
}

void AccStoreServer::ReplyWithMessage(const ock::acc::AccTcpRequestContext &ctx, int16_t code,
                                      const std::vector<uint8_t> &message) noexcept
{
    auto response = ock::acc::AccDataBuffer::Create(message.data(), message.size());
    if (response == nullptr) {
        STORE_LOG_ERROR("create response message failed");
        return;
    }

    ctx.Reply(code, response);
}

void AccStoreServer::TimerThreadTask() noexcept
{
    std::unordered_set<uint64_t> timeoutIds;
    pthread_setname_np(pthread_self(), "acc_store_timer");
    std::unique_lock<std::mutex> lockerGuard{storeMutex_};
    while (state_.load() != SS_EXITED) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        while (!timedWaiters_.empty()) {
            auto it = timedWaiters_.begin();
            if (it->first > timestamp) {
                break;
            }
            timeoutIds.insert(it->second.begin(), it->second.end());
            timedWaiters_.erase(it);
        }

        auto timeoutContexts = GetOutWaitersInLock(timeoutIds);
        lockerGuard.unlock();

        timeoutIds.clear();
        for (auto &ctx : timeoutContexts) {
            if (!ctx.Link()->Established()) {
                STORE_LOG_WARN("Link is not Established, reply timeout response for : " << ctx.SeqNo());
                continue;
            }
            STORE_LOG_DEBUG("reply timeout response for : " << ctx.SeqNo());
            ReplyWithMessage(ctx, StoreErrorCode::TIMEOUT, "<timeout>");
        }

        lockerGuard.lock();
        storeCond_.wait_for(lockerGuard, std::chrono::milliseconds(TIMER_POLL_MS),
                            [this]() { return (state_.load() == SS_EXITED); });
    }
}

void AccStoreServer::RankStateTask() noexcept
{
    pthread_setname_np(pthread_self(), "rank_state_ts");
    while (state_.load() != SS_EXITED) {
        std::unique_lock<std::mutex> lock(storeMutex_);
        storeCond_.wait(lock, [this] { return !rankStateTaskQueue_.empty() || (state_.load() == SS_EXITED); });
        if (state_.load() == SS_EXITED) {
            return;
        }

        union Transfer {
            uint32_t rankId;
            uint8_t data[sizeof(uint32_t)];
        } trans{};

        auto rankId = std::move(rankStateTaskQueue_.front());
        rankStateTaskQueue_.pop();
        trans.rankId = rankId;
        SmemMessage responseMessage{MessageType::WATCH_RANK_STATE};
        std::vector<uint8_t> value(trans.data, trans.data + sizeof(trans.data));
        responseMessage.values.push_back(value);
        auto response = SmemMessagePacker::Pack(responseMessage);
        for (auto it = rankStateWaiters_.begin(); it != rankStateWaiters_.end(); ++it) {
            if (!it->second.ReqCtx().Link()->Established()) {
                STORE_LOG_WARN(rankId << " link=" << it->first << " id=" << it->second.ReqCtx().Link()->Id());
                continue;
            }
            STORE_LOG_DEBUG("rankId: " << rankId << " down notify to linkId: " << it->first);
            ReplyWithMessage(it->second.ReqCtx(), StoreErrorCode::SUCCESS, response);
        }
    }
}

uint32_t AccStoreServer::ParseHeartbeatTimeoutS(const std::string &value) noexcept
{
    uint32_t timeoutS = DEFAULT_HEARTBEAT_TIMEOUT_S;
    if (!value.empty() && (value.find_first_not_of("0123456789") != std::string::npos ||
                           !mf::MfEnvUtil::GetOptionalUint(value, timeoutS) || timeoutS > MAX_HEARTBEAT_TIMEOUT_S)) {
        STORE_LOG_ERROR("invalid MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S=" << value << ", expected seconds in [0, "
                                                                       << MAX_HEARTBEAT_TIMEOUT_S << "], using default="
                                                                       << DEFAULT_HEARTBEAT_TIMEOUT_S);
        return DEFAULT_HEARTBEAT_TIMEOUT_S;
    }
    return timeoutS;
}

uint32_t AccStoreServer::ParseLinkRetryTimeoutS(const std::string &value) noexcept
{
    uint32_t timeoutS = DEFAULT_LINK_RETRY_TIMEOUT_S;
    if (!value.empty() && (value.find_first_not_of("0123456789") != std::string::npos ||
                           !mf::MfEnvUtil::GetOptionalUint(value, timeoutS) || timeoutS < MIN_LINK_RETRY_TIMEOUT_S ||
                           timeoutS > MAX_LINK_RETRY_TIMEOUT_S)) {
        STORE_LOG_ERROR("invalid MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S="
                        << value << ", expected seconds in [" << MIN_LINK_RETRY_TIMEOUT_S << ", "
                        << MAX_LINK_RETRY_TIMEOUT_S << "], using default=" << DEFAULT_LINK_RETRY_TIMEOUT_S);
        return DEFAULT_LINK_RETRY_TIMEOUT_S;
    }
    return timeoutS;
}

void AccStoreServer::CheckerThreadTask() noexcept
{
    pthread_setname_np(pthread_self(), "store_chk_sts");
    const auto timeout = std::chrono::seconds(heartBeatTimeoutS_);
    std::unordered_set<uint32_t> brokenLinks;
    std::unique_lock<std::mutex> lockerGuard{storeMutex_};
    while (state_.load() != SS_EXITED) {
        auto curTime = mf::StrUtil::GetNowTime();
        for (auto it = heartBeatMap_.begin(); it != heartBeatMap_.end();) {
            if (heartBeatTimeoutS_ > 0 && std::chrono::milliseconds(curTime - it->second) >= timeout) {
                STORE_LOG_TRACE("link(" << it->first << ") heartbeat expired, timeoutS=" << heartBeatTimeoutS_);
                brokenLinks.insert(it->first);
                it = heartBeatMap_.erase(it);
            } else {
                ++it;
            }
        }
        lockerGuard.unlock();
        for (auto linkId : brokenLinks) {
            LinkBrokenHandler(linkId); // private func, locks storeMutex_ internally
        }
        lockerGuard.lock();
        brokenLinks.clear();
        storeCond_.wait_for(lockerGuard, std::chrono::milliseconds(HEARTBEAT_INTERVAL),
                            [this]() { return (state_.load() == SS_EXITED); });
    }
    STORE_LOG_TRACE("checker thread exit");
}

Result AccStoreServer::RestoreFromBackend() noexcept
{
    if (!backend_->IsDistributed()) {
        return SM_OK;
    }
    STORE_LOG_TRACE("Starting restore from backend...");

    if (auto ret = RecoverAliveRankIds(aliveRankFromBackend_); ret != StoreErrorCode::SUCCESS) {
        STORE_LOG_WARN("Failed to recover alive rank IDs from backend");
        return SM_OK;
    }

    STORE_LOG_DEBUG("Restore from backend completed, alive ranks: " << aliveRankFromBackend_.size());

    /*  Seed aliveRankSet_ so the new Leader's FindOrInsertRank
        won't re-assign ranks already in use.  Do NOT seed reconnectedRankSet_ — it must only be populated by
        LinkConnectedHandler so QueryAlive accurately reflects which ranks actually have an active TCP connection.
    */
    for (auto rk : aliveRankFromBackend_) {
        aliveRankSet_.insert(rk);
    }

    return SM_OK;
}

bool AccStoreServer::GetStatus() noexcept
{
    if (!backend_->IsDistributed()) {
        return true;
    }
    static constexpr int retries = 5;
    static constexpr int retryIntervalSec = 2;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        std::string status;
        auto ret = backend_->Get(KEY_LEADER_STATUS, status);
        if (ret != 0) {
            STORE_LOG_WARN("Unable to get leader status from backend, ret: " << ret << ", attempt: " << (attempt + 1)
                                                                             << "/" << (retries + 1));
            // Self-heal: re-assert leader_status when this instance is the rightful leader
            // (KEY_LEADER points to us) but leader_status key is missing.
            // Only attempt once per check (attempt 0) to avoid storms.
            if (attempt == 0) {
                std::string leaderAddr;
                const std::string myAddr = listenIp_ + ":" + std::to_string(listenPort_);
                if (backend_->Get(KEY_LEADER, leaderAddr) == 0 && leaderAddr == myAddr) {
                    STORE_LOG_WARN("KEY_LEADER still points to this instance ("
                                   << leaderAddr << "); re-asserting leader_status=true (AutoRanking self-heal)");
                    UpdateStatus(true);
                } else {
                    STORE_LOG_WARN("KEY_LEADER=" << leaderAddr << " != " << myAddr << "; skip leader_status self-heal");
                }
            }
        } else if (status == "true") {
            STORE_LOG_DEBUG("Leader status: active");
            return true;
        } else {
            STORE_LOG_DEBUG("Leader status: inactive, attempt: " << (attempt + 1) << "/" << (retries + 1));
        }
        if (attempt < retries) {
            std::this_thread::sleep_for(std::chrono::seconds(retryIntervalSec));
        }
    }
    STORE_LOG_ERROR("Leader status check failed after " << (retries + 1) << " attempts");
    return false;
}

Result AccStoreServer::UpdateStatus(bool status) noexcept
{
    if (!backend_->IsDistributed()) {
        return SM_OK;
    }
    Result ret;
    if (status) {
        ret = backend_->Put(KEY_LEADER_STATUS, "true", EPHEMERAL_KEY_TTL_SEC);
        if (ret != SM_OK) {
            STORE_LOG_ERROR("Failed to set leader status to active, ret: " << ret);
        } else {
            STORE_LOG_TRACE("Leader status set to active");
        }
    } else {
        ret = backend_->Delete(KEY_LEADER_STATUS);
        if (ret != SM_OK) {
            STORE_LOG_ERROR("Failed to remove leader status, ret: " << ret);
        } else {
            STORE_LOG_TRACE("Leader status removed");
        }
    }

    return ret;
}

uint32_t AccStoreServer::GetRankIdByLinkId(uint32_t linkId) const noexcept
{
    // 注意：调用者必须已持有 storeMutex_（CheckerThreadTask / LinkBrokenHandler 均已持锁）
    auto it = linkRankMap_.find(linkId);
    return it != linkRankMap_.end() ? it->second : UINT32_MAX;
}

StoreErrorCode AccStoreServer::PersistWorldSize(uint32_t size) noexcept
{
    if (!backend_->IsDistributed()) {
        return StoreErrorCode::SUCCESS;
    }
    const std::string str = std::to_string(size);
    const std::vector<uint8_t> data(str.begin(), str.end());
    auto ret = backend_->Put(KEY_WORLD_SIZE, data, EPHEMERAL_KEY_TTL_SEC);
    if (ret != SUCCESS) {
        STORE_LOG_ERROR("Failed to persist world size: " << size);
    } else {
        STORE_LOG_TRACE("World size persisted: " << size);
    }
    return ret;
}

StoreErrorCode AccStoreServer::PersistAliveRankIds(const std::unordered_set<uint32_t> &ranks) noexcept
{
    STORE_LOG_INFO("PersistRanks n=" << ranks.size()
                                     << " dist=" << (backend_ != nullptr ? backend_->IsDistributed() : 0));
    if (!backend_->IsDistributed()) {
        return SUCCESS;
    }
    if (ranks.empty()) {
        auto ret = backend_->Delete(KEY_ALIVE_RANK_LIST);
        if (ret != SUCCESS) {
            STORE_LOG_ERROR("Failed to remove alive ranks key from backend");
            return ret;
        }
        STORE_LOG_TRACE("Alive ranks cleared in backend");
        return SUCCESS;
    }
    std::vector<uint32_t> orders;
    orders.insert(orders.end(), ranks.begin(), ranks.end());
    std::sort(orders.begin(), orders.end());

    std::stringstream ss;
    auto it = orders.begin();
    ss << *it;

    for (++it; it != orders.end(); ++it) {
        ss << "," << *it;
    }
    const std::string str = ss.str();
    const std::vector<uint8_t> data(str.begin(), str.end());
    auto ret = backend_->Put(KEY_ALIVE_RANK_LIST, data, 0);
    if (ret != SUCCESS) {
        STORE_LOG_ERROR("Failed to persist alive ranks, count: " << ranks.size() << ", ranks: " << str);
    } else {
        STORE_LOG_TRACE("Alive ranks persisted, count: " << ranks.size() << ", ranks: " << str);
    }
    return ret;
}

StoreErrorCode AccStoreServer::RecoverAliveRankIds(std::unordered_set<uint32_t> &outRanks) noexcept
{
    if (!backend_->IsDistributed()) {
        return SUCCESS;
    }
    outRanks.clear();
    std::string rankStr;
    auto ret = backend_->Get(KEY_ALIVE_RANK_LIST, rankStr);
    if (ret != 0) {
        STORE_LOG_WARN("Unable to get alive ranks from backend, ret: " << ret);
        return SUCCESS;
    }
    if (rankStr.empty()) {
        STORE_LOG_TRACE("No alive ranks found in backend");
        return SUCCESS;
    }

    auto list = mf::StrUtil::Split(rankStr, ',');
    for (const auto &idStr : list) {
        if (idStr.empty()) {
            continue;
        }
        uint32_t val;
        if (!mf::StrUtil::String2Uint(idStr, val)) {
            STORE_LOG_ERROR("Rank ID check failed: " << idStr);
            outRanks.clear();
            return ERROR;
        }
        outRanks.insert(static_cast<uint32_t>(val));
    }

    STORE_LOG_TRACE("Recovered alive ranks from backend, count: " << outRanks.size() << ", ranks: " << rankStr);
    return SUCCESS;
}

Result AccStoreServer::LaunchCleanupThread()
{
    const bool isFirstUpdate = aliveRankFromBackend_.empty();
    // First status update
    if (UpdateStatus(isFirstUpdate) != SM_OK) {
        STORE_LOG_ERROR("backend update status failed.");
        return SM_ERROR;
    }

    // If this is the first leader start (no old ranks), no recovery needed.
    if (isFirstUpdate) {
        STORE_LOG_INFO("no old ranks to recover, skip cleanup thread");
        return SM_OK;
    }

    // Launch recovery thread: SERVER_RECOVER_TIME window for old ranks to reconnect
    // (polled every RECOVER_PERIOD_TIME), then cleanup orphans and set status active.
    if (cleanupThread_.joinable()) {
        cleanupThread_.join();
    }

    cleanupThread_ = std::thread([this]() {
        STORE_LOG_INFO("LaunchCleanupThread recovery before wait for state...");
        {
            std::unique_lock<std::mutex> recoveryLock(recoveryMutex_);
            // 恢复窗口为 SERVER_RECOVER_TIME：以 RECOVER_PERIOD_TIME 为轮询周期
            // 循环等待，期间 rank 全部重连（UpdateRecoverState 置位 SS_RECOVERED）则提前退出。
            const uint64_t recoverDeadlineT = startupTimestamp_ + SERVER_RECOVER_TIME;
            while (state_.load() < SS_RECOVERED && mf::MonotonicTime::TimeUs() <= recoverDeadlineT) {
                recoveryCond_.wait_for(recoveryLock, std::chrono::seconds(RECOVER_PERIOD_TIME),
                                       [this]() { return state_.load() >= SS_RECOVERED; });
            }
        }
        STORE_LOG_INFO("LaunchCleanupThread recovery after wait for state: " << static_cast<int>(state_.load()));

        uint32_t currState = SS_RECOVERING;
        if (!state_.compare_exchange_strong(currState, SS_RECOVERED)) {
            if (currState < SS_RECOVERED) {
                state_.store(SS_RECOVERED);
            }
        }
        STORE_LOG_INFO("LaunchCleanupThread recovery state to SS_RECOVERED");
        CleanupStaleRanks();

        if (UpdateStatus(true) != SM_OK) {
            STORE_LOG_ERROR("LaunchCleanupThread recovery: set leader status active failed");
        }
        state_.store(SS_NORMAL);
        STORE_LOG_INFO("LaunchCleanupThread recovery thread finished");
    });
    return SM_OK;
}

void AccStoreServer::CleanupStaleRanks() noexcept
{
    std::unordered_set<uint32_t> ranksToRemove;
    {
        std::lock_guard<std::mutex> lock{storeMutex_};
        for (const uint32_t rank : reconnectedRankSet_) {
            aliveRankFromBackend_.erase(rank);
        }
        ranksToRemove = aliveRankFromBackend_;

        // 同步清除 aliveRankSet_ 中的 stale rank，释放rank槽位
        for (auto rankId : ranksToRemove) {
            aliveRankSet_.erase(rankId);
        }
    }

    if (groupManager_ != nullptr) {
        for (uint32_t rankId : ranksToRemove) {
            STORE_LOG_INFO("Remove old rankId: " << rankId);
            groupManager_->Checkout(rankId, 0, "cleanup-stale");
        }
    }

    // Process removals: push orphan ranks for leave notification + hybm_remove cleanup
    {
        std::lock_guard<std::mutex> lock{storeMutex_};
        for (uint32_t rankId : ranksToRemove) {
            rankStateTaskQueue_.push(rankId);
        }

        // 更新 etcd 持久化的 alive rank 列表
        if (PersistAliveRankIds(aliveRankSet_) != SUCCESS) {
            STORE_LOG_ERROR("Failed to persist alive ranks after cleaning stale ranks");
        }
    }

    // Notify and wait again (for leave notifications to be processed)
    storeCond_.notify_all();
    {
        std::unique_lock<std::mutex> lock{storeMutex_};
        if (storeCond_.wait_for(lock, std::chrono::seconds(STORE_WAIT_TIMEOUT_SEC),
                                [this] { return shouldStop_.load(std::memory_order_acquire); })) {
            return;
        }
    }

    // Final status update: mark leader as active now that recovery + cleanup is done
    if (UpdateStatus(true) != SM_OK) {
        STORE_LOG_ERROR("backend final update status failed in cleanup thread.");
    }
    STORE_LOG_TRACE("backend final update status successful in cleanup thread.");
}

void AccStoreServer::RestoreFromEtcdIfNeeded() noexcept
{
    if (etcdStore_ != nullptr && groupManager_ != nullptr) {
        etcdStore_->Recover(groupManager_.Get());
        // 兜底：以 alive_rank_list 为权威，校正 Recover 恢复的 states 中已不在图的
        // rank（如 etcd 抖动窗口内 states 未及时写 IDLE 而残留 CHECKED_IN/ACTIVE）。
        // 校正后显式持久化，保证 etcd 中 states 与 alive_rank_list 一致，避免后续
        // 分配/CheckIn 路由到已无进程的 rank。
        // 只对恢复结果中非 IDLE 的 rank 做对账：fresh start 全为 IDLE 时循环不做
        // 任何事，避免按 maxRanks（UINT32_MAX 兜底钳到 1024）全量空转并刷日志；
        // alive_rank_list 为空时仍会清理 states 中的残留 rank，保证同 rankId
        // 重启的客户端 CheckIn 不被非 IDLE 状态拒绝。
        const std::vector<RankState> recoveredStates = groupManager_->GetStates();
        std::vector<uint32_t> staleRanks;
        for (uint32_t i = 0; i < groupManager_->GetMaxRanks(); ++i) {
            if (recoveredStates[i] == RANK_IDLE || aliveRankFromBackend_.count(i) != 0) {
                continue;
            }
            if (groupManager_->Checkout(i, 0, "server-reconcile") == 0) {
                staleRanks.push_back(i);
            }
        }
        if (!staleRanks.empty()) {
            std::string rankStr;
            for (size_t k = 0; k < staleRanks.size(); ++k) {
                if (k > 0) {
                    rankStr += ",";
                }
                rankStr += std::to_string(staleRanks[k]);
            }
            STORE_LOG_WARN("Recover: stale ranks not in alive_rank_list, reset to IDLE, ranks=" << rankStr);
            (void)etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
        }
        // After recovery, subscribe to state changes for ongoing persistence
        groupManager_->SetOnStateChangeCallback([this]() {
            if (etcdStore_) {
                etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
                etcdStore_->FlushLinks(groupManager_->GetLinks(), groupManager_->GetMaxRanks());
            }
        });
        if (etcdStore_->IsEnabled()) {
            etcdStore_->StartFlushThread();
        }
    }
}

Result AccStoreServer::HandleControlExtendMemory(const ock::acc::AccTcpRequestContext &context,
                                                 SmemMessage &request) noexcept
{
    uint32_t rankId = 0;
    MultiBytes additionalSlices;
    if (SmemMessage::UnpackExtendMemory(request, rankId, additionalSlices) < 0) {
        STORE_LOG_ERROR("CONTROL ExtendMemory: failed to unpack message, seqNo: " << context.SeqNo());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid extend memory message");
        return SM_INVALID_PARAM;
    }
    if (groupManager_ == nullptr) {
        STORE_LOG_ERROR("CONTROL ExtendMemory: group manager not initialized");
        ReplyWithMessage(context, StoreErrorCode::ERROR, "group manager not initialized");
        return SM_ERROR;
    }
    int ret = groupManager_->ProcessExtendMemory(rankId, additionalSlices, request.requestId);
    if (ret != 0) {
        STORE_LOG_ERROR("CONTROL ExtendMemory: ProcessExtendMemory failed for rankId: " << rankId << ", ret: " << ret);
        ReplyWithMessage(context, StoreErrorCode::ERROR, "extend memory failed");
        return SM_ERROR;
    }
    ReplyWithMessage(context, StoreErrorCode::SUCCESS, "extend memory success");
    return SM_OK;
}

Result AccStoreServer::HandleControlJoin(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    RankFullInfo info;
    if (SmemMessage::UnpackJoin(request, info) < 0) {
        STORE_LOG_ERROR("CONTROL Join: failed to unpack message, seqNo: " << context.SeqNo());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid join message");
        return SM_INVALID_PARAM;
    }
    if (groupManager_ == nullptr) {
        STORE_LOG_ERROR("CONTROL Join: group manager not initialized");
        ReplyWithMessage(context, StoreErrorCode::ERROR, "group manager not initialized");
        return SM_ERROR;
    }
    int ret = groupManager_->CheckIn(info, request.requestId);
    if (ret != 0) {
        STORE_LOG_ERROR("CONTROL Join: CheckIn failed for rankId: " << info.rankId << ", ret: " << ret);
        ReplyWithMessage(context, StoreErrorCode::ERROR, "checkin failed");
        return SM_ERROR;
    }
    if (etcdStore_) {
        auto r = etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist states failed after join: " << static_cast<int>(r));
        }
        r = etcdStore_->PersistRankBase(info.rankId, info.baseInfo);
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist rank base failed: " << static_cast<int>(r));
        }
        r = etcdStore_->PersistRankExternal(info.rankId, info.externalInfo);
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist rank ext failed: " << static_cast<int>(r));
        }
        r = etcdStore_->PersistRankRoles(groupManager_->GetRoles(), groupManager_->GetMaxRanks());
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist rank roles failed: " << static_cast<int>(r));
        }
        auto alive = groupManager_->GetAliveRanks();
        r = etcdStore_->PersistAliveRanks(std::unordered_set<uint32_t>(alive.begin(), alive.end()));
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist alive ranks failed: " << static_cast<int>(r));
        }
    }
    ReplyWithMessage(context, StoreErrorCode::SUCCESS, "success");
    return SM_OK;
}

Result AccStoreServer::HandleControlLeave(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    uint32_t rankId = 0;
    STORE_LOG_DEBUG("HandleControlLeave: recv LEAVREQ seqNo=" << context.SeqNo() << " reqId=" << request.requestId);
    if (SmemMessage::UnpackLeave(request, rankId) < 0) {
        STORE_LOG_ERROR("CONTROL Leave: failed to unpack message, seqNo: " << context.SeqNo());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid leave message");
        return SM_INVALID_PARAM;
    }
    if (groupManager_ == nullptr) {
        STORE_LOG_ERROR("CONTROL Leave: group manager not initialized");
        ReplyWithMessage(context, StoreErrorCode::ERROR, "group manager not initialized");
        return SM_ERROR;
    }
    int ret = groupManager_->Checkout(rankId, request.requestId);
    if (ret != 0) {
        STORE_LOG_ERROR("CONTROL Leave: Checkout failed for rankId: " << rankId << ", ret: " << ret);
        ReplyWithMessage(context, StoreErrorCode::ERROR, "checkout failed");
        return SM_ERROR;
    }
    if (etcdStore_) {
        auto r = etcdStore_->PersistStates(groupManager_->GetStates(), groupManager_->GetMaxRanks());
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist states failed after leave: " << static_cast<int>(r));
        }
        r = etcdStore_->DeleteRank(rankId);
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd delete rank failed: " << static_cast<int>(r));
        }
        auto aliveVec = groupManager_->GetAliveRanks();
        r = etcdStore_->PersistAliveRanks(std::unordered_set<uint32_t>(aliveVec.begin(), aliveVec.end()));
        if (r != StoreErrorCode::SUCCESS) {
            STORE_LOG_ERROR("etcd persist alive ranks failed: " << static_cast<int>(r));
        }
    }
    ReplyWithMessage(context, StoreErrorCode::SUCCESS, "success");
    return SM_OK;
}

Result AccStoreServer::HandleControlLinkStateResponse(const ock::acc::AccTcpRequestContext &context,
                                                      SmemMessage &request) noexcept
{
    RankFullInfo rankInfo;
    std::vector<LinkStateEntry> entries;
    STORE_LOG_INFO("config store server side receive LNKSRSP");
    if (SmemMessage::UnpackLinkStateResponse(request, rankInfo, entries) < 0) {
        STORE_LOG_ERROR("CONTROL LinkStateResponse: failed to unpack message, seqNo: " << context.SeqNo());
        return SM_INVALID_PARAM;
    }
    if (groupManager_ != nullptr) {
        groupManager_->OnLinkStateResponse(rankInfo, entries, request.requestId);
    }
    return SM_OK;
}

Result AccStoreServer::HandleControlAck(const ock::acc::AccTcpRequestContext &context, SmemMessage &request,
                                        ControlOp ackOp) noexcept
{
    uint32_t senderRankId = 0;
    std::vector<uint32_t> targetRankIds;
    std::vector<uint8_t> ackResults;
    uint32_t singleTarget = 0;
    if (SmemMessage::UnpackAckBatch(request, ackOp, senderRankId, targetRankIds, ackResults) < 0) {
        if (SmemMessage::UnpackAck(request, ackOp, senderRankId, singleTarget) < 0) {
            STORE_LOG_ERROR("CONTROL ACK: failed to unpack, seqNo: " << context.SeqNo());
            return SM_INVALID_PARAM;
        }
        targetRankIds.push_back(singleTarget);
        ackResults.push_back(0);
    }
    if (groupManager_ != nullptr) {
        std::map<uint32_t, int32_t> rankRes;
        for (size_t i = 0; i < targetRankIds.size(); ++i) {
            rankRes[targetRankIds[i]] = (i < ackResults.size() && ackResults[i] != 0) ? -1 : 0;
        }
        groupManager_->OnControlAck(ackOp, senderRankId, rankRes, request.requestId);
    }
    return SM_OK;
}

namespace {
const char *ControlOpName(ControlOp op) noexcept
{
    switch (op) {
        case CONTROL_ADD_TO_WHITELIST:
            return "ADD_TO_WHITELIST";
        case CONTROL_REMOVE_FROM_WHITELIST:
            return "REMOVE_FROM_WHITELIST";
        case CONTROL_ESTABLISH_CONNECTION:
            return "ESTABLISH_CONNECTION";
        case CONTROL_JOIN:
            return "JOIN";
        case CONTROL_LEAVE:
            return "LEAVE";
        case CONTROL_QUERY_LINK_STATE:
            return "QUERY_LINK_STATE";
        case CONTROL_LINK_STATE_RESPONSE:
            return "LINK_STATE_RESPONSE";
        case CONTROL_ADD_TO_WHITELIST_ACK:
            return "ADD_TO_WHITELIST_ACK";
        case CONTROL_REMOVE_FROM_WHITELIST_ACK:
            return "REMOVE_FROM_WHITELIST_ACK";
        case CONTROL_ESTABLISH_CONNECTION_ACK:
            return "ESTABLISH_CONNECTION_ACK";
        default:
            return "UNKNOWN";
    }
}
} // namespace

uint32_t AccStoreServer::LookupRankIdByLink(uint32_t linkId) noexcept
{
    std::unique_lock<std::mutex> lock(storeMutex_);
    auto it = linkRankMap_.find(linkId);
    if (it == linkRankMap_.end()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return it->second;
}

Result AccStoreServer::ControlHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept
{
    int8_t op = SmemMessage::GetControlOp(request);
    if (op < 0) {
        STORE_LOG_ERROR("CONTROL message with invalid or missing ControlOp, seqNo: " << context.SeqNo());
        ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "invalid control op");
        return SM_INVALID_PARAM;
    }

    STORE_LOG_DEBUG("ControlHandler: op=" << ControlOpName(static_cast<ControlOp>(op)) << " rid=" << request.requestId);
    switch (static_cast<ControlOp>(op)) {
        case ControlOp::CONTROL_JOIN:
            return HandleControlJoin(context, request);
        case ControlOp::CONTROL_LEAVE:
            return HandleControlLeave(context, request);
        case ControlOp::CONTROL_EXTEND_MEMORY:
            return HandleControlExtendMemory(context, request);
        case ControlOp::CONTROL_LINK_STATE_RESPONSE:
            return HandleControlLinkStateResponse(context, request);
        case ControlOp::CONTROL_ADD_TO_WHITELIST_ACK:
        case ControlOp::CONTROL_REMOVE_FROM_WHITELIST_ACK:
        case ControlOp::CONTROL_ESTABLISH_CONNECTION_ACK:
            return HandleControlAck(context, request, static_cast<ControlOp>(op));
        default:
            STORE_LOG_ERROR("unhandled ControlOp: " << static_cast<int>(op) << " seqNo: " << context.SeqNo());
            ReplyWithMessage(context, StoreErrorCode::INVALID_MESSAGE, "unsupported control op");
            return SM_INVALID_PARAM;
    }
}

int AccStoreServer::SendControlToRank(uint32_t targetRankId, const std::vector<uint8_t> &data) noexcept
{
    std::unique_lock<std::mutex> lock(storeMutex_);
    auto it = rankLinks_.find(targetRankId);
    if (it == rankLinks_.end() || !it->second->Established()) {
        STORE_LOG_ERROR("SendControlToRank: no link for rankId: " << targetRankId);
        return -1;
    }
    auto dataBuf = ock::acc::AccDataBuffer::Create(data.data(), data.size());
    if (dataBuf == nullptr) {
        STORE_LOG_ERROR("SendControlToRank: failed to create data buffer for rankId: " << targetRankId);
        return -1;
    }
    auto ret = it->second->NonBlockSend(0, 0, dataBuf, nullptr);
    if (ret != SM_OK) {
        STORE_LOG_ERROR("SendControlToRank: NonBlockSend failed for rankId: " << targetRankId << ", ret: " << ret);
        return ret;
    }
    return 0;
}

namespace {
const char *ServerStateName(uint32_t state)
{
    switch (state) {
        case SS_INITED:
            return "SS_INITED";
        case SS_RECOVERING:
            return "SS_RECOVERING";
        case SS_RECOVERED:
            return "SS_RECOVERED";
        case SS_NORMAL:
            return "SS_NORMAL";
        case SS_EXITED:
            return "SS_EXITED";
        default:
            return "SS_UNKNOWN";
    }
}

const char *RankStateName(int8_t state)
{
    switch (state) {
        case RANK_CHECKED_IN:
            return "RANK_CHECKED_IN";
        case RANK_ACTIVE:
            return "RANK_ACTIVE";
        default:
            return "RANK_IDLE";
    }
}

const char *LinkStateName(int8_t state)
{
    switch (state) {
        case LINK_EXCHANGING:
            return "LINK_EXCHANGING";
        case LINK_EXCHANGED:
            return "LINK_EXCHANGED";
        case LINK_CONNECTING:
            return "LINK_CONNECTING";
        case LINK_CONNECTED:
            return "LINK_CONNECTED";
        default:
            return "LINK_IDLE";
    }
}

void AppendUintSetJson(std::string &out, const std::unordered_set<uint32_t> &values)
{
    std::vector<uint32_t> ranks(values.begin(), values.end());
    std::sort(ranks.begin(), ranks.end());
    out.push_back('[');
    for (size_t i = 0; i < ranks.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append(std::to_string(ranks[i]));
    }
    out.push_back(']');
}

// unstableLinks 明细条目：中间态链路，或双端存活却处于 IDLE 的断链
struct UnstableLinkInfo {
    uint32_t src;
    uint32_t dst;
    LinkState state;
    uint32_t retry;
    int64_t elapsedMs;
};

// 链路矩阵单次遍历的产出：摘要计数 + 异常明细（输出聚合，替代多个出参）
struct LinkSummary {
    uint32_t connectedLinks{0};
    uint32_t abnormalLinks{0};
    std::vector<UnstableLinkInfo> unstableLinks;
};

bool IsMiddleStateLink(LinkState st) noexcept
{
    return st == LINK_EXCHANGING || st == LINK_EXCHANGED || st == LINK_CONNECTING;
}

// 分离部署下 PROXY<->PROXY 不建链：统计链路数/异常链路时必须剔除，否则该对会被永久报成
// “双端存活却 IDLE”的异常，completionRate 也永远达不到 1。
bool IsPrunedPair(const AccStoreServer::ClusterStatusSnapshot &snap, uint32_t src, uint32_t dst) noexcept
{
    if (snap.roles.size() < snap.maxRanks) {
        return false; // 旧快照无角色信息：保持原统计口径
    }
    return snap.roles[src] == SMEM_GROUP_RANK_ROLE_PROXY && snap.roles[dst] == SMEM_GROUP_RANK_ROLE_PROXY;
}

// 期望链路数（方向性）= ACTIVE rank 的有向对数 − PROXY<->PROXY 的有向对数（剪枝对不建链）。
// 快照缺角色信息时 activeProxyRanks 为 0，公式自然退化为历史口径 activeRanks*(activeRanks-1)，无需单独分支。
uint64_t ExpectedLinks(uint32_t activeRanks, uint32_t activeProxyRanks) noexcept
{
    const auto directedPairs = [](uint32_t n) -> uint64_t {
        return (n < 2U) ? 0U : static_cast<uint64_t>(n) * (n - 1U);
    };
    return directedPairs(activeRanks) - directedPairs(activeProxyRanks);
}

// 单次遍历链路矩阵（数据取自集群状态快照），同时产出 connected/abnormal 计数与 unstable 明细
LinkSummary CollectLinkSummary(const AccStoreServer::ClusterStatusSnapshot &snap,
                               const std::chrono::steady_clock::time_point &now) noexcept
{
    LinkSummary summary;
    for (uint32_t src = 0; src < snap.maxRanks; ++src) {
        for (uint32_t dst = 0; dst < snap.maxRanks; ++dst) {
            if (src == dst) {
                continue; // 对角线无链路语义
            }
            if (IsPrunedPair(snap, src, dst)) {
                continue; // 剪枝对：既不计 connected，也不计 abnormal/unstable
            }
            const size_t idx = static_cast<size_t>(src) * snap.maxRanks + dst;
            const LinkState st = snap.links[idx];
            if (st == LINK_CONNECTED) {
                ++summary.connectedLinks;
                continue;
            }
            // 双端存活却非 CONNECTED 才算异常：对端已死的 IDLE 是预期状态，不计入
            const bool bothAlive = (snap.aliveRanks.count(src) > 0) && (snap.aliveRanks.count(dst) > 0);
            if (bothAlive) {
                ++summary.abnormalLinks;
            }
            const bool middleState = IsMiddleStateLink(st);
            if (!middleState && !(st == LINK_IDLE && bothAlive)) {
                continue;
            }
            UnstableLinkInfo info{src, dst, st, 0, -1};
            if (middleState) {
                const auto &tr = snap.transitions[idx];
                info.retry = tr.retryCount;
                // transitions_ 时间戳为 steady_clock 单调时钟，elapsed 同域计算，避免系统时钟跳变导致负值
                info.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - tr.timestamp).count();
            }
            summary.unstableLinks.push_back(info);
        }
    }
    return summary;
}

// linkSummary 摘要输出：expectedLinks 以 activeRanks 为基数（未 join 完成时不用 worldSize 压低完成率），
// 并已扣除 PROXY<->PROXY 剪枝对
void AppendLinkSummaryJson(std::string &out, uint32_t activeRanks, uint64_t expectedLinks, uint32_t connectedLinks,
                           uint32_t abnormalLinks) noexcept
{
    const double completionRate = (expectedLinks == 0) ? 1.0 : static_cast<double>(connectedLinks) / expectedLinks;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << completionRate;
    out.append(",\"linkSummary\":{\"activeRanks\":").append(std::to_string(activeRanks));
    out.append(",\"connectedLinks\":").append(std::to_string(connectedLinks));
    out.append(",\"expectedLinks\":").append(std::to_string(expectedLinks));
    out.append(",\"completionRate\":").append(oss.str());
    out.append(",\"abnormalLinks\":").append(std::to_string(abnormalLinks)).append("}");
}

// unstableLinks 明细输出：按 (retry 降序, elapsedMs 降序) 排序——最可能卡死的排最前；
// 超出 kUnstableLinksMaxEntries 截断，并在响应追加 unstableTruncated 计数
void AppendUnstableLinksJson(std::string &out, std::vector<UnstableLinkInfo> &unstableLinks) noexcept
{
    std::sort(unstableLinks.begin(), unstableLinks.end(), [](const UnstableLinkInfo &a, const UnstableLinkInfo &b) {
        if (a.retry != b.retry) {
            return a.retry > b.retry;
        }
        return a.elapsedMs > b.elapsedMs;
    });
    size_t outputCount = unstableLinks.size();
    if (outputCount > kUnstableLinksMaxEntries) {
        outputCount = kUnstableLinksMaxEntries;
    }
    out.append(",\"unstableLinks\":[");
    for (size_t i = 0; i < outputCount; ++i) {
        const UnstableLinkInfo &info = unstableLinks[i];
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"src\":").append(std::to_string(info.src));
        out.append(",\"dst\":").append(std::to_string(info.dst));
        out.append(",\"state\":\"").append(LinkStateName(info.state)).append("\"");
        out.append(",\"retry\":").append(std::to_string(info.retry));
        out.append(",\"elapsedMs\":").append(std::to_string(info.elapsedMs)).append("}");
    }
    out.push_back(']');
    if (unstableLinks.size() > outputCount) {
        out.append(",\"unstableTruncated\":").append(std::to_string(unstableLinks.size() - outputCount));
    }
}

// lastHeartbeat 数组输出
void AppendHeartbeatJson(std::string &out, const std::vector<std::pair<uint32_t, int64_t>> &heartbeats) noexcept
{
    out.append(",\"lastHeartbeat\":[");
    std::string sep = "";
    for (const auto &rankBeat : heartbeats) {
        out.append(sep)
            .append("{\"rank\":")
            .append(std::to_string(rankBeat.first))
            .append(",\"sinceLastMs\":")
            .append(std::to_string(rankBeat.second))
            .append("}");
        sep = ",";
    }
    out.push_back(']');
}

// ranks 数组输出：单次遍历同时统计 ACTIVE rank 数、其中 Proxy（无内存）数（供期望链路数计算），
// 并序列化非 IDLE rank（IDLE 属预期状态不输出），同时带上角色，便于现场直接解释
// “为什么这两条链路没有”：role=proxy 之间的链路被剪枝。
struct GmRankCounts {
    uint32_t activeRanks{0};
    uint32_t activeProxyRanks{0};
};

GmRankCounts AppendGmRanksJson(std::string &out, const AccStoreServer::ClusterStatusSnapshot &snap) noexcept
{
    const auto &states = snap.states;
    const bool hasRoles = snap.roles.size() >= snap.maxRanks;
    GmRankCounts counts;
    out.append(",\"ranks\":[");
    std::string sep = "";
    for (uint32_t rankId = 0; rankId < states.size(); ++rankId) {
        const bool isProxy = hasRoles && (snap.roles[rankId] == SMEM_GROUP_RANK_ROLE_PROXY);
        if (states[rankId] == RANK_ACTIVE) {
            ++counts.activeRanks;
            counts.activeProxyRanks += isProxy ? 1U : 0U;
        }
        if (states[rankId] == RANK_IDLE) {
            continue;
        }
        out.append(sep)
            .append("{\"rank\":")
            .append(std::to_string(rankId))
            .append(",\"state\":\"")
            .append(RankStateName(states[rankId]))
            .append("\"");
        if (hasRoles) {
            out.append(",\"role\":\"").append(RankRoleName(snap.roles[rankId])).append("\"");
        }
        out.append("}");
        sep = ",";
    }
    out.push_back(']');
    return counts;
}
} // namespace

// 持 storeMutex_ 拷出集群状态快照（短持锁，仅数据拷贝，不做 JSON 拼接）：
AccStoreServer::ClusterStatusSnapshot AccStoreServer::CaptureClusterStatus() noexcept
{
    ClusterStatusSnapshot snap;
    std::lock_guard<std::mutex> guard(storeMutex_);
    snap.listen = listenIp_ + ":" + std::to_string(listenPort_);
    snap.backendAliveRanks = aliveRankFromBackend_;
    snap.aliveRanks = aliveRankSet_;
    snap.reconnectedRanks = reconnectedRankSet_;
    const int64_t nowMs = mf::StrUtil::GetNowTime();
    snap.heartbeats.reserve(heartBeatMap_.size());
    for (const auto &item : heartBeatMap_) {
        const uint32_t rankId = GetRankIdByLinkId(item.first); // storeMutex_ 已持有
        if (rankId == UINT32_MAX) {
            continue; // 尚未建立 rank 映射的连接不输出
        }
        snap.heartbeats.emplace_back(rankId, nowMs - item.second);
    }
    std::sort(snap.heartbeats.begin(), snap.heartbeats.end());
    if (groupManager_ != nullptr) {
        snap.hasGm = true;
        snap.maxRanks = groupManager_->GetMaxRanks();
        groupManager_->GetSnapshot(snap.states, snap.links, snap.transitions, snap.roles);
    }
    return snap;
}

// 集群状态 JSON 输出
void AccStoreServer::GetClusterStatusJson(std::string &out) noexcept
{
    const ClusterStatusSnapshot snap = CaptureClusterStatus();
    out.push_back('{');
    const uint32_t state = state_.load(std::memory_order_acquire);
    out.append("\"state\":\"").append(ServerStateName(state)).append("\"");
    out.append(",\"worldSize\":").append(std::to_string(worldSize_));
    out.append(",\"listen\":\"").append(snap.listen).append("\"");
    out.append(",\"backendAliveRanks\":");
    AppendUintSetJson(out, snap.backendAliveRanks);
    out.append(",\"aliveRanks\":");
    AppendUintSetJson(out, snap.aliveRanks);
    out.append(",\"reconnectedRanks\":");
    AppendUintSetJson(out, snap.reconnectedRanks);
    AppendHeartbeatJson(out, snap.heartbeats);

    if (!snap.hasGm) {
        out.append(",\"groupManager\":null}");
        return;
    }
    out.append(",\"groupManager\":{\"maxRanks\":").append(std::to_string(snap.maxRanks));
    const GmRankCounts rankCounts = AppendGmRanksJson(out, snap);
    // 单次遍历矩阵产出摘要计数与异常明细，替代原 activeLinks 全量矩阵输出
    LinkSummary summary = CollectLinkSummary(snap, std::chrono::steady_clock::now());
    AppendLinkSummaryJson(out, rankCounts.activeRanks,
                          ExpectedLinks(rankCounts.activeRanks, rankCounts.activeProxyRanks), summary.connectedLinks,
                          summary.abnormalLinks);
    AppendUnstableLinksJson(out, summary.unstableLinks);
    out.append("}}");
}

} // namespace smem
} // namespace ock
