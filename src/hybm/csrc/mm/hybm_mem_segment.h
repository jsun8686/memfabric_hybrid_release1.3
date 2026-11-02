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
#ifndef MEM_FABRIC_HYBRID_HYBM_MEM_SEGMENT_H
#define MEM_FABRIC_HYBRID_HYBM_MEM_SEGMENT_H

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "hybm_common_include.h"
#include "hybm_mem_common.h"
#include "hybm_mem_slice.h"
#include "hybm_transport_common.h"

namespace ock {
namespace mf {
struct TransportExtraInfo {
    uint32_t rankId;
    transport::TransportMemoryKey memKey;
};

class MemSegment;
using MemSegmentPtr = std::shared_ptr<MemSegment>;

struct MemSliceStatus {
    MemSlicePtr slice;
    void *handle;

    explicit MemSliceStatus(MemSlicePtr s) noexcept : slice{std::move(s)}, handle(nullptr) {}
    MemSliceStatus(MemSlicePtr s, void *h) noexcept : slice{std::move(s)}, handle(h) {}
};

class MemSegment {
public:
    static MemSegmentPtr Create(const MemSegmentOptions &options, int entityId);

public:
    explicit MemSegment(const MemSegmentOptions &options, int eid) : options_{options}, entityId_{eid} {}
    virtual ~MemSegment() = default;

    /*
     * Validate options
     * @return 0 if ok
     */
    virtual Result ValidateOptions() noexcept = 0;

    virtual Result ReserveMemorySpace(void **address) noexcept = 0;

    virtual Result UnReserveMemorySpace() noexcept = 0;

    /*
     * Allocate memory according to segType
     * @return 0 if successful
     */
    virtual Result AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept = 0;

    /*
     * register memory according to segType
     * @return 0 if successful
     */
    virtual Result RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept = 0;

    /*
     * release one slice
     * @return 0 if successful
     */
    virtual Result ReleaseSliceMemory(const MemSlicePtr &slice) noexcept = 0;

    /*
     * Export exchange info according to infoExType
     * @return exchange info
     */
    virtual Result Export(std::string &exInfo) noexcept = 0;

    virtual Result Export(const MemSlicePtr &slice, std::string &exInfo) noexcept = 0;

    virtual Result GetExportSliceSize(size_t &size) noexcept = 0;

    /*
     * Import exchange info and translate it into data structure
     * @param allExInfo
     * @param overrideRank not HYBM_IMPORT_RANK_INVALID: replace the rankId carried in exInfo with this local
     *        rank, and remap the imported gva into the overrideRank's window (trans scene: one entity per
     *        process, each uniqueId connection gets a unique local rank). Only relevant to segments carrying
     *        rankId/gva layout.
     */
    virtual Result Import(const std::vector<std::string> &allExInfo, void *addresses[],
                          uint32_t overrideRank = HYBM_IMPORT_RANK_INVALID) noexcept = 0;

    /*
     * delete imported memory area according to rankid
     * @return 0 if successful
     */
    virtual Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept = 0;

    /*
     * @brief after Import exchange info, should call this func to make it work
     * @return 0 if successful
     */
    virtual Result Mmap() noexcept = 0;

    /*
     * After remove imported exchange info, should call this func to make it work
     * @return 0 if successful
     */
    virtual Result Unmap() noexcept = 0;

    /*
     * Attempt to cast the generic slice pointer to a MemSlicePtr.
     * Set 'quiet=true' to suppress error logging if this is part of a fallback or trial attempt.
     * @return pointer to MemSlice
     */
    virtual MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet = false) const noexcept = 0;

    /*
     * check memery area in this segment
     * @return true if in range
     */
    virtual bool MemoryInRange(const void *begin, uint64_t size) const noexcept = 0;

    /*
     * check whether address range belongs to the local portion of this segment
     * @return true if within the local rank's allocated portion
     */
    virtual bool IsLocalRange(const void *begin, uint64_t size) const noexcept
    {
        (void)begin;
        (void)size;
        return false;
    }

    /*
     * get memory type
     */
    virtual hybm_mem_type GetMemoryType() const noexcept = 0;

    virtual bool CheckSdmaReaches(uint32_t rankId) const noexcept;

    static Result InitDeviceInfo(int devId);

    Result RegisterMemCommon(const void *addr, uint64_t size, MemSlicePtr &slice);

    // imported gva → 本实体导入映射地址（lva）：同进程多实体同址共存（多 store 组窗口号撞车）时，
    // 全局台账的同 gva 行无法区分连接，数据面须按实体域解析；未命中返回 false
    virtual bool ResolveRemoteGva(uint64_t gva, uint64_t &lva)
    {
        return false;
    }

    // ---- gva -> remote rank resolution ----
    // VA layout invariant: gva = globalVirtualAddress_ + rank * maxSize + delta for any slice (delta < maxSize).
    // All 5 segments reserve by this layout in ReserveMemorySpace (locally base + localRank * maxSize),
    // and an imported gva also falls in the peer rank's window, so the rank is derived from the address with
    // no index table to maintain.
    // A miss (not reserved / outside the window) returns false and the caller falls back to its local rank.
    bool FindRemoteRankByVa(uint64_t va, uint32_t &remoteRank) const
    {
        if (globalVirtualAddress_ == nullptr || options_.maxSize == 0U || options_.rankCnt == 0U) {
            return false;
        }
        const uint64_t base = reinterpret_cast<uint64_t>(globalVirtualAddress_);
        // The lower bound must be checked first: if va < base the unsigned subtraction underflows into a
        // huge but "legitimate" rank.
        if (va < base) {
            return false;
        }
        const uint64_t offset = va - base;
        if (offset >= options_.maxSize * options_.rankCnt) {
            return false;
        }
        remoteRank = static_cast<uint32_t>(offset / options_.maxSize);
        return true;
    }

protected:
    static bool CanLocalHostReaches(uint32_t superPodId, uint32_t serverId, uint32_t deviceId) noexcept;
    static bool CanSdmaReaches(uint32_t superPodId, uint32_t serverId, uint32_t deviceId) noexcept;
    static void FillSysBootIdInfo() noexcept;
    static Result EnableRemotePeerAccess(int32_t remotePhyId) noexcept;

protected:
    const MemSegmentOptions options_;
    const int entityId_;
    // GVA base of the reserved region, set by each segment in ReserveMemorySpace(); FindRemoteRankByVa uses it
    uint8_t *globalVirtualAddress_{nullptr};

    static bool deviceInfoReady_;
    static int deviceId_;
    static int logicDeviceId_;
    static int devicePhyId_;
    static uint32_t pid_;
    static uint32_t sdid_;
    static uint32_t serverId_;
    static uint32_t superPodId_;
    static uint32_t bootIdHead_;
    static std::string sysBoolId_;
    static AscendSocType socType_;
    uint32_t sliceCount_{0};

private:
    static void ResetDeviceInfoInChild() noexcept;
    static bool atforkRegistered_;
};
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_MEM_SEGMENT_H
