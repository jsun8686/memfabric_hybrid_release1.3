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
#include "smem_trans_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "mf_syntactic_sugar.h"
#include "hybm.h"
#include "hybm_big_mem.h"
#include "hybm_data_op.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "smem_net_common.h"
#include "smem_store_factory.h"
#include "smem_trans_def.h"
#include "smem_tcp_config_store.h"
#include "smem_group_manager_client.h"

namespace ock {
namespace smem {
// LinkState wire values (keep in sync with LinkState in smem_group_manager_server.h)
constexpr int8_t TRANS_LINK_IDLE = 0;
constexpr int8_t TRANS_LINK_CONNECTED = 4;

SmemTransConnection::~SmemTransConnection()
{
    UnInitialize();
}

int32_t SmemTransConnection::Initialize()
{
    SM_VALIDATE_RETURN(rankId_ < SMEM_TRANS_RANK_COUNT_MAX, "rankId:" << rankId_ << " is too large.", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(entity_ != nullptr, "hybm entity is nullptr, name: " << name_, SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(rankAlloc_ != nullptr, "rank allocator missing, name: " << name_, SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(workerUniqueId_.port != 0, "local worker port is 0, name: " << name_, SM_INVALID_PARAM);

    auto coreStore = store_->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("Initialize failed, tcp store is nullptr, name: " << name_ << " rankId: " << rankId_);
        return SM_ERROR;
    }

    if (auto ret = SetupGroupManagerCallbacks(); ret != SMEM_OK) {
        SM_LOG_ERROR("Initialize failed to setup callbacks, ret: " << ret << " name: " << name_
                                                                   << " rankId: " << rankId_);
        return SM_ERROR;
    }
    return SM_OK;
}

void SmemTransConnection::UnInitialize()
{
    // Perform a graceful group leave so that peer ranks can synchronously
    // clean up their imported state via OnRemoveFromWhitelist.
    if (joined_.load()) {
        auto coreStore = store_->GetCoreStore();
        auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
        if (groupMgr != nullptr) {
            groupMgr->Leave();
        }
        joined_.store(false);
    }

    ClearStoreCallbacks();

    {
        mf::WriteGuard locker(remoteSliceRwMutex_);
        remoteSlices_.clear();
        rankToWorkerId_.clear();
        ranksRole_.clear();
    }

    // 每个对端独占一个 override rank 窗口，逐窗口回收；
    // entity_ 为 entry 所有（send/recv 二选一注入），不在此销毁
    ReleaseAllPeerRanks();
}

uint32_t SmemTransConnection::GetOrCreatePeerRank(uint32_t groupRankId, const std::string &uniqueId) noexcept
{
    std::lock_guard<std::mutex> lock(rankMutex_);
    auto it = peerOverrideRanks_.find(groupRankId);
    if (it != peerOverrideRanks_.end()) {
        return it->second.rank;
    }
    uint32_t rank = HYBM_IMPORT_RANK_INVALID;
    if (rankAlloc_(uniqueId, rank) != SM_OK || rank == HYBM_IMPORT_RANK_INVALID) {
        // 根因（窗口耗尽等）已在 entry 的 AllocOverrideRank 内记录
        SM_LOG_ERROR("alloc override rank failed, name: " << name_ << " peer rankId: " << groupRankId
                                                          << " uniqueId: " << uniqueId);
        return HYBM_IMPORT_RANK_INVALID;
    }
    PeerWindow window;
    window.rank = rank;
    window.uniqueId = uniqueId;
    peerOverrideRanks_.emplace(groupRankId, std::move(window));
    return rank;
}

uint32_t SmemTransConnection::AllocPeerOverrideRank(const RankFullInfo &peer) noexcept
{
    // 从对端 baseInfo 中轻量解析 session（WorkerUniqueId），转换为 uniqueId 传给 entry 级分配器，
    // 以便同一 uniqueId 重新建链时复用已分配的 override rank 窗口（无需完整反序列化/分配 desc）
    std::string uniqueId;
    if (!ExtractPeerUniqueId(peer, uniqueId)) {
        // baseInfo 异常时回退到 rank 维度的 key，保证建链（仅需 override rank 窗口）不受影响；
        // 完整对端导入在 ImportPeerInfo 中会按真正的错误路径处理
        SM_LOG_WARN("extract peer uniqueId failed, fallback to rank-based key, peer rankId: " << peer.rankId
                                                                                              << " name: " << name_);
        uniqueId = name_ + "#rank:" + std::to_string(peer.rankId);
    }
    return GetOrCreatePeerRank(peer.rankId, uniqueId);
}

bool SmemTransConnection::ExtractPeerUniqueId(const RankFullInfo &peer, std::string &uniqueId) const noexcept
{
    const size_t uSize = sizeof(SmemTransExchangeInfo::U);
    if (peer.baseInfo.size() < uSize) {
        return false;
    }
    SmemTransExchangeInfo::U u{};
    std::copy_n(peer.baseInfo.data(), uSize, reinterpret_cast<uint8_t *>(&u));
    u.session.reserved = 0U; // reserved 携带 role，需清零以匹配 ParseNameToUniqueId 的 WorkerId 约定
    WorkerIdUnion wu(u.session);
    uniqueId = WorkerIdToUniqueId(wu.workerId);
    return true;
}

uint32_t SmemTransConnection::FindPeerRank(uint32_t groupRankId) noexcept
{
    std::lock_guard<std::mutex> lock(rankMutex_);
    const auto it = peerOverrideRanks_.find(groupRankId);
    return (it == peerOverrideRanks_.end()) ? HYBM_IMPORT_RANK_INVALID : it->second.rank;
}

void SmemTransConnection::ReleaseAllPeerRanks() noexcept
{
    std::vector<PeerWindow> windows;
    {
        std::lock_guard<std::mutex> lock(rankMutex_);
        windows.reserve(peerOverrideRanks_.size());
        for (auto &kv : peerOverrideRanks_) {
            windows.push_back(std::move(kv.second));
        }
        peerOverrideRanks_.clear();
    }
    for (const auto &window : windows) {
        RemoveImportedWindow(window.rank);
    }
}

void SmemTransConnection::RemoveImportedWindow(uint32_t overrideRank) noexcept
{
    if (overrideRank == HYBM_IMPORT_RANK_INVALID) {
        // 无效窗口（分配失败/未建窗），无需回收
        return;
    }
    if (entity_ != nullptr) {
        auto ret = hybm_remove_imported(entity_, overrideRank, 0);
        if (ret != SM_OK) {
            SM_LOG_WARN("remove imported override rank failed, rank: " << overrideRank << " ret: " << ret);
        }
    }
}

void SmemTransConnection::ClearStoreCallbacks() noexcept
{
    // The store object stays cached in the factory map until uninit, so its
    // dispatcher must not keep lambdas capturing this destroyed connection.
    auto coreStore = store_->GetCoreStore();
    auto *tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        return;
    }
    auto *asyncMgr = tcpStore->GetAsyncDispatcher();
    if (asyncMgr != nullptr) {
        asyncMgr->SetWhitelistCallback(nullptr);
        asyncMgr->SetRemoveWhitelistCallback(nullptr);
        asyncMgr->SetConnectionCallback(nullptr);
        asyncMgr->SetLinkStateQueryCallback(nullptr);
        asyncMgr->SetAddSlicesCallback(nullptr);
    }
    auto *groupMgr = static_cast<SmemGroupManager *>(tcpStore);
    groupMgr->SetExecutor(SmemGroupManagerClientPtr());
}

int SmemTransConnection::SetupGroupManagerCallbacks() noexcept
{
    auto coreStore = store_->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("SetupGroupManagerCallbacks failed, tcp store is nullptr, name: " << name_);
        return SM_ERROR;
    }

    auto *asyncMgr = tcpStore->GetAsyncDispatcher();
    if (asyncMgr == nullptr) {
        SM_LOG_ERROR("SetupGroupManagerCallbacks: async dispatcher is null, name: " << name_);
        return SM_ERROR;
    }

    if (auto ret = RegisterAsyncCallbacks(asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto executor = SmMakeRef<SmemGroupManagerClient>();
    if (auto ret = RegisterExecutorCallbacks(executor.Get(), asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto *groupMgr = static_cast<SmemGroupManager *>(tcpStore);
    groupMgr->SetExecutor(executor.Get());
    SM_LOG_INFO("SmemTransConnection callbacks setup, rank " << groupMgr->GetLocalRankId() << " name: " << name_);
    return SMEM_OK;
}

int SmemTransConnection::RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    asyncMgr->SetWhitelistCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        return OnAddToWhitelist(rankId, others, reqId);
    });
    asyncMgr->SetRemoveWhitelistCallback(
        [this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
            return OnRemoveFromWhitelist(rankId, others, reqId);
        });
    asyncMgr->SetConnectionCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) {
        return OnEstablishConnection(rankId, peers, reqId);
    });
    asyncMgr->SetAddSlicesCallback([this](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        return OnAddSlices(extendingRankId, newSlices, reqId);
    });
    asyncMgr->SetLinkStateQueryCallback([this]() { return OnQueryLinkState(); });
    return SMEM_OK;
}

int SmemTransConnection::RegisterExecutorCallbacks(SmemGroupManagerClient *executor,
                                                   SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    executor->onAddToWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueAddToWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onRemoveFromWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueRemoveFromWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onEstablishConnection_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &peers,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(peers);
        asyncMgr->EnqueueEstablishConnection(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onQueryLinkState_ = [asyncMgr]() {
        asyncMgr->EnqueueQueryLinkState(0);
        return std::vector<LinkStateEntry>{};
    };
    executor->onAddSlices_ = [asyncMgr](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        MultiBytes copy(newSlices);
        asyncMgr->EnqueueAddSlices(extendingRankId, std::move(copy), reqId);
        return 0;
    };
    executor->onPromoteToActive_ = [this](uint32_t rankId) {
        joined_.store(true);
        SM_LOG_INFO("SmemTransConnection::onPromoteToActive rankId=" << rankId << " name: " << name_);
        try {
            joinComplete_->set_value();
        } catch (const std::future_error &e) {
            SM_LOG_WARN("onPromoteToActive: promise already satisfied: " << e.what());
        }
        return 0;
    };
    return SMEM_OK;
}

// SerializeTransExchangeInfo lives in smem_trans_connection.h (inline, shared with the entry)

static size_t DeserializeTransExchangeInfo(const uint8_t *buf, size_t bufLen, SmemTransExchangeInfo &info) noexcept
{
    constexpr size_t lenFieldSize = sizeof(uint32_t);
    const size_t uSize = sizeof(SmemTransExchangeInfo::U);
    const size_t headerSize = uSize + lenFieldSize;
    if (buf == nullptr || bufLen < headerSize) {
        return 0;
    }
    uint32_t descLen = 0;
    std::copy_n(buf + uSize, lenFieldSize, reinterpret_cast<uint8_t *>(&descLen));
    const size_t need = headerSize + descLen;
    if (bufLen < need) {
        return 0;
    }
    SmemTransExchangeInfo parsed{};
    std::copy_n(buf, uSize, reinterpret_cast<uint8_t *>(&parsed.u));
    parsed.hybmInfo.descLen = descLen;
    if (descLen > 0) {
        parsed.hybmInfo.desc = new (std::nothrow) uint8_t[descLen];
        if (parsed.hybmInfo.desc == nullptr) {
            return 0;
        }
        std::copy_n(buf + headerSize, descLen, parsed.hybmInfo.desc);
    }
    hybm_export_info_free(&info.hybmInfo);
    info = parsed;
    return need;
}

int SmemTransConnection::OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others,
                                          uint64_t reqId) noexcept
{
    if (others.empty()) {
        SM_LOG_DEBUG("OnAddToWhitelist rankId=" << rankId << " others empty, skip");
        return SMEM_OK;
    }

    for (auto &peer : others) {
        int ret = ImportPeerInfo(peer);
        SM_VALIDATE_RETURN(ret == SM_OK, "import peer info failed, ret:" << ret, ret);
    }
    SM_LOG_DEBUG("OnAddToWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SMEM_OK;
}

int SmemTransConnection::OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others,
                                               uint64_t reqId) noexcept
{
    for (const auto &other : others) {
        if (other.rankId != 0 && rankId_ != 0) {
            continue;
        }
        RemoveOnePeer(other.rankId);
    }
    SM_LOG_DEBUG("OnRemoveFromWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SM_OK;
}

void SmemTransConnection::RemoveOnePeer(uint32_t leavingRankId) noexcept
{
    // 该对端独占一个 override rank 窗口，整窗回收不影响组内其他成员的导入；
    // self connection 同时归还 entry 统一映射表的 override rank 引用（计数减一，归零后 rank 可复用）
    PeerWindow window;
    {
        std::lock_guard<std::mutex> lock(rankMutex_);
        auto it = peerOverrideRanks_.find(leavingRankId);
        if (it != peerOverrideRanks_.end()) {
            window = std::move(it->second);
            peerOverrideRanks_.erase(it);
        }
    }
    if (window.rank != HYBM_IMPORT_RANK_INVALID) {
        RemoveImportedWindow(window.rank);
        if (rankRelease_ != nullptr) {
            rankRelease_(window.uniqueId);
        }
    }
    mf::WriteGuard locker(remoteSliceRwMutex_);
    auto it = rankToWorkerId_.find(leavingRankId);
    if (it == rankToWorkerId_.end()) {
        return;
    }
    remoteSlices_.erase(it->second);
    rankToWorkerId_.erase(it);
    ranksRole_.erase(leavingRankId);
}

std::string SmemTransConnection::WorkerIdToUniqueId(const WorkerId &id) const noexcept
{
    WorkerIdUnion workerId{id};
    const WorkerUniqueId &w = workerId.session;

    char ipBuf[INET6_ADDRSTRLEN] = {0};
    if (w.address.type == ock::mf::IpV4) {
        struct in_addr addr;
        addr.s_addr = htonl(w.address.ip.ipv4.s_addr);
        inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));
    } else if (w.address.type == ock::mf::IpV6) {
        inet_ntop(AF_INET6, &w.address.ip.ipv6, ipBuf, sizeof(ipBuf));
    }
    return std::string(ipBuf) + ":" + std::to_string(w.port);
}

int SmemTransConnection::OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers,
                                               uint64_t reqId) noexcept
{
    if (peers.empty()) {
        SM_LOG_DEBUG("OnEstablishConnection rankId=" << rankId << " peers empty, skip");
        return SMEM_OK;
    }

    auto ret = hybm_mmap(entity_, 0);
    SM_VALIDATE_RETURN(ret == SM_OK, "hybm mmap failed, ret:" << ret, ret);
    // 每个对端独立 override rank 窗口，transport 按各对端的覆盖窗口建连（ConnectRank 幂等，
    // 重复 ESTABLISH 无副作用）
    std::vector<uint32_t> overrideRanks;
    for (const auto &p : peers) {
        if (p.rankId != 0 && rankId_ != 0) {
            continue;
        }
        auto overrideRank = AllocPeerOverrideRank(p);
        if (overrideRank == HYBM_IMPORT_RANK_INVALID) {
            SM_LOG_ERROR("OnEstablishConnection rankId=" << rankId
                                                         << " alloc override rank failed, peer rankId=" << p.rankId);
            return SM_ERROR;
        }
        overrideRanks.push_back(overrideRank);
    }
    if (overrideRanks.empty()) {
        return SMEM_OK;
    }

    ret = hybm_transport_connect(entity_, overrideRanks.data(), overrideRanks.size(), 0);
    if (ret != SMEM_OK) {
        SM_LOG_ERROR("OnEstablishConnection rankId=" << rankId << " hybm_transport_connect failed: " << ret);
        return ret;
    }

    SM_LOG_INFO("OnEstablishConnection rankId=" << rankId << " peers.size=" << peers.size());
    return SM_OK;
}

int SmemTransConnection::ImportPeerInfo(const RankFullInfo &peer) noexcept
{
    if (peer.rankId != 0 && rankId_ != 0) {
        return SMEM_OK;
    }
    const auto overrideRank = AllocPeerOverrideRank(peer);
    if (overrideRank == HYBM_IMPORT_RANK_INVALID) {
        SM_LOG_ERROR("import entity failed: alloc override rank failed, peer rankId: " << peer.rankId);
        return SMEM_ERROR;
    }
    // Parse the peer session from baseInfo so the WorkerId key matches
    // ParseNameToUniqueId (same convention as OnAddToWhitelist).
    SmemTransExchangeInfo entityInfo{};
    smem_trans_role_t role = SMEM_TRANS_NONE;
    WorkerId id;
    SM_VALIDATE_RETURN(ParsePeerEntity(peer, entityInfo, role, id), "parse failed!", SM_INVALID_PARAM);
    auto ret = hybm_import(entity_, &entityInfo.hybmInfo, 1, nullptr,
                           HYBM_FLAG_EXPORT_ENTITY | HYBM_IMPORT_RANK_ENCODE(overrideRank));
    hybm_export_info_free(&entityInfo.hybmInfo);
    if (ret != BM_OK) {
        SM_LOG_ERROR("import entity failed, peer rankId: " << peer.rankId << " overrideRank: " << overrideRank
                                                           << " ret: " << ret);
        return SMEM_ERROR;
    }
    // 提前登记 rank -> worker：ADD_SLICES 按 rank 路由，若等导入成功才记录，ADDWLST 失败时增量片会被丢弃
    {
        mf::WriteGuard locker(remoteSliceRwMutex_);
        rankToWorkerId_[peer.rankId] = id;
        ranksRole_[peer.rankId] = role;
    }

    std::vector<SmemTransExchangeInfo> slices;
    std::vector<void *> global;
    std::vector<LocalMapAddress> local;
    if (auto ret = ParsePeerSlices(peer, slices, local); ret != BM_OK) {
        SM_LOG_ERROR("parse slice from ESTABLISH failed, rank=" << peer.rankId << " ret=" << ret);
        return SM_INVALID_PARAM;
    }

    if (peer.externalInfo.empty()) {
        SM_LOG_WARN("peer slice is empty! peer rankId: " << peer.rankId << " overrideRank: " << overrideRank);
        return SM_OK;
    }
    global.resize(peer.externalInfo.size(), nullptr);
    std::vector<hybm_exchange_info> hybmInfos;
    hybmInfos.reserve(slices.size());
    for (const auto &s : slices) {
        hybmInfos.push_back(s.hybmInfo);
    }
    if (auto ret = hybm_import(entity_, hybmInfos.data(), hybmInfos.size(), global.data(),
                               HYBM_IMPORT_RANK_ENCODE(overrideRank));
        ret != BM_OK) {
        SM_LOG_ERROR("import slice from ESTABLISH failed, rank=" << peer.rankId << " overrideRank=" << overrideRank
                                                                 << " ret=" << ret);
        for (auto &s : slices) {
            hybm_export_info_free(&s.hybmInfo);
        }
        return SM_ERROR;
    }
    for (auto &s : slices) {
        hybm_export_info_free(&s.hybmInfo);
    }

    bool recorded = (role == SMEM_TRANS_BOTH || config_.role == SMEM_TRANS_BOTH || role != config_.role);
    if (recorded) { // same role, skip record
        mf::WriteGuard locker(remoteSliceRwMutex_);
        for (auto i = 0U; i < local.size(); ++i) {
            remoteSlices_[id].emplace(local[i].address, LocalMapAddress(global[i], local[i].size));
        }
    }
    return SM_OK;
}

int SmemTransConnection::ParseAddSlicesPayload(const MultiBytes &newSlices, uint32_t extendingRankId,
                                               std::vector<hybm_exchange_info> &hybmInfos,
                                               std::vector<LocalMapAddress> &local) noexcept
{
    hybmInfos.reserve(newSlices.size());
    local.reserve(newSlices.size());
    for (size_t i = 0; i < newSlices.size(); ++i) {
        SmemTransExchangeInfo info{};
        if (DeserializeTransExchangeInfo(newSlices[i].data(), newSlices[i].size(), info) == 0) {
            SM_LOG_ERROR("OnAddSlices: invalid slice payload, rank=" << extendingRankId << " idx=" << i
                                                                     << " total=" << newSlices.size()
                                                                     << " sliceLen=" << newSlices[i].size());
            for (auto &parsed : hybmInfos) {
                hybm_export_info_free(&parsed);
            }
            hybmInfos.clear();
            return SM_INVALID_PARAM;
        }
        hybmInfos.push_back(info.hybmInfo);
        local.push_back(info.u.address);
    }
    return SMEM_OK;
}

int SmemTransConnection::ImportIncrementalSlices(uint32_t extendingRankId, uint32_t overrideRank,
                                                 const WorkerId &peerId, smem_trans_role_t peerRole,
                                                 std::vector<hybm_exchange_info> &hybmInfos,
                                                 const std::vector<LocalMapAddress> &local) noexcept
{
    const size_t sliceCount = local.size();
    std::vector<void *> global(sliceCount, nullptr);
    const auto importRet =
        hybm_import(entity_, hybmInfos.data(), hybmInfos.size(), global.data(), HYBM_IMPORT_RANK_ENCODE(overrideRank));
    for (auto &parsed : hybmInfos) {
        hybm_export_info_free(&parsed);
    }
    if (importRet != BM_OK) {
        // 本路径不发 ack 也不推进 link：失败后没有本地重投通道（只能等对端再注册或重连），
        // 因此必须如实上抛，否则残缺清单被固化
        if (importRet == BM_P2P_NOT_READY) {
            SM_LOG_WARN("OnAddSlices: import not ready, rank=" << extendingRankId << " overrideRank=" << overrideRank
                                                               << " slices=" << sliceCount << " ret=" << importRet);
        } else {
            SM_LOG_ERROR("OnAddSlices: import failed, rank=" << extendingRankId << " overrideRank=" << overrideRank
                                                             << " slices=" << sliceCount << " ret=" << importRet);
        }
        return SMEM_ERROR;
    }
    const bool recorded = (peerRole == SMEM_TRANS_BOTH || config_.role == SMEM_TRANS_BOTH || peerRole != config_.role);
    if (recorded) {
        mf::WriteGuard locker(remoteSliceRwMutex_);
        // 写锁内二次确认：读锁到此处之间对端可能已离开，operator[] 会为其重建条目且无人清理
        auto it = rankToWorkerId_.find(extendingRankId);
        if (it == rankToWorkerId_.end() || !(it->second == peerId)) {
            SM_LOG_WARN("OnAddSlices: peer left before record, rank=" << extendingRankId
                                                                      << " overrideRank=" << overrideRank);
            return SMEM_OK;
        }
        for (size_t i = 0; i < sliceCount; ++i) {
            remoteSlices_[peerId].emplace(local[i].address, LocalMapAddress(global[i], local[i].size));
        }
    }
    SM_LOG_INFO("OnAddSlices: rank=" << extendingRankId << " overrideRank=" << overrideRank
                                     << " slices=" << sliceCount);
    return SMEM_OK;
}

int SmemTransConnection::OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept
{
    (void)reqId;
    if (entity_ == nullptr || newSlices.empty()) {
        return SMEM_OK;
    }
    // 未登记的 rank 属于别的连接（store 会向所有客户端广播），放行
    WorkerId peerId{};
    smem_trans_role_t peerRole = SMEM_TRANS_NONE;
    {
        mf::ReadGuard locker(remoteSliceRwMutex_);
        auto it = rankToWorkerId_.find(extendingRankId);
        if (it == rankToWorkerId_.end()) {
            SM_LOG_DEBUG("OnAddSlices skip unregistered rank=" << extendingRankId
                                                               << " sliceCount=" << newSlices.size());
            return SMEM_OK;
        }
        peerId = it->second;
        auto roleIt = ranksRole_.find(extendingRankId);
        if (roleIt != ranksRole_.end()) {
            peerRole = roleIt->second;
        }
    }
    // 同一 uniqueId 幂等复用窗口：增量导入必须与 ADDWLST 首轮落在同一 GVA 窗口
    const uint32_t overrideRank = GetOrCreatePeerRank(extendingRankId, WorkerIdToUniqueId(peerId));
    if (overrideRank == HYBM_IMPORT_RANK_INVALID) {
        SM_LOG_ERROR("OnAddSlices: alloc override rank failed, rank=" << extendingRankId);
        return SMEM_ERROR;
    }

    std::vector<hybm_exchange_info> hybmInfos;
    std::vector<LocalMapAddress> local;
    if (auto ret = ParseAddSlicesPayload(newSlices, extendingRankId, hybmInfos, local); ret != SMEM_OK) {
        return ret;
    }
    return ImportIncrementalSlices(extendingRankId, overrideRank, peerId, peerRole, hybmInfos, local);
}

std::vector<ock::smem::LinkStateEntry> SmemTransConnection::OnQueryLinkState() noexcept
{
    std::vector<ock::smem::LinkStateEntry> entries;
    if (entity_ == nullptr) {
        return entries;
    }
    mf::ReadGuard locker(remoteSliceRwMutex_);
    // 对端 slice 均导入到该对端的 override rank 窗口，连通性按各自的覆盖 rank 查询
    for (const auto &[peerRank, workerId] : rankToWorkerId_) {
        LinkStateEntry entry;
        entry.dstRankId = peerRank;
        const auto overrideRank = FindPeerRank(peerRank);
        if (overrideRank == HYBM_IMPORT_RANK_INVALID) {
            entry.state = TRANS_LINK_IDLE;
            entries.push_back(entry);
            continue;
        }
        hybm_data_op_type reachTypes = static_cast<hybm_data_op_type>(0);
        auto ret = hybm_entity_reach_types(entity_, overrideRank, reachTypes, 0);
        entry.state = (ret == 0 && reachTypes != 0) ? TRANS_LINK_CONNECTED : TRANS_LINK_IDLE;
        entries.push_back(entry);
    }
    SM_LOG_DEBUG("OnQueryLinkState: " << entries.size() << " peers");
    return entries;
}

static std::string uniqueToString(const WorkerId &unique)
{
    std::ostringstream oss;
    constexpr int WIDTH = 2;
    for (size_t i = 0; i < unique.size(); ++i) {
        oss << std::hex << std::setw(WIDTH) << std::setfill('0') << static_cast<int>(unique[i]);
        if (i < unique.size() - 1) {
            oss << ":";
        }
    }
    return oss.str();
}

Result SmemTransConnection::Join(uint32_t flags, const SmemTransExchangeInfo &entityInfo,
                                 const MultiBytes &externalSlices, uint8_t rankRole)
{
    (void)flags;
    auto coreStore = store_->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("SmemTransConnection::Join failed, group manager is nullptr, name: " << name_);
        return SM_ERROR;
    }

    joinComplete_ = std::make_shared<std::promise<void>>();

    // baseInfo 为 entry 共享 entity 的导出信息（entityInfo）；externalInfo 为 entry 已注册内存的
    // 全量 slice 序列化记录（join 时随成员信息登记到 server，供既有成员与晚加入者交换）
    RankFullInfo info{groupMgr->GetLocalRankId()};
    info.protocol = SMEM_RANK_PROTOCOL_TRANS;
    // self connection 声明为 PROVIDER，其余连接声明为 PROXY，供 server 剪枝 PROXY<->PROXY 链路
    info.role = rankRole;
    info.externalInfo = externalSlices;
    SerializeTransExchangeInfo(info.baseInfo, entityInfo);

    if (auto joinRet = groupMgr->Join(info); joinRet != SM_OK) {
        SM_LOG_ERROR("SmemTransConnection::Join failed, name: " << name_ << " rankId: " << rankId_
                                                                << " result: " << joinRet);
        return SM_ERROR;
    }

    const uint32_t groupJoinTimeoutSec =
        mf::MfEnvUtil::GetOptionalUintOrDefault(mf::env::MF_GROUP_JOIN_MAX_TIMEOUT, MF_GROUP_JOIN_DEFAULT_TIMEOUT);
    auto future = joinComplete_->get_future();
    if (future.wait_for(std::chrono::seconds(groupJoinTimeoutSec)) != std::future_status::ready) {
        SM_LOG_ERROR("SmemTransConnection::Join timeout waiting for PROMOTE_TO_ACTIVE, rank=" << rankId_
                                                                                              << " name: " << name_);
        groupMgr->Leave();
        return SM_ERROR;
    }

    SM_LOG_DEBUG("join success. rank: " << rankId_ << " name: " << name_);
    return SM_OK;
}

Result SmemTransConnection::BroadcastSlices(const MultiBytes &slices, const void *sliceVa, uint64_t sliceSize,
                                            uint64_t gva)
{
    if (slices.empty()) {
        return SM_OK;
    }
    auto coreStore = store_->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("BroadcastSlices: group manager is nullptr, name: " << name_);
        return SM_ERROR;
    }
    if (auto ret = groupMgr->ExtendMemory(slices); ret != SM_OK) {
        SM_LOG_ERROR("BroadcastSlices: ExtendMemory failed, ret=" << ret << " name: " << name_);
        return ret;
    }
    RecordSelfSlice(sliceVa, sliceSize, gva);
    return SM_OK;
}

void SmemTransConnection::RecordSelfSlice(const void *sliceVa, uint64_t sliceSize, uint64_t gva) noexcept
{
    if (gva == 0) {
        // 根因已在 hybm GetSliceVa 记录，此处仅提示该 slice 的自拷贝路径不可用
        SM_LOG_WARN("slice gva is 0, skip self slice record, sliceVa: " << sliceVa << " size: " << sliceSize
                                                                        << " name: " << name_);
        return;
    }
    // 键约定与 ParseNameToUniqueId 一致（pid/reserved 为 0），保证 uniqueId 为自身时能命中
    WorkerIdUnion workerId(workerUniqueId_);
    mf::WriteGuard locker(remoteSliceRwMutex_);
    remoteSlices_[workerId.workerId].emplace(sliceVa, LocalMapAddress(reinterpret_cast<void *>(gva), sliceSize));
}

Result SmemTransConnection::TransformAddr(Local2GlobalMap &maps, std::vector<void *> &addr, void *remoteAddrs[],
                                          const size_t dataSizes[], uint32_t size)
{
    for (auto i = 0U; i < size; i++) {
        if (remoteAddrs[i] == nullptr) {
            addr[i] = nullptr;
            continue;
        }

        auto pos = maps.lower_bound(remoteAddrs[i]);
        if (pos == maps.end()) {
            SM_LOG_ERROR("remote address[" << i << "] " << remoteAddrs[i] << " is invalid.");
            return SM_INVALID_PARAM;
        }

        if (dataSizes != nullptr &&
            (const uint8_t *)remoteAddrs[i] + dataSizes[i] > (const uint8_t *)(pos->first) + pos->second.size) {
            SM_LOG_ERROR("address[" << i << "], size[" << i << "]=" << dataSizes[i] << " out of range.");
            return SM_INVALID_PARAM;
        }

        addr[i] = (uint8_t *)pos->second.address + ((const uint8_t *)remoteAddrs[i] - (const uint8_t *)(pos->first));
    }
    return SM_OK;
}

Result SmemTransConnection::BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId,
                                              void *remoteAddrs[], const size_t dataSizes[], uint32_t batchSize,
                                              smem_bm_copy_type opcode, void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(localAddrs != nullptr, "invalid localAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remoteAddrs != nullptr, "invalid remoteAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(dataSizes != nullptr, "invalid dataSizes, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(batchSize != 0, "invalid batchSize, which is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(flags == 0 || flags == COPY_EXTEND_FLAG, "invalid flags", SM_INVALID_PARAM);
    for (auto i = 0U; i < batchSize; i++) {
        SM_VALIDATE_RETURN(localAddrs[i] != nullptr, "localAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(remoteAddrs[i] != nullptr, "remoteAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(dataSizes[i] != 0, "invalid dataSizes, which is 0", SM_INVALID_PARAM);
    }
    WorkerId unique;
    auto ret = ParseNameToUniqueId(remoteUniqueId, unique);
    if (ret != 0) {
        return ret;
    }

    std::vector<void *> mappedAddress(batchSize);
    mf::ReadGuard locker(remoteSliceRwMutex_);
    auto it = remoteSlices_.find(unique);
    if (it == remoteSlices_.end()) {
        SM_LOG_ERROR("session:(" << remoteUniqueId << ")(" << uniqueToString(unique) << ") not found.");
        return SM_INVALID_PARAM;
    }

    ret = TransformAddr(it->second, mappedAddress, remoteAddrs, dataSizes, batchSize);
    SM_ASSERT_RETURN_NOLOG(ret == SM_OK, ret);

    uint32_t flag = flags | ((stream != nullptr) ? ASYNC_COPY_FLAG : 0);
    switch (opcode) {
        case SMEMB_COPY_L2G: {
            hybm_batch_copy_params copyParams = {localAddrs, mappedAddress.data(), dataSizes, batchSize};
            ret = hybm_data_batch_copy(entity_, &copyParams, HYBM_DATA_COPY_DIRECTION_AUTO, stream, flag);
        } break;
        case SMEMB_COPY_G2L: {
            hybm_batch_copy_params copyParams = {mappedAddress.data(), localAddrs, dataSizes, batchSize};
            ret = hybm_data_batch_copy(entity_, &copyParams, HYBM_DATA_COPY_DIRECTION_AUTO, stream, flag);
        } break;
        default:
            SM_LOG_ERROR("unexpect copy type[" << opcode << "] is invalid.");
            return SM_INVALID_PARAM;
    }
    if (ret != 0) {
        SM_LOG_ERROR("batch copy data failed:" << ret << " name: " << name_ << " remote: " << remoteUniqueId);
    }
    return ret;
}

bool SmemTransConnection::ParseTransName(const std::string &name, ock::mf::net_addr_t &ip, uint16_t &port)
{
    UrlExtraction extraction;
    int ret = extraction.ExtractIpPortFromUrl(std::string("tcp://").append(name));
    if (ret != 0) {
        SM_LOG_ERROR("parse name failed, name=" << name << ", ret=" << ret);
        return false;
    }

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, extraction.ip.c_str(), &addr6) == 1) {
        ip.ip.ipv6 = addr6;
        ip.type = ock::mf::IpV6;
    } else {
        struct in_addr addr4;
        if (inet_pton(AF_INET, extraction.ip.c_str(), &addr4) != 1) {
            SM_LOG_ERROR("Invalid IP address format: " << extraction.ip);
            return false;
        }
        ip.ip.ipv4.s_addr = ntohl(addr4.s_addr);
        ip.type = ock::mf::IpV4;
    }
    port = extraction.port;
    return true;
}

bool SmemTransConnection::TryParseNameToWorkerId(const std::string &name, WorkerId &uniqueId)
{
    WorkerUniqueId workerUniqueId;
    if (!ParseTransName(name, workerUniqueId.address, workerUniqueId.port)) {
        return false;
    }
    WorkerIdUnion workerId{workerUniqueId};
    uniqueId = workerId.workerId;
    return true;
}

Result SmemTransConnection::ParseNameToUniqueId(const std::string &name, WorkerId &uniqueId) const
{
    // 一 connection 一对端 uniqueId：建链时已解析并缓存，命中即返回（只读成员，无需加锁）
    if (!name.empty() && name == remoteName_) {
        uniqueId = remoteWorkerId_;
        return SM_OK;
    }
    // 兜底：构造期未缓存（解析失败/裸构造）或传入 name 与建链 name 不一致时按需解析
    if (!TryParseNameToWorkerId(name, uniqueId)) {
        SM_LOG_ERROR("parse name failed, name: " << name);
        return SM_INVALID_PARAM;
    }
    return SM_OK;
}

bool SmemTransConnection::ParsePeerEntity(const RankFullInfo &peer, SmemTransExchangeInfo &entityInfo,
                                          smem_trans_role_t &role, WorkerId &id) noexcept
{
    if (DeserializeTransExchangeInfo(peer.baseInfo.data(), peer.baseInfo.size(), entityInfo) == 0) {
        SM_LOG_ERROR("ParsePeerEntity: invalid baseInfo size for rank " << peer.rankId
                                                                        << " size: " << peer.baseInfo.size());
        return false;
    }
    role = static_cast<smem_trans_role_t>(entityInfo.u.session.reserved);
    entityInfo.u.session.reserved = 0U; // reserved must be cleared so the WorkerId matches ParseNameToUniqueId
    WorkerIdUnion wu(entityInfo.u.session);
    id = wu.workerId;
    return true;
}

int SmemTransConnection::ParsePeerSlices(const RankFullInfo &peer, std::vector<SmemTransExchangeInfo> &slices,
                                         std::vector<LocalMapAddress> &local) noexcept
{
    slices.resize(peer.externalInfo.size());
    local.reserve(peer.externalInfo.size());
    for (size_t i = 0; i < peer.externalInfo.size(); ++i) {
        SmemTransExchangeInfo info{};
        if (DeserializeTransExchangeInfo(peer.externalInfo[i].data(), peer.externalInfo[i].size(), info) == 0) {
            SM_LOG_ERROR("ParsePeerSlices: invalid externalInfo size for rank "
                         << peer.rankId << " size: " << peer.externalInfo[i].size());
            for (auto &s : slices) {
                hybm_export_info_free(&s.hybmInfo);
            }
            return BM_INVALID_PARAM;
        }
        slices[i] = info;
        local.emplace_back(slices[i].u.address.address, slices[i].u.address.size);
    }
    return BM_OK;
}

} // namespace smem
} // namespace ock
