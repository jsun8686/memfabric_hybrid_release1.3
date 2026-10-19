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

#ifndef SMEM_SMEM_GROUP_MANAGER_DEF_H
#define SMEM_SMEM_GROUP_MANAGER_DEF_H

#include <cctype>
#include <string>
#include <vector>
#include <ostream>
#include <functional>
#include "smem_ref.h"

namespace ock::smem {

// Opaque byte blob for serialized rank metadata (e.g. endpoint, capabilities).
using Bytes = std::vector<uint8_t>;

// RankFullInfo::protocol values — identifies which module owns a rank membership.
constexpr uint8_t SMEM_RANK_PROTOCOL_DEFAULT = 0; // BM / unspecified
constexpr uint8_t SMEM_RANK_PROTOCOL_TRANS = 1;   // TRANS

// RankFullInfo::role values — 角色由本 rank 申请的内存推导（DRAM/HBM 都为 0 ⇒ PROXY）。
// A PROXY rank contributes no memory; two PROXY ranks are never linked, so the
// group manager skips their whitelist/establish exchange entirely. Every other
// role combination stays fully connected.
constexpr uint8_t SMEM_GROUP_RANK_ROLE_PROVIDER = 0; // contributes memory
constexpr uint8_t SMEM_GROUP_RANK_ROLE_PROXY = 1;    // no memory, proxy only
// 角色未知：本 rank 未声明角色（老版本对端不带 role 标签）、rank 已复位或查询越界。
// 未知一律按“非 PROXY”处理，因此不触发剪枝，链路行为与 PROVIDER 相同；但日志与集群状态
// 会如实显示 unknown，避免把“不知道”报成“贡献内存”。
constexpr uint8_t SMEM_GROUP_RANK_ROLE_UNKNOWN = 2;

// 日志用角色名：客户端上报与服务端落库两侧共用，保证排查角色问题的日志口径一致。
inline const char *RankRoleName(uint8_t role) noexcept
{
    switch (role) {
        case SMEM_GROUP_RANK_ROLE_PROXY:
            return "proxy";
        case SMEM_GROUP_RANK_ROLE_PROVIDER:
            return "provider";
        case SMEM_GROUP_RANK_ROLE_UNKNOWN:
        default:
            return "unknown";
    }
}

// 可重试的瞬时状态标记：P2P 尚未使能（对应 hybm 的 BM_P2P_NOT_READY）。经 OnAddToWhitelist
// 上抛给 GM 客户端 dispatcher，用于仅跳过该情形的失败 ERROR 打印（根因已在 segment 层记 WARN）。
constexpr int32_t SMEM_GROUP_P2P_NOT_READY = -406;

// Collection of Bytes, used for multi-address / multi-URL scenarios.
using MultiBytes = std::vector<Bytes>;

struct RankBaseInfo {
    uint32_t rankId;
    Bytes baseInfo;
    explicit RankBaseInfo(const uint32_t id = 0) noexcept : rankId{id} {}
};

struct RankFullInfo : RankBaseInfo {
    MultiBytes externalInfo;
    // Protocol that owns this rank's membership. 0 = default/BM, 1 = TRANS.
    // Carried in the JOINREQ body so the server can tell a Trans re-join
    // (which always sends a fresh JOINREQ) apart from a stale/duplicate one.
    uint8_t protocol{0};
    // Rank role (SMEM_GROUP_RANK_ROLE_*). Carried in the JOINREQ body so the
    // server can prune PROXY<->PROXY links; absent in older peers → unknown.
    uint8_t role{SMEM_GROUP_RANK_ROLE_UNKNOWN};
    explicit RankFullInfo(const uint32_t id = 0) noexcept : RankBaseInfo{id} {}
};

inline std::ostream &operator<<(std::ostream &os, const Bytes &bytes)
{
    os << "[";
    for (auto b : bytes) {
        if (isprint(b)) {
            os << b;
        } else {
            os << "\\x" << std::hex << static_cast<int>(b) << std::dec;
        }
    }
    os << "]";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const RankBaseInfo &info)
{
    os << "{rankId=" << info.rankId << ", baseInfo=" << info.baseInfo << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const RankFullInfo &info)
{
    os << "{rankId=" << info.rankId << ", baseInfo=" << info.baseInfo;
    os << ", externalInfo=[";
    for (size_t i = 0; i < info.externalInfo.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << info.externalInfo[i];
    }
    os << "]}";
    return os;
}

using SmemGroupCommandSender = std::function<int(uint32_t targetRankId, const std::vector<uint8_t> &data)>;

/**
 * @brief Control operation sub-type carried in SmemMessage with mt == CONTROL.
 */
enum ControlOp : int8_t {
    CONTROL_ADD_TO_WHITELIST = 0,
    CONTROL_REMOVE_FROM_WHITELIST = 1,
    CONTROL_ESTABLISH_CONNECTION = 2,
    CONTROL_JOIN = 3,
    CONTROL_LEAVE = 4,
    CONTROL_QUERY_LINK_STATE = 5,
    CONTROL_LINK_STATE_RESPONSE = 6,
    CONTROL_ADD_TO_WHITELIST_ACK = 7,
    CONTROL_REMOVE_FROM_WHITELIST_ACK = 8,
    CONTROL_ESTABLISH_CONNECTION_ACK = 9,
    CONTROL_PROMOTE_TO_ACTIVE = 10,
    CONTROL_EXTEND_MEMORY = 11, // client → server: extend memory with new slices
    CONTROL_ADD_SLICES = 12,    // server → clients: import new slices for a rank
};

const char *AckTagName(ControlOp op) noexcept;

struct LinkStateEntry {
    uint32_t dstRankId{0};
    int8_t state{0};
};

template<typename T>
inline std::string JoinRankIds(const T &items) noexcept
{
    std::string s;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            s += ",";
        }
        s += std::to_string(items[i].rankId);
    }
    return s;
}

inline std::string JoinRankIds(const std::vector<uint32_t> &ids) noexcept
{
    std::string s;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            s += ",";
        }
        s += std::to_string(ids[i]);
    }
    return s;
}

} // namespace ock::smem

#endif // SMEM_SMEM_GROUP_MANAGER_DEF_H
