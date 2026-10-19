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

#include <functional>
#include <sstream>

#define private public // Test access to heartbeat timeout parsing.
#include "smem_tcp_config_store_server.h"
#undef private
#include "smem_local_memory_backend.h"
#include "smem_cluster_status.h"
#include "smem_config_store_api.h"

namespace ock {
namespace smem {
namespace {

constexpr uint32_t K_WORLD_SIZE = 4;
constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_STATE_RECOVERED = 2;
constexpr uint32_t K_STATE_NORMAL = 3;
constexpr uint32_t K_STATE_EXITED = 4;
constexpr uint32_t K_DEFAULT_HEARTBEAT_TIMEOUT_S = 60;
constexpr uint32_t K_HEARTBEAT_TIMEOUT_S = 15;
constexpr uint32_t K_MAX_HEARTBEAT_TIMEOUT_S = 3600;
constexpr uint32_t K_DEFAULT_LINK_RETRY_TIMEOUT_S = 2;
constexpr uint32_t K_LINK_RETRY_TIMEOUT_S = 5;
constexpr uint32_t K_MIN_LINK_RETRY_TIMEOUT_S = 1;
constexpr uint32_t K_MAX_LINK_RETRY_TIMEOUT_S = 600;
constexpr uint32_t K_RETRY_COUNT = 2;
constexpr int64_t K_MIDDLE_STATE_ELAPSED_MS = 500;
constexpr uint32_t K_TRUNC_WORLD_SIZE = 24; // 24*23=552 对中间态链路，超过 256 条输出上限

/* Hand-written mock — no gmock dependency */
class MockBackend : public ConfigStoreBackend {
public:
    bool isDistributed = false;
    bool supportsTTL = false;
    std::string backendName = "MockBackend";

    /* Callbacks for Get — set to override default behavior */
    std::function<StoreErrorCode(const std::string &, std::vector<uint8_t> &)> onGet;

    /* Pre-programmed defaults */
    StoreErrorCode getResult = NOT_EXIST;
    StoreErrorCode putResult = SUCCESS;
    StoreErrorCode deleteResult = SUCCESS;
    StoreErrorCode initResult = SUCCESS;
    StoreErrorCode existResult = NOT_EXIST;

    std::string BackendName() const noexcept override
    {
        return backendName;
    }

    StoreErrorCode Get(const std::string &key, std::vector<uint8_t> &value) const noexcept override
    {
        if (onGet) {
            return onGet(key, value);
        }
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
        (void)key;
        (void)value;
        (void)ttl;
        return putResult;
    }

    StoreErrorCode Delete(const std::string &key) noexcept override
    {
        (void)key;
        return deleteResult;
    }

    StoreErrorCode Exist(const std::string &key) const noexcept override
    {
        (void)key;
        return existResult;
    }

    void Clear() noexcept override {}

    bool IsDistributed() const noexcept override
    {
        return isDistributed;
    }

    bool SupportsTTL() const noexcept override
    {
        return supportsTTL;
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
        return initResult;
    }

    void UnInitialize() override {}
};

// 统计子串出现次数（用于校验 unstableLinks 封顶后的条目数）
size_t CountOccurrences(const std::string &text, const std::string &sub)
{
    size_t count = 0;
    for (size_t pos = text.find(sub); pos != std::string::npos; pos = text.find(sub, pos + sub.size())) {
        ++count;
    }
    return count;
}

} // anonymous namespace

TEST(StoreServerStateTest, EnumValues)
{
    EXPECT_EQ(SS_INITED, 0);
    EXPECT_EQ(SS_RECOVERING, 1);
    EXPECT_EQ(SS_RECOVERED, K_STATE_RECOVERED);
    EXPECT_EQ(SS_NORMAL, K_STATE_NORMAL);
    EXPECT_EQ(SS_EXITED, K_STATE_EXITED);
}

class AccStoreServerTest : public ::testing::Test {
protected:
    StoreBackendPtr backend_;
    AccStoreServerPtr server_;

    void SetUp() override
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = false;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockBackend";
        rawBackend->initResult = SUCCESS;
        backend_ = StoreBackendPtr(rawBackend);
        server_ = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, backend_, true);
    }

    void TearDown() override
    {
        if (server_ != nullptr) {
            server_->Shutdown();
        }
    }

    StoreBackendPtr MakeDistributedBackend()
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = true;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockDistBackend";
        rawBackend->initResult = SUCCESS;
        rawBackend->getResult = NOT_EXIST;
        rawBackend->putResult = SUCCESS;
        rawBackend->deleteResult = SUCCESS;
        return StoreBackendPtr(rawBackend);
    }
};

TEST_F(AccStoreServerTest, HeartbeatTimeoutParsesSecondsAndZero)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("0"), 0U);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("15"), K_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3600"), K_MAX_HEARTBEAT_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, HeartbeatTimeoutMissingOrInvalidUsesDefault)
{
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS(""), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("-1"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("abc"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseHeartbeatTimeoutS("3601"), K_DEFAULT_HEARTBEAT_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, LinkRetryTimeoutParsesSecondsWithinRange)
{
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("1"), K_MIN_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("5"), K_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("600"), K_MAX_LINK_RETRY_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, LinkRetryTimeoutMissingOrInvalidUsesDefault)
{
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS(""), K_DEFAULT_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("-1"), K_DEFAULT_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("abc"), K_DEFAULT_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("0"), K_DEFAULT_LINK_RETRY_TIMEOUT_S);
    EXPECT_EQ(AccStoreServer::ParseLinkRetryTimeoutS("601"), K_DEFAULT_LINK_RETRY_TIMEOUT_S);
}

TEST_F(AccStoreServerTest, RestoreFromBackend_NonDistributedReturnsOk)
{
    EXPECT_EQ(server_->RestoreFromBackend(), SM_OK);
}

TEST_F(AccStoreServerTest, GetStatus_NonDistributedReturnsTrue)
{
    EXPECT_TRUE(server_->GetStatus());
}

TEST_F(AccStoreServerTest, UpdateStatus_NonDistributedReturnsOk)
{
    EXPECT_EQ(server_->UpdateStatus(true), SM_OK);
    EXPECT_EQ(server_->UpdateStatus(false), SM_OK);
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendWithRanks)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_ALIVE_RANK_LIST) {
            out = {'0', ',', '2'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendNoRanks)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        (void)out;
        if (key == KEY_ALIVE_RANK_LIST) {
            return NOT_EXIST;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, GetStatus_DistributedBackendLeaderActive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_LEADER_STATUS) {
            out = {'t', 'r', 'u', 'e'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_TRUE(server->GetStatus());
    server->Shutdown();
}

TEST_F(AccStoreServerTest, GetStatus_DistributedBackendLeaderInactive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        if (key == KEY_LEADER_STATUS) {
            out = {'f', 'a', 'l', 's', 'e'};
            return SUCCESS;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_FALSE(server->GetStatus());
    server->Shutdown();
}

TEST_F(AccStoreServerTest, UpdateStatus_DistributedBackendSetActive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->putResult = SUCCESS;

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->UpdateStatus(true), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, UpdateStatus_DistributedBackendSetInactive)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->deleteResult = SUCCESS;

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->UpdateStatus(false), SM_OK);
    server->Shutdown();
}

TEST_F(AccStoreServerTest, RestoreFromBackend_DistributedBackendGetFails)
{
    auto distBackend = MakeDistributedBackend();
    auto *mock = static_cast<MockBackend *>(distBackend.Get());
    mock->onGet = [](const std::string &key, std::vector<uint8_t> &out) -> StoreErrorCode {
        (void)out;
        if (key == KEY_ALIVE_RANK_LIST) {
            return ERROR;
        }
        return NOT_EXIST;
    };

    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    EXPECT_EQ(server->RestoreFromBackend(), SM_OK);
    server->Shutdown();
}

/* cluster status JSON：覆盖字段排序、状态/集合反映、折叠摘要与 GM 空对象豁免（mf 集群查询接口） */
TEST_F(AccStoreServerTest, ClusterStatusJsonReflectsStateAndSets)
{
    server_->state_.store(SS_NORMAL);
    server_->worldSize_ = 4;
    server_->aliveRankFromBackend_ = {2, 1};
    server_->reconnectedRankSet_ = {1};
    server_->aliveRankSet_ = {1};
    server_->heartBeatMap_[0] = 100;
    server_->heartBeatMap_[1] = 200;
    server_->linkRankMap_[0] = 0; // heartBeatMap_ 的 key 是 linkId，需经映射才能按 rank 输出
    server_->linkRankMap_[1] = 1;
    // GM：rank0/1 ACTIVE 且互联，rank2 CHECKED_IN；aliveRanks={1} 下无异常链路
    auto &gm = *server_->groupManager_.Get();
    gm.states_[0] = RANK_ACTIVE;
    gm.states_[1] = RANK_ACTIVE;
    gm.states_[2] = RANK_CHECKED_IN;
    gm.links_[0 * K_WORLD_SIZE + 1] = LINK_CONNECTED;
    gm.links_[1 * K_WORLD_SIZE + 0] = LINK_CONNECTED;

    const std::string json = server_->GetClusterStatusJson();
    /* 状态机/规模/端点与集合字段 */
    EXPECT_NE(json.find("\"state\":\"SS_NORMAL\""), std::string::npos);
    EXPECT_NE(json.find("\"worldSize\":4"), std::string::npos);
    EXPECT_NE(json.find("\"listen\":\"127.0.0.1:0\""), std::string::npos);
    EXPECT_NE(json.find("\"backendAliveRanks\":[1,2]"), std::string::npos);
    EXPECT_NE(json.find("\"aliveRanks\":[1]"), std::string::npos);
    EXPECT_NE(json.find("\"reconnectedRanks\":[1]"), std::string::npos);
    /* 心跳集：linkId 映射回 rank 后按 rank 排序输出，间隔为距上次心跳的毫秒数 */
    EXPECT_NE(json.find("{\"rank\":0,\"sinceLastMs\":"), std::string::npos);
    EXPECT_NE(json.find("{\"rank\":1,\"sinceLastMs\":"), std::string::npos);
    EXPECT_LT(json.find("{\"rank\":0,\"sinceLastMs\":"), json.find("{\"rank\":1,\"sinceLastMs\":"));
    /* GM rank 状态取值域为 RANK_* */
    EXPECT_NE(json.find("\"state\":\"RANK_ACTIVE\""), std::string::npos);
    EXPECT_NE(json.find("\"state\":\"RANK_CHECKED_IN\""), std::string::npos);
    /* 折叠契约：默认响应不含 activeLinks 全量矩阵，替换为 linkSummary + unstableLinks */
    EXPECT_EQ(json.find("\"activeLinks\""), std::string::npos);
    EXPECT_NE(json.find("\"linkSummary\":{\"activeRanks\":2,\"connectedLinks\":2,\"expectedLinks\":2,"
                        "\"completionRate\":1.000,\"abnormalLinks\":0}"),
              std::string::npos);
    EXPECT_NE(json.find("\"unstableLinks\":[]"), std::string::npos);
}

/* unstableLinks 用例①：中间态链路（EXCHANGING）收录，携带 retry 与 elapsedMs（非 -1） */
TEST_F(AccStoreServerTest, ClusterStatusUnstableLinksIncludeMiddleState)
{
    auto &gm = *server_->groupManager_.Get();
    server_->aliveRankSet_ = {0};
    gm.states_[0] = RANK_ACTIVE;
    gm.states_[1] = RANK_ACTIVE;
    const uint32_t idx = 0 * K_WORLD_SIZE + 1;
    gm.links_[idx] = LINK_EXCHANGING;
    gm.transitions_[idx] = {std::chrono::steady_clock::now() - std::chrono::milliseconds(K_MIDDLE_STATE_ELAPSED_MS),
                            K_RETRY_COUNT};

    const std::string json = server_->GetClusterStatusJson();
    EXPECT_NE(json.find("\"src\":0,\"dst\":1,\"state\":\"LINK_EXCHANGING\",\"retry\":2,\"elapsedMs\":"),
              std::string::npos);
    EXPECT_EQ(json.find("\"elapsedMs\":-1"), std::string::npos); // 中间态必有状态持续时长
    EXPECT_NE(json.find("\"linkSummary\":{\"activeRanks\":2,\"connectedLinks\":0,\"expectedLinks\":2,"
                        "\"completionRate\":0.000,\"abnormalLinks\":0}"),
              std::string::npos);
}

/* 分离部署观测：ranks 数组带上角色；剪枝对（PROXY<->PROXY）不计入 expectedLinks/abnormal/unstable */
TEST_F(AccStoreServerTest, ClusterStatusReportsRoleAndExcludesPrunedPairs)
{
    auto &gm = *server_->groupManager_.Get();
    server_->aliveRankSet_ = {0, 1, K_RANK_TWO};
    gm.states_[0] = RANK_ACTIVE;
    gm.states_[1] = RANK_ACTIVE;
    gm.states_[K_RANK_TWO] = RANK_ACTIVE;
    gm.rankRole_[0] = SMEM_GROUP_RANK_ROLE_PROXY;
    gm.rankRole_[1] = SMEM_GROUP_RANK_ROLE_PROXY;
    gm.rankRole_[K_RANK_TWO] = SMEM_GROUP_RANK_ROLE_PROVIDER;

    const std::string json = server_->GetClusterStatusJson();
    /* 现场直接可见谁是 Proxy */
    EXPECT_NE(json.find("{\"rank\":0,\"state\":\"RANK_ACTIVE\",\"role\":\"proxy\"}"), std::string::npos);
    EXPECT_NE(json.find("{\"rank\":1,\"state\":\"RANK_ACTIVE\",\"role\":\"proxy\"}"), std::string::npos);
    EXPECT_NE(json.find("{\"rank\":2,\"state\":\"RANK_ACTIVE\",\"role\":\"provider\"}"), std::string::npos);
    /* 3 个 ACTIVE rank 共 6 条有向链路，其中 0<->1 是剪枝对 → expectedLinks=4、abnormalLinks=4（另 4 条 IDLE） */
    EXPECT_NE(json.find("\"linkSummary\":{\"activeRanks\":3,\"connectedLinks\":0,\"expectedLinks\":4,"
                        "\"completionRate\":0.000,\"abnormalLinks\":4}"),
              std::string::npos);
    /* 剪枝对不进 unstableLinks 明细 */
    EXPECT_EQ(json.find("\"src\":0,\"dst\":1"), std::string::npos);
    EXPECT_EQ(json.find("\"src\":1,\"dst\":0"), std::string::npos);
}

/* unstableLinks 用例②：IDLE 且双端存活 = 断链，双向收录且 elapsedMs=-1 */
TEST_F(AccStoreServerTest, ClusterStatusUnstableLinksIncludeIdleWhenBothEndsAlive)
{
    auto &gm = *server_->groupManager_.Get();
    server_->aliveRankSet_ = {0, 1};
    gm.states_[0] = RANK_ACTIVE;
    gm.states_[1] = RANK_ACTIVE;
    // 链路矩阵保持默认 IDLE：0↔1 双端存活却无链路，双向均应收录

    const std::string json = server_->GetClusterStatusJson();
    EXPECT_NE(json.find("\"src\":0,\"dst\":1,\"state\":\"LINK_IDLE\",\"retry\":0,\"elapsedMs\":-1"), std::string::npos);
    EXPECT_NE(json.find("\"src\":1,\"dst\":0,\"state\":\"LINK_IDLE\",\"retry\":0,\"elapsedMs\":-1"), std::string::npos);
    EXPECT_NE(json.find("\"abnormalLinks\":2"), std::string::npos);
    EXPECT_NE(json.find("\"connectedLinks\":0"), std::string::npos);
}

/* unstableLinks 用例③：IDLE 且单端已死 = 预期状态，不收录（避免 rank 掉线制造海量噪声） */
TEST_F(AccStoreServerTest, ClusterStatusUnstableLinksExcludeIdleWhenOneEndDead)
{
    auto &gm = *server_->groupManager_.Get();
    server_->aliveRankSet_ = {0}; // rank1 已掉线
    gm.states_[0] = RANK_ACTIVE;
    gm.states_[1] = RANK_ACTIVE;

    const std::string json = server_->GetClusterStatusJson();
    EXPECT_NE(json.find("\"unstableLinks\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"abnormalLinks\":0"), std::string::npos);
    EXPECT_EQ(json.find("\"src\":0,\"dst\":1"), std::string::npos);
    EXPECT_EQ(json.find("\"src\":1,\"dst\":0"), std::string::npos);
}

/* unstableLinks 用例④：超出 kUnstableLinksMaxEntries 封顶截断并输出 unstableTruncated 计数 */
TEST_F(AccStoreServerTest, ClusterStatusUnstableLinksTruncatedToCap)
{
    auto server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_TRUNC_WORLD_SIZE, backend_, true);
    auto &gm = *server->groupManager_.Get();
    for (uint32_t src = 0; src < K_TRUNC_WORLD_SIZE; ++src) {
        for (uint32_t dst = 0; dst < K_TRUNC_WORLD_SIZE; ++dst) {
            if (src != dst) {
                gm.links_[src * K_TRUNC_WORLD_SIZE + dst] = LINK_EXCHANGING;
            }
        }
    }
    const uint32_t totalLinks = K_TRUNC_WORLD_SIZE * (K_TRUNC_WORLD_SIZE - 1);

    const std::string json = server->GetClusterStatusJson();
    EXPECT_EQ(CountOccurrences(json, "\"state\":\"LINK_EXCHANGING\""), kUnstableLinksMaxEntries);
    EXPECT_NE(json.find("\"unstableTruncated\":" + std::to_string(totalLinks - kUnstableLinksMaxEntries)),
              std::string::npos);
    EXPECT_EQ(json.find("\"activeLinks\""), std::string::npos);
    server->Shutdown();
}

/* C API：无 store 进程的空集语义（状态为 "[]" → SMEM_NOT_EXIST，buf 写入 "[]"） */
TEST_F(AccStoreServerTest, ClusterStatusCApiEmptySemantics)
{
    char buf[8];
    uint32_t len = 0;

    const int32_t ret = smem_config_store_get_cluster_status(buf, sizeof(buf), &len);
    EXPECT_NE(ret, SMEM_INVALID_PARAM);
    if (ret == SMEM_OK) {
        EXPECT_GE(len, 2U);
        EXPECT_EQ(buf[0], '[');
    } else {
        EXPECT_EQ(ret, SMEM_NOT_EXIST);
        EXPECT_EQ(std::string(buf, len), "[]");
    }
}

} // namespace smem
} // namespace ock
