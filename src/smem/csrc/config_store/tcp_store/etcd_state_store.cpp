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

#include <algorithm>
#include <cmath>

#include "mf_str_util.h"
#include "smem_config_store_backend.h"
#include "smem_config_store_logger.h"
#include "etcd_state_store.h"

namespace ock::smem {

EtcdStateStore::EtcdStateStore(StoreBackendPtr backend) noexcept : backend_(std::move(backend)) {}

EtcdStateStore::~EtcdStateStore() noexcept
{
    StopFlushThread();
}

// ── Retry constants for PutWithRetry ──
static constexpr int PUT_RETRY_MAX = 3;
static constexpr int PUT_RETRY_BASE_DELAY_MS = 10;

// ── Key helpers ──

std::string EtcdStateStore::RankBaseKey(uint32_t rankId) noexcept
{
    return keyRankPrefix + std::to_string(rankId) + "/base";
}

std::string EtcdStateStore::RankExtKey(uint32_t rankId) noexcept
{
    return keyRankPrefix + std::to_string(rankId) + "/ext";
}

// ── Serialization ──

StoreErrorCode EtcdStateStore::SerializeStates(const std::vector<RankState> &states, uint32_t maxRanks,
                                               std::vector<uint8_t> &out) noexcept
{
    uint32_t n = std::min(static_cast<uint32_t>(states.size()), maxRanks);
    out.resize(sizeof(uint32_t) + n);
    auto *ptr = out.data();
    std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&maxRanks)), sizeof(uint32_t), ptr);
    ptr += sizeof(uint32_t);
    for (uint32_t i = 0; i < n; ++i) {
        ptr[i] = static_cast<uint8_t>(states[i]);
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::DeserializeStates(const std::vector<uint8_t> &data, std::vector<RankState> &states,
                                                 uint32_t &maxRanks) noexcept
{
    if (data.size() < sizeof(uint32_t)) {
        return StoreErrorCode::ERROR;
    }
    std::copy_n(data.data(), sizeof(uint32_t), static_cast<uint8_t *>(static_cast<void *>(&maxRanks)));
    if (data.size() < sizeof(uint32_t) + maxRanks) {
        return StoreErrorCode::ERROR;
    }
    states.resize(maxRanks);
    for (uint32_t i = 0; i < maxRanks; ++i) {
        states[i] = static_cast<RankState>(data[sizeof(uint32_t) + i]);
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::SerializeRoles(const std::vector<uint8_t> &roles, uint32_t maxRanks,
                                              std::vector<uint8_t> &out) noexcept
{
    uint32_t n = std::min(static_cast<uint32_t>(roles.size()), maxRanks);
    out.resize(sizeof(uint32_t) + n);
    auto *ptr = out.data();
    std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&maxRanks)), sizeof(uint32_t), ptr);
    ptr += sizeof(uint32_t);
    for (uint32_t i = 0; i < n; ++i) {
        ptr[i] = roles[i];
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::DeserializeRoles(const std::vector<uint8_t> &data, std::vector<uint8_t> &roles,
                                                uint32_t &maxRanks) noexcept
{
    if (data.size() < sizeof(uint32_t)) {
        return StoreErrorCode::ERROR;
    }
    std::copy_n(data.data(), sizeof(uint32_t), static_cast<uint8_t *>(static_cast<void *>(&maxRanks)));
    if (data.size() < sizeof(uint32_t) + maxRanks) {
        return StoreErrorCode::ERROR;
    }
    roles.resize(maxRanks);
    for (uint32_t i = 0; i < maxRanks; ++i) {
        roles[i] = data[sizeof(uint32_t) + i];
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::SerializeLinks(const std::vector<LinkState> &links, uint32_t maxRanks,
                                              std::vector<uint8_t> &out) noexcept
{
    uint32_t count = maxRanks * maxRanks;
    uint32_t n = std::min(static_cast<uint32_t>(links.size()), count);
    out.resize(sizeof(uint32_t) + n);
    auto *ptr = out.data();
    std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&count)), sizeof(uint32_t), ptr);
    ptr += sizeof(uint32_t);
    for (uint32_t i = 0; i < n; ++i) {
        ptr[i] = static_cast<uint8_t>(links[i]);
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::DeserializeLinks(const std::vector<uint8_t> &data, std::vector<LinkState> &links,
                                                uint32_t &maxRanks) noexcept
{
    if (data.size() < sizeof(uint32_t)) {
        return StoreErrorCode::ERROR;
    }
    uint32_t count = 0;
    std::copy_n(data.data(), sizeof(uint32_t), static_cast<uint8_t *>(static_cast<void *>(&count)));
    maxRanks = static_cast<uint32_t>(std::sqrt(count));
    if (maxRanks * maxRanks != count) {
        return StoreErrorCode::ERROR;
    }
    if (data.size() < sizeof(uint32_t) + count) {
        return StoreErrorCode::ERROR;
    }
    links.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        links[i] = static_cast<LinkState>(data[sizeof(uint32_t) + i]);
    }
    return StoreErrorCode::SUCCESS;
}

// ── Helper: Put with simple retry ──
static StoreErrorCode PutWithRetry(StoreBackendPtr &backend, const std::string &key, const std::vector<uint8_t> &data,
                                   int64_t ttl, int maxRetries = PUT_RETRY_MAX) noexcept
{
    for (int i = 0; i < maxRetries; ++i) {
        auto ret = backend->Put(key, data, ttl);
        if (ret == StoreErrorCode::SUCCESS) {
            return ret;
        }
        if (i + 1 < maxRetries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(PUT_RETRY_BASE_DELAY_MS * (1 << i)));
        }
    }
    return StoreErrorCode::ERROR;
}

// ── C1: 同步写入 ──

StoreErrorCode EtcdStateStore::PersistStates(const std::vector<RankState> &states, uint32_t maxRanks) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    std::vector<uint8_t> data;
    auto ret = SerializeStates(states, maxRanks, data);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    return PutWithRetry(backend_, keyStates, data, 0);
}

StoreErrorCode EtcdStateStore::PersistAliveRanks(const std::unordered_set<uint32_t> &alive) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    if (alive.empty()) {
        return backend_->Delete(keyAlive);
    }
    std::vector<uint32_t> sorted(alive.begin(), alive.end());
    std::sort(sorted.begin(), sorted.end());
    std::string str;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) {
            str += ",";
        }
        str += std::to_string(sorted[i]);
    }
    std::vector<uint8_t> data(str.begin(), str.end());
    return PutWithRetry(backend_, keyAlive, data, 0);
}

StoreErrorCode EtcdStateStore::PersistRankBase(uint32_t rankId, const Bytes &data) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    if (data.empty()) {
        return backend_->Delete(RankBaseKey(rankId));
    }
    return PutWithRetry(backend_, RankBaseKey(rankId), data, 0);
}

StoreErrorCode EtcdStateStore::PersistRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    if (data.empty()) {
        return backend_->Delete(RankExtKey(rankId));
    }
    std::vector<uint8_t> buf;
    uint64_t count = data.size();
    buf.resize(buf.size() + sizeof(uint64_t));
    std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&count)), sizeof(uint64_t), buf.data());
    for (const auto &item : data) {
        uint64_t itemSize = item.size();
        buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&itemSize),
                   reinterpret_cast<const uint8_t *>(&itemSize) + sizeof(uint64_t));
        buf.insert(buf.end(), item.begin(), item.end());
    }
    return PutWithRetry(backend_, RankExtKey(rankId), buf, 0);
}

StoreErrorCode EtcdStateStore::PersistRankRoles(const std::vector<uint8_t> &roles, uint32_t maxRanks) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    std::vector<uint8_t> data;
    auto ret = SerializeRoles(roles, maxRanks, data);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    return PutWithRetry(backend_, keyRoles, data, 0);
}

StoreErrorCode EtcdStateStore::DeleteRank(uint32_t rankId) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::SUCCESS;
    }
    backend_->Delete(RankBaseKey(rankId));
    backend_->Delete(RankExtKey(rankId));
    return StoreErrorCode::SUCCESS;
}

// ── C2: 异步链接状态 ──

void EtcdStateStore::FlushLinks(const std::vector<LinkState> &links, uint32_t maxRanks) noexcept
{
    if (!IsEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(linksMutex_);
    pendingLinks_ = links;
    pendingMaxRanks_ = maxRanks;
    linksDirty_ = true;
}

void EtcdStateStore::StartFlushThread() noexcept
{
    if (!IsEnabled()) {
        return;
    }
    running_ = true;
    flushThread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(flushIntervalMs));
            if (!linksDirty_.exchange(false)) {
                continue;
            }
            std::vector<LinkState> links;
            uint32_t maxRanks = 0;
            {
                std::lock_guard<std::mutex> lock(linksMutex_);
                links = pendingLinks_;
                maxRanks = pendingMaxRanks_;
            }
            std::vector<uint8_t> data;
            if (SerializeLinks(links, maxRanks, data) == StoreErrorCode::SUCCESS) {
                backend_->Put(keyLinks, data, linksTtlSec);
            }
        }
    });
}

void EtcdStateStore::StopFlushThread() noexcept
{
    running_ = false;
    if (flushThread_.joinable()) {
        flushThread_.join();
    }
}

// ── 读取（恢复用） ──

StoreErrorCode EtcdStateStore::LoadStates(std::vector<RankState> &states, uint32_t &maxRanks) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    std::vector<uint8_t> data;
    auto ret = backend_->Get(keyStates, data);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    return DeserializeStates(data, states, maxRanks);
}

StoreErrorCode EtcdStateStore::LoadAliveRanks(std::unordered_set<uint32_t> &alive) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    std::string str;
    auto ret = backend_->Get(keyAlive, str);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    if (str.empty()) {
        return StoreErrorCode::SUCCESS;
    }
    auto parts = mf::StrUtil::Split(str, ',');
    for (const auto &p : parts) {
        if (p.empty()) {
            continue;
        }
        uint32_t id;
        if (mf::StrUtil::String2Uint(p, id)) {
            alive.insert(id);
        }
    }
    return StoreErrorCode::SUCCESS;
}

StoreErrorCode EtcdStateStore::LoadLinks(std::vector<LinkState> &links, uint32_t &maxRanks) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    std::vector<uint8_t> data;
    auto ret = backend_->Get(keyLinks, data);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    return DeserializeLinks(data, links, maxRanks);
}

StoreErrorCode EtcdStateStore::LoadRankBase(uint32_t rankId, Bytes &data) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    return backend_->Get(RankBaseKey(rankId), data);
}

StoreErrorCode EtcdStateStore::LoadRankRoles(std::vector<uint8_t> &roles, uint32_t &maxRanks) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    std::vector<uint8_t> data;
    auto ret = backend_->Get(keyRoles, data);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    return DeserializeRoles(data, roles, maxRanks);
}

StoreErrorCode EtcdStateStore::LoadRankExternal(uint32_t rankId, std::vector<Bytes> &data) noexcept
{
    if (!IsEnabled()) {
        return StoreErrorCode::ERROR;
    }
    std::vector<uint8_t> buf;
    auto ret = backend_->Get(RankExtKey(rankId), buf);
    if (ret != StoreErrorCode::SUCCESS) {
        return ret;
    }
    if (buf.size() < sizeof(uint64_t)) {
        return StoreErrorCode::ERROR;
    }
    uint64_t count = 0;
    std::copy_n(buf.data(), sizeof(uint64_t), static_cast<uint8_t *>(static_cast<void *>(&count)));
    size_t offset = sizeof(uint64_t);
    data.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        if (offset + sizeof(uint64_t) > buf.size()) {
            return StoreErrorCode::ERROR;
        }
        uint64_t itemSize = 0;
        std::copy_n(buf.data() + offset, sizeof(uint64_t), static_cast<uint8_t *>(static_cast<void *>(&itemSize)));
        offset += sizeof(uint64_t);
        if (offset + itemSize > buf.size()) {
            return StoreErrorCode::ERROR;
        }
        data[i].assign(buf.begin() + offset, buf.begin() + offset + itemSize);
        offset += itemSize;
    }
    return StoreErrorCode::SUCCESS;
}

// ── 完整恢复流程 ──

// 观测：一次性汇总恢复出的 Proxy rank 列表，供定位“为什么这些链路没建”，并与 rank 自身
// （BM Initialize 的 role 推导日志）互为交叉校验。列表长度设上限，避免大规模集群刷超长日志。
void LogRestoredRankRoles(const std::vector<uint8_t> &roles, size_t aliveCount) noexcept
{
    constexpr size_t kMaxLoggedProxies = 32U;
    std::string proxyRanks;
    size_t proxyCount = 0;
    for (size_t i = 0; i < roles.size(); ++i) {
        if (roles[i] != SMEM_GROUP_RANK_ROLE_PROXY) {
            continue;
        }
        ++proxyCount;
        if (proxyCount > kMaxLoggedProxies) {
            continue;
        }
        if (!proxyRanks.empty()) {
            proxyRanks.push_back(',');
        }
        proxyRanks += std::to_string(i);
    }
    if (proxyCount > kMaxLoggedProxies) {
        proxyRanks += ",...";
    }
    STORE_LOG_INFO("EtcdStateStore: rank roles restored, alive=" << aliveCount << ", proxyCount=" << proxyCount
                                                                 << ", proxyRanks=[" << proxyRanks << "]");
}

StoreErrorCode EtcdStateStore::Recover(SmemGroupManagerServer *server) noexcept
{
    if (!IsEnabled()) {
        STORE_LOG_INFO("EtcdStateStore: not distributed, skip recovery");
        return StoreErrorCode::SUCCESS;
    }

    // 1. 读 states；无持久化状态视为 fresh start
    std::vector<RankState> states;
    uint32_t maxRanks = 0;
    auto ret = LoadStates(states, maxRanks);
    if (ret != StoreErrorCode::SUCCESS) {
        STORE_LOG_WARN("EtcdStateStore: no states found, fresh start");
        return StoreErrorCode::SUCCESS;
    }

    // 2. 读 alive / links / rank info
    std::unordered_set<uint32_t> aliveSet;
    LoadAliveRanks(aliveSet); // non-fatal

    std::vector<LinkState> links;
    uint32_t linksMaxRanks = 0;
    if (LoadLinks(links, linksMaxRanks) == StoreErrorCode::SUCCESS && linksMaxRanks == maxRanks) {
        server->RestoreState(states, links, aliveSet);
    } else {
        STORE_LOG_WARN("EtcdStateStore: links stale or missing, recovering states only");
        server->RestoreState(states, aliveSet);
    }

    // 3. 恢复 rankBase_ / rankExternal_
    for (auto rankId : aliveSet) {
        Bytes base;
        if (LoadRankBase(rankId, base) == StoreErrorCode::SUCCESS && !base.empty()) {
            server->SetRankBase(rankId, base);
        }
        std::vector<Bytes> ext;
        if (LoadRankExternal(rankId, ext) == StoreErrorCode::SUCCESS && !ext.empty()) {
            server->SetRankExternal(rankId, ext);
        }
    }

    // 4. 恢复角色：与 states/links 一样整体只读一次，恢复耗时与 alive rank 数无关；
    //    必须在 RebuildActiveLinks/QueryLinkStates 之前完成，否则 PROXY<->PROXY 剪枝对会被重新拉起。
    std::vector<uint8_t> roles;
    uint32_t rolesMaxRanks = 0;
    if (LoadRankRoles(roles, rolesMaxRanks) == StoreErrorCode::SUCCESS && rolesMaxRanks == maxRanks) {
        server->SetRankRoles(roles);
        LogRestoredRankRoles(roles, aliveSet.size());
    } else {
        STORE_LOG_INFO("EtcdStateStore: rank roles missing or stale, all ranks treated as unknown");
    }

    // 5. 重建 activeLinks_
    server->RebuildActiveLinks();

    // 6. 查询所有存活 rank 的实际状态
    server->QueryLinkStates();

    STORE_LOG_INFO("EtcdStateStore: recovery complete, ranks=" << aliveSet.size());
    return StoreErrorCode::SUCCESS;
}

} // namespace ock::smem
