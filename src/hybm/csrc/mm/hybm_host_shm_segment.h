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
#ifndef MF_HYBRID_HYBM_HOST_SHM_SEGMENT_H
#define MF_HYBRID_HYBM_HOST_SHM_SEGMENT_H

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "hybm_mem_segment.h"
#include "hybm_mem_common.h"

namespace ock {
namespace mf {

// Export info structure for HOST_SHM segment, independent from HybmConnBasedSegment
struct ShmExportInfo {
    uint64_t magic{DRAM_SLICE_EXPORT_INFO_MAGIC};
    uint64_t version{EXPORT_INFO_VERSION};
    uint64_t mappingOffset{0};
    uint32_t sliceIndex{0};
    uint32_t rankId{0};
    uint64_t size{0};
    MemPageTblType pageTblType{};
    MemSegType memSegType{};
    MemSegInfoExchangeType exchangeType{};
    bool useHugetlbfs{false};
    // useMemfd=true 时本地大页由 memfd_create(MFD_HUGETLB) 支撑，不要求挂载 /dev/hugepages；
    // 对端通过 /proc/<memfdPid>/fd/<memfdFd> 打开同一块共享内存
    bool useMemfd{false};
    int32_t memfdPid{0};
    int32_t memfdFd{-1};
    char padding_[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 64]{};
};

static_assert(sizeof(ShmExportInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE, "ShmExportInfo must be 192 bytes");

class HybmHostShmSegment : public MemSegment {
public:
    explicit HybmHostShmSegment(const MemSegmentOptions &options, int eid) : MemSegment{options, eid} {}
    ~HybmHostShmSegment() override;

    Result ValidateOptions() noexcept override;
    Result ReserveMemorySpace(void **address) noexcept override;
    Result UnReserveMemorySpace() noexcept override;
    Result AllocLocalMemory(uint64_t size, std::shared_ptr<MemSlice> &slice) noexcept override;
    Result Export(std::string &exInfo) noexcept override;
    Result Export(const std::shared_ptr<MemSlice> &slice, std::string &exInfo) noexcept override;
    Result Import(const std::vector<std::string> &allExInfo, void *addresses[],
                  uint32_t overrideRank) noexcept override;
    Result Mmap() noexcept override;
    Result Unmap() noexcept override;
    MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept override;
    bool MemoryInRange(const void *begin, uint64_t size) const noexcept override;
    bool IsLocalRange(const void *begin, uint64_t size) const noexcept override;
    Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept override;
    Result RegisterMemory(const void *addr, uint64_t size, std::shared_ptr<MemSlice> &slice) noexcept override;
    Result ReleaseSliceMemory(const std::shared_ptr<MemSlice> &slice) noexcept override;
    Result GetExportSliceSize(size_t &size) noexcept override;
    hybm_mem_type GetMemoryType() const noexcept override
    {
        return HYBM_MEM_TYPE_HOST;
    }

private:
    // 对端 rank 共享内存的来源描述：memfd（经 /proc/<pid>/fd/<fd> 打开）或具名文件（/dev/hugepages、/dev/shm）
    struct ImportedShmSource {
        bool useHugetlbfs{false};
        bool useMemfd{false};
        int32_t pid{0};
        int32_t fd{-1};
    };

    void FreeMemory() noexcept;
    std::string GetShmFilePath(uint32_t rankId) const noexcept;
    std::string GetShmFilePath(uint32_t rankId, bool useHugetlbfs) const noexcept;
    Result MapLocalShm() noexcept;
    bool TryMapLocalMemfdHuge() noexcept;
    Result MapImportedShm(const ShmExportInfo &im) noexcept;
    Result RemapRemoteAsReserved(uint32_t rankId) noexcept;
    void CloseImportedShmFds() noexcept;
    bool TryHugetlbfsAvailable() noexcept;
    uint64_t ReserveLva(const ShmExportInfo &im) noexcept;
    int OpenImportedShmFd(uint32_t rankId) noexcept;

private:
    static uint64_t sUsedOffset_;

    uint64_t totalVirtualSize_{0UL};
    uint8_t *localVirtualBase_{nullptr};
    uint64_t allocatedSize_{0UL};
    uint32_t sliceCount_{0};
    std::map<uint32_t, MemSliceStatus> slices_;
    std::map<uint32_t, std::string> exportMap_;
    std::vector<ShmExportInfo> imports_;
    std::set<uint64_t> mappedGvaMem_;
    std::unordered_map<uint32_t, int> importedShmFds_;
    std::unordered_map<uint32_t, ImportedShmSource> importedSources_;
    int localShmFd_{-1};
    bool useHugetlbfs_{false};
    bool useMemfd_{false};
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_HOST_SHM_SEGMENT_H
