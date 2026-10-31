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

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <sstream>
#include <thread>

#define private public // Test access to heartbeat timeout parsing.
#include "smem_tcp_config_store_server.h"
#undef private
#include "acc_tcp_server_default.h"
#include "acc_tcp_link_complex_default.h"
#include "smem_local_memory_backend.h"
#include "smem_cluster_status.h"
#include "smem_config_store_api.h"
#include "mf_str_util.h"

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
constexpr int64_t K_HEARTBEAT_AGE_OLDER_MS = 100;
constexpr int64_t K_HEARTBEAT_AGE_NEWER_MS = 40;
/* 心跳豁免判定：静默 90s（>60s 阈值），其中 31s 归因于本进程不可观测 */
constexpr int64_t K_HEARTBEAT_RAW_ELAPSED_MS = 90000;
constexpr uint64_t K_HEARTBEAT_UNOBSERVABLE_MS = 31000;

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

    /* 记录被删除的 key，用于断言持久化存活名单是否被误删 */
    std::vector<std::string> deletedKeys;

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
        deletedKeys.push_back(key);
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

/* 桩 acc 服务：仅覆写 GetWorkerUnobservableMs 以注入非零的 worker 不可观测时长，
   使心跳巡检的"扣除"逻辑在 UT 中真正生效（真实 acc 服务需启动 worker 线程且计数不可控）。 */
class StubAccTcpServer : public ock::acc::AccTcpServerDefault {
public:
    uint64_t unobservableMs{0};

    uint64_t GetWorkerUnobservableMs(uint32_t workerIdx) const override
    {
        (void)workerIdx;
        return unobservableMs;
    }
};

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
    MockBackend *mockBackend_{nullptr};
    AccStoreServerPtr server_;

    void SetUp() override
    {
        auto *rawBackend = new MockBackend();
        rawBackend->isDistributed = false;
        rawBackend->supportsTTL = false;
        rawBackend->backendName = "MockBackend";
        rawBackend->initResult = SUCCESS;
        mockBackend_ = rawBackend;
        backend_ = StoreBackendPtr(rawBackend);
        server_ = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, backend_, true);
    }

    /* 构造心跳记录：stampMs 取"距今 elapsedMs"，unobsMs/workerIdx 可注入以覆盖豁免场景 */
    AccStoreServer::HeartbeatRecord MakeHeartbeat(int64_t elapsedMs, uint64_t unobsMs = 0, uint32_t workerIdx = 0)
    {
        AccStoreServer::HeartbeatRecord record;
        record.stampMs = AccStoreServer::NowMonotonicMs() - elapsedMs;
        record.unobsMs = unobsMs;
        record.workerIdx = workerIdx;
        return record;
    }

    /* 统计某 key 被删除的次数 */
    size_t CountDelete(const std::string &key)
    {
        return mockBackend_ == nullptr ? 0U
                                       : static_cast<size_t>(std::count(mockBackend_->deletedKeys.begin(),
                                                                        mockBackend_->deletedKeys.end(), key));
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

TEST_F(AccStoreServerTest, HeartbeatCheckerRemovesExpiredLinkAndKeepsLiveLink)
{
    server_->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    const auto timeout = std::chrono::seconds(K_DEFAULT_HEARTBEAT_TIMEOUT_S);
    server_->heartBeatMap_[0] = MakeHeartbeat(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    server_->heartBeatMap_[1] = MakeHeartbeat(0);
    server_->linkRankMap_[0] = 0;
    server_->linkRankMap_[1] = 1;
    server_->aliveRankSet_ = {0, 1};
    server_->state_.store(SS_NORMAL);

    std::promise<uint32_t> expiredLink;
    auto result = expiredLink.get_future();
    server_->RegisterBrokenLinkCHandler(
        [&expiredLink](uint32_t linkId, StoreBackendPtr &) { expiredLink.set_value(linkId); });
    std::thread checker([this]() { server_->CheckerThreadTask(); });
    const auto status = result.wait_for(std::chrono::seconds(2));
    server_->state_.store(SS_EXITED);
    server_->storeCond_.notify_all();
    checker.join();
    server_->RegisterBrokenLinkCHandler(nullptr);

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(result.get(), 0U);
    EXPECT_EQ(server_->heartBeatMap_.count(0), 0U);
    EXPECT_EQ(server_->heartBeatMap_.count(1), 1U);
    EXPECT_EQ(server_->aliveRankSet_.count(0), 0U);
    EXPECT_EQ(server_->aliveRankSet_.count(1), 1U);
}

TEST_F(AccStoreServerTest, HeartbeatSnapshotReportsElapsedMilliseconds)
{
    constexpr int64_t kOlderMs = 1250;
    constexpr int64_t kNewerMs = 40;
    const int64_t now = AccStoreServer::NowMonotonicMs();
    /* 两条记录共用同一 now：若各自调用 NowMonotonicMs()，两次取时刻可能差 1ms，
       下面"两链路静默时长之差"的断言会偶发失败 */
    server_->heartBeatMap_[0] = AccStoreServer::HeartbeatRecord{now - kOlderMs, 0U, 0U};
    server_->heartBeatMap_[1] = AccStoreServer::HeartbeatRecord{now - kNewerMs, 0U, 0U};
    server_->linkRankMap_[0] = 0;
    server_->linkRankMap_[1] = 1;

    const auto snapshot = server_->CaptureClusterStatus();
    const int64_t elapsedMs = AccStoreServer::NowMonotonicMs() - now;
    ASSERT_EQ(snapshot.heartbeats.size(), 2U);
    EXPECT_EQ(snapshot.heartbeats[0].first, 0U);
    EXPECT_EQ(snapshot.heartbeats[1].first, 1U);
    EXPECT_GE(snapshot.heartbeats[0].second, kOlderMs);
    EXPECT_LE(snapshot.heartbeats[0].second, kOlderMs + elapsedMs);
    EXPECT_EQ(snapshot.heartbeats[0].second - snapshot.heartbeats[1].second, kOlderMs - kNewerMs);
}

/* 判定语义：静默时长扣除"本进程不可观测窗口"后再与阈值比较，且不放松阈值本身 */
TEST_F(AccStoreServerTest, HeartbeatExpireJudgementDeductsUnobservableMs)
{
    constexpr int64_t kTimeoutMs = static_cast<int64_t>(K_DEFAULT_HEARTBEAT_TIMEOUT_S) * 1000;
    /* 无豁免：静默 90s > 60s → 超时 */
    EXPECT_TRUE(AccStoreServer::IsHeartbeatExpired(K_HEARTBEAT_RAW_ELAPSED_MS, 0U, kTimeoutMs));
    /* 静默 90s 中 31s 为本进程不可观测 → 有效静默 59s < 60s → 不超时 */
    EXPECT_FALSE(
        AccStoreServer::IsHeartbeatExpired(K_HEARTBEAT_RAW_ELAPSED_MS, K_HEARTBEAT_UNOBSERVABLE_MS, kTimeoutMs));
    /* 有效静默恰好等于阈值 → 超时（扣除后仍需达到阈值，阈值未放松） */
    const int64_t kExpiredRawMs = kTimeoutMs + static_cast<int64_t>(K_HEARTBEAT_UNOBSERVABLE_MS);
    EXPECT_TRUE(AccStoreServer::IsHeartbeatExpired(kExpiredRawMs, K_HEARTBEAT_UNOBSERVABLE_MS, kTimeoutMs));
    /* 巡检关闭（timeoutMs<=0）时永不算超时 */
    EXPECT_FALSE(AccStoreServer::IsHeartbeatExpired(K_HEARTBEAT_RAW_ELAPSED_MS, 0U, 0));
}

/* 扣除量纯函数：快照差按静默时长封顶；快照未增长（含回绕）或静默时长非正时扣除为 0 */
TEST_F(AccStoreServerTest, UnobservableDeltaClampedBySilenceDuration)
{
    /* 正常差：快照 100→200ms，静默 1000ms → 扣除 100ms */
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(100U, 200U, 1000), 100U);
    /* 超过静默时长被封顶：快照 0→5000ms（worker 长时间饥饿），静默 1000ms → 扣除封顶 1000ms */
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(0U, 5000U, 1000), 1000U);
    /* 快照差为 0：无可扣除窗口 */
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(100U, 100U, 1000), 0U);
    /* 快照回绕/负差（cur < prev）：按 0 处理，不得产生无符号下溢 */
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(500U, 100U, 1000), 0U);
    /* 静默时长非正（时刻未前进）：扣除为 0 */
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(0U, 5000U, 0), 0U);
    EXPECT_EQ(AccStoreServer::ClampUnobservableDeltaMs(0U, 5000U, -5), 0U);
}

/* 心跳超时不等价于对端退出（也可能只是本窗口内未观测到心跳），不得删除持久化的存活名单；删除有两条路径，二者都要拦住 */
TEST_F(AccStoreServerTest, HeartbeatTimeoutMustNotDeletePersistedAliveRanks)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);
    server->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;

    /* 模拟真实链路：linkRankMap_ 中存在该链路，aliveRankSet_ 中只剩它 */
    server->linkRankMap_[0] = 0;
    server->aliveRankSet_ = {0};
    server->LinkBrokenHandler(0U, AccStoreServer::BrokenReason::HEARTBEAT_TIMEOUT);
    EXPECT_EQ(server->aliveRankSet_.size(), 0U);
    EXPECT_EQ(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);

    /* 真实断链仍须清理持久化名单（可能由上述两条路径各删一次，故只断言"发生过删除"） */
    server->linkRankMap_[1] = 1;
    server->aliveRankSet_ = {1};
    server->LinkBrokenHandler(1U, AccStoreServer::BrokenReason::TCP_BROKEN);
    EXPECT_GT(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
}

/* 一参重载（另一条可能绕过存活名单保护的路径）：ReleaseLink 触发的断链回调不携带原因，
   靠 heartbeatTimeoutLinkIds_ 预置标记 + 回调发生在巡检线程上来还原为 HEARTBEAT_TIMEOUT：
   摘除 rank，但不得删除 KEY_ALIVE_RANK_LIST。 */
TEST_F(AccStoreServerTest, HeartbeatTimeoutOneArgHandlerRestoresReasonAndKeepsPersistedAliveRanks)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);
    server->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    /* 模拟巡检线程身份：ReleaseLink 触发的回调在巡检线程上同步执行 */
    server->checkerThreadId_ = std::this_thread::get_id();

    server->linkRankMap_[0] = 0;
    server->aliveRankSet_ = {0};
    server->heartbeatTimeoutLinkIds_.insert(0); // CheckerThreadTask 在 ReleaseLink 前预置的标记

    ock::acc::AccTcpLinkComplexPtr link = new ock::acc::AccTcpLinkComplexDefault(-1, "127.0.0.1:0", 0U);
    server->LinkBrokenHandler(link); // 一参重载：不携带断链原因

    EXPECT_EQ(server->linkRankMap_.count(0U), 0U);
    EXPECT_EQ(server->aliveRankSet_.size(), 0U);
    /* 还原为 HEARTBEAT_TIMEOUT：不得删除持久化存活名单 */
    EXPECT_EQ(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
    /* 预置标记已被消费，不会残留误标后续真实断链 */
    EXPECT_EQ(server->heartbeatTimeoutLinkIds_.count(0U), 0U);
}

/* 防御：预置标记残留时，真实 TCP 断链（回调不在巡检线程上）必须按 TCP_BROKEN 处理，
   不得被判成心跳超时，从而漏删 KEY_ALIVE_RANK_LIST（该删不删）。 */
TEST_F(AccStoreServerTest, RealTcpBrokenNotMisjudgedByStaleHeartbeatMarker)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);
    server->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    server->checkerThreadId_ = std::this_thread::get_id(); // 巡检线程 = 本线程

    server->linkRankMap_[0] = 0;
    server->aliveRankSet_ = {0};
    server->heartbeatTimeoutLinkIds_.insert(0); // 残留的预置标记

    /* acc worker 线程上报真实断链：回调不在巡检线程上 */
    ock::acc::AccTcpLinkComplexPtr link = new ock::acc::AccTcpLinkComplexDefault(-1, "127.0.0.1:0", 0U);
    std::thread worker([&server, &link] { server->LinkBrokenHandler(link); });
    worker.join();

    /* 真实断链须清理持久化名单 */
    EXPECT_GT(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
}

/* 混合场景：前面若干 rank 因心跳超时被摘除（持久化名单仍保留它们），最后一条链路是真实断链。
   此时同样集群全断，删除会连带丢掉那些 rank 的恢复窗口，故判据必须是"删下去会丢什么"，
   而不是只看触发本次全断的断链原因；反之，保留项已被后续写入覆盖时仍须正常删除（该删不删也不允许）。 */
TEST_F(AccStoreServerTest, TcpBrokenAfterHeartbeatTimeoutsKeepsPersistedAliveRanks)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);
    server->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;

    /* 上次持久化成功的集合为 {0,1}（镜像由 PersistAliveRankIds 维护，与后端内容一致） */
    ASSERT_EQ(server->PersistAliveRankIds({0, 1}), SUCCESS);
    EXPECT_EQ(server->persistedAliveRanks_, (std::unordered_set<uint32_t>{0, 1}));
    server->aliveRankSet_ = {0, 1};
    server->linkRankMap_[0] = 0;
    server->linkRankMap_[1] = 1;

    /* rank0 心跳超时摘除：不写回名单，rank0 仍留在持久化名单里 */
    server->LinkBrokenHandler(0U, AccStoreServer::BrokenReason::HEARTBEAT_TIMEOUT);
    EXPECT_EQ(server->aliveRankSet_.size(), 1U);

    /* 最后一条链路真实断链 → 集群全断，但名单里还有待恢复的 rank0，不得整份删除 */
    server->LinkBrokenHandler(1U, AccStoreServer::BrokenReason::TCP_BROKEN);
    EXPECT_EQ(server->aliveRankSet_.size(), 0U);
    EXPECT_EQ(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);

    /* 保留项已被后续持久化覆盖（镜像与内存一致）后，真实断链仍须清理名单 */
    ASSERT_EQ(server->PersistAliveRankIds({2}), SUCCESS);
    server->aliveRankSet_ = {2};
    server->linkRankMap_[2] = 2;
    server->LinkBrokenHandler(2U, AccStoreServer::BrokenReason::TCP_BROKEN);
    EXPECT_GT(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
}

/* 第三条删除路径（换届恢复窗口 + 心跳超时）：CleanupStaleRanks 也会经 PersistAliveRankIds 持久化，
   清理后存活集为空时不得删掉 KEY_ALIVE_RANK_LIST。
   失效链：恢复中心跳超时摘除的 rank 会被同时移出 reconnectedRankSet_，故被 cleanup 视作 stale rank
   从 aliveRankSet_ 剔除；若窗口内其余链路也都是心跳超时摘除，存活集变空 → 老写法直接删除名单。 */
TEST_F(AccStoreServerTest, CleanupStaleRanksKeepsPersistedAliveRanksOnHeartbeatTimeout)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);
    server->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    /* 换届恢复：后端恢复出 rank1，内存集合与镜像都从 {1} 起步 */
    server->aliveRankFromBackend_ = {1};
    server->aliveRankSet_ = {1};
    server->persistedAliveRanks_ = {1};
    server->reconnectedRankSet_ = {1};
    server->linkRankMap_[0] = 1;
    /* rank1 重连后又被心跳超时摘除：同时被移出 reconnectedRankSet_ → cleanup 视其为 stale rank */
    server->LinkBrokenHandler(0U, AccStoreServer::BrokenReason::HEARTBEAT_TIMEOUT);
    ASSERT_TRUE(server->aliveRankSet_.empty());

    /* 清理 stale rank：存活集变空，但镜像里仍有待恢复的 rank1，不得删除名单 */
    server->shouldStop_.store(true); // 令 CleanupStaleRanks 不阻塞在收尾等待上
    server->CleanupStaleRanks();
    EXPECT_EQ(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
    EXPECT_EQ(server->persistedAliveRanks_, (std::unordered_set<uint32_t>{1}));

    /* 反例：镜像中没有保留项时，空集合仍须正常删除名单（护栏不能退化成"永不删除"） */
    server->persistedAliveRanks_.clear();
    server->aliveRankSet_.clear();
    server->aliveRankFromBackend_.clear();
    server->CleanupStaleRanks();
    EXPECT_GT(std::count(distRaw->deletedKeys.begin(), distRaw->deletedKeys.end(), KEY_ALIVE_RANK_LIST), 0);
}

/* 换届恢复：RestoreFromBackend 需同时播种 aliveRankSet_ 与镜像 persistedAliveRanks_（两者都在
   storeMutex_ 保护下写入，避免与 acc worker 线程上的 LinkBrokenHandler 并发读写同一容器）。 */
TEST_F(AccStoreServerTest, RestoreFromBackendSeedsAliveSetAndPersistedMirror)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    distRaw->onGet = [](const std::string &key, std::vector<uint8_t> &value) {
        if (key == KEY_ALIVE_RANK_LIST) {
            const std::string ranks = "0,3";
            value.assign(ranks.begin(), ranks.end());
            return SUCCESS;
        }
        return NOT_EXIST;
    };
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);

    ASSERT_EQ(server->RestoreFromBackend(), SM_OK);
    EXPECT_EQ(server->aliveRankSet_, (std::unordered_set<uint32_t>{0, 3}));
    EXPECT_EQ(server->persistedAliveRanks_, (std::unordered_set<uint32_t>{0, 3}));
}

/* 外部断链回调（memcache 侧 ClearResource → RemoveBlobs，不可重入的全表扫描）：
   1) 必须在 storeMutex_ 临界区之外调用（否则长持锁会阻塞心跳写入与巡检）；
   2) 多个 acc worker 线程并发断链时，回调体仍须被串行化。 */
TEST_F(AccStoreServerTest, ExternalBrokenCallbackRunsOutsideStoreMutexAndIsSerialized)
{
    auto *distRaw = new MockBackend();
    distRaw->isDistributed = true;
    distRaw->initResult = SUCCESS;
    StoreBackendPtr distBackend(distRaw);
    AccStoreServerPtr server = SmMakeRef<AccStoreServer>("127.0.0.1", 0, K_WORLD_SIZE, distBackend, true);
    ASSERT_TRUE(server != nullptr);
    server->state_.store(SS_NORMAL);

    /* 单线程路径：回调里直接 try_lock storeMutex_，能拿到说明调用点确实在锁外 */
    bool storeMutexFreeInCallback = false;
    server->RegisterBrokenLinkCHandler([&server, &storeMutexFreeInCallback](uint32_t, StoreBackendPtr &) {
        if (server->storeMutex_.try_lock()) {
            storeMutexFreeInCallback = true;
            server->storeMutex_.unlock();
        }
    });
    server->LinkBrokenHandler(50U, AccStoreServer::BrokenReason::TCP_BROKEN);
    EXPECT_TRUE(storeMutexFreeInCallback);

    /* 并发路径：多线程同时断链，回调体不得并发进入 */
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    std::atomic<int> callbackCount{0};
    server->RegisterBrokenLinkCHandler([&](uint32_t, StoreBackendPtr &) {
        const int cur = inFlight.fetch_add(1) + 1;
        int prevMax = maxInFlight.load();
        while (cur > prevMax && !maxInFlight.compare_exchange_weak(prevMax, cur)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        inFlight.fetch_sub(1);
        callbackCount.fetch_add(1);
    });

    constexpr int kThreads = 4;
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&server, i]() {
            server->LinkBrokenHandler(100U + static_cast<uint32_t>(i), AccStoreServer::BrokenReason::TCP_BROKEN);
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }

    EXPECT_EQ(callbackCount.load(), kThreads);
    EXPECT_EQ(maxInFlight.load(), 1);
    server->RegisterBrokenLinkCHandler(nullptr);
}

/* 巡检端到端：静默超阈值的链路被剔除，健康链路保留，且超时路径不删持久化名单 */
TEST_F(AccStoreServerTest, HeartbeatCheckerExpiryDeductsUnobservableAndSkipsPersistedDelete)
{
    server_->state_.store(SS_NORMAL);
    server_->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    server_->heartBeatMap_[0] = MakeHeartbeat(61000); // 61s > 60s 阈值
    server_->heartBeatMap_[1] = MakeHeartbeat(0);

    std::thread checker([this] { server_->CheckerThreadTask(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> guard{server_->heartBeatMutex_};
            removed = server_->heartBeatMap_.find(0U) == server_->heartBeatMap_.end();
        }
        if (removed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    server_->state_.store(SS_EXITED);
    server_->storeCond_.notify_all();
    checker.join();

    std::lock_guard<std::mutex> guard{server_->heartBeatMutex_};
    EXPECT_EQ(server_->heartBeatMap_.count(0U), 0U);
    EXPECT_EQ(server_->heartBeatMap_.count(1U), 1U);
    /* 心跳超时剔除链路后不得删除持久化存活名单 */
    EXPECT_EQ(CountDelete(KEY_ALIVE_RANK_LIST), 0U);
}

/* 巡检端到端扣除真正生效：注入非零 worker 不可观测时长（31s）后，
   静默 61s 的链路扣除 31s → 有效 30s < 60s 阈值 → 必须保留（不得误剔）；
   静默 95s 的链路扣除 31s → 有效 64s ≥ 60s → 仍被剔除。 */
TEST_F(AccStoreServerTest, HeartbeatCheckerDeductsInjectedUnobservableMs)
{
    auto *stubRaw = new StubAccTcpServer();
    stubRaw->unobservableMs = K_HEARTBEAT_UNOBSERVABLE_MS; // 注入 31s 不可观测时长
    server_->accTcpServer_ = stubRaw;
    server_->state_.store(SS_NORMAL);
    server_->heartBeatTimeoutS_ = K_DEFAULT_HEARTBEAT_TIMEOUT_S;
    server_->heartBeatMap_[0] = MakeHeartbeat(61000); // 61s：扣除后 30s → 豁免
    server_->heartBeatMap_[2] = MakeHeartbeat(95000); // 95s：扣除后 64s → 超时
    server_->linkRankMap_[0] = 0;
    server_->linkRankMap_[2] = 2;
    server_->aliveRankSet_ = {0, 2};

    std::thread checker([this] { server_->CheckerThreadTask(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool removed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> guard{server_->heartBeatMutex_};
            removed = server_->heartBeatMap_.find(2U) == server_->heartBeatMap_.end();
        }
        if (removed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    server_->state_.store(SS_EXITED);
    server_->storeCond_.notify_all();
    checker.join();

    EXPECT_TRUE(removed); // 巡检确实跑过一轮（95s 那条被判超时移除）
    std::lock_guard<std::mutex> guard{server_->heartBeatMutex_};
    EXPECT_EQ(server_->heartBeatMap_.count(0U), 1U); // 61s 那条因扣除而豁免，必须保留
    EXPECT_EQ(server_->heartBeatMap_.count(2U), 0U);
    /* 心跳超时路径不得删除持久化存活名单 */
    EXPECT_EQ(CountDelete(KEY_ALIVE_RANK_LIST), 0U);
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
    server_->heartBeatMap_[0] = MakeHeartbeat(K_HEARTBEAT_AGE_OLDER_MS);
    server_->heartBeatMap_[1] = MakeHeartbeat(K_HEARTBEAT_AGE_NEWER_MS);
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
