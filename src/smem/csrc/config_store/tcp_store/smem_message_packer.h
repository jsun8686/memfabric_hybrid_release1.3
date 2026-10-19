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

#ifndef SMEM_SMEM_MESSAGE_PACKER_H
#define SMEM_SMEM_MESSAGE_PACKER_H

#include <cstdint>
#include <string>
#include <vector>
#include "smem_group_manager_def.h"

namespace ock {
namespace smem {

constexpr const char *WATCH_RANK_DOWN_KEY = "WATCH_RANK_DOWN_KEY";
#ifdef UT_ENABLED
constexpr uint32_t HEARTBEAT_INTERVAL = 100; // ms
#else
constexpr uint32_t HEARTBEAT_INTERVAL = 2000; // ms (2s interval)
#endif
const uint64_t MAX_KEY_COUNT = 1024ULL;
const uint64_t MAX_KEY_SIZE = 2048ULL;
const uint64_t MAX_VALUE_COUNT = 1024ULL;
const uint64_t MAX_VALUE_SIZE = 64 * 1024 * 1024ULL;
enum MessageType : int16_t {
    SET,
    GET,
    PREFIX,
    WATCH,
    ADD,
    REMOVE,
    APPEND,
    CAS,
    WRITE,
    QUERY_ALIVE,
    WATCH_RANK_STATE,
    HEARTBEAT,
    UNWATCH,
    CONTROL,
    INVALID_MSG
};

struct SmemMessage {
    SmemMessage() noexcept : mt{MessageType::INVALID_MSG} {}

    explicit SmemMessage(MessageType type) noexcept : mt{type} {}

    SmemMessage(MessageType type, std::string k) noexcept : mt{type}
    {
        keys.emplace_back(std::move(k));
    }

    SmemMessage(MessageType type, std::vector<uint8_t> v) noexcept : mt{type}
    {
        values.emplace_back(std::move(v));
    }

    SmemMessage(MessageType type, std::string k, std::vector<uint8_t> v) noexcept : mt{type}
    {
        keys.emplace_back(std::move(k));
        values.emplace_back(std::move(v));
    }

    SmemMessage(MessageType type, std::string k, std::vector<uint8_t> v, std::vector<uint8_t> vv) noexcept : mt{type}
    {
        keys.emplace_back(std::move(k));
        values.emplace_back(std::move(v));
        values.emplace_back(std::move(vv));
    }

    SmemMessage(MessageType type, std::vector<std::string> ks) noexcept : mt{type}, keys{std::move(ks)} {}

    SmemMessage(MessageType type, std::vector<std::string> ks, int64_t value) noexcept : mt{type}, keys{std::move(ks)}
    {
        values.emplace_back(reinterpret_cast<const uint8_t *>(&value),
                            reinterpret_cast<const uint8_t *>(&value) + sizeof(int64_t));
    }

    SmemMessage(MessageType type, std::vector<std::vector<uint8_t>> vs) noexcept : mt{type}, values{std::move(vs)} {}

    static SmemMessage PackAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others) noexcept;
    static int64_t UnpackAddToWhitelist(const SmemMessage &msg, uint32_t &rankId,
                                        std::vector<RankFullInfo> &others) noexcept;

    static SmemMessage PackRemoveFromWhitelist(uint32_t rankId, const std::vector<RankBaseInfo> &others) noexcept;
    static int64_t UnpackRemoveFromWhitelist(const SmemMessage &msg, uint32_t &rankId,
                                             std::vector<RankBaseInfo> &others) noexcept;

    static SmemMessage PackEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &others) noexcept;
    static int64_t UnpackEstablishConnection(const SmemMessage &msg, uint32_t &rankId,
                                             std::vector<RankFullInfo> &others) noexcept;

    static SmemMessage PackJoin(const RankFullInfo &info) noexcept;
    static int64_t UnpackJoin(const SmemMessage &msg, RankFullInfo &info) noexcept;

    static SmemMessage PackLeave(uint32_t rankId) noexcept;
    static int64_t UnpackLeave(const SmemMessage &msg, uint32_t &rankId) noexcept;

    static SmemMessage PackQueryLinkState(uint32_t rankId) noexcept;
    static int64_t UnpackQueryLinkState(const SmemMessage &msg, uint32_t &rankId) noexcept;

    static SmemMessage PackLinkStateResponse(const RankFullInfo &rankInfo,
                                             const std::vector<LinkStateEntry> &entries) noexcept;
    // The trailing role tag is mandatory: every peer sends it, a missing tag means the payload is
    // truncated, in which case unpacking fails instead of silently degrading a known role.
    static int64_t UnpackLinkStateResponse(const SmemMessage &msg, RankFullInfo &rankInfo,
                                           std::vector<LinkStateEntry> &entries) noexcept;

    static SmemMessage PackPromoteToActive(uint32_t rankId) noexcept;
    static int64_t UnpackPromoteToActive(const SmemMessage &msg, uint32_t &rankId) noexcept;

    static SmemMessage PackExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices) noexcept;
    static int64_t UnpackExtendMemory(const SmemMessage &msg, uint32_t &rankId, MultiBytes &additionalSlices) noexcept;

    static SmemMessage PackAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices) noexcept;
    static int64_t UnpackAddSlices(const SmemMessage &msg, uint32_t &extendingRankId, MultiBytes &newSlices) noexcept;

    static SmemMessage PackAckBatch(ControlOp ackOp, uint32_t senderRankId, const std::vector<uint32_t> &targetRankIds,
                                    const std::vector<int32_t> &results = {}) noexcept;
    static int64_t UnpackAck(const SmemMessage &msg, ControlOp &ackOp, uint32_t &senderRankId,
                             uint32_t &targetRankId) noexcept;
    static int64_t UnpackAckBatch(const SmemMessage &msg, ControlOp &ackOp, uint32_t &senderRankId,
                                  std::vector<uint32_t> &targetRankIds, std::vector<uint8_t> &results) noexcept;

    static int8_t GetControlOp(const SmemMessage &msg) noexcept;

    MessageType mt;
    int64_t userDef{-1L};
    uint64_t requestId = 0;
    std::vector<std::string> keys;
    std::vector<std::vector<uint8_t>> values;
};

class SmemMessagePacker {
public:
    static std::vector<uint8_t> Pack(const SmemMessage &message) noexcept;

    static bool Full(const uint8_t *buffer, const uint64_t bufferLen) noexcept;

    static int64_t Unpack(const uint8_t *buffer, const uint64_t bufferLen, SmemMessage &message) noexcept;

    template<class T>
    static std::vector<uint8_t> PackPod(const T &v) noexcept
    {
        auto begin = reinterpret_cast<const uint8_t *>(&v);
        return std::vector<uint8_t>{begin, begin + sizeof(T)};
    }

    template<class T>
    static T UnpackPod(const std::vector<uint8_t> &vec) noexcept
    {
        return *reinterpret_cast<const T *>(vec.data());
    }

private:
    template<class T>
    static void PackValue(std::vector<uint8_t> &dest, T value) noexcept
    {
        dest.insert(dest.end(), reinterpret_cast<const uint8_t *>(&value),
                    reinterpret_cast<const uint8_t *>(&value) + sizeof(T));
    }

    static void PackString(std::vector<uint8_t> &dest, const std::string &str) noexcept;

    static void PackBytes(std::vector<uint8_t> &dest, const std::vector<uint8_t> &bytes) noexcept;
};

} // namespace smem
} // namespace ock

#endif // SMEM_SMEM_MESSAGE_PACKER_H
