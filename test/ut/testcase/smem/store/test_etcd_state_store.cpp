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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "etcd_state_store.h"
#include "smem_local_memory_backend.h"
#include "smem_group_manager_server.h"

namespace ock {
namespace smem {

constexpr uint32_t K_MAX_RANKS = 8;
constexpr uint32_t K_WAIT_MS = 300;
constexpr uint32_t K_SETTLE_MS = 250;
constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_THREE = 3;
constexpr uint32_t K_LINK_COUNT_TWO = 2;
constexpr uint32_t K_RANK_FIVE = 5;
constexpr uint32_t K_TEST_BYTES_LEN = 4;
constexpr uint32_t K_TEST_BYTES_SEED = 1;
constexpr uint32_t K_TEST_BYTES_SEED_TWO = 2;

/* Hand-written mock — records every Put/Delete, configurable Get results */
class RecordingMockBackend : public ConfigStoreBackend {
public:
    bool isDistributed = true;
    std::unordered_map<std::string, std::vector<uint8_t>> kv;
    std::vector<std::string> putKeys;
    std::vector<std::string> deletedKeys;
    StoreErrorCode getResult = SUCCESS;
    StoreErrorCode putResult = SUCCESS;
    StoreErrorCode deleteResult = SUCCESS;
    int putCallCount = 0;

    std::string BackendName() const noexcept override
    {
        return "RecordingMockBackend";
    }

    StoreErrorCode Get(const std::string &key, std::vector<uint8_t> &value) const noexcept override
    {
        auto it = kv.find(key);
        if (it == kv.end()) {
            return NOT_EXIST;
        }
        value = it->second;
        return getResult;
    }

    StoreErrorCode PrefixGet(const std::string &key, PrefixGetMap &value) const noexcept override
    {
        (void)key;
        (void)value;
        return getResult;
    }

    StoreErrorCode Put(const std::string &key, const std::vector<uint8_t> &value, int64_t ttl) noexcept override
    {
        (void)ttl;
        putCallCount++;
        putKeys.push_back(key);
        kv[key] = value;
        return putResult;
    }

    StoreErrorCode Delete(const std::string &key) noexcept override
    {
        deletedKeys.push_back(key);
        kv.erase(key);
        return deleteResult;
    }

    StoreErrorCode Exist(const std::string &key) const noexcept override
    {
        (void)key;
        return kv.empty() ? NOT_EXIST : SUCCESS;
    }

    void Clear() noexcept override {}

    bool IsDistributed() const noexcept override
    {
        return isDistributed;
    }

    bool SupportsTTL() const noexcept override
    {
        return true;
    }

    StoreErrorCode AcquireDistributedLock(const std::string &name) noexcept override
    {
        (void)name;
        return SUCCESS;
    }

    StoreErrorCode ReleaseDistributedLock(const std::string &name) noexcept override
    {
        (void)name;
        return SUCCESS;
    }

    StoreErrorCode TryAcquireDistributedLock(const std::string &name, int64_t ttl) noexcept override
    {
        (void)name;
        (void)ttl;
        return SUCCESS;
    }

    StoreErrorCode Initialize(const std::string &addr, const std::string &port, const std::string &rank) override
    {
        (void)addr;
        (void)port;
        (void)rank;
        return SUCCESS;
    }

    void UnInitialize() override {}
};

class EtcdStateStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        backend_ = SmMakeRef<RecordingMockBackend>();
        store_ = std::make_unique<EtcdStateStore>(Convert<RecordingMockBackend, ConfigStoreBackend>(backend_));
    }

    void TearDown() override
    {
        store_.reset();
        backend_ = nullptr;
    }

    static Bytes MakeBytes(uint8_t seed, size_t len)
    {
        Bytes data(len);
        for (size_t i = 0; i < len; ++i) {
            data[i] = static_cast<uint8_t>(seed + i);
        }
        return data;
    }

    SmRef<RecordingMockBackend> backend_;
    std::unique_ptr<EtcdStateStore> store_;
};

/* ================================================================== */
/*  Disabled backend (non-distributed) — methods short-circuit        */
/* ================================================================== */

TEST_F(EtcdStateStoreTest, DisabledBackendShortCircuitsPersistAndLoad)
{
    backend_->isDistributed = false;

    std::vector<RankState> states{RANK_ACTIVE, RANK_CHECKED_IN};
    EXPECT_EQ(store_->PersistStates(states, K_MAX_RANKS), SUCCESS);
    EXPECT_EQ(store_->PersistAliveRanks({0U, 2U}), SUCCESS);
    EXPECT_EQ(store_->PersistRankBase(0U, MakeBytes(1, K_RANK_TWO * K_RANK_TWO)), SUCCESS);
    EXPECT_EQ(store_->PersistRankExternal(0U, {MakeBytes(1, 2)}), SUCCESS);
    EXPECT_EQ(store_->PersistRankRoles({SMEM_GROUP_RANK_ROLE_PROXY}, K_MAX_RANKS), SUCCESS);
    EXPECT_EQ(store_->DeleteRank(0U), SUCCESS);
    store_->FlushLinks({LINK_CONNECTED}, K_MAX_RANKS);

    std::vector<RankState> loadedStates;
    uint32_t maxRanks = 0;
    EXPECT_EQ(store_->LoadStates(loadedStates, maxRanks), ERROR);
    std::unordered_set<uint32_t> alive;
    EXPECT_EQ(store_->LoadAliveRanks(alive), ERROR);
    std::vector<LinkState> links;
    EXPECT_EQ(store_->LoadLinks(links, maxRanks), ERROR);
    Bytes base;
    EXPECT_EQ(store_->LoadRankBase(0U, base), ERROR);
    std::vector<Bytes> ext;
    EXPECT_EQ(store_->LoadRankExternal(0U, ext), ERROR);
    std::vector<uint8_t> roles;
    uint32_t rolesMaxRanks = 0;
    EXPECT_EQ(store_->LoadRankRoles(roles, rolesMaxRanks), ERROR);

    EXPECT_TRUE(backend_->putKeys.empty());
    EXPECT_TRUE(backend_->deletedKeys.empty());
}

/* ================================================================== */
/*  Persist paths                                                     */
/* ================================================================== */

TEST_F(EtcdStateStoreTest, PersistStatesWritesSerializedData)
{
    // States vector must be sized to maxRanks (DeserializeStates validates against maxRanks).
    std::vector<RankState> states{RANK_CHECKED_IN, RANK_ACTIVE, RANK_IDLE};
    uint32_t maxRanks = 3;
    EXPECT_EQ(store_->PersistStates(states, maxRanks), SUCCESS);

    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/states");

    std::vector<RankState> loaded;
    uint32_t loadedMaxRanks = 0;
    ASSERT_EQ(store_->LoadStates(loaded, loadedMaxRanks), SUCCESS);
    EXPECT_EQ(loadedMaxRanks, maxRanks);
    ASSERT_EQ(loaded.size(), 3U);
    EXPECT_EQ(loaded[0], RANK_CHECKED_IN);
    EXPECT_EQ(loaded[1], RANK_ACTIVE);
    EXPECT_EQ(loaded[K_RANK_TWO], RANK_IDLE);
}

TEST_F(EtcdStateStoreTest, PersistRankRolesRoundTripSharesSingleKey)
{
    /* 角色按单个 blob 持久化（与 states/links 同构）：恢复时只需 1 次 etcd GET，
     * 恢复耗时与 alive rank 数无关。 */
    std::vector<uint8_t> roles{SMEM_GROUP_RANK_ROLE_PROVIDER, SMEM_GROUP_RANK_ROLE_PROXY};
    EXPECT_EQ(store_->PersistRankRoles(roles, K_RANK_TWO), SUCCESS);

    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/roles");

    std::vector<uint8_t> loadedRoles;
    uint32_t loadedMaxRanks = 0;
    ASSERT_EQ(store_->LoadRankRoles(loadedRoles, loadedMaxRanks), SUCCESS);
    EXPECT_EQ(loadedMaxRanks, K_RANK_TWO);
    ASSERT_EQ(loadedRoles.size(), static_cast<size_t>(K_RANK_TWO));
    EXPECT_EQ(loadedRoles[0], SMEM_GROUP_RANK_ROLE_PROVIDER);
    EXPECT_EQ(loadedRoles[1], SMEM_GROUP_RANK_ROLE_PROXY);
}

TEST_F(EtcdStateStoreTest, PersistAliveRanksSortsAndJoins)
{
    EXPECT_EQ(store_->PersistAliveRanks({3U, 1U, 2U}), SUCCESS);

    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/alive_ranks");

    std::unordered_set<uint32_t> alive;
    ASSERT_EQ(store_->LoadAliveRanks(alive), SUCCESS);
    EXPECT_EQ(alive, (std::unordered_set<uint32_t>{1U, 2U, 3U}));
}

TEST_F(EtcdStateStoreTest, PersistAliveRanksEmptyDeletesKey)
{
    EXPECT_EQ(store_->PersistAliveRanks({}), SUCCESS);
    ASSERT_EQ(backend_->deletedKeys.size(), 1U);
    EXPECT_EQ(backend_->deletedKeys[0], "/memfabric_hybrid/group_manager/alive_ranks");
}

TEST_F(EtcdStateStoreTest, PersistRankBaseRoundTrip)
{
    auto data = MakeBytes(7, 16);
    EXPECT_EQ(store_->PersistRankBase(5U, data), SUCCESS);
    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/ranks/5/base");

    Bytes loaded;
    ASSERT_EQ(store_->LoadRankBase(5U, loaded), SUCCESS);
    EXPECT_EQ(loaded, data);
}

TEST_F(EtcdStateStoreTest, PersistRankBaseEmptyDeletesKey)
{
    EXPECT_EQ(store_->PersistRankBase(5U, {}), SUCCESS);
    ASSERT_EQ(backend_->deletedKeys.size(), 1U);
    EXPECT_EQ(backend_->deletedKeys[0], "/memfabric_hybrid/group_manager/ranks/5/base");
}

TEST_F(EtcdStateStoreTest, PersistRankExternalRoundTrip)
{
    std::vector<Bytes> data{MakeBytes(1, 2), MakeBytes(3, 4), MakeBytes(5, 6)};
    EXPECT_EQ(store_->PersistRankExternal(7U, data), SUCCESS);
    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/ranks/7/ext");

    std::vector<Bytes> loaded;
    ASSERT_EQ(store_->LoadRankExternal(7U, loaded), SUCCESS);
    ASSERT_EQ(loaded.size(), 3U);
    EXPECT_EQ(loaded[0], data[0]);
    EXPECT_EQ(loaded[1], data[1]);
    EXPECT_EQ(loaded[K_RANK_TWO], data[K_RANK_TWO]);
}

TEST_F(EtcdStateStoreTest, PersistRankExternalEmptyDeletesKey)
{
    EXPECT_EQ(store_->PersistRankExternal(7U, {}), SUCCESS);
    ASSERT_EQ(backend_->deletedKeys.size(), 1U);
    EXPECT_EQ(backend_->deletedKeys[0], "/memfabric_hybrid/group_manager/ranks/7/ext");
}

TEST_F(EtcdStateStoreTest, DeleteRankDeletesBothKeys)
{
    EXPECT_EQ(store_->PersistRankBase(K_RANK_THREE, MakeBytes(K_TEST_BYTES_SEED, K_TEST_BYTES_LEN)), SUCCESS);
    EXPECT_EQ(store_->PersistRankExternal(3U, {MakeBytes(K_TEST_BYTES_SEED_TWO, K_TEST_BYTES_LEN)}), SUCCESS);
    backend_->deletedKeys.clear();

    EXPECT_EQ(store_->DeleteRank(3U), SUCCESS);
    ASSERT_EQ(backend_->deletedKeys.size(), 2U);
    EXPECT_EQ(backend_->deletedKeys[0], "/memfabric_hybrid/group_manager/ranks/3/base");
    EXPECT_EQ(backend_->deletedKeys[1], "/memfabric_hybrid/group_manager/ranks/3/ext");
}

/* ================================================================== */
/*  Deserialize error paths                                           */
/* ================================================================== */

TEST_F(EtcdStateStoreTest, LoadStatesFailsOnTruncatedData)
{
    backend_->kv["/memfabric_hybrid/group_manager/states"] = {0x01};
    std::vector<RankState> states;
    uint32_t maxRanks = 0;
    EXPECT_EQ(store_->LoadStates(states, maxRanks), ERROR);
}

TEST_F(EtcdStateStoreTest, LoadAliveRanksIgnoresInvalidEntries)
{
    backend_->kv["/memfabric_hybrid/group_manager/alive_ranks"] = {'1', ',', 'x', ',', '3'};
    std::unordered_set<uint32_t> alive;
    ASSERT_EQ(store_->LoadAliveRanks(alive), SUCCESS);
    EXPECT_EQ(alive, (std::unordered_set<uint32_t>{1U, 3U}));
}

TEST_F(EtcdStateStoreTest, LoadLinksRoundTrip)
{
    std::vector<LinkState> links{LINK_IDLE, LINK_EXCHANGED, LINK_CONNECTED, LINK_CONNECTING};
    store_->FlushLinks(links, K_LINK_COUNT_TWO);
    // FlushLinks only stages; the flush thread performs the Put.
    store_->StartFlushThread();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    store_->StopFlushThread();

    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/links");

    std::vector<LinkState> loaded;
    uint32_t maxRanks = 0;
    ASSERT_EQ(store_->LoadLinks(loaded, maxRanks), SUCCESS);
    EXPECT_EQ(maxRanks, 2U);
    ASSERT_EQ(loaded.size(), 4U);
    EXPECT_EQ(loaded[0], LINK_IDLE);
    EXPECT_EQ(loaded[1], LINK_EXCHANGED);
    EXPECT_EQ(loaded[K_RANK_TWO], LINK_CONNECTED);
    EXPECT_EQ(loaded[K_RANK_THREE], LINK_CONNECTING);
}

TEST_F(EtcdStateStoreTest, LoadRankExternalFailsOnTruncatedData)
{
    backend_->kv["/memfabric_hybrid/group_manager/ranks/1/ext"] = {0x01};
    std::vector<Bytes> ext;
    EXPECT_EQ(store_->LoadRankExternal(1U, ext), ERROR);
}

/* ================================================================== */
/*  Flush thread lifecycle                                            */
/* ================================================================== */

TEST_F(EtcdStateStoreTest, FlushThreadWritesOnlyWhenDirty)
{
    store_->StartFlushThread();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_SETTLE_MS));
    // No dirty flag set → no Put.
    EXPECT_TRUE(backend_->putKeys.empty());

    store_->FlushLinks({LINK_CONNECTED, LINK_IDLE}, K_LINK_COUNT_TWO);
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    store_->StopFlushThread();

    ASSERT_EQ(backend_->putKeys.size(), 1U);
    EXPECT_EQ(backend_->putKeys[0], "/memfabric_hybrid/group_manager/links");
}

TEST_F(EtcdStateStoreTest, FlushThreadDisabledBackendDoesNotStart)
{
    backend_->isDistributed = false;
    store_->StartFlushThread();
    EXPECT_FALSE(store_->IsEnabled());
    store_->StopFlushThread();
}

/* ================================================================== */
/*  Recover paths                                                     */
/* ================================================================== */

TEST_F(EtcdStateStoreTest, RecoverSkipsWhenNotDistributed)
{
    backend_->isDistributed = false;
    SmemGroupCommandSender sender = [](uint32_t, const std::vector<uint8_t> &) -> int { return 0; };
    auto server = SmMakeRef<SmemGroupManagerServer>(sender, 4);
    EXPECT_EQ(store_->Recover(server.Get()), SUCCESS);
}

TEST_F(EtcdStateStoreTest, RecoverFreshStartWhenNoEpoch)
{
    SmemGroupCommandSender sender = [](uint32_t, const std::vector<uint8_t> &) -> int { return 0; };
    auto server = SmMakeRef<SmemGroupManagerServer>(sender, 4);
    EXPECT_EQ(store_->Recover(server.Get()), SUCCESS);
}

} // namespace smem
} // namespace ock
