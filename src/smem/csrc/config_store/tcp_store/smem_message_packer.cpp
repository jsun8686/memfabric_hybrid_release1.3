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
#include <arpa/inet.h>
#include <cstring>

#include "smem_logger.h"
#include "smem_message_packer.h"

namespace ock {
namespace smem {
namespace {
template<class T>
bool ReadScalar(const uint8_t *buffer, uint64_t bufferLen, uint64_t &offset, T &value) noexcept
{
    if (buffer == nullptr || offset + sizeof(T) > bufferLen) {
        return false;
    }
    std::copy_n(buffer + offset, sizeof(T), static_cast<uint8_t *>(static_cast<void *>(&value)));
    offset += sizeof(T);
    return true;
}
// ACK message keys layout: [controlOp, senderRankId] (2 items minimum)
constexpr size_t K_ACK_MIN_KEYS = 2;
} // namespace

std::vector<uint8_t> SmemMessagePacker::Pack(const SmemMessage &message) noexcept
{
    // size + userDef + requestId + mt + keyN + vN
    constexpr uint64_t baseSize = 5U * sizeof(uint64_t) + sizeof(MessageType);
    uint64_t totalSize = baseSize;
    for (auto &key : message.keys) {
        totalSize += (sizeof(uint64_t) + key.size());
    }
    for (auto &value : message.values) {
        totalSize += (sizeof(uint64_t) + value.size());
    }

    std::vector<uint8_t> result;
    result.reserve(totalSize);
    PackValue(result, totalSize);
    PackValue(result, message.userDef);
    PackValue(result, message.requestId);
    PackValue(result, message.mt);

    PackValue(result, message.keys.size());
    for (auto &key : message.keys) {
        PackString(result, key);
    }

    PackValue(result, message.values.size());
    for (auto &value : message.values) {
        PackBytes(result, value);
    }

    return result;
}

bool SmemMessagePacker::Full(const uint8_t *buffer, const uint64_t bufferLen) noexcept
{
    constexpr uint64_t baseSize = 5U * sizeof(uint64_t) + sizeof(MessageType);
    if (buffer == nullptr || bufferLen < baseSize) {
        return false;
    }

    uint64_t offset = 0ULL;
    uint64_t totalSize = 0ULL;
    if (!ReadScalar(buffer, bufferLen, offset, totalSize)) {
        return false;
    }
    return bufferLen >= totalSize;
}

int64_t SmemMessagePacker::Unpack(const uint8_t *buffer, const uint64_t bufferLen, SmemMessage &message) noexcept
{
    SM_ASSERT_RETURN_NOLOG(buffer != nullptr, -1);
    SM_ASSERT_RETURN_NOLOG(Full(buffer, bufferLen), -1);

    uint64_t length = 0ULL;
    uint64_t totalSize = 0ULL;
    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, totalSize), -1);

    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, message.userDef), -1);

    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, message.requestId), -1);

    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, message.mt), -1);
    SM_ASSERT_RETURN_NOLOG(message.mt >= MessageType::SET && message.mt <= MessageType::INVALID_MSG, -1);

    uint64_t keyCount = 0;
    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, keyCount), -1);
    SM_ASSERT_RETURN_NOLOG(keyCount <= MAX_KEY_COUNT, -1);
    message.keys.reserve(keyCount);

    for (auto i = 0UL; i < keyCount; i++) {
        uint64_t keySize = 0;
        SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, keySize), -1);

        SM_ASSERT_RETURN_NOLOG(keySize <= MAX_KEY_SIZE && length + keySize <= bufferLen, -1);
        message.keys.emplace_back(reinterpret_cast<const char *>(buffer + length), keySize);
        length += keySize;
    }

    uint64_t valueCount = 0;
    SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, valueCount), -1);
    SM_ASSERT_RETURN_NOLOG(valueCount <= MAX_VALUE_COUNT, -1);
    message.values.reserve(valueCount);

    for (auto i = 0UL; i < valueCount; i++) {
        uint64_t valueSize = 0;
        SM_ASSERT_RETURN_NOLOG(ReadScalar(buffer, bufferLen, length, valueSize), -1);
        SM_ASSERT_RETURN_NOLOG(valueSize <= MAX_VALUE_SIZE && length + valueSize <= bufferLen, -1);

        message.values.emplace_back(buffer + length, buffer + length + valueSize);
        length += valueSize;
    }
    SM_ASSERT_RETURN_NOLOG(totalSize == length, -1);
    return static_cast<int64_t>(totalSize);
}

void SmemMessagePacker::PackString(std::vector<uint8_t> &dest, const std::string &str) noexcept
{
    PackValue(dest, static_cast<uint64_t>(str.size()));
    if (!str.empty()) {
        dest.insert(dest.end(), str.data(), str.data() + str.size());
    }
}

void SmemMessagePacker::PackBytes(std::vector<uint8_t> &dest, const std::vector<uint8_t> &bytes) noexcept
{
    PackValue(dest, static_cast<uint64_t>(bytes.size()));
    dest.insert(dest.end(), bytes.begin(), bytes.end());
}

namespace {

void AppendUint32(std::vector<uint8_t> &buf, uint32_t val) noexcept
{
    buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&val),
               reinterpret_cast<const uint8_t *>(&val) + sizeof(uint32_t));
}

void AppendUint64(std::vector<uint8_t> &buf, uint64_t val) noexcept
{
    buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&val),
               reinterpret_cast<const uint8_t *>(&val) + sizeof(uint64_t));
}

bool ReadUint32(const std::vector<uint8_t> &buf, uint64_t &offset, uint32_t &val) noexcept
{
    if (offset + sizeof(uint32_t) > buf.size()) {
        return false;
    }
    std::copy_n(buf.data() + offset, sizeof(uint32_t), static_cast<uint8_t *>(static_cast<void *>(&val)));
    offset += sizeof(uint32_t);
    return true;
}

bool ReadUint64(const std::vector<uint8_t> &buf, uint64_t &offset, uint64_t &val) noexcept
{
    if (offset + sizeof(uint64_t) > buf.size()) {
        return false;
    }
    std::copy_n(buf.data() + offset, sizeof(uint64_t), static_cast<uint8_t *>(static_cast<void *>(&val)));
    offset += sizeof(uint64_t);
    return true;
}

bool ReadBytes(const std::vector<uint8_t> &buf, uint64_t &offset, Bytes &out) noexcept
{
    uint64_t len = 0;
    if (!ReadUint64(buf, offset, len)) {
        return false;
    }
    if (offset + len > buf.size()) {
        return false;
    }
    out.assign(buf.begin() + offset, buf.begin() + offset + len);
    offset += len;
    return true;
}

} // namespace

SmemMessage SmemMessage::PackAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ADD_TO_WHITELIST));
    msg.userDef = static_cast<int64_t>(rankId);
    std::vector<uint8_t> buf;
    uint64_t count = others.size();
    AppendUint64(buf, count);
    for (const auto &info : others) {
        AppendUint32(buf, info.rankId);
        AppendUint64(buf, info.baseInfo.size());
        buf.insert(buf.end(), info.baseInfo.begin(), info.baseInfo.end());
        uint64_t extCount = info.externalInfo.size();
        AppendUint64(buf, extCount);
        for (const auto &ext : info.externalInfo) {
            AppendUint64(buf, ext.size());
            buf.insert(buf.end(), ext.begin(), ext.end());
        }
    }
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackAddToWhitelist(const SmemMessage &msg, uint32_t &rankId,
                                          std::vector<RankFullInfo> &others) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_ADD_TO_WHITELIST) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, offset, count)) {
        return -1;
    }
    others.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        if (!ReadUint32(buf, offset, others[i].rankId)) {
            return -1;
        }
        if (!ReadBytes(buf, offset, others[i].baseInfo)) {
            return -1;
        }
        uint64_t extCount = 0;
        if (!ReadUint64(buf, offset, extCount)) {
            return -1;
        }
        others[i].externalInfo.resize(extCount);
        for (uint64_t j = 0; j < extCount; ++j) {
            if (!ReadBytes(buf, offset, others[i].externalInfo[j])) {
                return -1;
            }
        }
    }
    return static_cast<int64_t>(offset);
}

SmemMessage SmemMessage::PackRemoveFromWhitelist(uint32_t rankId, const std::vector<RankBaseInfo> &others) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_REMOVE_FROM_WHITELIST));
    msg.userDef = static_cast<int64_t>(rankId);
    std::vector<uint8_t> buf;
    uint64_t count = others.size();
    AppendUint64(buf, count);
    for (const auto &info : others) {
        AppendUint32(buf, info.rankId);
        AppendUint64(buf, info.baseInfo.size());
        buf.insert(buf.end(), info.baseInfo.begin(), info.baseInfo.end());
    }
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackRemoveFromWhitelist(const SmemMessage &msg, uint32_t &rankId,
                                               std::vector<RankBaseInfo> &others) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_REMOVE_FROM_WHITELIST) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, offset, count)) {
        return -1;
    }
    others.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        if (!ReadUint32(buf, offset, others[i].rankId)) {
            return -1;
        }
        if (!ReadBytes(buf, offset, others[i].baseInfo)) {
            return -1;
        }
    }
    return static_cast<int64_t>(offset);
}

SmemMessage SmemMessage::PackEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &others) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ESTABLISH_CONNECTION));
    msg.userDef = static_cast<int64_t>(rankId);
    std::vector<uint8_t> buf;
    uint64_t count = others.size();
    AppendUint64(buf, count);
    for (const auto &info : others) {
        AppendUint32(buf, info.rankId);
        AppendUint64(buf, info.baseInfo.size());
        buf.insert(buf.end(), info.baseInfo.begin(), info.baseInfo.end());
        uint64_t extCount = info.externalInfo.size();
        AppendUint64(buf, extCount);
        for (const auto &ext : info.externalInfo) {
            AppendUint64(buf, ext.size());
            buf.insert(buf.end(), ext.begin(), ext.end());
        }
    }
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackEstablishConnection(const SmemMessage &msg, uint32_t &rankId,
                                               std::vector<RankFullInfo> &others) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_ESTABLISH_CONNECTION) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, offset, count)) {
        return -1;
    }
    others.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        if (!ReadUint32(buf, offset, others[i].rankId)) {
            return -1;
        }
        if (!ReadBytes(buf, offset, others[i].baseInfo)) {
            return -1;
        }
        uint64_t extCount = 0;
        if (!ReadUint64(buf, offset, extCount)) {
            return -1;
        }
        others[i].externalInfo.resize(extCount);
        for (uint64_t j = 0; j < extCount; ++j) {
            if (!ReadBytes(buf, offset, others[i].externalInfo[j])) {
                return -1;
            }
        }
    }
    return static_cast<int64_t>(offset);
}

SmemMessage SmemMessage::PackJoin(const RankFullInfo &info) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_JOIN));
    msg.userDef = static_cast<int64_t>(info.rankId);
    std::vector<uint8_t> buf;
    AppendUint64(buf, info.baseInfo.size());
    buf.insert(buf.end(), info.baseInfo.begin(), info.baseInfo.end());
    uint64_t extCount = info.externalInfo.size();
    AppendUint64(buf, extCount);
    for (const auto &ext : info.externalInfo) {
        AppendUint64(buf, ext.size());
        buf.insert(buf.end(), ext.begin(), ext.end());
    }
    buf.push_back(info.protocol);
    buf.push_back(info.role);
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackJoin(const SmemMessage &msg, RankFullInfo &info) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_JOIN) {
        return -1;
    }
    info.rankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    if (!ReadBytes(buf, offset, info.baseInfo)) {
        return -1;
    }
    uint64_t extCount = 0;
    if (!ReadUint64(buf, offset, extCount)) {
        return -1;
    }
    info.externalInfo.resize(extCount);
    for (uint64_t j = 0; j < extCount; ++j) {
        if (!ReadBytes(buf, offset, info.externalInfo[j])) {
            return -1;
        }
    }
    // Protocol tag is an extension appended after the fixed fields; older peers
    // omit it, in which case it defaults to 0 (BM/default). The rank role tag
    // follows the protocol tag and defaults to UNKNOWN when absent.
    info.protocol = SMEM_RANK_PROTOCOL_DEFAULT;
    info.role = SMEM_GROUP_RANK_ROLE_UNKNOWN;
    if (offset < buf.size()) {
        info.protocol = buf[offset++];
    }
    if (offset < buf.size()) {
        info.role = buf[offset++];
    }
    return static_cast<int64_t>(offset);
}

SmemMessage SmemMessage::PackLeave(uint32_t rankId) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_LEAVE));
    msg.userDef = static_cast<int64_t>(rankId);
    return msg;
}

int64_t SmemMessage::UnpackLeave(const SmemMessage &msg, uint32_t &rankId) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_LEAVE) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    return 0;
}

int8_t SmemMessage::GetControlOp(const SmemMessage &msg) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty()) {
        return -1;
    }
    try {
        return static_cast<int8_t>(std::stoi(msg.keys[0]));
    } catch (...) {
        return -1;
    }
}

SmemMessage SmemMessage::PackQueryLinkState(uint32_t rankId) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_QUERY_LINK_STATE));
    msg.userDef = static_cast<int64_t>(rankId);
    return msg;
}

int64_t SmemMessage::UnpackQueryLinkState(const SmemMessage &msg, uint32_t &rankId) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_QUERY_LINK_STATE) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    return 0;
}

SmemMessage SmemMessage::PackLinkStateResponse(const RankFullInfo &rankInfo,
                                               const std::vector<LinkStateEntry> &entries) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_LINK_STATE_RESPONSE));
    msg.userDef = static_cast<int64_t>(rankInfo.rankId);
    std::vector<uint8_t> infoBuf;
    AppendUint32(infoBuf, rankInfo.rankId);
    AppendUint64(infoBuf, rankInfo.baseInfo.size());
    infoBuf.insert(infoBuf.end(), rankInfo.baseInfo.begin(), rankInfo.baseInfo.end());
    uint64_t extCount = rankInfo.externalInfo.size();
    AppendUint64(infoBuf, extCount);
    for (const auto &ext : rankInfo.externalInfo) {
        AppendUint64(infoBuf, ext.size());
        infoBuf.insert(infoBuf.end(), ext.begin(), ext.end());
    }
    // Role tag is mandatory: it always follows the fixed fields, and the unpacking side treats a
    // missing tag as a truncated payload.
    infoBuf.push_back(rankInfo.role);
    msg.values.emplace_back(std::move(infoBuf));
    std::vector<uint8_t> buf;
    uint64_t count = entries.size();
    AppendUint64(buf, count);
    for (const auto &entry : entries) {
        AppendUint32(buf, entry.dstRankId);
        buf.push_back(static_cast<uint8_t>(entry.state));
    }
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackLinkStateResponse(const SmemMessage &msg, RankFullInfo &rankInfo,
                                             std::vector<LinkStateEntry> &entries) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.size() < K_ACK_MIN_KEYS) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_LINK_STATE_RESPONSE) {
        return -1;
    }
    rankInfo.rankId = static_cast<uint32_t>(msg.userDef);
    const auto &infoBuf = msg.values[0];
    uint64_t offset = 0;
    if (!ReadUint32(infoBuf, offset, rankInfo.rankId)) {
        return -1;
    }
    if (!ReadBytes(infoBuf, offset, rankInfo.baseInfo)) {
        return -1;
    }
    uint64_t extCount = 0;
    if (!ReadUint64(infoBuf, offset, extCount)) {
        return -1;
    }
    rankInfo.externalInfo.resize(extCount);
    for (uint64_t j = 0; j < extCount; ++j) {
        if (!ReadBytes(infoBuf, offset, rankInfo.externalInfo[j])) {
            return -1;
        }
    }
    // Role tag always follows the fixed fields; a truncated payload must not silently degrade the
    // rank role, so treat it as a malformed message.
    if (offset >= infoBuf.size()) {
        return -1;
    }
    rankInfo.role = infoBuf[offset++];
    const auto &buf = msg.values[1];
    uint64_t entryOffset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, entryOffset, count)) {
        return -1;
    }
    entries.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        if (!ReadUint32(buf, entryOffset, entries[i].dstRankId)) {
            return -1;
        }
        if (entryOffset + 1 > buf.size()) {
            return -1;
        }
        entries[i].state = static_cast<int8_t>(buf[entryOffset]);
        ++entryOffset;
    }
    return static_cast<int64_t>(offset + entryOffset);
}

SmemMessage SmemMessage::PackPromoteToActive(uint32_t rankId) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_PROMOTE_TO_ACTIVE));
    msg.userDef = static_cast<int64_t>(rankId);
    return msg;
}

int64_t SmemMessage::UnpackPromoteToActive(const SmemMessage &msg, uint32_t &rankId) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_PROMOTE_TO_ACTIVE) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    return 0;
}

SmemMessage SmemMessage::PackAckBatch(ControlOp ackOp, uint32_t senderRankId,
                                      const std::vector<uint32_t> &targetRankIds,
                                      const std::vector<int32_t> &results) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(static_cast<int8_t>(ackOp)));
    msg.keys.emplace_back(std::to_string(senderRankId));
    std::vector<uint8_t> data(sizeof(uint32_t) + (sizeof(uint32_t) + sizeof(uint8_t)) * targetRankIds.size());
    uint32_t net = htonl(static_cast<uint32_t>(targetRankIds.size()));
    std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&net)), sizeof(net), data.data());
    for (size_t i = 0; i < targetRankIds.size(); ++i) {
        uint32_t netId = htonl(targetRankIds[i]);
        std::copy_n(static_cast<const uint8_t *>(static_cast<const void *>(&netId)), sizeof(netId),
                    data.data() + sizeof(uint32_t) + i * (sizeof(uint32_t) + sizeof(uint8_t)));
        uint8_t r = (i < results.size() && results[i] != 0) ? 1 : 0;
        data[sizeof(uint32_t) + i * (sizeof(uint32_t) + sizeof(uint8_t)) + sizeof(uint32_t)] = r;
    }
    msg.values.emplace_back(std::move(data));
    return msg;
}

int64_t SmemMessage::UnpackAck(const SmemMessage &msg, ControlOp &ackOp, uint32_t &senderRankId,
                               uint32_t &targetRankId) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.size() < K_ACK_MIN_KEYS || msg.values.empty()) {
        return -1;
    }
    ackOp = static_cast<ControlOp>(GetControlOp(msg));
    try {
        senderRankId = static_cast<uint32_t>(std::stoul(msg.keys[1]));
    } catch (...) {
        return -1;
    }
    if (msg.values[0].size() < sizeof(uint32_t)) {
        return -1;
    }
    targetRankId = SmemMessagePacker::UnpackPod<uint32_t>(msg.values[0]);
    return 0;
}

int64_t SmemMessage::UnpackAckBatch(const SmemMessage &msg, ControlOp &ackOp, uint32_t &senderRankId,
                                    std::vector<uint32_t> &targetRankIds, std::vector<uint8_t> &results) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.size() < K_ACK_MIN_KEYS || msg.values.empty()) {
        return -1;
    }
    ackOp = static_cast<ControlOp>(GetControlOp(msg));
    try {
        senderRankId = static_cast<uint32_t>(std::stoul(msg.keys[1]));
    } catch (...) {
        return -1;
    }
    const uint8_t *data = msg.values[0].data();
    size_t len = msg.values[0].size();
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t net;
    std::copy_n(data, sizeof(net), static_cast<uint8_t *>(static_cast<void *>(&net)));
    uint32_t count = ntohl(net);
    const size_t entrySize = sizeof(uint32_t) + sizeof(uint8_t);
    if (len < sizeof(uint32_t) + count * entrySize) {
        if (len < sizeof(uint32_t) * (count + 1)) {
            return -1;
        }
        targetRankIds.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::copy_n(data + sizeof(uint32_t) * (i + 1), sizeof(net),
                        static_cast<uint8_t *>(static_cast<void *>(&net)));
            targetRankIds[i] = ntohl(net);
        }
        results.assign(count, 0);
        return 0;
    }
    targetRankIds.resize(count);
    results.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::copy_n(data + sizeof(uint32_t) + i * entrySize, sizeof(net),
                    static_cast<uint8_t *>(static_cast<void *>(&net)));
        targetRankIds[i] = ntohl(net);
        results[i] = data[sizeof(uint32_t) + i * entrySize + sizeof(uint32_t)];
    }
    return 0;
}

SmemMessage SmemMessage::PackExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_EXTEND_MEMORY));
    msg.userDef = static_cast<int64_t>(rankId);
    std::vector<uint8_t> buf;
    uint64_t count = additionalSlices.size();
    AppendUint64(buf, count);
    for (const auto &slice : additionalSlices) {
        AppendUint64(buf, slice.size());
        buf.insert(buf.end(), slice.begin(), slice.end());
    }
    msg.values.emplace_back(std::move(buf));
    return msg;
}

int64_t SmemMessage::UnpackExtendMemory(const SmemMessage &msg, uint32_t &rankId, MultiBytes &additionalSlices) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_EXTEND_MEMORY) {
        return -1;
    }
    rankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, offset, count)) {
        return -1;
    }
    additionalSlices.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t len = 0;
        if (!ReadUint64(buf, offset, len)) {
            return -1;
        }
        additionalSlices[i].resize(len);
        if (offset + len > buf.size()) {
            return -1;
        }
        std::copy(buf.begin() + offset, buf.begin() + offset + len, additionalSlices[i].begin());
        offset += len;
    }
    return static_cast<int64_t>(offset);
}

SmemMessage SmemMessage::PackAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    msg.keys.emplace_back(std::to_string(CONTROL_ADD_SLICES));
    msg.userDef = static_cast<int64_t>(extendingRankId);
    std::vector<uint8_t> buf;
    uint64_t count = newSlices.size();
    AppendUint64(buf, count);
    for (const auto &slice : newSlices) {
        AppendUint64(buf, slice.size());
        buf.insert(buf.end(), slice.begin(), slice.end());
    }
    msg.values.emplace_back(std::move(buf));
    msg.requestId = 0; // set by caller
    return msg;
}

int64_t SmemMessage::UnpackAddSlices(const SmemMessage &msg, uint32_t &extendingRankId, MultiBytes &newSlices) noexcept
{
    if (msg.mt != MessageType::CONTROL || msg.keys.empty() || msg.values.empty()) {
        return -1;
    }
    if (GetControlOp(msg) != CONTROL_ADD_SLICES) {
        return -1;
    }
    extendingRankId = static_cast<uint32_t>(msg.userDef);
    const auto &buf = msg.values[0];
    uint64_t offset = 0;
    uint64_t count = 0;
    if (!ReadUint64(buf, offset, count)) {
        return -1;
    }
    newSlices.resize(count);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t len = 0;
        if (!ReadUint64(buf, offset, len)) {
            return -1;
        }
        newSlices[i].resize(len);
        if (offset + len > buf.size()) {
            return -1;
        }
        std::copy(buf.begin() + offset, buf.begin() + offset + len, newSlices[i].begin());
        offset += len;
    }
    return static_cast<int64_t>(offset);
}

} // namespace smem
} // namespace ock
