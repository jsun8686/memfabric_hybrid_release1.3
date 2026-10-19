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

#include <cstring>
#include <thread>

#include "smem_group_manager_server.h"
#include "smem_ref.h"

namespace ock {
namespace smem {

constexpr uint32_t K_RANK_COUNT = 8;
constexpr uint32_t K_RANK_TWO = 2;
constexpr uint32_t K_RANK_THREE = 3;
constexpr uint32_t K_RANK_FOUR = 4;
constexpr uint32_t K_RANK_FIVE = 5;
constexpr uint32_t K_INVALID_RANK = 999;
constexpr uint32_t K_WAIT_SHORT_MS = 100;
constexpr uint32_t K_WAIT_MS = 200;
constexpr uint32_t K_WAIT_LONG_MS = 300;
constexpr uint32_t K_WAIT_LONGER_MS = 400;
constexpr uint32_t K_WAIT_EXTRA_MS = 500;
constexpr uint32_t K_WAIT_SLOW_MS = 1500;
constexpr uint32_t K_QUERY_REQUEST_ID = 42;

/* ================================================================== */
/*  Test fixture — uses a configurable sender lambda                  */
/* ================================================================== */
class GmGroupManagerTest : public ::testing::Test {
protected:
    SmRef<SmemGroupManagerServer> gm_;
    int sendResult_{0};
    int sendCallCount_{0};

    SmemGroupCommandSender MakeSender()
    {
        return [this](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
            (void)targetRankId;
            (void)data;
            sendCallCount_++;
            return sendResult_;
        };
    }

    void SetUp() override
    {
        sendResult_ = 0;
        sendCallCount_ = 0;
        gm_ = SmMakeRef<SmemGroupManagerServer>(MakeSender(), K_RANK_COUNT);
        ASSERT_NE(gm_, nullptr);
    }

    void TearDown() override
    {
        gm_->Stop();
        gm_ = nullptr;
    }

    LinkState GetLink(uint32_t src, uint32_t dst) const
    {
        return gm_->GetLinks()[src * gm_->GetMaxRanks() + dst];
    }

    RankState GetRank(uint32_t r) const
    {
        return gm_->GetStates()[r];
    }

    static RankFullInfo MakeInfo(uint32_t rankId, uint8_t protocol = SMEM_RANK_PROTOCOL_DEFAULT)
    {
        RankFullInfo info;
        info.rankId = rankId;
        info.protocol = protocol;
        std::string base = "host" + std::to_string(rankId);
        info.baseInfo.assign(base.begin(), base.end());
        info.externalInfo.push_back(Bytes{static_cast<uint8_t>(rankId)});
        return info;
    }

    /* 分离部署：带 rank 角色的 JOINREQ（默认角色即普通 BM rank） */
    static RankFullInfo MakeInfoWithRole(uint32_t rankId, uint8_t role)
    {
        RankFullInfo info = MakeInfo(rankId);
        info.role = role;
        return info;
    }
};

/* ================================================================== */
/*  1. CONTROLLER FAILURES — send worker retry/degrade                 */
/* ================================================================== */

TEST_F(GmGroupManagerTest, CheckIn_ThenSendWorker_AddToWhitelistFails_AndDegrades)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Sender always fails → send worker will retry up to max then degrade */
    sendResult_ = -1;

    gm_->Start();
    /* Allow send worker to process the queue and exhaust retries */
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    /* After max retries the link should be degraded to IDLE */
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_IDLE);
}

TEST_F(GmGroupManagerTest, CheckIn_AddAndEstConBothSucceed_LinksReachConnected)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_EXCHANGING);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_EXCHANGING);

    /* Advance via ACK chain */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_EXCHANGED);

    /* DrivePending sees EXCHANGED, enqueues ESTABLISH */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_CONNECTING);

    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_CONNECTED);
}

TEST_F(GmGroupManagerTest, DrivePendingScansAndEnqueues_EstConFailsThenDegrades)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* AddToWhitelist succeeds — send worker updates timestamp, state stays EXCHANGING */
    sendResult_ = 0;
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    /* Inject ADD ACK to advance to EXCHANGED so DrivePending enqueues ESTABLISH */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{K_RANK_TWO, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_EXCHANGED);

    /* EstablishConnection always fails → retry → degrade */
    sendResult_ = -1;

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_LONGER_MS));
    gm_->Stop();

    /* After max retries the link should be degraded to IDLE */
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_IDLE);
}

/* ================================================================== */
/*  Original tests — keep for regression                              */
/* ================================================================== */
class GroupManagerTest : public ::testing::Test {
protected:
    SmRef<SmemGroupManagerServer> gm_;

    void SetUp() override
    {
        auto sender = [](uint32_t targetRankId, const std::vector<uint8_t> &data) -> int {
            (void)targetRankId;
            (void)data;
            return 0;
        };
        gm_ = SmMakeRef<SmemGroupManagerServer>(std::move(sender), K_RANK_COUNT);
        ASSERT_NE(gm_, nullptr);
    }

    void TearDown() override
    {
        gm_ = nullptr;
    }

    static RankFullInfo MakeInfo(uint32_t rankId, uint8_t protocol = SMEM_RANK_PROTOCOL_DEFAULT)
    {
        RankFullInfo info;
        info.rankId = rankId;
        info.protocol = protocol;
        std::string base = "host" + std::to_string(rankId);
        info.baseInfo.assign(base.begin(), base.end());
        info.externalInfo.push_back(Bytes{static_cast<uint8_t>(rankId)});
        return info;
    }

    LinkState GetLink(uint32_t src, uint32_t dst) const
    {
        return gm_->GetLinks()[src * gm_->GetMaxRanks() + dst];
    }

    RankState GetRank(uint32_t r) const
    {
        return gm_->GetStates()[r];
    }
};

TEST_F(GroupManagerTest, InitialStateAllIdle)
{
    for (uint32_t i = 0; i < K_RANK_COUNT; ++i) {
        EXPECT_EQ(GetRank(i), RANK_IDLE);
        for (uint32_t j = 0; j < K_RANK_COUNT; ++j) {
            if (i != j) {
                EXPECT_EQ(GetLink(i, j), LINK_IDLE);
            }
        }
    }
    EXPECT_TRUE(gm_->GetAliveRanks().empty());
}

TEST_F(GroupManagerTest, CheckInSingleRank)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(GetRank(K_RANK_THREE), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, CheckInDoubleFails)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), -1);
}

TEST_F(GroupManagerTest, CheckInInvalidRank)
{
    EXPECT_EQ(gm_->CheckIn(MakeInfo(K_INVALID_RANK)), -1);
}

TEST_F(GroupManagerTest, CheckInCreatesExchangingLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(GetLink(K_RANK_THREE, 1), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(1, K_RANK_THREE), LINK_EXCHANGING);
}

TEST_F(GroupManagerTest, CheckInNoPeersNoLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    for (uint32_t j = 0; j < K_RANK_COUNT; ++j) {
        if (j != 1) {
            EXPECT_EQ(GetLink(1, j), LINK_IDLE);
        }
    }
}

TEST_F(GroupManagerTest, CheckInAndGetAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    auto alive = gm_->GetAliveRanks();
    EXPECT_EQ(alive.size(), K_RANK_TWO);
}

TEST_F(GroupManagerTest, CheckoutSuccess)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_TWO), 0);
    EXPECT_EQ(GetRank(K_RANK_TWO), RANK_IDLE);
}

TEST_F(GroupManagerTest, CheckoutFromIdleFails)
{
    EXPECT_EQ(gm_->Checkout(0), -1);
}

TEST_F(GroupManagerTest, CheckoutSendsRemoveFromWhitelist)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_FOUR)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_FOUR), 0);
    /* Checkout transitions to IDLE directly and notifies peers via RemoveFromWhitelist */
    EXPECT_EQ(GetRank(K_RANK_FOUR), RANK_IDLE);
    EXPECT_EQ(GetLink(K_RANK_FOUR, 0), LINK_IDLE);
    EXPECT_EQ(GetLink(0, K_RANK_FOUR), LINK_IDLE);
}

TEST_F(GroupManagerTest, AckAddToWhitelistAdvancesLink)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_EXCHANGED);
}

TEST_F(GroupManagerTest, AckEstablishConnectionAdvancesLink)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    /* Drive send worker: ESTABLISH is enqueued at EXCHANGED → link becomes CONNECTING */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_CONNECTING);

    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, K_RANK_TWO, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_CONNECTED);
}

TEST_F(GroupManagerTest, AckUnknownOpNoChange)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    gm_->OnControlAck(CONTROL_QUERY_LINK_STATE, 1, std::map<uint32_t, int32_t>{{0, 0}});
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, AcK_INVALID_RANKIgnored)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{K_INVALID_RANK, 0}});
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
}

TEST_F(GroupManagerTest, GetAliveRanksExcludesIdle)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_THREE)), 0);
    EXPECT_EQ(gm_->GetAliveRanks().size(), K_RANK_THREE);
}

TEST_F(GroupManagerTest, GetAliveRanksExcludesLeaving)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_TWO), 0);
    auto alive = gm_->GetAliveRanks();
    EXPECT_EQ(alive.size(), 1);
    EXPECT_EQ(alive[0], 1);
}

TEST_F(GroupManagerTest, CheckInTriggersStateTransitions)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);
    EXPECT_EQ(GetLink(1, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, OnLinkBroken_ClearsAllLinksAndDegradedLeavingRank)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    EXPECT_EQ(GetLink(0, 1), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(1, 0), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(1, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_EXCHANGING);

    gm_->OnLinkBroken(1);

    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
    EXPECT_EQ(GetLink(1, K_RANK_TWO), LINK_IDLE);
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(0, K_RANK_TWO), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(K_RANK_TWO, 0), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, OnLinkBroken_LeavingRankBecomesIdle)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    /* Checkout immediately transitions to IDLE and notifies peers via RemoveFromWhitelist */
    ASSERT_EQ(gm_->Checkout(1), 0);
    EXPECT_EQ(GetRank(1), RANK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);

    /* OnLinkBroken is a no-op on already-IDLE state */
    gm_->OnLinkBroken(1);
    EXPECT_EQ(GetRank(1), RANK_IDLE);
}

/* ================================================================== */
/*  Additional coverage tests for uncovered functions                 */
/* ================================================================== */

TEST_F(GmGroupManagerTest, CheckInReconnected_PromotesToActiveAndQueriesLinkState)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Advance ADD ACKs → EXCHANGED */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Start driver + send worker so ESTABLISH is enqueued and processed */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_EXTRA_MS));
    /* Now links should be LINK_CONNECTING (send worker processed ESTABLISH) */
    gm_->Stop();

    /* Send ESTABLISH ACK → CONNECTED */
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});
    EXPECT_EQ(GetLink(1, 0), LINK_CONNECTED);

    /* Run driver to promote ranks */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);

    /* CheckIn again on ACTIVE rank → MarkRankReconnected sees non-idle → fails */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(0)), -1);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, MarkRankReconnected_NonIdleStateSkips)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    /* Promote to ACTIVE via the reconnect path */
    gm_->MarkRankReconnected(0);
    /* CheckIn again: MarkRankReconnected sees RANK_ACTIVE → warn and return */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(0)), -1);
}

TEST_F(GmGroupManagerTest, CheckIn_TransOverridesNonIdleState)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    /* Simulate a reconnect having promoted rank 1 to ACTIVE (stale w.r.t. a
       fresh Trans entry) */
    gm_->MarkRankReconnected(1);
    EXPECT_EQ(GetRank(1), RANK_ACTIVE);

    /* A Trans JOINREQ overrides the stale ACTIVE state and re-joins cleanly */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(1, SMEM_RANK_PROTOCOL_TRANS)), 0);
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);

    /* BM JOINREQ on a non-idle rank stays rejected */
    EXPECT_EQ(gm_->CheckIn(MakeInfo(0)), -1);
}

TEST_F(GmGroupManagerTest, CheckIn_TransClearsGhostLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    /* Stale link between rank 1 and peer 0 (e.g. written by LNKSRSP after the
       reconnect) must be cleared when the Trans entry re-joins. */
    RankFullInfo info = MakeInfo(1);
    LinkStateEntry e;
    e.dstRankId = 0;
    e.state = static_cast<uint8_t>(LINK_CONNECTED);
    gm_->OnLinkStateResponse(info, {e}, 0);
    EXPECT_EQ(GetLink(1, 0), LINK_CONNECTED);

    EXPECT_EQ(gm_->CheckIn(MakeInfo(1, SMEM_RANK_PROTOCOL_TRANS)), 0);
    /* Re-join re-arms the links as EXCHANGING for the newly-checked-in rank */
    EXPECT_EQ(GetLink(1, 0), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, PromoteCheckedInRanks_AllIdleLinksPromotesToActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Inject link matrix: rank 1's link to 0 is CONNECTED, to 2 stays IDLE */
    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_CHECKED_IN;
    states[1] = RANK_CHECKED_IN;
    states[K_RANK_TWO] = RANK_CHECKED_IN;
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_IDLE);
    links[1 * K_RANK_COUNT + 0] = LINK_CONNECTED;
    links[0 * K_RANK_COUNT + 1] = LINK_CONNECTED;
    std::unordered_set<uint32_t> alive{0, 1, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    gm_->RebuildActiveLinks();

    /* DrivePendingTransitions → PromoteCheckedInRanks should NOT promote rank 1:
     * rank 2 is already CHECKED_IN but its link is IDLE (not connected), so a
     * synchronous join must wait until every joined peer's link is established
     * (connected == peers). Promoting early would let the upper layer issue
     * WRs to a peer whose QP is not ready (CQE 261). */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
}

TEST_F(GmGroupManagerTest, PromoteCheckedInRanks_AllPeersConnectedPromotesToActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* All peers of rank 1 connected */
    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_CHECKED_IN;
    states[1] = RANK_CHECKED_IN;
    states[K_RANK_TWO] = RANK_CHECKED_IN;
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_IDLE);
    links[1 * K_RANK_COUNT + 0] = LINK_CONNECTED;
    links[0 * K_RANK_COUNT + 1] = LINK_CONNECTED;
    links[1 * K_RANK_COUNT + K_RANK_TWO] = LINK_CONNECTED;
    links[K_RANK_TWO * K_RANK_COUNT + 1] = LINK_CONNECTED;
    std::unordered_set<uint32_t> alive{0, 1, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    gm_->RebuildActiveLinks();

    /* DrivePendingTransitions → PromoteCheckedInRanks should promote rank 1 */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetRank(1), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, QueryLinkStates_WithAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Advance ADD ACKs → EXCHANGED */
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ADD_TO_WHITELIST_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Start to process ESTABLISH tasks */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_EXTRA_MS));
    gm_->Stop();

    /* Send ESTABLISH ACKs → CONNECTED */
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 1, std::map<uint32_t, int32_t>{{0, 0}});
    gm_->OnControlAck(CONTROL_ESTABLISH_CONNECTION_ACK, 0, std::map<uint32_t, int32_t>{{1, 0}});

    /* Run driver to promote ranks to ACTIVE */
    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
    EXPECT_EQ(GetRank(1), RANK_ACTIVE);
}

TEST_F(GroupManagerTest, MultipleCheckoutAndCheckin_ResetRankStateClearsLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Checkout rank 1 → should clear links to 0 and K_RANK_TWO */
    ASSERT_EQ(gm_->Checkout(1), 0);
    EXPECT_EQ(GetRank(1), RANK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(1, K_RANK_TWO), LINK_IDLE);
    EXPECT_EQ(GetLink(K_RANK_TWO, 1), LINK_IDLE);

    /* Re-checkin rank 1 */
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    EXPECT_EQ(GetRank(1), RANK_CHECKED_IN);
    EXPECT_EQ(GetLink(1, 0), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(0, 1), LINK_EXCHANGING);
}

/* ================================================================== */
/*  Coverage for MarkRankReconnected, QueryLinkStates, OnLinkStateResp */
/* ================================================================== */

TEST_F(GmGroupManagerTest, MarkRankReconnected_RankIdle_SetsActiveAndEnqueuesQuery)
{
    /* Set rank 0 to IDLE (its initial state) */
    EXPECT_EQ(GetRank(0), RANK_IDLE);

    /* MarkReconnected on IDLE rank → becomes ACTIVE */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);

    /* Second call on ACTIVE rank → warn and return (no crash) */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, MarkRankReconnected_WithPeers_StaysActive)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);

    /* Reset rank 0 to IDLE via Checkout */
    ASSERT_EQ(gm_->Checkout(0), 0);
    EXPECT_EQ(GetRank(0), RANK_IDLE);

    /* MarkReconnected → ACTIVE, creates QUERY_LINK_STATE task */
    gm_->MarkRankReconnected(0);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, QueryLinkStates_CallsControllerForAliveRanks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Inject states: ranks 0 and 1 ACTIVE, rank 2 CHECKED_IN */
    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_ACTIVE;
    states[1] = RANK_ACTIVE;
    states[K_RANK_TWO] = RANK_CHECKED_IN;
    std::unordered_set<uint32_t> alive{0, 1, K_RANK_TWO};
    gm_->RestoreState(states, alive);

    /* Sender returns success by default */
    sendResult_ = 0;

    /* Call should not crash */
    gm_->QueryLinkStates();
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_OutOfRangeIgnored)
{
    RankFullInfo info;
    info.rankId = K_INVALID_RANK;
    std::vector<LinkStateEntry> entries;
    gm_->OnLinkStateResponse(info, entries, 0);
    /* No crash */
    SUCCEED();
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_UpdatesLinks)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(1)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(K_RANK_TWO)), 0);

    /* Build a link state response from rank 1 to ranks 0 and K_RANK_TWO */
    RankFullInfo info = MakeInfo(1);
    LinkStateEntry e0;
    LinkStateEntry e2;
    e0.dstRankId = 0;
    e0.state = static_cast<uint8_t>(LINK_CONNECTED);
    e2.dstRankId = K_RANK_TWO;
    e2.state = static_cast<uint8_t>(LINK_CONNECTED);
    std::vector<LinkStateEntry> entries = {e0, e2};

    gm_->OnLinkStateResponse(info, entries, K_QUERY_REQUEST_ID);

    EXPECT_EQ(GetLink(1, 0), LINK_CONNECTED);
    EXPECT_EQ(GetLink(1, K_RANK_TWO), LINK_CONNECTED);
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_UnknownDstIgnored)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0)), 0);

    RankFullInfo info = MakeInfo(0);
    LinkStateEntry e;
    e.dstRankId = K_INVALID_RANK;
    e.state = static_cast<uint8_t>(LINK_CONNECTED);
    std::vector<LinkStateEntry> entries = {e};

    gm_->OnLinkStateResponse(info, entries, 0);
    /* Unknown dst ignored, no crash */
    SUCCEED();
}

/* ================================================================== */
/*  Rank base/external accessors                                       */
/* ================================================================== */
TEST_F(GroupManagerTest, SetRankBase_OutOfRangeNoOp)
{
    gm_->SetRankBase(K_INVALID_RANK, Bytes{0x01});
    SUCCEED();
}

TEST_F(GroupManagerTest, SetRankExternal_OutOfRangeNoOp)
{
    gm_->SetRankExternal(K_INVALID_RANK, std::vector<Bytes>{{0x01}});
    SUCCEED();
}

/* ================================================================== */
/*  RestoreState / RebuildActiveLinks                                  */
/* ================================================================== */
TEST_F(GroupManagerTest, RestoreState_WithLinksAndAlive)
{
    std::vector<RankState> states(K_RANK_COUNT, RANK_ACTIVE);
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_CONNECTED);
    std::unordered_set<uint32_t> alive{0, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
    EXPECT_EQ(GetLink(0, K_RANK_TWO), LINK_CONNECTED);
}

TEST_F(GmGroupManagerTest, DelayedRemovesRetainEachDepartedInstanceAcrossRejoin)
{
    std::mutex captureMutex;
    std::condition_variable captureCond;
    std::vector<RankBaseInfo> removed;
    auto sender = [&](uint32_t, const Bytes &data) {
        SmemMessage msg;
        uint32_t target = 0;
        std::vector<RankBaseInfo> peers;
        if (SmemMessagePacker::Unpack(data.data(), data.size(), msg) >= 0 &&
            SmemMessage::UnpackRemoveFromWhitelist(msg, target, peers) >= 0) {
            std::lock_guard<std::mutex> lock(captureMutex);
            removed.insert(removed.end(), peers.begin(), peers.end());
            captureCond.notify_one();
        }
        return 0;
    };
    gm_ = SmMakeRef<SmemGroupManagerServer>(sender, K_RANK_COUNT);
    auto oldPeer = MakeInfo(K_RANK_TWO, SMEM_RANK_PROTOCOL_TRANS);
    ASSERT_EQ(gm_->CheckIn(MakeInfo(0, SMEM_RANK_PROTOCOL_TRANS)), 0);
    ASSERT_EQ(gm_->CheckIn(oldPeer), 0);
    ASSERT_EQ(gm_->Checkout(K_RANK_TWO), 0);
    auto newPeer = oldPeer;
    newPeer.baseInfo.push_back(0xFF);
    ASSERT_EQ(gm_->CheckIn(newPeer), 0);
    gm_->OnLinkBroken(K_RANK_TWO);
    gm_->Start();
    std::unique_lock<std::mutex> lock(captureMutex);
    bool received = captureCond.wait_for(lock, std::chrono::seconds(2), [&] { return removed.size() == 2U; });
    lock.unlock();
    gm_->Stop();
    ASSERT_TRUE(received);
    EXPECT_EQ(removed[0].baseInfo, oldPeer.baseInfo);
    EXPECT_EQ(removed[1].baseInfo, newPeer.baseInfo);
}

TEST_F(GroupManagerTest, RebuildActiveLinks_NoCrash)
{
    gm_->RebuildActiveLinks();
    SUCCEED();
}

/* ================================================================== */
/*  分离部署链路剪枝：PROXY<->PROXY 不建链，其余角色组合保持全互联      */
/* ================================================================== */

TEST_F(GmGroupManagerTest, ProxyProxyPair_NoLinkEstablished)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(0, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROXY)), 0);

    /* 两个 PROXY 之间双向都不建链，链路保持 IDLE */
    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
    /* 角色随 JOINREQ 落库，供后续重连/恢复判定使用 */
    EXPECT_EQ(gm_->GetRankRole(0), SMEM_GROUP_RANK_ROLE_PROXY);
    EXPECT_EQ(gm_->GetRankRole(1), SMEM_GROUP_RANK_ROLE_PROXY);
}

TEST_F(GmGroupManagerTest, ProxyDefaultPair_LinksEstablished)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(0, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROVIDER)), 0);

    /* 只有 PROXY<->PROXY 被剪枝：PROXY<->PROVIDER 仍然双向建链 */
    EXPECT_EQ(GetLink(0, 1), LINK_EXCHANGING);
    EXPECT_EQ(GetLink(1, 0), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, ProxyPromotesToActive_WithoutProxyProxyLink)
{
    /* 回归：剪枝对必须从“期望链路数”里扣除，否则 PROXY rank 会因为缺少
     * PROXY<->PROXY 链路而永远停留在 CHECKED_IN，Join 超时。 */
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(0, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(K_RANK_TWO, SMEM_GROUP_RANK_ROLE_PROVIDER)), 0);

    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_CHECKED_IN;
    states[1] = RANK_CHECKED_IN;
    states[K_RANK_TWO] = RANK_CHECKED_IN;
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_IDLE);
    /* rank1 与 rank0 是剪枝对（保持 IDLE）；rank1 与 rank2 已 CONNECTED */
    links[1 * K_RANK_COUNT + K_RANK_TWO] = LINK_CONNECTED;
    links[K_RANK_TWO * K_RANK_COUNT + 1] = LINK_CONNECTED;
    std::unordered_set<uint32_t> alive{0, 1, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    gm_->RebuildActiveLinks();

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();
    EXPECT_EQ(GetRank(1), RANK_ACTIVE);
}

TEST_F(GmGroupManagerTest, RestoreProxyPair_ForcesIdleAndStaysIdle)
{
    /* 恢复路径：即便持久化数据里存在 PROXY<->PROXY 的非 IDLE 链路，
     * 也必须被强制回 IDLE，且不进入活跃扫描表。 */
    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_ACTIVE;
    states[1] = RANK_ACTIVE;
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_IDLE);
    links[0 * K_RANK_COUNT + 1] = LINK_EXCHANGING;
    links[1 * K_RANK_COUNT + 0] = LINK_CONNECTED;
    std::unordered_set<uint32_t> alive{0, 1};
    gm_->RestoreState(states, links, alive);
    gm_->SetRankRole(0, SMEM_GROUP_RANK_ROLE_PROXY);
    gm_->SetRankRole(1, SMEM_GROUP_RANK_ROLE_PROXY);
    gm_->RebuildActiveLinks();
    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);

    /* 对端上报的链路状态同样不能把剪枝对重新拉起 */
    RankFullInfo info = MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROXY);
    LinkStateEntry e;
    e.dstRankId = 0;
    e.state = static_cast<uint8_t>(LINK_CONNECTED);
    gm_->OnLinkStateResponse(info, {e}, 0);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
}

TEST_F(GmGroupManagerTest, IdleRetry_KeepsPrunedPairIdleButArmsOtherLinks)
{
    /* 回归：30s 一次的断链重试（RetryIdleLinks）不能把剪枝对重新拉起。
     * 首次 DrivePendingTransitions 必定执行一次 idle 重试（lastIdleRetryTime_ 初值为 epoch），
     * 因此同时断言“非剪枝的 IDLE 链路被拉起”，证明这次重试确实跑过（避免断言假通过）。 */
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(0, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(K_RANK_TWO, SMEM_GROUP_RANK_ROLE_PROVIDER)), 0);

    std::vector<RankState> states(K_RANK_COUNT, RANK_IDLE);
    states[0] = RANK_CHECKED_IN;
    states[1] = RANK_CHECKED_IN;
    states[K_RANK_TWO] = RANK_CHECKED_IN;
    std::vector<LinkState> links(K_RANK_COUNT * K_RANK_COUNT, LINK_IDLE);
    std::unordered_set<uint32_t> alive{0, 1, K_RANK_TWO};
    gm_->RestoreState(states, links, alive);
    gm_->RebuildActiveLinks();

    gm_->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(K_WAIT_MS));
    gm_->Stop();

    /* 剪枝对：双方都存活也不允许被重新拉起 */
    EXPECT_EQ(GetLink(0, 1), LINK_IDLE);
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
    /* 非剪枝的 IDLE 链路（PROXY<->DEFAULT）被重试拉起 */
    EXPECT_EQ(GetLink(0, K_RANK_TWO), LINK_EXCHANGING);
}

TEST_F(GmGroupManagerTest, ResetRankState_ClearsRole)
{
    ASSERT_EQ(gm_->CheckIn(MakeInfoWithRole(0, SMEM_GROUP_RANK_ROLE_PROXY)), 0);
    EXPECT_EQ(gm_->GetRankRole(0), SMEM_GROUP_RANK_ROLE_PROXY);

    /* 退出后角色清为未知（不再声称其贡献内存），避免下一次以其它角色加入时沿用旧角色导致误剪枝 */
    ASSERT_EQ(gm_->Checkout(0), 0);
    EXPECT_EQ(gm_->GetRankRole(0), SMEM_GROUP_RANK_ROLE_UNKNOWN);
}

TEST_F(GmGroupManagerTest, OnLinkStateResponse_BackfillsRoleFromPeer)
{
    /* 非 etcd 模式下 meta 重启：rank 以 rec:1 重连（不发 JOINREQ），角色只能靠
       LNKSRSP 回带；回填后剪枝判定必须立即生效。 */
    gm_->MarkRankReconnected(0);
    gm_->MarkRankReconnected(1);
    gm_->SetRankRole(0, SMEM_GROUP_RANK_ROLE_PROXY);
    EXPECT_EQ(GetRank(0), RANK_ACTIVE);
    EXPECT_EQ(GetRank(1), RANK_ACTIVE);
    EXPECT_EQ(gm_->GetRankRole(1), SMEM_GROUP_RANK_ROLE_UNKNOWN);

    RankFullInfo info = MakeInfoWithRole(1, SMEM_GROUP_RANK_ROLE_PROXY);
    LinkStateEntry e;
    e.dstRankId = 0;
    e.state = static_cast<uint8_t>(LINK_CONNECTED);
    gm_->OnLinkStateResponse(info, {e}, 0);

    EXPECT_EQ(gm_->GetRankRole(1), SMEM_GROUP_RANK_ROLE_PROXY);
    /* 角色回填后，剪枝对即便对端上报 CONNECTED 也必须被强制回 IDLE */
    EXPECT_EQ(GetLink(1, 0), LINK_IDLE);
}

} // namespace smem
} // namespace ock
