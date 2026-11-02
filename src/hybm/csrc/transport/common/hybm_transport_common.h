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

#ifndef MF_HYBRID_HYBM_TRANSPORT_COMMON_H
#define MF_HYBRID_HYBM_TRANSPORT_COMMON_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <ostream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include "hybm_def.h"
#include "hybm_define.h"
#include "dl_hccp_def.h"

namespace ock {
namespace mf {
namespace transport {
constexpr uint32_t REG_MR_FLAG_DRAM = 0x1U;
constexpr uint32_t REG_MR_FLAG_HBM = 0x2U;
constexpr uint32_t REG_MR_FLAG_SELF = 0x4U;
constexpr uint32_t REG_MR_FLAG_ACL_DRAM = 0x8U;

constexpr int32_t REG_MR_ACCESS_FLAG_LOCAL_WRITE = 0x1;
constexpr int32_t REG_MR_ACCESS_FLAG_REMOTE_WRITE = 0x2;
constexpr int32_t REG_MR_ACCESS_FLAG_REMOTE_READ = 0x4;
constexpr int32_t REG_MR_ACCESS_FLAG_BOTH_READ_WRITE = 0x7;
constexpr uint32_t KEY_SIZE = 14;

enum TransportType {
    TT_HCCP = 0,
    TT_HCOM,
    TT_COMPOSE,
    TT_SDMA,
    TT_BUTT,
};

struct TransportOptions {
    uint32_t rankId;
    uint32_t rankCount;
    uint32_t protocol;
    hybm_type initialType;
    hybm_role_type role;
    hybm_scene scene{HYBM_SCENE_DEFAULT}; // 场景，传输层据此区分 BM / TRANS 等建链语义
    std::string nic;
    hybm_tls_config tlsOption;
};

static inline std::ostream &operator<<(std::ostream &output, const TransportOptions &options)
{
    output << "TransportOptions(rankId=" << options.rankId << ", count=" << options.rankCount
           << ", scene=" << options.scene << ", nic=" << options.nic << ")";
    return output;
}

struct TransportMemoryRegion {
    uint64_t addr = 0;                                   /* virtual address of memory could be hbm or host dram */
    uint64_t size = 0;                                   /* size of memory to be registered */
    int32_t access = REG_MR_ACCESS_FLAG_BOTH_READ_WRITE; /* access right by local and remote */
    uint32_t flags = 0;                                  /* optional flags: 加一个flag标识是DRAM还是HBM */
};

static inline std::ostream &operator<<(std::ostream &output, const TransportMemoryRegion &mr)
{
    output << "MemoryRegion(size=0x" << std::hex << mr.size << ", addr=0x" << mr.addr << ", access=0x" << mr.access
           << ", flags=0x" << mr.flags << ")";
    return output;
}

// Legacy slot indices kept for device-internal payload layouts (HCOMM meta/flag offsets).
constexpr uint32_t TRANS_KEY_DEV_SLOT1 = KEY_SIZE;     // start of slot 1
constexpr uint32_t TRANS_KEY_DEV_SLOT2 = KEY_SIZE * 2; // start of slot 2
constexpr uint32_t TRANS_KEY_DEV_SLOT3 = KEY_SIZE * 3; // start of slot 3
constexpr uint32_t TRANS_KEY_HOST_SLOT = KEY_SIZE * 6; // legacy host slot
constexpr uint32_t TRANS_KEY_TOTAL_SLOTS = 7;          // legacy total slots (6 device + 1 host)

// A compose-merged key is a self-describing envelope that carries the device and host payloads back to back:
//   [0] = device word count, [1] = host word count, [2, 2+deviceWords) = device payload, then host payload.
// Each payload is variable length, so enabled transports only pay for the words they actually use.
constexpr uint32_t TRANS_KEY_ENVELOPE_WORDS = 2;
constexpr uint32_t TRANS_KEY_MAX_WORDS = 4096U;

struct TransportMemoryKey {
    std::vector<uint64_t> keys;

    bool operator<(const TransportMemoryKey &other) const
    {
        return std::lexicographical_compare(keys.begin(), keys.end(), other.keys.begin(), other.keys.end());
    }

    bool operator==(const TransportMemoryKey &other) const
    {
        return keys == other.keys;
    }
    bool operator!=(const TransportMemoryKey &other) const
    {
        return !(*this == other);
    }

    uint8_t *ByteData()
    {
        return reinterpret_cast<uint8_t *>(keys.data());
    }
    const uint8_t *ByteData() const
    {
        return reinterpret_cast<const uint8_t *>(keys.data());
    }
    size_t WordCount() const
    {
        return keys.size();
    }
    size_t ByteSize() const
    {
        return keys.size() * sizeof(uint64_t);
    }
    bool Empty() const
    {
        return keys.empty();
    }

    void ResizeWords(size_t words)
    {
        keys.resize(words, 0);
    }
    void ResizeBytes(size_t bytes)
    {
        ResizeWords((bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
    }
};

struct TransportPrivateData {
    char ip[128]; // 支持 IPv6 URL，包括协议头和端口号 (tcp://[ipv6]:port)
    TransportMemoryKey key;
};

// Byte-view helpers for transport-specific payload structs carried inside a TransportMemoryKey.
template<class DataType>
inline void WriteStructToKey(const DataType &value, TransportMemoryKey &key)
{
    key.ResizeBytes(sizeof(DataType));
    std::memcpy(key.ByteData(), &value, sizeof(DataType));
}

template<class DataType>
inline bool ReadStructFromKey(const TransportMemoryKey &key, DataType &value)
{
    if (key.ByteSize() < sizeof(DataType)) {
        return false;
    }
    std::memcpy(&value, key.ByteData(), sizeof(DataType));
    return true;
}

// Merge a device payload into the compose envelope; host payload is appended separately by WriteHostMemoryKey.
inline void WriteDeviceMemoryKey(const TransportMemoryKey &deviceKey, TransportMemoryKey &merged)
{
    merged.keys.assign(TRANS_KEY_ENVELOPE_WORDS + deviceKey.keys.size(), 0);
    merged.keys[0] = deviceKey.keys.size();
    merged.keys[1] = 0;
    std::copy(deviceKey.keys.begin(), deviceKey.keys.end(), merged.keys.begin() + TRANS_KEY_ENVELOPE_WORDS);
}

inline void WriteHostMemoryKey(const TransportMemoryKey &hostKey, TransportMemoryKey &merged)
{
    const size_t deviceWords = (merged.keys.size() >= TRANS_KEY_ENVELOPE_WORDS) ? merged.keys[0] : 0U;
    const size_t prefix = TRANS_KEY_ENVELOPE_WORDS + deviceWords;
    merged.keys.resize(prefix + hostKey.keys.size(), 0);
    merged.keys[1] = hostKey.keys.size();
    std::copy(hostKey.keys.begin(), hostKey.keys.end(), merged.keys.begin() + prefix);
}

inline void ReadDeviceMemoryKey(const TransportMemoryKey &merged, TransportMemoryKey &deviceKey)
{
    deviceKey.keys.clear();
    if (merged.keys.size() < TRANS_KEY_ENVELOPE_WORDS) {
        return;
    }
    const size_t deviceWords = merged.keys[0];
    if (TRANS_KEY_ENVELOPE_WORDS + deviceWords > merged.keys.size()) {
        return;
    }
    deviceKey.keys.assign(merged.keys.begin() + TRANS_KEY_ENVELOPE_WORDS,
                          merged.keys.begin() + TRANS_KEY_ENVELOPE_WORDS + deviceWords);
}

inline void ReadHostMemoryKey(const TransportMemoryKey &merged, TransportMemoryKey &hostKey)
{
    hostKey.keys.clear();
    if (merged.keys.size() < TRANS_KEY_ENVELOPE_WORDS) {
        return;
    }
    const size_t deviceWords = merged.keys[0];
    const size_t hostWords = merged.keys[1];
    if (TRANS_KEY_ENVELOPE_WORDS + deviceWords + hostWords > merged.keys.size()) {
        return;
    }
    const auto begin = merged.keys.begin() + TRANS_KEY_ENVELOPE_WORDS + deviceWords;
    hostKey.keys.assign(begin, begin + hostWords);
}

static inline std::ostream &operator<<(std::ostream &output, const TransportMemoryKey &key)
{
    output << "MemoryKey";
    for (auto word : key.keys) {
        output << "-" << word;
    }
    return output;
}

struct TransportRankPrepareInfo {
    std::string nic;
    TransportPrivateData privateData{};
    hybm_role_type role{HYBM_ROLE_PEER};
    std::vector<TransportMemoryKey> memKeys;
    TransportRankPrepareInfo() {}
    TransportRankPrepareInfo(std::string n, TransportMemoryKey k) : nic{std::move(n)}, memKeys{k} {}
    TransportRankPrepareInfo(std::string n, std::vector<TransportMemoryKey> ks)
        : nic{std::move(n)}, memKeys{std::move(ks)}
    {}
};

static inline std::ostream &operator<<(std::ostream &output, const TransportRankPrepareInfo &info)
{
    output << "PrepareInfo(nic=" << info.nic << ", role=" << info.role << ", privateData=(ip=" << info.privateData.ip
           << ", key=" << info.privateData.key << "), memKeys=[";
    for (auto &key : info.memKeys) {
        output << key << " ";
    }
    output << "])";
    return output;
}

struct HybmTransPrepareOptions {
    std::unordered_map<uint32_t, TransportRankPrepareInfo> options;
};

static inline std::ostream &operator<<(std::ostream &output, const HybmTransPrepareOptions &info)
{
    output << "PrepareOptions(";
    for (auto &op : info.options) {
        output << op.first << " => " << op.second << ", ";
    }
    output << ")";
    return output;
}

} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_TRANSPORT_COMMON_H
