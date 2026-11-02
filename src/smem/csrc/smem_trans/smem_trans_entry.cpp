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
#include "smem_trans_entry.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "acc_tcp_port_util.h"
#include "hybm.h"
#include "hybm_big_mem.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "smem.h"
#include "smem_group_manager_client.h"
#include "smem_store_factory.h"
#include "smem_tcp_config_store.h"
#include "smem_trans_def.h"
#include "smem_trans_entry_manager.h"

namespace ock {
namespace smem {
// Default config-store port range, consistent with smem HA layer (network_endpoint_util.h)
constexpr uint16_t TRANS_PORT_RANGE_START = 9000;
constexpr uint16_t TRANS_PORT_RANGE_MAX = 65535;
constexpr uint32_t TRANS_PORT_SELECT_MAX_RETRY = 64;
constexpr uint64_t TRANS_MAX_BATCH_COUNT = 1024ULL * 1024;
// reserve 128GB hbm va for malloc per rank, refine to configurable later
constexpr uint64_t TRANS_RESERVE_HBM_VA_SIZE = 1024ULL * 1024 * 1024 * 1024 * 5; // 5T

static void GetPortRange(uint16_t &minPort, uint16_t &maxPort)
{
    minPort = TRANS_PORT_RANGE_START;
    maxPort = TRANS_PORT_RANGE_MAX;
    const auto parsePort = [](const char *envName, uint16_t def, uint16_t &out) {
        out = def;
        const char *val = std::getenv(envName);
        if (val == nullptr || val[0] == '\0') {
            return;
        }
        uint32_t parsed = 0;
        if (!mf::MfEnvUtil::GetUint(std::string(val), parsed) || parsed > 65535UL) {
            SM_LOG_WARN("invalid port env " << envName << "='" << val << "', fallback to " << def);
            return;
        }
        out = static_cast<uint16_t>(parsed);
    };
    uint16_t startP = TRANS_PORT_RANGE_START;
    uint16_t endP = TRANS_PORT_RANGE_MAX;
    parsePort("MF_CONFIG_STORE_PORT_START", TRANS_PORT_RANGE_START, startP);
    parsePort("MF_CONFIG_STORE_PORT_END", TRANS_PORT_RANGE_MAX, endP);
    if (startP > endP) {
        SM_LOG_WARN("invalid port range start=" << startP << " > end=" << endP << ", fallback to default ["
                                                << TRANS_PORT_RANGE_START << "," << TRANS_PORT_RANGE_MAX << "]");
        return;
    }
    minPort = startP;
    maxPort = endP;
}

static uint16_t FindAvailableTcpPort(int &sockfd)
{
    uint16_t minPort = TRANS_PORT_RANGE_START;
    uint16_t maxPort = TRANS_PORT_RANGE_MAX;
    GetPortRange(minPort, maxPort);
    return ock::acc::AccFindAvailableTcpPort(sockfd, minPort, maxPort);
}

static void ParseIpPortFromUrl(const std::string &uniqueId, std::string &ip, uint16_t &port)
{
    ip.clear();
    port = 0;
    auto pos = uniqueId.rfind(':');
    if (pos == std::string::npos) {
        ip = uniqueId;
        SM_LOG_INFO("url='" << uniqueId << "' has no port suffix, auto-select port");
        return;
    }
    if (pos + 1 >= uniqueId.size()) {
        ip = uniqueId.substr(0, pos);
        SM_LOG_INFO("url='" << uniqueId << "' has empty port suffix, auto-select port");
        return;
    }
    const std::string portStr = uniqueId.substr(pos + 1);
    for (char c : portStr) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            ip = uniqueId;
            SM_LOG_WARN("url='" << uniqueId << "' port suffix '" << portStr
                                << "' is non-numeric, treat whole as ip and auto-select port");
            return;
        }
    }
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(portStr);
    } catch (...) {
        ip = uniqueId;
        SM_LOG_WARN("url='" << uniqueId << "' port suffix '" << portStr << "' is out of range, auto-select port");
        return;
    }
    if (parsed > 65535UL) {
        ip = uniqueId;
        SM_LOG_WARN("url='" << uniqueId << "' port " << parsed << " > 65535, auto-select port");
        return;
    }
    ip = uniqueId.substr(0, pos);
    port = static_cast<uint16_t>(parsed);
}

SmemTransEntryPtr SmemTransEntry::Create(const smem_trans_config_t &config)
{
    SmemTransEntryPtr transEntry;
    auto result = SmemTransEntryManager::Instance().CreateEntry(config, transEntry);
    if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("create trans entry failed, result: " << result << ", url: " << config.url);
        return nullptr;
    }

    result = transEntry->Initialize();
    if (result != SM_OK) {
        SmemTransEntryManager::Instance().RemoveEntryByPtr(reinterpret_cast<uintptr_t>(transEntry.Get()));
        SM_LOG_AND_SET_LAST_ERROR("initialize trans entry failed, result: " << result << ", url: " << config.url);
        return nullptr;
    }
    return transEntry;
}

SmemTransEntry::~SmemTransEntry()
{
    UnInitialize();
}

Result SmemTransEntry::Initialize()
{
    auto ret = CreateStoreServer();
    if (ret != SM_OK) {
        return ret;
    }

    ret = StartExecutor();
    if (ret != SM_OK) {
        return ret;
    }

    ret = CreateHybmEntities();
    if (ret != SM_OK) {
        SM_LOG_ERROR("create hybm entities failed, url: " << name_ << " ret: " << ret);
        executor_->Stop();
        return ret;
    }

    ret = CreateSelfConnection();
    if (ret != SM_OK) {
        SM_LOG_ERROR("create self connection failed, url: " << name_ << " ret: " << ret);
        executor_->Stop();
        return ret;
    }
    SM_LOG_INFO("smem trans entry initialize success, url: " << name_ << " rpcPort: " << rpcPort_ << " sendEntityId: "
                                                             << sendEntityId_ << " recvEntityId: " << recvEntityId_);
    return SM_OK;
}

Result SmemTransEntry::CreateStoreServer()
{
    std::string ip;
    uint16_t port = 0;
    ParseIpPortFromUrl(name_, ip, port);
    if (ip.empty()) {
        SM_LOG_ERROR("parse url failed, url: " << name_);
        return SM_INVALID_PARAM;
    }

    if (port == 0) {
        return CreateStoreServerWithAutoPort(ip);
    }

    name_ = ip + ":" + std::to_string(port);
    selfUrl_ = std::string("tcp://") + name_;
    rpcPort_ = port;
    auto ret = smem_create_config_store(selfUrl_.c_str(), SMEM_STORE_SKIP_RECOVER);
    if (ret != SM_OK) {
        SM_LOG_ERROR("smem_create_config_store failed, storeUrl: " << selfUrl_ << " rpcPort: " << rpcPort_
                                                                   << " ret: " << ret);
        return ret;
    }

    if (!SmemTransConnection::ParseTransName(name_, workerUniqueId_.address, workerUniqueId_.port)) {
        SM_LOG_ERROR("parse local worker failed, name: " << name_);
        return SM_INVALID_PARAM;
    }
    return SM_OK;
}

Result SmemTransEntry::CreateStoreServerWithAutoPort(const std::string &ip)
{
    for (uint32_t attempt = 0; attempt < TRANS_PORT_SELECT_MAX_RETRY; ++attempt) {
        int probeFd = -1;
        auto candidatePort = FindAvailableTcpPort(probeFd);
        if (candidatePort == 0) {
            SM_LOG_ERROR("no available port for store server, ip: " << ip << " attempt: " << attempt);
            return SM_ERROR;
        }
        close(probeFd); // release so the real listener can bind+listen

        name_ = ip + ":" + std::to_string(candidatePort);
        selfUrl_ = std::string("tcp://") + name_;
        auto ret = smem_create_config_store(selfUrl_.c_str(), SMEM_STORE_SKIP_RECOVER);
        if (ret == SM_OK) {
            rpcPort_ = candidatePort;
            SM_LOG_INFO("store server port auto detected, storeUrl: " << selfUrl_ << " attempt: " << attempt);
            if (!SmemTransConnection::ParseTransName(name_, workerUniqueId_.address, workerUniqueId_.port)) {
                SM_LOG_ERROR("parse local worker failed, name: " << name_);
                return SM_INVALID_PARAM;
            }
            return SM_OK;
        }
        SM_LOG_WARN("create config store on port " << candidatePort << " failed, ret: " << ret << ", retry");
    }
    SM_LOG_ERROR("failed to allocate store server port after " << TRANS_PORT_SELECT_MAX_RETRY
                                                               << " retries, ip: " << ip);
    return SM_ERROR;
}

// Default worker thread count of the trans executor, overridable by env MF_TRANS_THREAD_NUM
constexpr uint32_t TRANS_EXECUTOR_DEFAULT_THREAD_NUM = 4;

static uint32_t GetExecutorThreadNum()
{
    const char *val = std::getenv("MF_TRANS_THREAD_NUM");
    if (val == nullptr || val[0] == '\0') {
        return TRANS_EXECUTOR_DEFAULT_THREAD_NUM;
    }
    uint32_t parsed = 0;
    if (!mf::MfEnvUtil::GetUint(std::string(val), parsed) || parsed == 0) {
        SM_LOG_WARN("invalid MF_TRANS_THREAD_NUM='" << val << "', fallback to " << TRANS_EXECUTOR_DEFAULT_THREAD_NUM);
        return TRANS_EXECUTOR_DEFAULT_THREAD_NUM;
    }
    return parsed;
}

Result SmemTransEntry::StartExecutor()
{
    executor_ = std::make_unique<ExecutorService>(GetExecutorThreadNum());
    executor_->SetThreadName("smem_trans_exec");
    if (!executor_->Start()) {
        SM_LOG_ERROR("start executor failed, url: " << name_);
        executor_.reset();
        return SM_ERROR;
    }
    return SM_OK;
}

// 双 entity 的 rankId 约定：recvEntity 用 0、sendEntity 用 SMEM_TRANS_RANK_COUNT_MAX-1。
// transport 按 rankId 判定 server/client（rank 小者 server 被动等待、大者 client 主动建链），
// 这样 send entity 恒为 client、recv entity 恒为 server，避免两端都判为同一角色而无法建链；
// 对端 override rank 只落在中间区间 [1, SMEM_TRANS_RANK_COUNT_MAX-2]，与本地两窗口互不重叠。
constexpr uint32_t TRANS_RECV_ENTITY_RANK = 0U;
constexpr uint32_t TRANS_SEND_ENTITY_RANK = SMEM_TRANS_RANK_COUNT_MAX - 1U;

Result SmemTransEntry::CreateHybmEntities()
{
    // 双 entity 各分配一个进程级 entity id，保证 hybm entity id 与 transport 端口互不冲突
    auto ret = SmemTransEntryManager::Instance().AllocEntityId(sendEntityId_);
    if (ret != SM_OK) {
        SM_LOG_ERROR("alloc send entity id failed, url: " << name_);
        return ret;
    }
    ret = SmemTransEntryManager::Instance().AllocEntityId(recvEntityId_);
    if (ret != SM_OK) {
        SM_LOG_ERROR("alloc recv entity id failed, url: " << name_);
        return ret;
    }

    ret = CreateOneHybmEntity(sendEntityId_, sendEntityInfo_, sendEntity_, TRANS_SEND_ENTITY_RANK);
    if (ret != SM_OK) {
        return ret;
    }
    ret = CreateOneHybmEntity(recvEntityId_, recvEntityInfo_, recvEntity_, TRANS_RECV_ENTITY_RANK);
    if (ret != SM_OK) {
        hybm_export_info_free(&sendEntityInfo_.hybmInfo);
        hybm_destroy_entity(sendEntity_, 0);
        sendEntity_ = nullptr;
        return ret;
    }
    return SM_OK;
}

Result SmemTransEntry::CreateOneHybmEntity(uint32_t entityId, SmemTransExchangeInfo &entityInfo, hybm_entity_t &entity,
                                           uint32_t rankId)
{
    auto options = GenerateHybmOptions(entityId, rankId);
    options.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_DEFAULT);
    if (!ApplyDataOpType(options, SMEMB_DATA_OP_SDMA, HYBM_DOP_TYPE_SDMA, "device_sdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_RDMA, "device_rdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_URMA, HYBM_DOP_TYPE_DEVICE_URMA, "device_urma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_UBOE, HYBM_DOP_TYPE_DEVICE_UBOE, "device_uboe") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_RDMA, HYBM_DOP_TYPE_HOST_RDMA, "host_rdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_URMA, HYBM_DOP_TYPE_HOST_URMA, "host_urma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_TCP, HYBM_DOP_TYPE_HOST_TCP, "host_tcp")) {
        SM_LOG_ERROR("apply data op type failed, url: " << name_ << " entityId: " << entityId);
        return SM_ERROR;
    }

    auto hybmEntityId = entityId + HYBM_ENTITY_ID_TRANS_BASE;
    entity = hybm_create_entity(hybmEntityId, &options, 0);
    if (entity == nullptr) {
        SM_LOG_ERROR("hybm_create_entity failed, entityId: " << hybmEntityId << " url: " << name_);
        return SM_ERROR;
    }

    auto ret = hybm_reserve_mem_space(entity, 0);
    if (ret != SM_OK) {
        SM_LOG_ERROR("hybm_reserve_mem_space failed, ret: " << ret << " entityId: " << hybmEntityId);
        hybm_destroy_entity(entity, 0);
        entity = nullptr;
        return SM_ERROR;
    }

    entityInfo.hybmInfo = {};
    ret = hybm_export(entity, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo.hybmInfo);
    if (ret != SM_OK) {
        SM_LOG_ERROR("export entity info failed, ret: " << ret << " entityId: " << hybmEntityId);
        hybm_export_info_free(&entityInfo.hybmInfo);
        hybm_destroy_entity(entity, 0);
        entity = nullptr;
        return SM_ERROR;
    }
    entityInfo.u.session = workerUniqueId_;
    entityInfo.u.session.reserved = config_.role;
    SM_LOG_INFO("hybm entity created, entityId: " << hybmEntityId << " rankCount: " << options.rankCount
                                                  << " rankId: " << options.rankId);
    return SM_OK;
}

static hybm_role_type MapTransRoleToHybmRole(smem_trans_role_t role)
{
    if (role == SMEM_TRANS_SENDER) {
        return HYBM_ROLE_SENDER;
    }
    if (role == SMEM_TRANS_RECEIVER) {
        return HYBM_ROLE_RECEIVER;
    }
    return HYBM_ROLE_PEER;
}

hybm_options SmemTransEntry::GenerateHybmOptions(uint32_t entityId, uint32_t rankId)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    // 本地注册内存位于本 entity 自身 rankId 的窗口：recvEntity=0、sendEntity=SMEM_TRANS_RANK_COUNT_MAX-1；
    // 远端对端切片导入到各自 override rank 窗口 [1, SMEM_TRANS_RANK_COUNT_MAX-2]，与本地两窗口互不重叠。
    options.rankCount = SMEM_TRANS_RANK_COUNT_MAX;
    options.rankId = rankId;
    options.devId = config_.deviceId;
    options.scene = HYBM_SCENE_TRANS;
    options.role = MapTransRoleToHybmRole(config_.role);
    options.dramShmFd = -1;
    options.enable56BitsGva = true; // trans enabled
    bzero(options.tag, sizeof(options.tag));
    bzero(options.tagOpInfo, sizeof(options.tagOpInfo));

    options.memType = static_cast<hybm_mem_type>(HYBM_MEM_TYPE_DEVICE);
    options.maxHBMSize = TRANS_RESERVE_HBM_VA_SIZE;
    options.maxDRAMSize = 0;
    options.hostVASpace = 0;
    options.deviceVASpace = 0;

    bzero(options.transUrl, sizeof(options.transUrl));
    if (config_.nic[0] != '\0') {
        constexpr size_t NIC_SIZE = sizeof(options.transUrl);
        size_t urlLen = strnlen(config_.nic, sizeof(config_.nic));
        size_t maxChars = std::min(urlLen, NIC_SIZE - 1);
        std::copy_n(config_.nic, maxChars, options.transUrl);
    } else {
        uint16_t port = 11000 + entityId;
        auto url = "tcp://127.0.0.1:" + std::to_string(port);
        constexpr size_t NIC_SIZE = sizeof(options.transUrl);
        size_t maxChars = std::min(url.length(), NIC_SIZE - 1);
        std::copy_n(url.c_str(), maxChars, options.transUrl);
    }

    options.tlsOption.tlsEnable = config_.hcomTlsConfig.tlsEnable;
    std::copy_n(config_.hcomTlsConfig.caPath, SMEM_TLS_PATH_SIZE, options.tlsOption.caPath);
    std::copy_n(config_.hcomTlsConfig.crlPath, SMEM_TLS_PATH_SIZE, options.tlsOption.crlPath);
    std::copy_n(config_.hcomTlsConfig.certPath, SMEM_TLS_PATH_SIZE, options.tlsOption.certPath);
    std::copy_n(config_.hcomTlsConfig.keyPath, SMEM_TLS_PATH_SIZE, options.tlsOption.keyPath);
    std::copy_n(config_.hcomTlsConfig.keyPassPath, SMEM_TLS_PATH_SIZE, options.tlsOption.keyPassPath);
    std::copy_n(config_.hcomTlsConfig.packagePath, SMEM_TLS_PATH_SIZE, options.tlsOption.packagePath);
    std::copy_n(config_.hcomTlsConfig.decrypterLibPath, SMEM_TLS_PATH_SIZE, options.tlsOption.decrypterLibPath);

    return std::move(options);
}

bool SmemTransEntry::ApplyDataOpType(hybm_options &options, uint32_t flag, hybm_data_op_type hybmFlag,
                                     const char *opName) noexcept
{
    if (!(config_.dataOpType & flag)) {
        return true;
    }
#if !defined(ASCEND_NPU)
    // host 类 op 为纯主机侧传输，不依赖昇腾设备 SDK，非 NPU 构建同样可用
    const bool needNpu = (flag & (SMEMB_DATA_OP_HOST_RDMA | SMEMB_DATA_OP_HOST_URMA | SMEMB_DATA_OP_HOST_TCP)) == 0U;
    if (needNpu) {
        SM_LOG_ERROR("current memfabric-hybrid binary is not built for ascend npu, can not use " << opName
                                                                                                 << " optype.");
        return false;
    }
#endif
    auto temp = static_cast<uint32_t>(options.bmDataOpType) | hybmFlag;
    options.bmDataOpType = static_cast<hybm_data_op_type>(temp);
    return true;
}

Result SmemTransEntry::AllocOverrideRank(const std::string &uniqueId, uint32_t &rank)
{
    // rank 0 (recvEntity 本地窗口) 与 rank SMEM_TRANS_RANK_COUNT_MAX-1 (sendEntity 本地窗口) 均被保留，
    // 故连接的 override rank 只在 [1, SMEM_TRANS_RANK_COUNT_MAX-1) 区间内分配。
    // The unified uniqueId -> rank mapping is shared by the send/recv entities (both configured with
    // the same rank layout), so the same peer gets the same window on either entity. The mapping is
    // reference counted: the rank is recycled only after the last reference is released.
    std::lock_guard<std::mutex> lock(overrideRankMutex_);
    auto it = uniqueIdToRank_.find(uniqueId);
    if (it != uniqueIdToRank_.end()) {
        it->second.refCount++;
        rank = it->second.rank;
        return SM_OK;
    }
    std::unordered_set<uint32_t> usedRanks;
    usedRanks.reserve(uniqueIdToRank_.size());
    for (const auto &kv : uniqueIdToRank_) {
        usedRanks.insert(kv.second.rank);
    }
    for (uint32_t candidate = 1U; candidate < SMEM_TRANS_RANK_COUNT_MAX - 1U; ++candidate) {
        if (usedRanks.insert(candidate).second) {
            uniqueIdToRank_[uniqueId] = OverrideRankEntry{candidate, 1U};
            rank = candidate;
            SM_LOG_INFO("alloc override rank:" << rank << " uniqueId:" << uniqueId);
            return SM_OK;
        }
    }
    SM_LOG_ERROR("override rank exhausted, max: " << SMEM_TRANS_RANK_COUNT_MAX - 2U << " uniqueId: " << uniqueId
                                                  << " url: " << name_);
    return SM_ERROR;
}

void SmemTransEntry::ReleaseOverrideRank(const std::string &uniqueId)
{
    std::lock_guard<std::mutex> lock(overrideRankMutex_);
    auto it = uniqueIdToRank_.find(uniqueId);
    if (it == uniqueIdToRank_.end()) {
        SM_LOG_WARN("release override rank skipped, no mapping, uniqueId: " << uniqueId << " url: " << name_);
        return;
    }
    if (--it->second.refCount != 0) {
        return;
    }
    auto rank = it->second.rank;
    uniqueIdToRank_.erase(it);
    SM_LOG_INFO("override rank fully released, rank: " << rank << " uniqueId: " << uniqueId << " url: " << name_);
}

Result SmemTransEntry::CreateSelfConnection()
{
    // self connection 连接本地 store server，绑定 recv entity，承载被动建链时的信息交互；
    // 建链过程不持 connMutex_，完成后在锁内登记，与读侧（GetOrCreateConnection/RegisterOneMemory）互斥
    auto conn = CreateAndJoinConnection(name_, true);
    if (conn == nullptr) {
        SM_LOG_ERROR("create self connection failed, url: " << name_);
        return SM_ERROR;
    }
    std::lock_guard<std::mutex> lock(connMutex_);
    selfConnection_ = conn;
    return SM_OK;
}

void SmemTransEntry::UnInitialize()
{
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
        return; // already uninitialized, keep idempotent for dtor after explicit call
    }
    // store 的生命周期长于 entry：先失效断链回调的存活令牌，避免 entry 销毁后回调触碰本对象
    brokenHandlerAlive_->store(false);

    if (executor_ != nullptr) {
        executor_->Stop(); // workers drain queued tasks first, entry members are still valid here
        executor_.reset();
    }
    // 在锁内摘除 self connection（读侧同一把锁取副本），实际析构放到锁外，
    // 避免在 connMutex_ 内执行 Leave/回收窗口等其它加锁操作
    SmemTransConnectionPtr selfConn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        // 从 connections_ 移除各 uniqueId 记录时同步归还其 override rank 引用；
        // 条目析构释放连接引用，各连接 UnInitialize 回收其 override-rank 窗口
        for (const auto &kv : connections_) {
            ReleaseOverrideRank(kv.first);
        }
        connections_.clear();
        selfConn = selfConnection_; // 先持一份引用再摘除成员，保证真正的析构发生在锁外
        selfConnection_ = nullptr;
        connCv_.notify_all();
    }
    selfConn = nullptr; // self connection leaves the local group and recycles its windows
    {
        std::lock_guard<std::mutex> lock(overrideRankMutex_);
        uniqueIdToRank_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(batchMutex_);
        batchStatusMap_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(memMutex_);
        sendSliceIds_.clear();
        sendExportedSlices_.clear();
        recvSliceIds_.clear();
        recvExportedSlices_.clear();
    }
    if (sendEntity_ != nullptr) {
        hybm_export_info_free(&sendEntityInfo_.hybmInfo);
        hybm_destroy_entity(sendEntity_, 0);
        sendEntity_ = nullptr;
    }
    if (recvEntity_ != nullptr) {
        hybm_export_info_free(&recvEntityInfo_.hybmInfo);
        hybm_destroy_entity(recvEntity_, 0);
        recvEntity_ = nullptr;
    }
    SM_LOG_INFO("smem trans entry uninitialized, url: " << name_ << " rpcPort: " << rpcPort_);
}

Result SmemTransEntry::CreateConnectionStore(const std::string &storeUrl, StorePtr &store)
{
    StorePtr confStore = StoreFactory::CreateStoreByUrl(storeUrl, ConfigStoreModel::CSM_CLIENT, UINT32_MAX, -1,
                                                        static_cast<int32_t>(config_.initTimeout));
    if (confStore == nullptr) {
        SM_LOG_ERROR("CreateStoreByUrl failed, storeUrl: " << storeUrl << " initTimeout: " << config_.initTimeout
                                                           << " reason: " << StoreFactory::GetFailedReason());
        return SM_ERROR;
    }
    store = StoreFactory::PrefixStore(confStore, "TRANS_");
    return SM_OK;
}

Result SmemTransEntry::ApplyAutoRanking(const StorePtr &store, uint32_t &rankId)
{
    std::vector<uint8_t> rankIdData;
    auto ret = store->GetCoreStore()->Get(AutoRankingStr, rankIdData, SMEM_DEFAUT_WAIT_TIME * SECOND_TO_MILLSEC);
    if (ret == SM_OK && rankIdData.size() == sizeof(uint32_t)) {
        union Transfer {
            uint32_t rankId;
            uint8_t data[4];
        } trans{};
        std::copy_n(rankIdData.begin(), sizeof(trans.data), trans.data);
        rankId = trans.rankId;
        auto configStoreManager = Convert<ConfigStore, ConfigStoreManager>(store);
        configStoreManager->SetRankId(static_cast<int32_t>(trans.rankId));
        SM_LOG_TRACE("Success to auto ranking rankId: " << trans.rankId);
        return SM_OK;
    }
    SM_LOG_ERROR("Failed to auto ranking, ret: " << ret << ", dataSize: " << rankIdData.size());
    return SM_ERROR;
}

SmemTransConnectionPtr SmemTransEntry::CreateAndJoinConnection(const std::string &uniqueId, bool isSelf)
{
    SM_LOG_INFO("connection start to " << uniqueId << (isSelf ? " (self)" : ""));
    const std::string storeUrl = std::string("tcp://") + uniqueId;
    StorePtr store;
    auto ret = CreateConnectionStore(storeUrl, store);
    if (ret != SM_OK) {
        SM_LOG_ERROR("create connection store failed, uniqueId: " << uniqueId << " storeUrl: " << storeUrl);
        return nullptr;
    }

    uint32_t rankId = 0;
    ret = ApplyAutoRanking(store, rankId);
    if (ret != SM_OK) {
        SM_LOG_ERROR("auto ranking failed, uniqueId: " << uniqueId << " ret: " << ret);
        return nullptr;
    }

    // 主动建链绑定 send entity，self connection 绑定 recv entity；
    // override rank 窗口按对端（组内成员）在 connection 内部经分配回调惰性分配，
    // 同一 uniqueId 在两侧 entity 中复用同一个窗口（entry 统一映射表）
    auto rankAlloc = [this](const std::string &uid, uint32_t &rank) { return AllocOverrideRank(uid, rank); };
    // 仅 self connection 注入释放回调：本组内对端离开（RemoveOnePeer）时归还引用；
    // 主动建链的引用由 entry 在从 connections_ 移除该 uniqueId 记录时归还
    RankReleaseCb rankRelease;
    if (isSelf) {
        rankRelease = [this](const std::string &uid) { ReleaseOverrideRank(uid); };
    }
    // uniqueId 即该连接唯一的对端 name，传入连接以便建链时解析并缓存 name -> WorkerId
    auto conn = SmMakeRef<SmemTransConnection>(config_, name_, uniqueId, rankId, isSelf ? recvEntity_ : sendEntity_,
                                               store, workerUniqueId_, std::move(rankAlloc), std::move(rankRelease));
    ret = conn->Initialize();
    if (ret != SM_OK) {
        SM_LOG_ERROR("connection initialize failed, uniqueId: " << uniqueId << " rankId: " << rankId
                                                                << " ret: " << ret);
        return nullptr;
    }
    // Join 携带该 entity 全量已注册 slice 的序列化记录（externalInfo），随成员信息登记到目标组的 server，
    // 供既有成员与晚加入者交换；后续新增 slice 经 self connection 的 ExtendMemory 广播
    MultiBytes externalSlices;
    {
        std::lock_guard<std::mutex> lock(memMutex_);
        externalSlices = isSelf ? recvExportedSlices_ : sendExportedSlices_;
    }
    // self connection 声明为 PROVIDER，其余连接声明为 PROXY，供 group manager 剪枝 PROXY<->PROXY 链路
    ret = conn->Join(0, isSelf ? recvEntityInfo_ : sendEntityInfo_, externalSlices,
                     isSelf ? SMEM_GROUP_RANK_ROLE_PROVIDER : SMEM_GROUP_RANK_ROLE_PROXY);
    if (ret != SM_OK) {
        SM_LOG_ERROR("connection join failed, uniqueId: " << uniqueId << " rankId: " << rankId << " ret: " << ret);
        return nullptr;
    }
    if (!isSelf) {
        // 断链处理：store 客户端链路断开时移除该 uniqueId 的连接记录；
        // 捕获存活令牌而非依赖 entry 存活（store 生命周期长于 entry）
        auto alive = brokenHandlerAlive_;
        StoreManagerPtr manager = Convert<ConfigStore, ConfigStoreManager>(store);
        if (manager == nullptr) {
            SM_LOG_ERROR("convert store to ConfigStoreManager failed, uniqueId: " << uniqueId);
            return nullptr;
        }
        manager->RegisterClientBrokenHandler([this, alive, uniqueId]() -> int {
            if (!alive->load()) {
                return 0;
            }
            OnStoreLinkBroken(uniqueId);
            return 0;
        });
    }
    SM_LOG_INFO("connection success to " << uniqueId << ", rankId: " << rankId);
    return conn;
}

void SmemTransEntry::OnStoreLinkBroken(const std::string &uniqueId)
{
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        auto it = connections_.find(uniqueId);
        if (it == connections_.end()) {
            return;
        }
        // 连接引用在此释放；若仍有传输任务持有 SmRef，连接随最后一个引用析构
        connections_.erase(it);
    }
    // 主动建链的引用随 connections_ 记录移除而归还（计数减一，归零后 rank 可复用）
    ReleaseOverrideRank(uniqueId);
    SM_LOG_INFO("store link broken, removed connection record, uniqueId: " << uniqueId);
}

SmemTransConnectionPtr SmemTransEntry::GetOrCreateConnection(const std::string &uniqueId)
{
    if (stopping_.load()) {
        SM_LOG_ERROR("entry is stopping, reject new connection, uniqueId: " << uniqueId);
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(connMutex_);

    // 指向本地 store 的传输走 self connection，避免对同一 store 重复建链加组；
    // 在 connMutex_ 内取副本，避免与 UnInitialize 摘除 selfConnection_ 竞态
    if (uniqueId == name_) {
        return selfConnection_;
    }

    while (pendingConnections_.count(uniqueId) != 0) {
        connCv_.wait(lock);
    }
    auto it = connections_.find(uniqueId);
    if (it != connections_.end()) {
        return it->second;
    }

    pendingConnections_.insert(uniqueId);
    lock.unlock();

    auto conn = CreateAndJoinConnection(uniqueId, false);

    lock.lock();
    pendingConnections_.erase(uniqueId);
    connCv_.notify_all();
    if (conn != nullptr) {
        connections_[uniqueId] = conn;
        SM_LOG_INFO("lazy connected to " << uniqueId << " (total connections: " << connections_.size() << ")");
    }
    return conn;
}

Result SmemTransEntry::RegisterLocalMemory(const void *address, uint64_t size, uint32_t flags)
{
    std::vector<std::pair<const void *, size_t>> regMemories;
    regMemories.emplace_back(address, size);
    return RegisterLocalMemories(regMemories, flags);
}

void SmemTransEntry::AlignMemory(const void *&address, uint64_t &size)
{
    constexpr auto NPU_PAGE_SIZE = 2UL * 1024UL * 1024UL;
    constexpr auto NPU_PAGE_MASK = ~(NPU_PAGE_SIZE - 1UL);

    auto pointer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
    auto alignPtr = (pointer & NPU_PAGE_MASK);
    auto diff = pointer - alignPtr;
    size += diff;
    size = ((size + NPU_PAGE_SIZE - 1) & NPU_PAGE_MASK);
    address = reinterpret_cast<const void *>(alignPtr);
}

std::vector<std::pair<const void *, size_t>>
SmemTransEntry::CombineMemories(std::vector<std::pair<const void *, size_t>> &input)
{
    std::sort(input.begin(), input.end());
    std::vector<std::pair<const void *, size_t>> result;
    auto current = input[0];
    for (auto i = 1U; i < input.size(); i++) {
        // Merge only on real overlap (end > next.start); exact adjacency (end == next.start) is NOT
        // merged, to avoid cross-heap merging whose size exceeds a single heap's capacity and gets
        if ((const uint8_t *)current.first + current.second > (const uint8_t *)input[i].first) {
            ptrdiff_t diff = ((const uint8_t *)input[i].first - (const uint8_t *)current.first);
            if (static_cast<size_t>(diff) > std::numeric_limits<size_t>::max() - input[i].second) {
                result.emplace_back(current);
                current = input[i];
                continue;
            }
            current.second = std::max(current.second, diff + input[i].second);
        } else {
            result.emplace_back(current);
            current = input[i];
        }
    }
    result.emplace_back(current);
    return result;
}

Result SmemTransEntry::RegisterLocalMemories(const std::vector<std::pair<const void *, size_t>> &regMemories,
                                             uint32_t flags)
{
    if (regMemories.empty()) {
        return SM_OK;
    }
    if (regMemories.size() > TRANS_MAX_BATCH_COUNT) {
        SM_LOG_ERROR("register memories count exceeds limit, count: " << regMemories.size());
        return SM_INVALID_PARAM;
    }
    if (sendEntity_ == nullptr || recvEntity_ == nullptr) {
        SM_LOG_ERROR("entities not created, url: " << name_);
        return SM_ERROR;
    }
    for (const auto &m : regMemories) {
        if (m.first == nullptr || m.second == 0) {
            SM_LOG_ERROR("invalid address or size, address: " << m.first << " size: " << m.second);
            return SM_INVALID_PARAM;
        }
    }

    // register_mem 直接透传给共享 hybm entity，smem 层不缓存内存快照：
    // 每片 slice 生成交换记录后立即 ExtendMemory 到 self connection（server 累积并向组内广播），
    // 之后创建的连接在 Join 时以 externalInfo 携带全量 slice 记录
    auto alignedMemories = regMemories;
    for (auto &m : alignedMemories) {
        AlignMemory(m.first, m.second);
    }
    auto mergedMemories = CombineMemories(alignedMemories);

    std::lock_guard<std::mutex> lock(memMutex_);
    for (const auto &m : mergedMemories) {
        auto ret = RegisterOneMemory(m.first, m.second, flags);
        if (ret != SM_OK) {
            return ret;
        }
    }
    return SM_OK;
}

// 查询 slice 的 GVA：自拷贝路径（uniqueId 为自身）依赖 self connection 的 sliceVa -> gva 映射变换地址；
// 查询失败仅降级该 slice 的自拷贝能力，不影响注册与对端广播
static uint64_t QuerySliceGva(hybm_entity_t entity, hybm_mem_slice_t slice)
{
    auto gva = reinterpret_cast<uint64_t>(hybm_get_slice_va(entity, slice, HYBM_GET_SLICE_GVA));
    if (gva == 0) {
        SM_LOG_WARN("query slice gva failed, self copy unavailable, slice: " << slice);
    }
    return gva;
}

// 在单个 entity 上注册一片内存并导出交换记录；区间内重复注册由 segment 层 fake 掉
// （返回首次注册的同一 slice ID），此处按 ID 去重：已 export 过的 slice 不再生成记录，
// 避免对端出现 "整段 handle + 子块 u.address" 的错配记录
Result SmemTransEntry::RegisterAndExportSlice(hybm_entity_t entity, std::unordered_set<uint64_t> &sliceIds,
                                              const void *address, uint64_t size, SliceRecord &record)
{
    auto slice = hybm_register_local_memory(entity, address, size, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("hybm_register_local_memory failed, address: " << address << " size: " << size);
        return SM_ERROR;
    }
    record.slice = slice;
    const auto sliceKey = reinterpret_cast<uint64_t>(slice);
    if (!sliceIds.insert(sliceKey).second) {
        SM_LOG_INFO("fake register, skip duplicated export: address: " << address << " size: " << size);
        return SM_OK;
    }
    // fake/区间扩展后 slice 登记的范围可能与本次调用入参不同，
    // u.address 必须用 slice 的真实范围，保证与广播的 handle 覆盖一致，
    // 否则对端 TransformAddr 会因记录范围不含目标地址而报 out of range
    record.sliceVa = hybm_get_slice_va(entity, slice, 0);
    record.sliceSize = hybm_get_slice_size(entity, slice);
    if (record.sliceVa == nullptr || record.sliceSize == 0) {
        SM_LOG_ERROR("query slice range failed, address: " << address << " size: " << size);
        hybm_free_local_memory(entity, slice, size, 0);
        sliceIds.erase(sliceKey);
        return SM_ERROR;
    }
    record.gva = QuerySliceGva(entity, slice);

    SmemTransExchangeInfo info{};
    auto ret = hybm_export(entity, slice, 0, &info.hybmInfo);
    if (ret != 0) {
        SM_LOG_ERROR("export slice failed, address: " << address << " size: " << size << " ret: " << ret);
        hybm_free_local_memory(entity, slice, size, 0);
        sliceIds.erase(sliceKey);
        hybm_export_info_free(&info.hybmInfo);
        return SM_ERROR;
    }
    info.u.address = LocalMapAddress(record.sliceVa, record.sliceSize);
    record.serialized.emplace_back();
    SerializeTransExchangeInfo(record.serialized.back(), info);
    hybm_export_info_free(&info.hybmInfo);
    return SM_OK;
}

Result SmemTransEntry::RegisterOneMemory(const void *address, uint64_t size, uint32_t flags)
{
    (void)flags; // reserved, not passed to hybm yet
    // 所有注册同时落在 send/recv 两个 entity 上：send 记录累积供主动建链 Join 携带，
    // recv 记录立即 ExtendMemory 到 self connection（server 登记并向组内广播）
    SliceRecord sendRecord;
    auto ret = RegisterAndExportSlice(sendEntity_, sendSliceIds_, address, size, sendRecord);
    if (ret != SM_OK) {
        return ret;
    }
    if (sendRecord.serialized.empty()) {
        return SM_OK; // fake 重复注册，两侧此前均已记录
    }

    SliceRecord recvRecord;
    ret = RegisterAndExportSlice(recvEntity_, recvSliceIds_, address, size, recvRecord);
    if (ret != SM_OK || recvRecord.serialized.empty()) {
        // recv 侧失败或状态异常，回滚 send 侧注册与去重登记，允许同区间注册重试
        hybm_free_local_memory(sendEntity_, sendRecord.slice, size, 0);
        sendSliceIds_.erase(reinterpret_cast<uint64_t>(sendRecord.slice));
        if (ret == SM_OK) {
            SM_LOG_ERROR("recv slice export missing after send export, address: " << address << " size: " << size);
            return SM_ERROR;
        }
        return ret;
    }
    sendExportedSlices_.push_back(sendRecord.serialized.back());

    // 在 connMutex_ 内取 self connection 副本：避免与 UnInitialize 摘除 selfConnection_ 竞态，
    // 副本的引用计数保证广播期间连接不被回收
    SmemTransConnectionPtr selfConn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        selfConn = selfConnection_;
    }
    if (selfConn == nullptr) {
        SM_LOG_ERROR("self connection missing, can not extend memory, address: " << address
                                                                                 << " size: " << recvRecord.sliceSize);
        sendSliceIds_.erase(reinterpret_cast<uint64_t>(sendRecord.slice));
        recvSliceIds_.erase(reinterpret_cast<uint64_t>(recvRecord.slice));
        return SM_ERROR;
    }
    // 生成交换记录后立即 ExtendMemory 到 self connection：server 登记该 slice 并向组内
    // 其他成员广播 ADD_SLICES；同时将 sliceVa -> gva 记入 self connection 的 remoteSlices_，
    // 供 uniqueId 为自身的传输做地址变换；失败时回滚两侧去重登记（保留注册），允许重试
    ret = selfConn->BroadcastSlices(recvRecord.serialized, recvRecord.sliceVa, recvRecord.sliceSize, recvRecord.gva);
    if (ret != SM_OK) {
        SM_LOG_ERROR("extend memory on self connection failed, address: " << address << " size: "
                                                                          << recvRecord.sliceSize << " ret: " << ret);
        sendSliceIds_.erase(reinterpret_cast<uint64_t>(sendRecord.slice));
        recvSliceIds_.erase(reinterpret_cast<uint64_t>(recvRecord.slice));
        return ret;
    }
    recvExportedSlices_.push_back(recvRecord.serialized.back());
    return SM_OK;
}

Result SmemTransEntry::UnregisterLocalMemory(const void *address)
{
    SM_VALIDATE_RETURN(address != nullptr, "invalid address, which is null", SM_INVALID_PARAM);
    // smem 层不再缓存注册快照，且 hybm 未提供按地址反注册的 C 接口（需 slice 句柄），
    // deregister 仅保留 API 兼容；内存释放依赖 hybm entity 销毁时的统一回收
    return SM_OK;
}

Result SmemTransEntry::SyncTransfer(void *localAddr, const std::string &remoteUniqueId, void *remoteAddr,
                                    size_t dataSize, smem_bm_copy_type opcode, void *stream, uint32_t flags)
{
    return BatchSyncTransfer(&localAddr, remoteUniqueId, &remoteAddr, &dataSize, 1U, opcode, stream, flags);
}

Result SmemTransEntry::BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId, void *remoteAddrs[],
                                         const size_t dataSizes[], uint32_t batchSize, smem_bm_copy_type opcode,
                                         void *stream, uint32_t flags)
{
    if (stopping_.load()) {
        SM_LOG_ERROR("entry is stopping, reject transfer, uniqueId: " << remoteUniqueId);
        return SM_ERROR;
    }
    auto conn = GetOrCreateConnection(remoteUniqueId);
    if (conn == nullptr) {
        SM_LOG_ERROR("get or create connection failed, uniqueId: " << remoteUniqueId);
        return SM_ERROR;
    }
    return conn->BatchSyncTransfer(localAddrs, remoteUniqueId, remoteAddrs, dataSizes, batchSize, opcode, stream,
                                   flags);
}

Result SmemTransEntry::ValidateBatchParams(const smem_trans_batch_params_t &param, uint64_t &batchSize)
{
    SM_VALIDATE_RETURN(param.destUrl != nullptr, "invalid destUrl, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(strlen(param.destUrl) != 0, "invalid destUrl, which is empty", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param.srcList != nullptr, "invalid srcList, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param.destList != nullptr, "invalid destList, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param.sizeList != nullptr, "invalid sizeList, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param.batchSize != 0, "invalid batchSize, which is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param.opcode == SMEM_TRANS_OP_READ || param.opcode == SMEM_TRANS_OP_WRITE,
                       "invalid opcode: " << param.opcode, SM_INVALID_PARAM);
    for (auto i = 0U; i < param.batchSize; i++) {
        SM_VALIDATE_RETURN(param.srcList[i] != nullptr, "srcList has null item " << i, SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(param.destList[i] != nullptr, "destList has null item " << i, SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(param.sizeList[i] != 0, "sizeList has zero item " << i, SM_INVALID_PARAM);
    }
    batchSize = param.batchSize;
    return SM_OK;
}

Result SmemTransEntry::SubmitBatchCopy(const smem_trans_batch_params_t &param, uint64_t &batchId)
{
    batchId = 0;
    uint64_t batchSize = 0;
    auto ret = ValidateBatchParams(param, batchSize);
    if (ret != SM_OK) {
        return ret;
    }

    if (stopping_.load()) {
        SM_LOG_ERROR("entry is stopping, reject batch copy, destUrl: " << param.destUrl);
        return SM_ERROR;
    }

    // 建链（可能阻塞等待 join）在提交线程完成，执行线程直接复用传入的连接；
    // SmRef 捕获保证任务执行期间连接不被回收
    auto conn = GetOrCreateConnection(param.destUrl);
    if (conn == nullptr) {
        SM_LOG_ERROR("get or create connection failed, destUrl: " << param.destUrl);
        return SM_ERROR;
    }

    auto task = std::make_shared<BatchTask>();
    task->destUrl = param.destUrl;
    task->srcList.assign(param.srcList, param.srcList + batchSize);
    task->destList.assign(param.destList, param.destList + batchSize);
    task->sizeList.assign(param.sizeList, param.sizeList + batchSize);
    task->opcode = param.opcode;
    task->stream = param.stream;
    task->flags = 0; // param.flags is reserved and not passed down yet

    std::lock_guard<std::mutex> lock(batchMutex_);
    auto id = ++batchIdGen_;
    batchStatusMap_[id] = BatchState{};
    if (!executor_->Execute([this, id, task, conn]() { ExecuteBatchTask(id, task, conn); })) {
        batchStatusMap_.erase(id);
        SM_LOG_ERROR("submit batch copy task failed, queue full, batchId: " << id << " destUrl: " << task->destUrl);
        return SM_ERROR;
    }
    batchId = id;
    return SM_OK;
}

void SmemTransEntry::ExecuteBatchTask(uint64_t batchId, const std::shared_ptr<BatchTask> &task,
                                      const SmemTransConnectionPtr &conn)
{
    const auto copyType = (task->opcode == SMEM_TRANS_OP_WRITE) ? SMEMB_COPY_L2G : SMEMB_COPY_G2L;
    auto ret =
        conn->BatchSyncTransfer(task->srcList.data(), task->destUrl, task->destList.data(), task->sizeList.data(),
                                static_cast<uint32_t>(task->srcList.size()), copyType, task->stream, task->flags);
    if (ret != SM_OK) {
        SM_LOG_ERROR("batch copy failed, batchId: " << batchId << " destUrl: " << task->destUrl << " ret: " << ret);
    }
    FinishBatchTask(batchId, ret);
}

void SmemTransEntry::FinishBatchTask(uint64_t batchId, int32_t ret)
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    auto it = batchStatusMap_.find(batchId);
    if (it == batchStatusMap_.end()) {
        return;
    }
    it->second.done = true;
    it->second.ret = ret;
}

int32_t SmemTransEntry::QueryBatchStatus(uint64_t batchId, int32_t *result, uint32_t flags)
{
    (void)flags;
    std::lock_guard<std::mutex> lock(batchMutex_);
    auto it = batchStatusMap_.find(batchId);
    if (it == batchStatusMap_.end()) {
        SM_LOG_ERROR("batch task not found, batchId: " << batchId);
        return SM_INVALID_PARAM;
    }
    if (!it->second.done) {
        return SM_NEED_RETRY;
    }
    *result = it->second.ret;
    batchStatusMap_.erase(it);
    return SM_OK;
}

} // namespace smem
} // namespace ock
