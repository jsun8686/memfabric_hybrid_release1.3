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
#ifndef MF_HYBRID_HYBM_ACL_NOTIFY_H
#define MF_HYBRID_HYBM_ACL_NOTIFY_H

#include <cstdint>
#include <memory>

constexpr uint32_t ACL_NOTIFY_WAIT_TIMEOUT_S = 60U;

namespace ock {
namespace mf {
class HybmAclNotify {
public:
    explicit HybmAclNotify(void *aclStream = nullptr, uint32_t timeoutSec = ACL_NOTIFY_WAIT_TIMEOUT_S) noexcept
        : aclStream_(aclStream), timeoutSec_(timeoutSec)
    {}
    virtual ~HybmAclNotify();

    HybmAclNotify(const HybmAclNotify &) = delete;
    HybmAclNotify &operator=(const HybmAclNotify &) = delete;
    HybmAclNotify(HybmAclNotify &&other) noexcept;
    HybmAclNotify &operator=(HybmAclNotify &&other) noexcept;

    int32_t Init(uint64_t regBlockBase = 0U);

    int32_t Wait();

    bool Inited() const;
    void *GetStream() const;
    uint32_t GetNotifyId() const;
    uint64_t GetOffset() const;
    uint64_t GetRegAddr() const;
    uint32_t GetRegLen() const;
    void Reset() noexcept;

private:
    int32_t ResolveRegAddr(uint64_t regBlockBase);
    int32_t ResolveRegAddrByRt(uint32_t notifyId);
    int32_t ResolveRegOffsetByHal(uint32_t notifyId, uint64_t &offset);

    void *notify_{nullptr};
    uint32_t notifyId_{UINT32_MAX};
    uint64_t regAddr_{0U};
    uint32_t regLen_{0U};
    uint64_t offset_{0U};
    void *aclStream_{nullptr};
    uint32_t timeoutSec_{ACL_NOTIFY_WAIT_TIMEOUT_S};
    bool inited_{false};
};

inline bool HybmAclNotify::Inited() const
{
    return inited_;
}

inline void *HybmAclNotify::GetStream() const
{
    return aclStream_;
}

inline uint32_t HybmAclNotify::GetNotifyId() const
{
    return notifyId_;
}

inline uint64_t HybmAclNotify::GetOffset() const
{
    return offset_;
}

inline uint64_t HybmAclNotify::GetRegAddr() const
{
    return regAddr_;
}

inline uint32_t HybmAclNotify::GetRegLen() const
{
    return regLen_;
}

using HybmAclNotifyPtr = std::shared_ptr<HybmAclNotify>;
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_ACL_NOTIFY_H
