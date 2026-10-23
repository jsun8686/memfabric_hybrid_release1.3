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
#include <thread>
#include <chrono>
#include "hybm_transport_common.h"
#include "device_qp_common.h"
#include "hybm_types.h"

#define private   public
#define protected public
#include "joinable_ranks_qp_manager.h"
#include "aicpu_joinable_ranks_qp_manager.h"
#include "dl_hccp_def.h"
#include "dl_hccp_api.h"
#include "dl_acl_api.h"
#undef private
#undef protected

static constexpr uint16_t K_PORT_8000 = 8000;
static constexpr uint16_t K_PORT_8001 = 8001;
static constexpr uint32_t K_RANK_COUNT = 4;
static constexpr uint32_t K_RANK_ID_0 = 0;
static constexpr uint32_t K_RANK_ID_1 = 1;
static constexpr uint32_t K_RANK_ID_2 = 2;
static constexpr uint32_t K_RANK_ID_3 = 3;
static constexpr uint32_t K_OUT_OF_RANGE_RANK = 100;
static constexpr uint32_t K_OUT_OF_RANGE_INDEX = 10;
static constexpr uint32_t K_DEFAULT_SLEEP_MS = 2000;

static void *ToVoidPtr(uintptr_t i)
{
    return reinterpret_cast<void *>(i);
}

using namespace ock::mf;
using namespace ock::mf::transport::device;

// Testable subclass to mock external dependencies like CreateServerSocket().
class TestableJoinableRanksQpManager : public JoinableRanksQpManager {
public:
    using JoinableRanksQpManager::JoinableRanksQpManager;

public:
    int CreateServerSocket() noexcept
    {
        return BM_OK;
    }
    void *CreateLocalSocket() noexcept
    {
        return ToVoidPtr(0x1234);
    }
};

// 测试夹具
class JoinableRanksQpManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static constexpr uint16_t kPort8002 = 8002;
        sockaddr_in devNet{};
        devNet.sin_family = AF_INET;
        devNet.sin_port = htons(kPort8002);
        inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
        // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
        manager = std::make_unique<TestableJoinableRanksQpManager>(0, 0, K_RANK_ID_0, K_RANK_COUNT, devNet, 0);
    }

    void TearDown() override
    {
        if (manager) {
            manager->Shutdown();
        }
    }

    std::unique_ptr<TestableJoinableRanksQpManager> manager;
};

// 测试构造函数
TEST_F(JoinableRanksQpManagerTest, Constructor)
{
    EXPECT_TRUE(manager != nullptr);
    EXPECT_EQ(manager->GetQpHandleWithRankId(0), nullptr);
}

// 测试 SetRemoteRankInfo 添加远程排名、空参数、越界分支
TEST_F(JoinableRanksQpManagerTest, SetRemoteRankInfo)
{
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_1, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    int ret = manager->SetRemoteRankInfo(ranks);
    EXPECT_EQ(ret, BM_OK);
    // 空 ranks
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> emptyRanks;
    ret = manager->SetRemoteRankInfo(emptyRanks);
    EXPECT_EQ(ret, BM_OK);
    // 越界 rankId
    ranks.clear();
    ranks.emplace(K_OUT_OF_RANGE_RANK, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ret = manager->SetRemoteRankInfo(ranks);
    EXPECT_EQ(ret, BM_OK);
}

// 测试 RemoveRanks 正常、空参数、越界分支
TEST_F(JoinableRanksQpManagerTest, RemoveRanks)
{
    std::unordered_set<uint32_t> toRemove{K_RANK_ID_1, K_RANK_ID_3};
    int ret = manager->RemoveRanks(toRemove);
    EXPECT_EQ(ret, BM_OK);
    // 空 ranks
    std::unordered_set<uint32_t> emptyRemove;
    ret = manager->RemoveRanks(emptyRemove);
    EXPECT_EQ(ret, BM_OK);
    // 越界 rankId
    std::unordered_set<uint32_t> outOfRange{K_OUT_OF_RANGE_RANK};
    ret = manager->RemoveRanks(outOfRange);
    EXPECT_EQ(ret, BM_OK);
}

// 测试 RemoveRanks 会清理 newClients_ / newServers_ 中待处理的 rank
// 场景: SetRemoteRankInfo 将 rank 加入 newClients_ 后, 上层在 WaitQpConnections 完成前调用 RemoveRanks
// 如果 newClients_ 没有被清理, 后续 ServerSideRunLoop 会再次尝试连接已删除的 rank 并报 "no ip address"
TEST_F(JoinableRanksQpManagerTest, RemoveRanksClearsNewClients)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    // 设置远程 rank 2,3 的连接信息 → newClients_ = {2, 3}
    std::unordered_map<uint32_t, ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_2, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_3, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);
    EXPECT_TRUE(clientManager->newClients_.count(K_RANK_ID_2));
    EXPECT_TRUE(clientManager->newClients_.count(K_RANK_ID_3));

    // 模拟 WaitQpConnections 尚未执行, 上层已调用 RemoveRanks
    std::unordered_set<uint32_t> toRemove{K_RANK_ID_2};
    clientManager->RemoveRanks(toRemove);

    // 验证 newClients_ 中的 rank 2 已被清理, rank 3 仍然保留
    EXPECT_FALSE(clientManager->newClients_.count(K_RANK_ID_2));
    EXPECT_TRUE(clientManager->newClients_.count(K_RANK_ID_3));
}

TEST_F(JoinableRanksQpManagerTest, TransTags_DistinguishRanksSharingAnIp)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    JoinableRanksQpManager sender(0, 0, 0, 66, devNet, HYBM_ROLE_SENDER);
    JoinableRanksQpManager receiver(0, 0, 65, 66, devNet, HYBM_ROLE_RECEIVER);

    char senderTo64[HCCP_SOCK_CONN_TAG_SIZE]{};
    char senderTo65[HCCP_SOCK_CONN_TAG_SIZE]{};
    char receiverTo0[HCCP_SOCK_CONN_TAG_SIZE]{};
    sender.FillHccpTag(senderTo64, 64);
    sender.FillHccpTag(senderTo65, 65);
    receiver.FillHccpTag(receiverTo0, 0);

    EXPECT_STREQ(senderTo64, "TRANS_TAG_0_64");
    EXPECT_STREQ(senderTo65, "TRANS_TAG_0_65");
    EXPECT_STREQ(senderTo65, receiverTo0);
    EXPECT_STRNE(senderTo64, senderTo65);

    JoinableRanksQpManager bmManager(0, 0, 0, 4, devNet, HYBM_ROLE_PEER);
    char bmTag[HCCP_SOCK_CONN_TAG_SIZE]{};
    bmManager.FillHccpTag(bmTag, 1);
    EXPECT_STREQ(bmTag, "BM_TAG");
}

TEST_F(JoinableRanksQpManagerTest, RemovedRank_CancelsStaleSocketWaitBatch)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    JoinableRanksQpManager transManager(0, 0, 0, 66, devNet, HYBM_ROLE_SENDER);

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "10.20.0.2", &remote.sin_addr);
    ock::mf::transport::TransportMemoryKey key{};
    transManager.SetRemoteRankInfo({{64, ConnectRankInfo(HYBM_ROLE_RECEIVER, remote, key)},
                                    {65, ConnectRankInfo(HYBM_ROLE_RECEIVER, remote, key)}});
    ASSERT_TRUE(transManager.IsConnectBatchCurrent({64, 65}));

    transManager.RemoveRanks({64});
    EXPECT_FALSE(transManager.IsConnectBatchCurrent({64, 65}));
    EXPECT_TRUE(transManager.IsConnectBatchCurrent({65}));
}

TEST_F(JoinableRanksQpManagerTest, SameRankReplacementPreservesNewAddressAndCancelsOldReadiness)
{
    sockaddr_in oldNet{};
    oldNet.sin_family = AF_INET;
    oldNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "10.20.0.2", &oldNet.sin_addr);
    sockaddr_in newNet = oldNet;
    newNet.sin_port = htons(K_PORT_8001);
    JoinableRanksQpManager manager(0, 0, 0, 66, oldNet, HYBM_ROLE_SENDER);
    ock::mf::transport::TransportMemoryKey key{};
    manager.SetRemoteRankInfo({{64, ConnectRankInfo(HYBM_ROLE_RECEIVER, oldNet, key)}});
    manager.connections_[64].qpStatus = 1;
    EXPECT_TRUE(manager.CheckQpReady({64}));
    manager.RemoveRanks({64});
    manager.SetRemoteRankInfo({{64, ConnectRankInfo(HYBM_ROLE_RECEIVER, newNet, key)}});
    EXPECT_FALSE(manager.CheckQpReady({64}));
    EXPECT_FALSE(manager.IsConnectBatchCurrent({64}));
    EXPECT_EQ(manager.connections_[64].remoteNet.sin_port, oldNet.sin_port);
    manager.RemoveRanksProcess({64});
    EXPECT_EQ(manager.connections_[64].remoteNet.sin_port, newNet.sin_port);
    EXPECT_EQ(manager.connections_[64].remoteNet.sin_addr.s_addr, newNet.sin_addr.s_addr);
    EXPECT_TRUE(manager.IsConnectBatchCurrent({64}));
    EXPECT_FALSE(manager.CheckQpReady({64}));
    EXPECT_TRUE(manager.replacementNetworks_.empty());
}

TEST_F(JoinableRanksQpManagerTest, ScenarioRegression_NoIpAddressAfterLeave)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    auto mgr = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};

    // Phase 1: 新 rank 2 加入 → newClients_ = {2}, connections_[2] 有有效 IP
    mgr->SetRemoteRankInfo({{K_RANK_ID_2, ConnectRankInfo(HYBM_ROLE_PEER, net, mk)}});
    ASSERT_TRUE(mgr->newClients_.count(K_RANK_ID_2));
    ASSERT_NE(mgr->connections_[K_RANK_ID_2].remoteNet.sin_addr.s_addr, 0U);

    // Phase 2: 模拟管线在 GenerateWhiteList 失败 → 提前 return
    //         (UT 环境 RaSocketWhiteListAdd 调用会失败)
    //         WaitQpConnections 没走到 → rank 2 残留在 newClients_
    mgr->ServerSideHandleNewClients({K_RANK_ID_2});
    ASSERT_TRUE(mgr->newClients_.count(K_RANK_ID_2)); // 残留确认

    // Phase 3: 模拟上层触发 leave → RemoveRanks({2})
    //   (修复前): 不清理 newClients_ → mgr->newClients_ 仍有 2
    //   (修复后): 清理 newClients_   → mgr->newClients_ 已清 2
    mgr->RemoveRanks({K_RANK_ID_2});

    // Phase 4: 模拟 RemoveRanksProcess 清空 connections_[2] (IP 被清零)
    std::memset(&mgr->connections_[K_RANK_ID_2], 0, sizeof(ConnectionChannel));
    ASSERT_EQ(mgr->connections_[K_RANK_ID_2].remoteNet.sin_addr.s_addr, 0U);

    // Phase 5: ★ 模拟下一轮 ServerSideRunLoop 的拷贝行为
    //   修复前: snapshot = {2}  → 非空 → 走 GenerateWhiteList({2})
    //           connections_[2].remoteNet.s_addr = 0 → 打印 "rankId: 2, no ip address."
    //   修复后: snapshot = {}   → 空 → 不会走 GenerateWhiteList → 无错误
    auto snapshot = mgr->newClients_;

    // 如果 snapshot 非空, 显式调用 GenerateWhiteList 展示 "no ip address" 错误
    if (!snapshot.empty()) {
        // 仅在修复前会进入此分支: 触发实际的 "no ip address" 错误
        int ret = mgr->GenerateWhiteList(snapshot);
        EXPECT_EQ(ret, BM_ERROR) << "修复前: newClients_ 残留 rank " << *snapshot.begin()
                                 << ", GenerateWhiteList 返回 BM_ERROR, 日志应出现 'no ip address'.";
    }

    // ★★ 核心断言: snapshot 应为空 (修复前: 残留 → 断言失败; 修复后: 已清 → 断言通过)
    EXPECT_TRUE(snapshot.empty()) << "修复前: newClients_ 残留 rank "
                                  << (snapshot.empty() ? -1 : static_cast<int>(*snapshot.begin()))
                                  << ", 下一轮循环必调 GenerateWhiteList 并报 'no ip address'";
}

// 模拟完整异常场景: ServerSideHandleNewClients 管线未到达 WaitQpConnections 就提前返回
// (因 GenerateWhiteList 调用 HW API 失败), 随后 RemoveRanks 被调用.
// 验证修复: RemoveRanks 能清理 newClients_ 中残留的 rank, 不再报 "no ip address"
TEST_F(JoinableRanksQpManagerTest, RemoveRanksAfterPipelineFailure)
{
    // rank 1 作为 server, 接待 client rank 2, 3
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    auto serverManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    // 设置 remote rank 2, 3 的 IP 信息 → newClients_ = {2, 3}
    std::unordered_map<uint32_t, ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_2, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_3, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    serverManager->SetRemoteRankInfo(ranks);

    // Step 1: ServerSideHandleNewClients 执行管线
    //   GenerateWhiteList → UT 环境 RaSocketWhiteListAdd 失败 → return ERROR
    //   WaitSocketConnections / WaitQpConnections 没有走到，因此 rank 2, 3 残留在 newClients_ 中
    serverManager->ServerSideHandleNewClients({K_RANK_ID_2, K_RANK_ID_3});
    EXPECT_TRUE(serverManager->newClients_.count(K_RANK_ID_2));
    EXPECT_TRUE(serverManager->newClients_.count(K_RANK_ID_3));

    // Step 2: 模拟上层触发 rank 2 leave → RemoveRanks
    serverManager->RemoveRanks({K_RANK_ID_2});

    // Step 3: ★ 验证 newClients_ 中的 rank 2 被清理, rank 3 保留
    //   (修复前: rank 2 残留 → 下一轮循环报 "no ip address")
    //   (修复后: rank 2 被 RemoveRanks 同步删除)
    EXPECT_FALSE(serverManager->newClients_.count(K_RANK_ID_2));
    EXPECT_TRUE(serverManager->newClients_.count(K_RANK_ID_3));
}

// 测试 RemoveRanks 会清理 newServers_ 中待处理的 server rank
TEST_F(JoinableRanksQpManagerTest, RemoveRanksClearsNewServers)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // rank 2 作为 client, 连接 server (rank 0, 1)
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(2, 2, 2, K_RANK_COUNT, devNet, 0);

    std::unordered_map<uint32_t, ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(0, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_1, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);
    EXPECT_TRUE(clientManager->newServers_.count(0));
    EXPECT_TRUE(clientManager->newServers_.count(K_RANK_ID_1));

    // 移除 server rank 0
    std::unordered_set<uint32_t> toRemove{0};
    clientManager->RemoveRanks(toRemove);

    // 验证 newServers_ 中的 rank 0 已被清理, rank 1 仍然保留
    EXPECT_FALSE(clientManager->newServers_.count(0));
    EXPECT_TRUE(clientManager->newServers_.count(K_RANK_ID_1));
}

// 测试 Startup 和 Shutdown，空参数、重复调用分支
TEST_F(JoinableRanksQpManagerTest, StartupShutdown)
{
    void *fakeRdma = ToVoidPtr(0x1234);
    int ret = manager->Startup(fakeRdma);
    EXPECT_EQ(ret, BM_DL_FUNCTION_FAILED);
    manager->Shutdown();
    // 多次 Shutdown
    manager->Shutdown();
}

// 测试 Startup 使用空 rdma 句柄
TEST_F(JoinableRanksQpManagerTest, StartupWithNullRdma)
{
    int ret = manager->Startup(nullptr);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// 测试 GetQpHandleWithRankId 边界和异常分支
TEST_F(JoinableRanksQpManagerTest, GetQpHandleWithRankIdEdgeCases)
{
    // rankId 超出 rankCount_
    auto *qp = manager->GetQpHandleWithRankId(K_OUT_OF_RANGE_RANK);
    EXPECT_EQ(qp, nullptr);
    // qpArray_[rankId] == nullptr
    qp = manager->GetQpHandleWithRankId(K_RANK_ID_2);
    EXPECT_EQ(qp, nullptr);
}

// 测试 GetQpHandleWithRankId 在超出范围时返回 nullptr
TEST_F(JoinableRanksQpManagerTest, GetQpHandleWithRankIdOutOfRange)
{
    auto *qp = manager->GetQpHandleWithRankId(K_OUT_OF_RANGE_INDEX);
    EXPECT_EQ(qp, nullptr);
}

// 测试 PutQpHandle 边界分支
TEST_F(JoinableRanksQpManagerTest, PutQpHandle)
{
    auto *qp = new ock::mf::transport::device::UserQpInfo;
    qp->qpHandle = ToVoidPtr(0x5555);
    qp->ref.store(1);
    manager->PutQpHandle(qp);
    // ref > 1
    qp = new ock::mf::transport::device::UserQpInfo;
    qp->qpHandle = ToVoidPtr(0x5555);
    qp->ref.store(K_RANK_ID_2);
    manager->PutQpHandle(qp);
    delete qp;
}

// 测试 CheckQpReady 边界和异常分支
TEST_F(JoinableRanksQpManagerTest, CheckQpReadyEdgeCases)
{
    // 没有连接
    std::vector<uint32_t> rankIds{1, 2};
    bool ready = manager->CheckQpReady(rankIds);
    EXPECT_FALSE(ready);
    // 超出范围
    std::vector<uint32_t> outOfRange{K_OUT_OF_RANGE_INDEX};
    ready = manager->CheckQpReady(outOfRange);
    EXPECT_FALSE(ready);
    // 空 rankIds
    std::vector<uint32_t> emptyIds;
    ready = manager->CheckQpReady(emptyIds);
    EXPECT_TRUE(ready);
    // 忽略自身排名
    std::vector<uint32_t> selfIds{0};
    ready = manager->CheckQpReady(selfIds);
    EXPECT_TRUE(ready);
}

// 测试线程循环在运行标志为 false 时退出
TEST_F(JoinableRanksQpManagerTest, StartupClient)
{
    void *fakeRdma = ToVoidPtr(0x1234);
    // 初始化 manager【1卡】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);
    clientManager->Startup(fakeRdma);
    std::this_thread::sleep_for(std::chrono::milliseconds(K_OUT_OF_RANGE_RANK));
    clientManager->Shutdown();
}

// 测试线程循环在运行标志为 false 时退出
TEST_F(JoinableRanksQpManagerTest, ThreadLoopExit)
{
    void *fakeRdma = ToVoidPtr(0x1234);
    manager->Startup(fakeRdma);
    std::this_thread::sleep_for(std::chrono::milliseconds(K_OUT_OF_RANGE_RANK));
    manager->Shutdown();
}

TEST_F(JoinableRanksQpManagerTest, StartClientSide)
{
    int ret = manager->StartClientSide();
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(JoinableRanksQpManagerTest, ServerSideHandleNewClients)
{
    std::set<uint32_t> newRanks = {K_RANK_ID_0, K_RANK_ID_1, K_RANK_ID_2, K_RANK_ID_3};
    manager->ServerSideHandleNewClients(newRanks);

    // 初始化 manager【1卡】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(0, 0, K_RANK_ID_0, K_RANK_COUNT, devNet, 0);

    // 设置remoet 连接信息【2，3】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_2, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_3, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->connections_.resize(K_RANK_COUNT); // 模拟已有连接状态
    clientManager->SetRemoteRankInfo(ranks);

    std::set<uint32_t> newClients = {K_RANK_ID_2, K_RANK_ID_3};
    clientManager->connections_[K_RANK_ID_2].socketHandle = ToVoidPtr(0x1111);
    clientManager->connections_[K_RANK_ID_3].socketHandle = ToVoidPtr(0x2222);
    int ret = clientManager->GenerateWhiteList(newClients);
    // RaSocketWhiteListAdd 时失败
    EXPECT_EQ(ret, BM_OK);

    clientManager->ServerSideHandleNewClients(newClients);
}

TEST_F(JoinableRanksQpManagerTest, ServerSideRunLoop)
{
    // 设置运行标志
    manager->running_.store(true);
    // 添加一些新客户端以触发处理分支
    manager->newClients_ = {K_RANK_ID_1, K_RANK_ID_2};
    // 在单独线程中运行循环
    std::thread loopThread([this]() { manager->ServerSideRunLoop(); });
    // 等待一小段时间让循环开始
    std::this_thread::sleep_for(std::chrono::milliseconds(K_DEFAULT_SLEEP_MS));
    // 停止循环
    manager->running_.store(false);
    manager->cond_.notify_all();
    // 等待线程结束
    loopThread.join();
    // 验证循环已停止（无崩溃）
    EXPECT_FALSE(manager->running_.load());
}

TEST_F(JoinableRanksQpManagerTest, ClientSideRunLoop)
{
    // 设置运行标志
    manager->running_.store(true);
    // 添加一些新服务器以触发处理分支（假设 rankId_ = 0，服务器是较低的 ranks）
    manager->newServers_ = {K_RANK_ID_1, K_RANK_ID_2}; // 模拟新服务器连接
    // 在单独线程中运行循环
    std::thread loopThread([this]() { manager->ClientSideRunLoop(); });
    // 等待一小段时间让循环开始
    std::this_thread::sleep_for(std::chrono::milliseconds(K_DEFAULT_SLEEP_MS));
    // 停止循环
    manager->running_.store(false);
    manager->cond_.notify_all();
    // 等待线程结束
    loopThread.join();
    // 验证循环已停止（无崩溃）
    EXPECT_FALSE(manager->running_.load());
}

TEST_F(JoinableRanksQpManagerTest, WaitSocketConnections)
{
    // 测试空 newRanks
    std::set<uint32_t> emptyRanks;
    int ret = manager->WaitSocketConnections(emptyRanks);
    EXPECT_EQ(ret, BM_OK);
    // 测试非空 newRanks
    std::set<uint32_t> newRanks = {0, 1, 2};

    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<JoinableRanksQpManager>(0, 0, 0, K_RANK_COUNT, devNet, 0);

    // 设置remoet 连接信息【1，1】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(1, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_2, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);

    clientManager->connections_.resize(K_RANK_COUNT); // 模拟已有连接状态
    clientManager->connections_[1].socketHandle = ToVoidPtr(0x1111);
    clientManager->connections_[1].qpStatus = 1; // 模拟连接成功
    ret = clientManager->WaitSocketConnections(newRanks);
    // DlHccpApi::RaGetSockets 失败
    EXPECT_EQ(ret, 1);
}

TEST_F(JoinableRanksQpManagerTest, MakeQpConnections)
{
    // 空集合分支
    std::set<uint32_t> emptyRanks = {};
    manager->MakeQpConnections(emptyRanks);

    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<JoinableRanksQpManager>(0, 0, 0, K_RANK_COUNT, devNet, 0);
    std::set<uint32_t> newRanks = {K_RANK_ID_0, K_RANK_ID_1, K_RANK_ID_2};

    // 设置remoet 连接信息【1，1】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(1, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_2, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);

    clientManager->connections_.resize(K_RANK_COUNT); // 模拟已有连接状态
    clientManager->connections_[1].socketFd = ToVoidPtr(0x1111);
    clientManager->connections_[1].qpHandle = ToVoidPtr(0x2222); // 模拟连接未完成
    clientManager->connections_[1].qpConnectCalled = false;

    clientManager->connections_[K_RANK_ID_2].socketFd = ToVoidPtr(0x1111);
    clientManager->connections_[K_RANK_ID_2].qpStatus = 1; // 模拟连接成功
    clientManager->MakeQpConnections(newRanks);
}

TEST_F(JoinableRanksQpManagerTest, WaitQpConnections)
{
    // 空集合分支
    std::set<uint32_t> emptyRanks = {};
    manager->WaitQpConnections(emptyRanks);

    // 初始化 manager【1卡】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    clientManager->connections_.resize(K_RANK_COUNT); // 模拟已有连接状态
    clientManager->connections_[K_RANK_ID_3].qpHandle = ToVoidPtr(0x2222);
    clientManager->connections_[K_RANK_ID_3].qpConnectCalled = true;
    clientManager->connections_[K_RANK_ID_3].qpStatus = 1; // 模拟连接完成

    clientManager->connections_[K_RANK_ID_2].qpHandle = ToVoidPtr(0x1111);
    clientManager->connections_[K_RANK_ID_2].qpConnectCalled = true;

    // 非空集合，代码会走完的,K_RANK_ID_0 会加入server, K_RANK_ID_2,kRankId3为client, 但是没有设置status,不会进入
    std::set<uint32_t> newRanks = {K_RANK_ID_0, K_RANK_ID_2, K_RANK_ID_3};
    clientManager->WaitQpConnections(newRanks);
}

TEST_F(JoinableRanksQpManagerTest, GenerateWhiteList)
{
    // 初始化 manager【1卡】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    // 设置remoet 连接信息【2，3】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_2, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_3, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);

    std::set<uint32_t> newClients = {K_RANK_ID_2, K_RANK_ID_3};
    int ret = clientManager->GenerateWhiteList(newClients);
    // RaSocketWhiteListAdd 时失败
    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(JoinableRanksQpManagerTest, GenerateWhiteListEmpty)
{
    // 初始化 manager【1卡】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(1, 1, 1, K_RANK_COUNT, devNet, 0);

    // 设置remoet 连接信息【2，3】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_2, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_RANK_ID_3, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->connections_.resize(K_RANK_COUNT); // 模拟已有连接状态
    clientManager->SetRemoteRankInfo(ranks);

    std::set<uint32_t> newClients = {K_RANK_ID_2, K_RANK_ID_3};
    clientManager->connections_[K_RANK_ID_2].socketHandle = ToVoidPtr(0x1111);
    clientManager->connections_[K_RANK_ID_3].socketHandle = ToVoidPtr(0x2222);
    int ret = clientManager->GenerateWhiteList(newClients);
    // RaSocketWhiteListAdd 时失败
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(JoinableRanksQpManagerTest, CreateConnectionToServers)
{
    // 初始化 manager【2卡是client】
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(K_PORT_8000);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    // uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet
    auto clientManager = std::make_unique<TestableJoinableRanksQpManager>(2, 2, 2, K_RANK_COUNT, devNet, 0);

    // 设置server连接信息【0卡是server】
    std::unordered_map<uint32_t, ock::mf::transport::device::ConnectRankInfo> ranks;
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(K_PORT_8001);
    inet_pton(AF_INET, "192.168.1.1", &net.sin_addr);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(0, ock::mf::transport::device::ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    clientManager->SetRemoteRankInfo(ranks);

    // client客户端【2卡】连接 server【0卡】
    std::set<uint32_t> newServers = {0};
    int ret = clientManager->CreateConnectionToServers(newServers);
    // CreateLocalSocket依赖动态库接口，ut环境无法打通，肯定失败
    EXPECT_EQ(ret, BM_ERROR);
}

// ---------------------------------------------------------------------------
// AicpuJoinableRanksQpManager (AI QP manager) unit tests
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t K_AI_DB_INDEX = 0x00030000U;
constexpr uint64_t K_AI_QP_ADDR = 0xDEADBEEFULL;
constexpr uint16_t K_AI_PORT = 8100;

sockaddr_in MakeAiDevNet(uint16_t port)
{
    sockaddr_in devNet{};
    devNet.sin_family = AF_INET;
    devNet.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &devNet.sin_addr);
    return devNet;
}

std::unique_ptr<AicpuJoinableRanksQpManager> MakeAiManager(uint32_t rankCount = K_RANK_COUNT)
{
    return std::make_unique<AicpuJoinableRanksQpManager>(0, 0, K_RANK_ID_0, rankCount, MakeAiDevNet(K_AI_PORT),
                                                         HYBM_ROLE_PEER);
}

struct AicpuQpApiGuard {
    aclrtMallocFunc oldMalloc{DlAclApi::pAclrtMalloc};
    aclrtFreeFunc oldFree{DlAclApi::pAclrtFree};
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    raQpAiCreateFunc oldQpCreate{DlHccpApi::gRaQpAiCreate};

    ~AicpuQpApiGuard()
    {
        DlAclApi::pAclrtMalloc = oldMalloc;
        DlAclApi::pAclrtFree = oldFree;
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlHccpApi::gRaQpAiCreate = oldQpCreate;
    }
};

int32_t FakeAiMallocOk(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    if (ptr != nullptr) {
        *ptr = reinterpret_cast<void *>(0x5000UL);
    }
    return 0;
}

int32_t FakeAiMallocFail(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    if (ptr != nullptr) {
        *ptr = nullptr;
    }
    return -1;
}

int FakeAiFreeOk(void *ptr)
{
    (void)ptr;
    return 0;
}

int32_t FakeAiMemcpyOk(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    return 0;
}

int32_t FakeAiMemcpyFail(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    return -1;
}

int FakeAiQpCreateOk(void *rdmaHandle, const HccpQpExtAttrs *attr, HccpAiQpInfo *aiQpInfo, void **qpHandle)
{
    (void)rdmaHandle;
    (void)attr;
    if (qpHandle == nullptr || aiQpInfo == nullptr) {
        return -1;
    }
    *qpHandle = reinterpret_cast<void *>(0xA1A1UL);
    aiQpInfo->aiQpAddr = K_AI_QP_ADDR;
    aiQpInfo->sqIndex = 3U;
    aiQpInfo->dbIndex = K_AI_DB_INDEX;
    aiQpInfo->data_plane_info.sq.wqn = 7U;
    aiQpInfo->data_plane_info.sq.depth = 128U;
    aiQpInfo->data_plane_info.sq.buf_addr = 0x7777UL;
    return 0;
}

int FakeAiQpCreateFail(void *rdmaHandle, const HccpQpExtAttrs *attr, HccpAiQpInfo *aiQpInfo, void **qpHandle)
{
    (void)rdmaHandle;
    (void)attr;
    (void)aiQpInfo;
    if (qpHandle != nullptr) {
        *qpHandle = nullptr;
    }
    return -1;
}

} // namespace

TEST(AicpuJoinableRanksQpManagerTest, QueryEmptyState)
{
    auto manager = MakeAiManager();
    ASSERT_NE(manager, nullptr);
    EXPECT_EQ(manager->GetDeviceQpAddr(K_RANK_ID_0), 0U);
    EXPECT_EQ(manager->GetDbIndex(K_RANK_ID_0), AicpuJoinableRanksQpManager::INVALID_DB_INDEX);
    EXPECT_EQ(manager->GetQpInfoAddress(), nullptr);
}

TEST(AicpuJoinableRanksQpManagerTest, SetRemoteRankInfoSkipsOutOfRange)
{
    auto manager = MakeAiManager();
    std::unordered_map<uint32_t, ConnectRankInfo> ranks;
    sockaddr_in net = MakeAiDevNet(K_AI_PORT + 1);
    ock::mf::transport::TransportMemoryKey mk{};
    ranks.emplace(K_RANK_ID_1, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    ranks.emplace(K_OUT_OF_RANGE_RANK, ConnectRankInfo(HYBM_ROLE_PEER, net, mk));
    EXPECT_EQ(manager->SetRemoteRankInfo(ranks), BM_OK);
    EXPECT_EQ(manager->currentRanksInfo_.count(K_RANK_ID_1), 1U);
    EXPECT_EQ(manager->currentRanksInfo_.count(K_OUT_OF_RANGE_RANK), 0U);
}

TEST(AicpuJoinableRanksQpManagerTest, CreateQpHandleStoresAiQpInfo)
{
    AicpuQpApiGuard guard;
    DlHccpApi::gRaQpAiCreate = &FakeAiQpCreateOk;
    auto manager = MakeAiManager();
    manager->rdmaHandle_ = reinterpret_cast<void *>(0x1234UL);

    void *qp = nullptr;
    EXPECT_EQ(manager->CreateQpHandle(K_RANK_ID_1, qp), BM_OK);
    EXPECT_NE(qp, nullptr);
    EXPECT_EQ(manager->GetDeviceQpAddr(K_RANK_ID_1), K_AI_QP_ADDR);
    EXPECT_EQ(manager->GetDbIndex(K_RANK_ID_1), K_AI_DB_INDEX);

    DlHccpApi::gRaQpAiCreate = &FakeAiQpCreateFail;
    void *qp2 = nullptr;
    EXPECT_EQ(manager->CreateQpHandle(K_RANK_ID_2, qp2), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(manager->GetDeviceQpAddr(K_RANK_ID_2), 0U);
}

TEST(AicpuJoinableRanksQpManagerTest, ReserveFillAndReleaseQpInfo)
{
    AicpuQpApiGuard guard;
    DlAclApi::pAclrtMalloc = &FakeAiMallocOk;
    DlAclApi::pAclrtFree = &FakeAiFreeOk;
    DlAclApi::pAclrtMemcpy = &FakeAiMemcpyOk;
    DlHccpApi::gRaQpAiCreate = &FakeAiQpCreateOk;
    auto manager = MakeAiManager();
    manager->rdmaHandle_ = reinterpret_cast<void *>(0x1234UL);

    EXPECT_TRUE(manager->ReserveQpInfoSpace());
    EXPECT_TRUE(manager->ReserveQpInfoSpace());

    void *qp = nullptr;
    EXPECT_EQ(manager->CreateQpHandle(K_RANK_ID_1, qp), BM_OK);
    manager->FillQpInfo();
    EXPECT_NE(manager->GetQpInfoAddress(), nullptr);

    manager->ReleaseQpInfoSpace();
    manager->ReleaseQpInfoSpace();
    EXPECT_EQ(manager->GetQpInfoAddress(), nullptr);
}

TEST(AicpuJoinableRanksQpManagerTest, ReserveQpInfoSpaceMallocFailThenStartupFails)
{
    AicpuQpApiGuard guard;
    DlAclApi::pAclrtMalloc = &FakeAiMallocFail;
    DlAclApi::pAclrtFree = &FakeAiFreeOk;
    auto manager = MakeAiManager();
    EXPECT_FALSE(manager->ReserveQpInfoSpace());
    EXPECT_EQ(manager->Startup(reinterpret_cast<void *>(0x1234UL)), BM_MALLOC_FAILED);
}

TEST(AicpuJoinableRanksQpManagerTest, FillQpInfoDeviceCopyFails)
{
    AicpuQpApiGuard guard;
    DlAclApi::pAclrtMalloc = &FakeAiMallocOk;
    DlAclApi::pAclrtFree = &FakeAiFreeOk;
    DlAclApi::pAclrtMemcpy = &FakeAiMemcpyFail;
    auto manager = MakeAiManager();
    EXPECT_TRUE(manager->ReserveQpInfoSpace());
    manager->FillQpInfo();
}

TEST(AicpuJoinableRanksQpManagerTest, LayoutFillAndCopyHelpers)
{
    auto manager = MakeAiManager(2U);

    std::vector<uint8_t> buf(4096U, 0U);
    AiQpRMAQueueInfo head{};
    manager->SetupHostLayout(head, buf.data());
    EXPECT_EQ(head.count, 1U);
    EXPECT_NE(head.sq, nullptr);
    EXPECT_NE(head.mr, nullptr);

    manager->FillRankEntries(head); // no ranks and no ai qp info: skip branches
    manager->qpInfo_ = reinterpret_cast<AiQpRMAQueueInfo *>(0x9000UL);
    manager->FixupDevicePointers(head);
    EXPECT_NE(head.mr, nullptr);
    manager->qpInfo_ = nullptr;

    AiQpRMAWQ wq{};
    ai_data_plane_wq srcWq{};
    srcWq.wqn = 11U;
    srcWq.depth = 64U;
    srcWq.swdb_addr = 0xAAA1UL;
    srcWq.db_reg = 0xBBB1UL;
    AicpuJoinableRanksQpManager::CopyAiWQInfo(wq, srcWq, DBMode::SW_DB, 4U);
    EXPECT_EQ(wq.wqn, 11U);
    EXPECT_EQ(wq.dbAddr, 0xAAA1UL);
    AicpuJoinableRanksQpManager::CopyAiWQInfo(wq, srcWq, DBMode::HW_DB, 4U);
    EXPECT_EQ(wq.dbAddr, 0xBBB1UL);

    AiQpRMACQ cq{};
    ai_data_plane_cq srcCq{};
    srcCq.cqn = 5U;
    srcCq.swdb_addr = 0xCCC1UL;
    AicpuJoinableRanksQpManager::CopyAiCQInfo(cq, srcCq, DBMode::SW_DB);
    EXPECT_EQ(cq.cqn, 5U);
    EXPECT_EQ(cq.dbAddr, 0xCCC1UL);
}
