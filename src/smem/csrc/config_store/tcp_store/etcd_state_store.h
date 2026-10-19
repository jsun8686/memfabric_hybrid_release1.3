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

#ifndef SMEM_ETCD_STATE_STORE_H
#define SMEM_ETCD_STATE_STORE_H

#include <atomic>
#include <thread>
#include <vector>
#include <unordered_set>

#include "smem_config_store_backend.h"
#include "smem_config_store_errno.h"
#include "smem_group_manager_def.h"
#include "smem_group_manager_server.h"
#include "smem_ref.h"

namespace ock::smem {

/**
 * @brief Persists SmemGroupManagerServer state to etcd and recovers it on leader restart.
 *
 * Key layout (prefix: /memfabric_hybrid/group_manager/):
 *   states           → bytes                — serialized RankState[]
 *   alive_ranks      → "id1,id2,..."        — comma-separated
 *   links            → bytes + TTL=30s      — serialized LinkState[] (async flush)
 *   ranks/<id>/base  → bytes                — rankBase_[id]
 *   ranks/<id>/ext   → bytes                — rankExternal_[id]
 */
class EtcdStateStore {
public:
    explicit EtcdStateStore(StoreBackendPtr backend) noexcept;
    ~EtcdStateStore() noexcept;

    EtcdStateStore(const EtcdStateStore &) = delete;
    EtcdStateStore &operator=(const EtcdStateStore &) = delete;

    // ── C1: 关键状态 · 同步写入 ──

    StoreErrorCode PersistStates(const std::vector<RankState> &states, uint32_t maxRanks) noexcept;
    StoreErrorCode PersistAliveRanks(const std::unordered_set<uint32_t> &alive) noexcept;
    StoreErrorCode PersistRankBase(uint32_t rankId, const Bytes &data) noexcept;
    StoreErrorCode PersistRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept;
    StoreErrorCode PersistRankRoles(const std::vector<uint8_t> &roles, uint32_t maxRanks) noexcept;
    StoreErrorCode DeleteRank(uint32_t rankId) noexcept;

    // ── C2: 链接状态 · 异步批量写入 ──

    void FlushLinks(const std::vector<LinkState> &links, uint32_t maxRanks) noexcept;
    void StartFlushThread() noexcept;
    void StopFlushThread() noexcept;

    // ── 恢复 ──

    StoreErrorCode LoadStates(std::vector<RankState> &states, uint32_t &maxRanks) noexcept;
    StoreErrorCode LoadAliveRanks(std::unordered_set<uint32_t> &alive) noexcept;
    StoreErrorCode LoadLinks(std::vector<LinkState> &links, uint32_t &maxRanks) noexcept;
    StoreErrorCode LoadRankBase(uint32_t rankId, Bytes &data) noexcept;
    StoreErrorCode LoadRankExternal(uint32_t rankId, std::vector<Bytes> &data) noexcept;
    StoreErrorCode LoadRankRoles(std::vector<uint8_t> &roles, uint32_t &maxRanks) noexcept;

    // ── 恢复完整流程（供 AccStoreServer 调用） ──

    StoreErrorCode Recover(SmemGroupManagerServer *server) noexcept;

    bool IsEnabled() const noexcept
    {
        return backend_ != nullptr && backend_->IsDistributed();
    }

private:
    static constexpr auto keyPrefix = "/memfabric_hybrid/group_manager/";
    static constexpr auto keyStates = "/memfabric_hybrid/group_manager/states";
    static constexpr auto keyAlive = "/memfabric_hybrid/group_manager/alive_ranks";
    static constexpr auto keyLinks = "/memfabric_hybrid/group_manager/links";
    static constexpr auto keyRoles = "/memfabric_hybrid/group_manager/roles";
    static constexpr auto keyRankPrefix = "/memfabric_hybrid/group_manager/ranks/";

    static constexpr int64_t linksTtlSec = 30;
    static constexpr uint32_t flushIntervalMs = 100;

    static std::string RankBaseKey(uint32_t rankId) noexcept;
    static std::string RankExtKey(uint32_t rankId) noexcept;

    static StoreErrorCode SerializeStates(const std::vector<RankState> &states, uint32_t maxRanks,
                                          std::vector<uint8_t> &out) noexcept;
    static StoreErrorCode DeserializeStates(const std::vector<uint8_t> &data, std::vector<RankState> &states,
                                            uint32_t &maxRanks) noexcept;
    // 角色与 states/links 一样按“单个 blob”序列化：恢复时只需 1 次 etcd GET，耗时与 alive rank 数无关。
    static StoreErrorCode SerializeRoles(const std::vector<uint8_t> &roles, uint32_t maxRanks,
                                         std::vector<uint8_t> &out) noexcept;
    static StoreErrorCode DeserializeRoles(const std::vector<uint8_t> &data, std::vector<uint8_t> &roles,
                                           uint32_t &maxRanks) noexcept;
    static StoreErrorCode SerializeLinks(const std::vector<LinkState> &links, uint32_t maxRanks,
                                         std::vector<uint8_t> &out) noexcept;
    static StoreErrorCode DeserializeLinks(const std::vector<uint8_t> &data, std::vector<LinkState> &links,
                                           uint32_t &maxRanks) noexcept;

    StoreBackendPtr backend_;
    std::atomic<bool> linksDirty_{false};
    std::mutex linksMutex_;
    std::vector<LinkState> pendingLinks_;
    uint32_t pendingMaxRanks_{0};
    std::thread flushThread_;
    std::atomic<bool> running_{false};
};

} // namespace ock::smem

#endif // SMEM_ETCD_STATE_STORE_H
