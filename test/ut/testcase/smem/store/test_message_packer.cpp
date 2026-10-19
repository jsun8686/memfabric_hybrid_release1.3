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
#include <arpa/inet.h>
#include "smem_message_packer.h"

using namespace ock::smem;

static RankBaseInfo MakeRankBaseInfo(uint32_t rankId, const std::vector<uint8_t> &baseInfo)
{
    RankBaseInfo info;
    info.rankId = rankId;
    info.baseInfo = baseInfo;
    return info;
}

static RankFullInfo MakeRankFullInfo(uint32_t rankId, const std::vector<uint8_t> &baseInfo,
                                     const std::vector<std::vector<uint8_t>> &externalInfo)
{
    RankFullInfo info;
    info.rankId = rankId;
    info.baseInfo = baseInfo;
    info.externalInfo = externalInfo;
    return info;
}

TEST(PackAddToWhitelist, RoundTrip_Basic)
{
    std::vector<RankFullInfo> others;
    others.push_back(MakeRankFullInfo(1, {0x01, 0x02, 0x03}, {}));
    others.push_back(MakeRankFullInfo(2, {0x04, 0x05}, {{0x10}}));

    auto msg = SmemMessage::PackAddToWhitelist(42, others);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 42);
    EXPECT_EQ(msg.keys.size(), 1u);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_ADD_TO_WHITELIST));
    EXPECT_EQ(msg.values.size(), 1u);

    uint32_t rankId = 0;
    std::vector<RankFullInfo> result;
    auto ret = SmemMessage::UnpackAddToWhitelist(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 42u);
    EXPECT_EQ(result.size(), others.size());
    for (size_t i = 0; i < others.size(); ++i) {
        EXPECT_EQ(result[i].rankId, others[i].rankId);
        EXPECT_EQ(result[i].baseInfo, others[i].baseInfo);
        EXPECT_EQ(result[i].externalInfo.size(), others[i].externalInfo.size());
        for (size_t j = 0; j < others[i].externalInfo.size(); ++j) {
            EXPECT_EQ(result[i].externalInfo[j], others[i].externalInfo[j]);
        }
    }
}

TEST(PackAddToWhitelist, RoundTrip_Empty)
{
    std::vector<RankFullInfo> others;
    auto msg = SmemMessage::PackAddToWhitelist(0, others);
    EXPECT_EQ(msg.userDef, 0);

    uint32_t rankId = 99;
    std::vector<RankFullInfo> result;
    auto ret = SmemMessage::UnpackAddToWhitelist(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 0u);
    EXPECT_EQ(result.size(), 0u);
}

TEST(PackRemoveFromWhitelist, RoundTrip_Basic)
{
    std::vector<RankBaseInfo> others;
    others.push_back(MakeRankBaseInfo(10, {0xAA, 0xBB}));
    others.push_back(MakeRankBaseInfo(20, {0xCC}));
    others.push_back(MakeRankBaseInfo(30, {}));

    auto msg = SmemMessage::PackRemoveFromWhitelist(100, others);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 100);
    EXPECT_EQ(msg.keys.size(), 1u);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_REMOVE_FROM_WHITELIST));

    uint32_t rankId = 0;
    std::vector<RankBaseInfo> result;
    auto ret = SmemMessage::UnpackRemoveFromWhitelist(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 100u);
    EXPECT_EQ(result.size(), others.size());
    for (size_t i = 0; i < others.size(); ++i) {
        EXPECT_EQ(result[i].rankId, others[i].rankId);
        EXPECT_EQ(result[i].baseInfo, others[i].baseInfo);
    }
}

TEST(PackRemoveFromWhitelist, DifferentFromAddToWhitelist)
{
    std::vector<RankBaseInfo> others;
    others.push_back(MakeRankBaseInfo(1, {0x01}));

    auto msgAdd = SmemMessage::PackAddToWhitelist(5, {MakeRankFullInfo(1, {0x01}, {})});
    auto msgRemove = SmemMessage::PackRemoveFromWhitelist(5, others);
    EXPECT_NE(msgAdd.keys[0], msgRemove.keys[0]);

    uint32_t rankId = 0;
    std::vector<RankFullInfo> resultFull;
    std::vector<RankBaseInfo> resultBase;
    EXPECT_EQ(SmemMessage::UnpackAddToWhitelist(msgRemove, rankId, resultFull), -1);
    EXPECT_EQ(SmemMessage::UnpackRemoveFromWhitelist(msgAdd, rankId, resultBase), -1);
}

TEST(PackEstablishConnection, RoundTrip_Basic)
{
    std::vector<RankFullInfo> others;
    others.push_back(MakeRankFullInfo(1, {0x01}, {{0x10, 0x20}, {0x30}}));
    others.push_back(MakeRankFullInfo(2, {0x02, 0x03}, {}));
    others.push_back(MakeRankFullInfo(3, {}, {{0x40}}));

    auto msg = SmemMessage::PackEstablishConnection(7, others);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 7);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_ESTABLISH_CONNECTION));

    uint32_t rankId = 0;
    std::vector<RankFullInfo> result;
    auto ret = SmemMessage::UnpackEstablishConnection(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 7u);
    EXPECT_EQ(result.size(), others.size());
    for (size_t i = 0; i < others.size(); ++i) {
        EXPECT_EQ(result[i].rankId, others[i].rankId);
        EXPECT_EQ(result[i].baseInfo, others[i].baseInfo);
        EXPECT_EQ(result[i].externalInfo.size(), others[i].externalInfo.size());
        for (size_t j = 0; j < others[i].externalInfo.size(); ++j) {
            EXPECT_EQ(result[i].externalInfo[j], others[i].externalInfo[j]);
        }
    }
}

TEST(PackEstablishConnection, RoundTrip_Empty)
{
    std::vector<RankFullInfo> others;
    auto msg = SmemMessage::PackEstablishConnection(255, others);

    uint32_t rankId = 0;
    std::vector<RankFullInfo> result;
    auto ret = SmemMessage::UnpackEstablishConnection(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 255u);
    EXPECT_EQ(result.size(), 0u);
}

TEST(UnpackAddToWhitelist, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    msg.keys.emplace_back(std::to_string(CONTROL_ADD_TO_WHITELIST));
    msg.values.emplace_back();

    uint32_t rankId = 0;
    std::vector<RankFullInfo> others;
    auto ret = SmemMessage::UnpackAddToWhitelist(msg, rankId, others);
    EXPECT_EQ(ret, -1);
}

TEST(UnpackAddToWhitelist, MissingKeys)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.values.emplace_back();

    uint32_t rankId = 0;
    std::vector<RankFullInfo> others;
    auto ret = SmemMessage::UnpackAddToWhitelist(msg, rankId, others);
    EXPECT_EQ(ret, -1);
}

TEST(GetControlOp, ValidOps)
{
    auto msgAdd = SmemMessage::PackAddToWhitelist(0, {});
    EXPECT_EQ(SmemMessage::GetControlOp(msgAdd), CONTROL_ADD_TO_WHITELIST);

    auto msgRemove = SmemMessage::PackRemoveFromWhitelist(0, {});
    EXPECT_EQ(SmemMessage::GetControlOp(msgRemove), CONTROL_REMOVE_FROM_WHITELIST);

    auto msgEstablish = SmemMessage::PackEstablishConnection(0, {});
    EXPECT_EQ(SmemMessage::GetControlOp(msgEstablish), CONTROL_ESTABLISH_CONNECTION);

    RankFullInfo info;
    info.rankId = 0;
    auto msgJoin = SmemMessage::PackJoin(info);
    EXPECT_EQ(SmemMessage::GetControlOp(msgJoin), CONTROL_JOIN);

    auto msgLeave = SmemMessage::PackLeave(0);
    EXPECT_EQ(SmemMessage::GetControlOp(msgLeave), CONTROL_LEAVE);
}

TEST(GetControlOp, InvalidMessage)
{
    SmemMessage msg{MessageType::SET};
    EXPECT_EQ(SmemMessage::GetControlOp(msg), -1);

    SmemMessage msgNoKeys{MessageType::CONTROL};
    EXPECT_EQ(SmemMessage::GetControlOp(msgNoKeys), -1);
}

TEST(PackJoin, RoundTrip)
{
    RankFullInfo info;
    info.rankId = 42;
    info.baseInfo = {0x01, 0x02, 0x03};
    info.externalInfo = {{0x10, 0x20}, {0x30}};
    info.protocol = SMEM_RANK_PROTOCOL_TRANS;

    auto msg = SmemMessage::PackJoin(info);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 42);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_JOIN));
    EXPECT_EQ(msg.values.size(), 1u);

    RankFullInfo result;
    auto ret = SmemMessage::UnpackJoin(msg, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(result.rankId, 42u);
    EXPECT_EQ(result.baseInfo, info.baseInfo);
    EXPECT_EQ(result.externalInfo.size(), info.externalInfo.size());
    for (size_t i = 0; i < info.externalInfo.size(); ++i) {
        EXPECT_EQ(result.externalInfo[i], info.externalInfo[i]);
    }
    EXPECT_EQ(result.protocol, SMEM_RANK_PROTOCOL_TRANS);
}

TEST(PackJoin, MissingProtocolDefaultsToDefault)
{
    /* A peer that packs without the protocol tag (old format) must unpack
       with protocol == DEFAULT so the server treats it as BM. */
    RankFullInfo info;
    info.rankId = 3;
    info.baseInfo = {0xaa};

    auto msg = SmemMessage::PackJoin(info);
    RankFullInfo result;
    auto ret = SmemMessage::UnpackJoin(msg, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(result.rankId, 3u);
    EXPECT_EQ(result.protocol, SMEM_RANK_PROTOCOL_DEFAULT);
}

TEST(PackJoin, EmptyInfo)
{
    RankFullInfo info;
    info.rankId = 0;

    auto msg = SmemMessage::PackJoin(info);
    RankFullInfo result;
    auto ret = SmemMessage::UnpackJoin(msg, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(result.rankId, 0u);
    EXPECT_EQ(result.baseInfo.size(), 0u);
    EXPECT_EQ(result.externalInfo.size(), 0u);
}

TEST(PackLeave, RoundTrip)
{
    auto msg = SmemMessage::PackLeave(7);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 7);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_LEAVE));

    uint32_t rankId = 0;
    auto ret = SmemMessage::UnpackLeave(msg, rankId);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(rankId, 7u);
}

TEST(PackLeave, WrongControlOp)
{
    auto msg = SmemMessage::PackLeave(7);
    EXPECT_EQ(SmemMessage::GetControlOp(msg), CONTROL_LEAVE);

    uint32_t rankId = 0;
    RankFullInfo info;
    EXPECT_EQ(SmemMessage::UnpackJoin(msg, info), -1);
}

TEST(PackEstablishConnection, NestedEmptyFields)
{
    std::vector<RankFullInfo> others;
    others.push_back(MakeRankFullInfo(0, {}, {}));
    others.push_back(MakeRankFullInfo(1, {0x11}, {{0x22}}));
    others.push_back(MakeRankFullInfo(2, {}, {{0x33, 0x44}}));

    auto msg = SmemMessage::PackEstablishConnection(999, others);

    uint32_t rankId = 0;
    std::vector<RankFullInfo> result;
    auto ret = SmemMessage::UnpackEstablishConnection(msg, rankId, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(rankId, 999u);
    EXPECT_EQ(result.size(), 3u);

    EXPECT_EQ(result[0].rankId, 0u);
    EXPECT_EQ(result[0].baseInfo.size(), 0u);
    EXPECT_EQ(result[0].externalInfo.size(), 0u);

    EXPECT_EQ(result[1].rankId, 1u);
    EXPECT_EQ(result[1].baseInfo, std::vector<uint8_t>{0x11});
    EXPECT_EQ(result[1].externalInfo.size(), 1u);
    EXPECT_EQ(result[1].externalInfo[0], (std::vector<uint8_t>{0x22}));

    EXPECT_EQ(result[2].rankId, 2u);
    EXPECT_EQ(result[2].baseInfo.size(), 0u);
    EXPECT_EQ(result[2].externalInfo.size(), 1u);
    EXPECT_EQ(result[2].externalInfo[0], (std::vector<uint8_t>{0x33, 0x44}));
}

TEST(PackQueryLinkState, RoundTrip)
{
    auto msg = SmemMessage::PackQueryLinkState(42);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 42);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_QUERY_LINK_STATE));

    uint32_t rankId = 0;
    auto ret = SmemMessage::UnpackQueryLinkState(msg, rankId);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(rankId, 42u);
}

TEST(PackQueryLinkState, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    msg.keys.emplace_back(std::to_string(CONTROL_QUERY_LINK_STATE));
    uint32_t rankId = 0;
    EXPECT_EQ(SmemMessage::UnpackQueryLinkState(msg, rankId), -1);
}

TEST(PackLinkStateResponse, RoundTrip_Basic)
{
    RankFullInfo rankInfo;
    rankInfo.rankId = 10;
    rankInfo.baseInfo = {0xAA, 0xBB, 0xCC};
    rankInfo.externalInfo = {{0x01}, {0x02, 0x03}};

    std::vector<LinkStateEntry> entries;
    LinkStateEntry e1;
    e1.dstRankId = 1;
    e1.state = 4;
    entries.push_back(e1);
    LinkStateEntry e2;
    e2.dstRankId = 3;
    e2.state = 0;
    entries.push_back(e2);
    LinkStateEntry e3;
    e3.dstRankId = 5;
    e3.state = 1;
    entries.push_back(e3);

    auto msg = SmemMessage::PackLinkStateResponse(rankInfo, entries);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.userDef, 10);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_LINK_STATE_RESPONSE));
    EXPECT_EQ(msg.values.size(), 2u);

    RankFullInfo resultInfo;
    std::vector<LinkStateEntry> result;
    auto ret = SmemMessage::UnpackLinkStateResponse(msg, resultInfo, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(resultInfo.rankId, 10u);
    EXPECT_EQ(resultInfo.baseInfo, rankInfo.baseInfo);
    EXPECT_EQ(resultInfo.externalInfo.size(), rankInfo.externalInfo.size());
    for (size_t i = 0; i < rankInfo.externalInfo.size(); ++i) {
        EXPECT_EQ(resultInfo.externalInfo[i], rankInfo.externalInfo[i]);
    }
    EXPECT_EQ(result.size(), entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(result[i].dstRankId, entries[i].dstRankId);
        EXPECT_EQ(result[i].state, entries[i].state);
    }
}

TEST(PackLinkStateResponse, RoundTrip_Empty)
{
    RankFullInfo rankInfo;
    rankInfo.rankId = 0;

    std::vector<LinkStateEntry> entries;
    auto msg = SmemMessage::PackLinkStateResponse(rankInfo, entries);

    RankFullInfo resultInfo;
    std::vector<LinkStateEntry> result;
    auto ret = SmemMessage::UnpackLinkStateResponse(msg, resultInfo, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(resultInfo.rankId, 0u);
    EXPECT_EQ(result.size(), 0u);
}

TEST(PackLinkStateResponse, RoundTrip_CarriesRole)
{
    /* 角色随 LNKSRSP 回带：非 etcd 模式下 meta 重启后，重连的 rank 靠这条通道
       让服务端重新拿到角色（重连路径不发 JOINREQ）。 */
    RankFullInfo rankInfo;
    rankInfo.rankId = 11;
    rankInfo.baseInfo = {0xAA};
    rankInfo.role = SMEM_GROUP_RANK_ROLE_PROXY;

    std::vector<LinkStateEntry> entries;
    auto msg = SmemMessage::PackLinkStateResponse(rankInfo, entries);

    RankFullInfo resultInfo;
    std::vector<LinkStateEntry> result;
    auto ret = SmemMessage::UnpackLinkStateResponse(msg, resultInfo, result);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(resultInfo.rankId, 11u);
    EXPECT_EQ(resultInfo.role, SMEM_GROUP_RANK_ROLE_PROXY);
}

TEST(PackLinkStateResponse, MissingRoleRejected)
{
    /* 角色尾标签是必需字段：同版本对端必带，缺标签只可能是报文被截断，解包必须失败由
       调用方丢弃，而不是把未知角色当成 provider 继续用。 */
    RankFullInfo rankInfo;
    rankInfo.rankId = 9;
    rankInfo.baseInfo = {0x01};

    std::vector<LinkStateEntry> entries;
    auto msg = SmemMessage::PackLinkStateResponse(rankInfo, entries);
    ASSERT_FALSE(msg.values[0].empty());
    msg.values[0].pop_back(); /* 去掉 role 尾标签，模拟截断报文 */

    RankFullInfo resultInfo;
    std::vector<LinkStateEntry> result;
    EXPECT_EQ(SmemMessage::UnpackLinkStateResponse(msg, resultInfo, result), -1);
}

/* ================================================================== */
/*  Single ACK                                                         */
/* ================================================================== */
TEST(PackAck, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    msg.keys.emplace_back("0");
    msg.keys.emplace_back("1");
    msg.values.emplace_back(SmemMessagePacker::PackPod(uint32_t{5}));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    uint32_t targetRankId = 0;
    EXPECT_EQ(SmemMessage::UnpackAck(msg, ackOp, senderRankId, targetRankId), -1);
}

TEST(PackAck, MissingKeys)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back("0");
    msg.values.emplace_back(SmemMessagePacker::PackPod(uint32_t{5}));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    uint32_t targetRankId = 0;
    EXPECT_EQ(SmemMessage::UnpackAck(msg, ackOp, senderRankId, targetRankId), -1);
}

TEST(PackAck, EmptyValues)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back("0");
    msg.keys.emplace_back("1");

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    uint32_t targetRankId = 0;
    EXPECT_EQ(SmemMessage::UnpackAck(msg, ackOp, senderRankId, targetRankId), -1);
}

TEST(PackAck, NonNumericSenderKey)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back("8");
    msg.keys.emplace_back("not_a_number");
    msg.values.emplace_back(SmemMessagePacker::PackPod(uint32_t{5}));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    uint32_t targetRankId = 0;
    EXPECT_EQ(SmemMessage::UnpackAck(msg, ackOp, senderRankId, targetRankId), -1);
}

/* ================================================================== */
/*  Batch ACK                                                          */
/* ================================================================== */
TEST(PackAckBatch, RoundTrip_Single)
{
    std::vector<uint32_t> targets = {42};
    std::vector<int32_t> results = {0};
    auto msg = SmemMessage::PackAckBatch(CONTROL_ESTABLISH_CONNECTION_ACK, 3, targets, results);
    EXPECT_EQ(msg.mt, MessageType::CONTROL);
    EXPECT_EQ(msg.keys[0], std::to_string(CONTROL_ESTABLISH_CONNECTION_ACK));
    EXPECT_EQ(msg.keys[1], "3");

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    auto ret = SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(ackOp, CONTROL_ESTABLISH_CONNECTION_ACK);
    EXPECT_EQ(senderRankId, 3u);
    ASSERT_EQ(gotTargets.size(), 1u);
    EXPECT_EQ(gotTargets[0], 42u);
    ASSERT_EQ(gotResults.size(), 1u);
    EXPECT_EQ(gotResults[0], 0);
}

TEST(PackAckBatch, RoundTrip_Multiple)
{
    std::vector<uint32_t> targets = {1, 2, 3};
    std::vector<int32_t> results = {0, -1, 0};
    auto msg = SmemMessage::PackAckBatch(CONTROL_REMOVE_FROM_WHITELIST_ACK, 0, targets, results);

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 999;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    auto ret = SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(senderRankId, 0u);
    ASSERT_EQ(gotTargets.size(), 3u);
    EXPECT_EQ(gotTargets[0], 1u);
    EXPECT_EQ(gotTargets[1], 2u);
    EXPECT_EQ(gotTargets[2], 3u);
    ASSERT_EQ(gotResults.size(), 3u);
    EXPECT_EQ(gotResults[0], 0);
    EXPECT_EQ(gotResults[1], 1);
    EXPECT_EQ(gotResults[2], 0);
}

TEST(PackAckBatch, RoundTrip_FallbackPath)
{
    /* Craft a batch message with only count + rank IDs (no result bytes).
       This triggers the legacy fallback path where results are assumed 0. */
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ESTABLISH_CONNECTION_ACK));
    msg.keys.emplace_back("5");
    std::vector<uint8_t> data;
    auto appendNet = [&data](uint32_t v) {
        uint32_t net = htonl(v);
        data.insert(data.end(), reinterpret_cast<uint8_t *>(&net), reinterpret_cast<uint8_t *>(&net) + sizeof(net));
    };
    appendNet(2);  // count
    appendNet(10); // id1
    appendNet(20); // id2
    msg.values.emplace_back(std::move(data));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    auto ret = SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults);
    EXPECT_EQ(ret, 0);
    ASSERT_EQ(gotTargets.size(), 2u);
    EXPECT_EQ(gotTargets[0], 10u);
    EXPECT_EQ(gotTargets[1], 20u);
    ASSERT_EQ(gotResults.size(), 2u);
    EXPECT_EQ(gotResults[0], 0);
    EXPECT_EQ(gotResults[1], 0);
}

TEST(PackAckBatch, ValueTooSmall_ReturnsMinus1)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ESTABLISH_CONNECTION_ACK));
    msg.keys.emplace_back("0");
    msg.values.emplace_back(std::vector<uint8_t>(1, 0));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    EXPECT_EQ(SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults), -1);
}

TEST(PackAckBatch, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    msg.keys.emplace_back("0");
    msg.keys.emplace_back("1");
    msg.values.emplace_back(std::vector<uint8_t>(sizeof(uint32_t) * 3, 0));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    EXPECT_EQ(SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults), -1);
}

TEST(PackAckBatch, NonNumericSenderKey)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back("11");
    msg.keys.emplace_back("bad");
    msg.values.emplace_back(std::vector<uint8_t>(sizeof(uint32_t) * 2, 0));

    ControlOp ackOp = CONTROL_ADD_TO_WHITELIST_ACK;
    uint32_t senderRankId = 0;
    std::vector<uint32_t> gotTargets;
    std::vector<uint8_t> gotResults;
    EXPECT_EQ(SmemMessage::UnpackAckBatch(msg, ackOp, senderRankId, gotTargets, gotResults), -1);
}

/* ================================================================== */
/*  Unpack error paths for EstablishConnection                         */
/* ================================================================== */
TEST(UnpackEstablishConnection, TruncatedValue)
{
    auto msg = SmemMessage::PackEstablishConnection(5, {MakeRankFullInfo(1, {0x01}, {})});
    /* Truncate the value bytes to trigger read failure */
    msg.values[0].resize(1);

    uint32_t rankId = 0;
    std::vector<RankFullInfo> result;
    EXPECT_EQ(SmemMessage::UnpackEstablishConnection(msg, rankId, result), -1);
}

/* ================================================================== */
/*  Unpack error paths for RemoveFromWhitelist                         */
/* ================================================================== */
TEST(UnpackRemoveFromWhitelist, TruncatedValue)
{
    auto msg = SmemMessage::PackRemoveFromWhitelist(5, {MakeRankBaseInfo(1, {0x01})});
    msg.values[0].resize(1);

    uint32_t rankId = 0;
    std::vector<RankBaseInfo> result;
    EXPECT_EQ(SmemMessage::UnpackRemoveFromWhitelist(msg, rankId, result), -1);
}

TEST(PackRemoveFromWhitelist, WrongControlOp)
{
    auto msg = SmemMessage::PackAddToWhitelist(5, {MakeRankFullInfo(1, {0x01}, {})});
    uint32_t rankId = 0;
    std::vector<RankBaseInfo> result;
    EXPECT_EQ(SmemMessage::UnpackRemoveFromWhitelist(msg, rankId, result), -1);
}

TEST(PackLinkStateResponse, WrongControlOp)
{
    auto queryMsg = SmemMessage::PackQueryLinkState(10);
    RankFullInfo resultInfo;
    std::vector<LinkStateEntry> result;
    EXPECT_EQ(SmemMessage::UnpackLinkStateResponse(queryMsg, resultInfo, result), -1);

    RankFullInfo rankInfo;
    rankInfo.rankId = 10;
    std::vector<LinkStateEntry> entries;
    auto respMsg = SmemMessage::PackLinkStateResponse(rankInfo, entries);
    uint32_t rankId;
    EXPECT_EQ(SmemMessage::UnpackQueryLinkState(respMsg, rankId), -1);
}

/* ================================================================== */
/*  UnpackJoin error paths (truncated value buffer)                   */
/* ================================================================== */
TEST(UnpackJoin, TruncatedBaseInfo)
{
    RankFullInfo info;
    info.rankId = 42;
    info.baseInfo = {0x01, 0x02, 0x03};
    info.externalInfo = {{0x10}};

    auto msg = SmemMessage::PackJoin(info);
    /* Truncate so ReadBytes for baseInfo fails */
    msg.values[0].resize(1);

    RankFullInfo result;
    EXPECT_EQ(SmemMessage::UnpackJoin(msg, result), -1);
}

TEST(UnpackJoin, TruncatedExtCount)
{
    RankFullInfo info;
    info.rankId = 42;
    info.baseInfo = {0x01, 0x02, 0x03};
    info.externalInfo = {{0x10}};

    auto msg = SmemMessage::PackJoin(info);
    /* Truncate right after baseInfo(8+3) so ReadUint64 for extCount fails */
    msg.values[0].resize(8 + 3);

    RankFullInfo result;
    EXPECT_EQ(SmemMessage::UnpackJoin(msg, result), -1);
}

TEST(UnpackJoin, WrongControlOpFails)
{
    RankFullInfo info;
    info.rankId = 42;
    info.baseInfo = {0x01, 0x02, 0x03};

    auto msg = SmemMessage::PackJoin(info);
    /* Rewrite control op so GetControlOp != CONTROL_JOIN */
    msg.keys[0] = std::to_string(CONTROL_LEAVE);

    RankFullInfo result;
    EXPECT_EQ(SmemMessage::UnpackJoin(msg, result), -1);
}

TEST(UnpackJoin, TruncatedExternalInfo)
{
    RankFullInfo info;
    info.rankId = 42;
    info.baseInfo = {0x01, 0x02, 0x03};
    info.externalInfo = {{0x10, 0x11}, {0x20}};

    auto msg = SmemMessage::PackJoin(info);
    /* baseInfo(8+3) + extCount(8) + ext[0](8+2), then truncate mid ext[1] */
    msg.values[0].resize(8 + 3 + 8 + 8 + 2 + 1);

    RankFullInfo result;
    EXPECT_EQ(SmemMessage::UnpackJoin(msg, result), -1);
}

/* ================================================================== */
/*  PromoteToActive / ExtendMemory / AddSlices                         */
/* ================================================================== */

TEST(PackPromoteToActive, RoundTrip)
{
    auto msg = SmemMessage::PackPromoteToActive(7);
    uint32_t rankId = 0;
    EXPECT_EQ(SmemMessage::UnpackPromoteToActive(msg, rankId), 0);
    EXPECT_EQ(rankId, 7u);
}

TEST(UnpackPromoteToActive, WrongControlOp)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_LEAVE));
    msg.userDef = 9;
    uint32_t rankId = 0;
    EXPECT_EQ(SmemMessage::UnpackPromoteToActive(msg, rankId), -1);
}

TEST(UnpackPromoteToActive, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    uint32_t rankId = 0;
    EXPECT_EQ(SmemMessage::UnpackPromoteToActive(msg, rankId), -1);
}

TEST(PackExtendMemory, RoundTrip)
{
    MultiBytes slices{{0x01}, {0x02, 0x03}};
    auto msg = SmemMessage::PackExtendMemory(3, slices);
    uint32_t rankId = 0;
    MultiBytes out;
    EXPECT_GT(SmemMessage::UnpackExtendMemory(msg, rankId, out), 0);
    EXPECT_EQ(rankId, 3u);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], slices[0]);
    EXPECT_EQ(out[1], slices[1]);
}

TEST(PackExtendMemory, EmptySlices)
{
    auto msg = SmemMessage::PackExtendMemory(5, {});
    uint32_t rankId = 0;
    MultiBytes out;
    EXPECT_GT(SmemMessage::UnpackExtendMemory(msg, rankId, out), 0);
    EXPECT_EQ(rankId, 5u);
    EXPECT_TRUE(out.empty());
}

TEST(UnpackExtendMemory, TruncatedValue)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_EXTEND_MEMORY));
    msg.userDef = 3;
    msg.values.emplace_back(std::vector<uint8_t>{0x01});
    uint32_t rankId = 0;
    MultiBytes out;
    EXPECT_EQ(SmemMessage::UnpackExtendMemory(msg, rankId, out), -1);
}

TEST(UnpackExtendMemory, WrongControlOp)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_LEAVE));
    msg.userDef = 3;
    msg.values.emplace_back(std::vector<uint8_t>{0x00});
    uint32_t rankId = 0;
    MultiBytes out;
    EXPECT_EQ(SmemMessage::UnpackExtendMemory(msg, rankId, out), -1);
}

TEST(PackAddSlices, RoundTrip)
{
    MultiBytes slices{{0xAA}, {0xBB, 0xCC}};
    auto msg = SmemMessage::PackAddSlices(4, slices);
    uint32_t extendingRankId = 0;
    MultiBytes out;
    EXPECT_GT(SmemMessage::UnpackAddSlices(msg, extendingRankId, out), 0);
    EXPECT_EQ(extendingRankId, 4u);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], slices[0]);
    EXPECT_EQ(out[1], slices[1]);
}

TEST(UnpackAddSlices, TruncatedValue)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ADD_SLICES));
    msg.userDef = 4;
    msg.values.emplace_back(std::vector<uint8_t>{0x01});
    uint32_t extendingRankId = 0;
    MultiBytes out;
    EXPECT_EQ(SmemMessage::UnpackAddSlices(msg, extendingRankId, out), -1);
}

TEST(UnpackAddSlices, WrongControlOp)
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_LEAVE));
    msg.userDef = 4;
    msg.values.emplace_back(std::vector<uint8_t>{0x00});
    uint32_t extendingRankId = 0;
    MultiBytes out;
    EXPECT_EQ(SmemMessage::UnpackAddSlices(msg, extendingRankId, out), -1);
}

TEST(UnpackAddSlices, InvalidMessageType)
{
    SmemMessage msg{MessageType::SET};
    uint32_t extendingRankId = 0;
    MultiBytes out;
    EXPECT_EQ(SmemMessage::UnpackAddSlices(msg, extendingRankId, out), -1);
}
