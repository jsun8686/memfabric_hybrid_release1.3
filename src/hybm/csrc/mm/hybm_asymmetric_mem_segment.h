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

#ifndef MF_HYBRID_HYBM_ASYMMETRIC_MEM_SEGMENT_H
#define MF_HYBRID_HYBM_ASYMMETRIC_MEM_SEGMENT_H

#include <atomic>
#include <bitset>

#include "dl_hal_api.h"
#include "hybm_dev_legacy_segment.h"
#include "hybm_mem_segment.h"

namespace ock {
namespace mf {
constexpr size_t MAX_PEER_DEVICES = 16;
// 共享名首字节类型标记：IPC（C 字符串名）或 VMM（128B 二进制 share_handle）
constexpr uint16_t USER_HBM_NAME_TYPE_IPC = 0x1U;
constexpr uint16_t USER_HBM_NAME_TYPE_VMM = 0x2U;
// max(DEVICE_SHM_NAME_SIZE + 1, MEM_SHARE_HANDLE_LEN) + 1：1 字节类型标记 + IPC 名或 128B share_handle
constexpr uint32_t USER_HBM_NAME_MAX_LEN = MEM_SHARE_HANDLE_LEN + 1U;
struct RegisterSlice {
    MemSlicePtr slice;
    std::string name;       // HBM shared 为带类型标记的定长名（129B）；DRAM/非 shared 为空串
    bool hostMapped{false}; // DRAM 注册时是否执行过 HalHostRegister（释放时对称回滚）
    RegisterSlice() = default;
    RegisterSlice(MemSlicePtr s, std::string n) noexcept : slice(std::move(s)), name(std::move(n)) {}
    RegisterSlice(MemSlicePtr s, std::string n, bool mapped) noexcept
        : slice(std::move(s)), name(std::move(n)), hostMapped(mapped)
    {}
};

struct HbmExportDeviceInfo {
    uint64_t magic{ENTITY_EXPORT_INFO_MAGIC};
    uint32_t segmentType{SEGMENT_TYPE_USER_DEV};
    uint32_t sdid{0};
    uint32_t pid{0};
    uint32_t serverId{0};
    uint32_t superPodId{0};
    uint32_t rankId{0};
    uint32_t devicePhyId{0};

    // Padding to make total size UNIFIED_EXCHANGE_SEG_INFO_SIZE(192) bytes
    char padding_[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 36]{};
};
static_assert(sizeof(HbmExportDeviceInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE,
              "HbmExportDeviceInfo must be UNIFIED_EXCHANGE_SEG_INFO_SIZE(192) bytes,"
              " compatible with HostSdmaExportInfo");
static_assert(offsetof(HbmExportDeviceInfo, segmentType) == SEGMENT_TYPE_OFFSET, "segmentType offset mismatch!");

// trans 用户注册 slice 的统一导出结构：HBM/DRAM 共用一个 magic，
// 介质由 segmentType（USER_DEV/USER_DRAM）区分，导入端据此选择映射路径
struct UserSliceExportInfo {
    uint64_t magic{USER_MEM_SLICE_EXPORT_INFO_MAGIC};
    uint32_t segmentType{SEGMENT_TYPE_USER_DEV};
    uint32_t serverId{0};
    uint64_t gvaOffset{0}; // offset from globalVirtualAddress_ (this entity's reserved GVA base)
    uint64_t address{0};   // lva（HBM: 设备 VA；DRAM: host VA）
    uint64_t size{0};
    uint32_t superPodId{0};
    uint32_t rankId{0};
    uint32_t devicePhyId{0};
    // 共享名：name[0] 为类型标记（USER_HBM_NAME_TYPE_*），name+1 为 IPC 名（C 字符串）或
    // 128B 二进制 share_handle（可含 \0，全链路必须定长构造 std::string，禁止 strlen 截断）；
    // DRAM/非 shared 恒为全零
    char name[USER_HBM_NAME_MAX_LEN]{};

    // Padding to make total size UNIFIED_EXCHANGE_SEG_INFO_SIZE(192) bytes
    char padding_[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 181]{};
};
static_assert(sizeof(UserSliceExportInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE, "UserSliceExportInfo must be 192 bytes");
static_assert(offsetof(UserSliceExportInfo, segmentType) == SEGMENT_TYPE_OFFSET, "segmentType offset mismatch!");

class AsymmetricMemSegment : public HybmDevLegacySegment {
public:
    AsymmetricMemSegment(const MemSegmentOptions &options, int eid) noexcept;
    ~AsymmetricMemSegment() override;
    Result ValidateOptions() noexcept override;
    Result ReserveMemorySpace(void **address) noexcept override;
    Result UnReserveMemorySpace() noexcept override;
    Result AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept override;
    Result RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept override;
    Result ReleaseSliceMemory(const MemSlicePtr &slice) noexcept override;
    Result Export(std::string &exInfo) noexcept override;
    Result Export(const MemSlicePtr &slice, std::string &exInfo) noexcept override;
    Result GetExportSliceSize(size_t &size) noexcept override;
    Result Import(const std::vector<std::string> &allExInfo, void *addresses[],
                  uint32_t overrideRank) noexcept override;
    Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept override;
    Result Mmap() noexcept override;
    Result Unmap() noexcept override;
    MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept override;
    bool MemoryInRange(const void *begin, uint64_t size) const noexcept override;
    void CloseMemory() noexcept;
    hybm_mem_type GetMemoryType() const noexcept override
    {
        // 段主类型为 DEVICE；slice 级内存类型以 MemSlice::memType_ 为准
        // （唯一调用点 AllocMemLocal 不经过本段）
        return HYBM_MEM_TYPE_DEVICE;
    }
    bool CheckSdmaReaches(uint32_t rankId) const noexcept override;

private:
    Result RegisterDeviceMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept;
    Result RegisterHostMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept;
    Result CheckHostRegisterAllowed() const noexcept;
    bool NeedHostDeviceMapping() const noexcept;
    Result ExportSlice(const RegisterSlice &regSlice, std::string &exInfo) noexcept;
    Result ImportDeviceInfo(const std::string &info, uint32_t overrideRank) noexcept;
    Result ImportSliceInfo(const std::string &info, MemSlicePtr &remoteSlice, uint32_t overrideRank) noexcept;
    Result ImportHbmSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept;
    Result ImportDramSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept;
    // 幂等查找：已导入过（白名单重试/命令重放）时返回既有 slice，避免重复映射与重复计数
    MemSlicePtr FindReusableImport(uint64_t expectGva) noexcept;
    Result EnsurePeerAccess(uint32_t remotePhyId) noexcept;
    void ReleaseHostMapping(uint64_t hostVa) noexcept; // HalHostUnregisterEx 封装
    void RollbackImportedSlices(const std::vector<MemSlicePtr> &slices) noexcept;
    void RemoveSliceInfo(const uint32_t rankId) noexcept;
    bool ResolveRemoteGva(uint64_t gva, uint64_t &lva) override;

    // ===== IPC/VMM 双路径共享 helper：分流点集中在以下函数，业务函数只感知 name 类型 =====
    Result UserMemExportName(const void *ptr, uint64_t len, char *name) noexcept;
    Result ExportVmmShareName(const void *ptr, char *name) noexcept;
    Result UserMemSetSuperPodPid(const char *name, uint32_t sdid, uint32_t pid) noexcept;
    uint64_t ReserveLva(const UserSliceExportInfo &info) noexcept;
    Result UserMemImportAndMapName(void **ptr, const UserSliceExportInfo &info) noexcept;
    Result ImportAndMapVmmName(void **ptr, const UserSliceExportInfo &info) noexcept;
    void UserMemUnImportName(uint32_t sliceId) noexcept;
    void CloseRemoteIpcMemory(const RegisterSlice &reg) noexcept;
    void UnmapRemoteVmmMemory(const RegisterSlice &reg) noexcept;
    Result UserMemDestroyName(const char *name) noexcept;

private:
    std::mutex mutex_;
    std::bitset<MAX_PEER_DEVICES> enablePeerDevices_;
    std::map<uint32_t, RegisterSlice> registerSlices_;
    std::map<uint32_t, RegisterSlice> remoteSlices_;
    std::map<uint32_t, std::vector<MemSlicePtr>> rankToRemoteSlices_;
    std::map<uint32_t, HbmExportDeviceInfo> importedDeviceInfo_;
    std::map<std::string, UserSliceExportInfo> importedSliceInfo_;
    std::map<uint64_t, drv_mem_handle_t *> vmmLvaToHandle_{};
    std::vector<std::string> memNames_{};
    std::atomic<bool> unReserveDone_{false};
};
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_ASYMMETRIC_MEM_SEGMENT_H
