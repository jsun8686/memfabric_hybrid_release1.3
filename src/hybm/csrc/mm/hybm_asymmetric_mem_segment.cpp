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
#include "hybm_asymmetric_mem_segment.h"

#include <algorithm>
#include <cstring>

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_networks_common.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {
constexpr uint8_t MAX_DEVICE_COUNT = 16;
AsymmetricMemSegment::AsymmetricMemSegment(const MemSegmentOptions &options, int eid) noexcept
    : HybmDevLegacySegment{options, eid}
{}

AsymmetricMemSegment::~AsymmetricMemSegment()
{
    UnReserveMemorySpace();
}

Result AsymmetricMemSegment::ValidateOptions() noexcept
{
    return BM_OK;
}

Result AsymmetricMemSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "address is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(options_.enable56BitsGva == true,
                             "options_.enable56BitsGva = " << options_.enable56BitsGva, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCnt,
                             "rank(" << options_.rankId << ") but total " << options_.rankCnt, BM_INVALID_PARAM);

    totalVirtualSize_ = options_.maxSize * options_.rankCnt;
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(
        {totalVirtualSize_, 0, HYBM_MEM_TYPE_DEVICE, options_.enable56BitsGva, options_.scene});
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "gvaInfo.va[HVM_GVA] = " << gvaInfo.va[HVM_GVA], BM_ERROR);
    globalVirtualAddress_ = reinterpret_cast<uint8_t *>(gvaInfo.va[HVM_GVA]);
    lvaBase_ = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    unReserveDone_ = false;
    return BM_OK;
}

Result AsymmetricMemSegment::UnReserveMemorySpace() noexcept
{
    // Both ReleaseResources() and the destructor invoke this; only the first
    // call performs any release, later calls return silently.
    if (unReserveDone_.exchange(true)) {
        return BM_OK;
    }
    BM_LOG_INFO("un-reserve memory space.");
    if (!memNames_.empty() && options_.shared) {
        for (auto &name : memNames_) {
            (void)UserMemDestroyName(name.c_str());
        }
        BM_LOG_INFO("Finish to destroy memory names.");
    } else {
        BM_LOG_INFO("Sender does not need to destroy memory names.");
    }
    memNames_.clear();
    CloseMemory();
    return BM_OK;
}

Result AsymmetricMemSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    BM_LOG_ERROR("AsymmetricMemSegment NOT SUPPORT AllocLocalMemory");
    return BM_NOT_SUPPORTED;
}

namespace {
// 用户注册内存判型：优先 HAL 属性查询（drvMemGetAttribute，A2/A3/A5 均支持，比地址段判断更准确）；
// 查询失败（pDrvMemGetAttribute 为 OPTIONAL 加载，老 HAL/无卡环境为空）或 no_xpu 构建
// （宏编译隔离）时，回退 HBM 地址段判断。属性解读与 HybmVaManager::GetLocalMemoryType 的
// ASCEND_950 路径保持一致。
bool IsDeviceMemoryAddr(uint64_t va)
{
#if defined(ASCEND_NPU)
    DVattribute attr{};
    const auto ret = DlHalApi::DrvMemGetAttribute(static_cast<DVdeviceptr>(va), &attr);
    // 打印 HAL 返回的原始 memType：A5(950) 为 SVM 统一编址架构，对 host 用户态内存也可能返回
    // SVM/LOCK 类属性（语义是"统一空间内设备可达"而非"物理介质为 HBM"），需实测确认取值域。
    // 注意 dl_hal_api_def.h 另有一套位域定义（MEM_PHY_BIT=14: MEM_TYPE_DDR/HBM），若 rawMemType
    // 命中位域形态，介质判型应改用 phy 位而非 DV_MEM_* 三值全等。
    BM_LOG_DEBUG("DrvMemGetAttribute: va=" << VaToStr(va) << " ret=" << ret
                                           << " rawMemType=" << static_cast<uint32_t>(attr.memType));
    if (ret == BM_OK) {
        return attr.memType == DV_MEM_SVM_DEVICE || attr.memType == DV_MEM_LOCK_DEV ||
               attr.memType == DV_MEM_LOCK_DEV_DVPP || attr.memType == DV_MEM_LOCK_HOST;
    }
    BM_LOG_DEBUG("DrvMemGetAttribute unavailable, fallback to address check: va=" << VaToStr(va));
#endif
    return IsHbmAddr(va);
}
} // namespace

namespace {
// 共享名类型提取：空名/未知首字节返回 0（调用方按 no-op 处理）
inline uint16_t UserMemNameType(const char *name)
{
    return name == nullptr ? 0U : static_cast<uint16_t>(static_cast<uint8_t>(name[0]));
}

inline uint16_t UserMemNameType(const std::string &name)
{
    return name.empty() ? 0U : static_cast<uint16_t>(static_cast<uint8_t>(name[0]));
}

// 日志用共享名预览：IPC 按跳过类型字节的 C 字符串打印；VMM share_handle 为二进制（可含 \0），
// 仅打印类型标识与前 8 字节十六进制，避免二进制数据污染日志
std::string UserMemNamePreview(const char *name)
{
    if (name == nullptr) {
        return "<null>";
    }
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_VMM) {
        return std::string(name + 1);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string preview = "vmm:";
    for (size_t i = 1; i <= 8; ++i) {
        const auto byte = static_cast<uint8_t>(name[i]);
        preview.push_back(kHex[byte >> 4]);
        preview.push_back(kHex[byte & 0xF]);
    }
    return preview;
}

std::string UserMemNamePreview(const std::string &name)
{
    return UserMemNamePreview(name.c_str());
}

// 释放 VMM handle 的静默封装：失败仅告警（引用泄漏风险），不打断调用方主流程
void ReleaseVmmHandleQuietly(drv_mem_handle_t *handle)
{
    if (handle == nullptr) {
        return;
    }
    const auto ret = DlHalApi::HalMemRelease(handle);
    if (ret != 0) {
        BM_LOG_WARN("HalMemRelease failed, handle may leak: ret=" << ret);
    }
}
} // namespace

Result AsymmetricMemSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    if (addr == nullptr || size == 0) {
        BM_LOG_ERROR("input address parameter is invalid, rankId=" << options_.rankId);
        return BM_INVALID_PARAM;
    }

    // 同一已注册区间内的重复注册复用既有 slice（首次调用注册生效，后续跳过），
    // 避免 offload 池多段注册触发 2MiB 对齐重叠与重复导出；对端按首个锚点+偏移寻址
    const auto va = reinterpret_cast<uint64_t>(addr);
    for (auto &it : registerSlices_) {
        const auto &exist = it.second.slice;
        const auto base = exist->vAddress_;
        if (va >= base && va + size <= base + exist->size_) {
            BM_LOG_INFO("fake register, reuse existing slice: rankId=" << options_.rankId << " addr=" << VaToStr(va)
                                                                       << " size=" << size << " sliceIdx=" << it.first);
            slice = exist;
            return BM_OK;
        }
    }

    //offload 池内存（GVM 窗口）首次注册时，向 VaManager 查分配时登记的整段区间，
    // 以整个 allocation 的 [base, size) 注册（单分配=单 handle=单锚点）；后续落在该区间内的
    // 注册由上方复用逻辑跳过
    uint64_t regBase = va;
    uint64_t regSize = size;
    const uint64_t gvmStart =
        socType_ == AscendSocType::ASCEND_950 ? HYBM_GLOBAL_GVM_START_ADDR_A5 : HYBM_GLOBAL_GVM_START_ADDR;
    if (va >= gvmStart && va < gvmStart + HYBM_GVM_MAX_POOL_SIZE) {
        // 分配记录 HVA/DVA=allocAddr；导入记录 HVA=0、DVA=lva——按 DVA 查询对两种 rank 均命中
        auto [allocInfo, found] = HybmVaManager::GetInstance().FindAllocByVa(va, HVM_DVA);
        if (found && allocInfo.base.va[HVM_DVA] != 0) {
            regBase = allocInfo.base.va[HVM_DVA];
            regSize = allocInfo.base.size;
            BM_LOG_INFO("expand registration to alloc interval: rankId=" << options_.rankId << " addr=" << VaToStr(va)
                                                                         << " size=" << size << " regBase="
                                                                         << VaToStr(regBase) << " regSize=" << regSize);
        }
    }

    // allocatedSize_ is the bump pointer of this entity's window (lvaBase_ = globalVirtualAddress_ + rankId * maxSize),
    // so the bound is the window capacity options_.maxSize.
    if (allocatedSize_ + regSize > options_.maxSize) {
        BM_LOG_ERROR("gva space exhausted: rankId=" << options_.rankId << " allocated=" << allocatedSize_
                                                    << " request=" << regSize << " maxSize=" << options_.maxSize);
        return BM_ERROR;
    }

    if (IsDeviceMemoryAddr(reinterpret_cast<uint64_t>(regBase))) {
        return RegisterDeviceMemory(reinterpret_cast<const void *>(regBase), regSize, slice);
    }
    return RegisterHostMemory(reinterpret_cast<const void *>(regBase), regSize, slice);
}

Result AsymmetricMemSegment::RegisterDeviceMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    char name[USER_HBM_NAME_MAX_LEN]{};
    Result ret = BM_OK;
    if (options_.shared) {
        ret = UserMemExportName(addr, size, name);
        if (ret != BM_OK) {
            // 根因日志已在 UserMemExportName 内记录
            return ret;
        }
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto &remoteDev : importedDeviceInfo_) {
        if (!options_.shared ||
            !CanSdmaReaches(remoteDev.second.superPodId, remoteDev.second.serverId, remoteDev.second.devicePhyId)) {
            continue;
        }
        ret = UserMemSetSuperPodPid(name, remoteDev.second.sdid, remoteDev.second.pid);
        if (ret != BM_OK) {
            // 根因日志已在 UserMemSetSuperPodPid 内记录
            (void)UserMemDestroyName(name);
            return BM_DL_FUNCTION_FAILED;
        }
    }

    uint64_t gva = reinterpret_cast<uint64_t>(lvaBase_) + allocatedSize_;
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(addr), size);
    // onlyGva=true: the HBM segment does no device-side DVA translation and only needs the GVA table. The whole
    // record (including the dva/hva fields) is still stored in the GVA table, so the data-plane GVA->HVA / GVA->DVA
    // translation is unaffected; the previous host_rdma branch additionally wrote the DVA/HVA tables to match
    // the ConnBasedSegment layout, whose readers (HVA->GVA reverse lookup, HVA->DVA translation) either
    // already have a fallback or are not triggered on this media.
    ret = HybmVaManager::GetInstance().AddVaInfo({gva, slice->vAddress_, slice->vAddress_, size, HYBM_MEM_TYPE_DEVICE},
                                                 true);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " size=" << size
                                                 << " ret=" << ret);
        if (options_.shared) {
            (void)UserMemDestroyName(name);
        }
        slice = nullptr;
        return ret;
    }

    // VMM share_handle 为二进制（可含 \0），std::string 必须定长构造，禁止隐式 strlen 截断
    const std::string shareName = options_.shared ? std::string(name, USER_HBM_NAME_MAX_LEN) : std::string{};
    memNames_.emplace_back(shareName);
    registerSlices_.emplace(slice->index_, RegisterSlice{slice, shareName});
    allocatedSize_ += size;
    uniqueLock.unlock();
    return BM_OK;
}

Result AsymmetricMemSegment::CheckHostRegisterAllowed() const noexcept
{
    constexpr auto connOps = HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP |
                             HYBM_DOP_TYPE_HOST_SHM | HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA |
                             HYBM_DOP_TYPE_DEVICE_UBOE;
    // dataOpType=0（未指定传输方式，如 RECEIVER 角色的 smem_trans 注册）：NeedHostDeviceMapping()
    // 为 false，不做设备映射，仅登记 GVA，保持兼容放行
    if (options_.dataOpType == 0U) {
        return BM_OK;
    }
    // 通信语义放行,内存语义报错
    // 放行会导致数据面在无映射 GVA 上失败，必须在注册入口显式报错
    if ((options_.dataOpType & connOps) != 0U) {
        return BM_OK;
    }
    // 错误返回路径必须记录根因日志（代码规范）：rankId/socType/dataOpType 为关键参数
    BM_LOG_ERROR("register dram not supported: rankId=" << options_.rankId << " socType=" << static_cast<int>(socType_)
                                                        << " dataOpType=" << options_.dataOpType);
    return BM_NOT_SUPPORTED;
}

bool AsymmetricMemSegment::NeedHostDeviceMapping() const noexcept
{
    // 仅设备侧连接类 op 需要把 host DRAM 映射进设备地址空间；
    // SDMA 引擎不支持访问外部注册 DRAM（评审结论），纯 SDMA 注册已在 CheckHostRegisterAllowed 拒绝
    return (options_.dataOpType &
            (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) != 0U;
}

Result AsymmetricMemSegment::RegisterHostMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    auto ret = CheckHostRegisterAllowed();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);

    uint64_t hostVa = reinterpret_cast<uint64_t>(addr);
    uint64_t gva = reinterpret_cast<uint64_t>(lvaBase_) + allocatedSize_;
    uint64_t dva = 0;
    bool hostMapped = false;
    if (NeedHostDeviceMapping()) {
        // 复用已有映射：同一 HVA 已登记过 DVA 时（如同一段 DRAM 在另一实体注册过）直接复用，
        // 避免对同一地址重复 HalHostRegister 失败（驱动对已注册地址返回错误）
        dva = HybmVaManager::GetInstance().TransformVa(hostVa, HVM_HVA, HVM_DVA);
        if (dva == 0) {
            void *output = nullptr;
            auto halRet =
                DlHalApi::HalHostRegister(const_cast<void *>(addr), size, HOST_MEM_MAP_DEV, logicDeviceId_, &output);
            if (halRet != 0) {
                BM_LOG_ERROR("HalHostRegister failed: rankId=" << options_.rankId << " deviceId=" << logicDeviceId_
                                                               << " addr=" << VaToStr(hostVa) << " size=" << size
                                                               << " ret=" << halRet);
                return BM_DL_FUNCTION_FAILED;
            }
            dva = reinterpret_cast<uint64_t>(output);
            hostMapped = true;
        } else {
            BM_LOG_INFO("reuse existing dva mapping: rankId=" << options_.rankId << " hostVa=" << VaToStr(hostVa)
                                                              << " dva=" << VaToStr(dva));
        }
    }

    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva, hostVa, size);
    // Registering the same DRAM block via replay on several connections is auto-detected as an alias when
    // dva/hva/size/memType are all equal; only the new gva identity is recorded.
    ret = HybmVaManager::GetInstance().AddVaInfo({gva, dva, hostVa, size, HYBM_MEM_TYPE_HOST});
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " size=" << size
                                                 << " ret=" << ret);
        if (hostMapped) {
            ReleaseHostMapping(hostVa);
        }
        slice = nullptr;
        return ret;
    }

    std::unique_lock<std::mutex> uniqueLock{mutex_};
    registerSlices_.emplace(slice->index_, RegisterSlice{slice, "", hostMapped});
    allocatedSize_ += size;
    uniqueLock.unlock();
    BM_LOG_INFO("register dram memory: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " hostVa="
                                                << VaToStr(hostVa) << " dva=" << VaToStr(dva) << " size=" << size);
    return BM_OK;
}

void AsymmetricMemSegment::ReleaseHostMapping(uint64_t hostVa) noexcept
{
    auto ret = DlHalApi::HalHostUnregisterEx(reinterpret_cast<void *>(hostVa), logicDeviceId_, HOST_MEM_MAP_DEV);
    if (ret != 0) {
        BM_LOG_ERROR("HalHostUnregisterEx failed: rankId=" << options_.rankId << " deviceId=" << logicDeviceId_
                                                           << " hostVa=" << VaToStr(hostVa) << " ret=" << ret);
    }
}

Result AsymmetricMemSegment::EnsurePeerAccess(uint32_t remotePhyId) noexcept
{
    if (remotePhyId == static_cast<uint32_t>(devicePhyId_) || enablePeerDevices_.test(remotePhyId)) {
        return BM_OK;
    }
    auto ret = EnableRemotePeerAccess(static_cast<int32_t>(remotePhyId));
    if (ret != BM_OK) {
        return ret;
    }
    enablePeerDevices_.set(remotePhyId);
    BM_LOG_DEBUG("enable peer access for : " << remotePhyId);
    return BM_OK;
}

Result AsymmetricMemSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr.");
        return BM_INVALID_PARAM;
    }

    auto pos = registerSlices_.find(slice->index_);
    if (pos == registerSlices_.end()) {
        BM_LOG_ERROR("release slice : " << slice->index_ << " not exist.");
        return BM_INVALID_PARAM;
    }

    Result ret = BM_OK;
    if (pos->second.slice->memType_ == HYBM_MEM_TYPE_HOST) {
        // 先注销 VA 记录再决定是否释放设备映射：HVA/DVA 行被多实体别名记录共享
        // （同一段 DRAM 在 send/recv 实体注册），仅当本次注销使所有涉及行计数归零
        // （RemoveOneVaInfo 返回 true，映射已无存活引用者）时才 HalHostUnregisterEx，
        // 提前释放会使仍持有引用的对端实体数据面失效（驱动返回 BUSY）
        bool freed = HybmVaManager::GetInstance().RemoveOneVaInfo(pos->second.slice->gva_);
        if (pos->second.hostMapped && freed) {
            ReleaseHostMapping(pos->second.slice->vAddress_);
        }
    } else if (options_.shared) {
        ret = UserMemDestroyName(pos->second.name.c_str());
        if (ret != BM_OK) {
            // 根因日志已在 UserMemDestroyName 内记录
            return ret;
        }
        // 名称已即时销毁，需从 UnReserveMemorySpace 的待销毁列表移除，避免重复销毁
        auto nameIt = std::find(memNames_.begin(), memNames_.end(), pos->second.name);
        if (nameIt != memNames_.end()) {
            memNames_.erase(nameIt);
        }
    }

    registerSlices_.erase(pos);
    return ret;
}

Result AsymmetricMemSegment::Export(std::string &exInfo) noexcept
{
    HbmExportDeviceInfo info;
    info.devicePhyId = devicePhyId_;
    info.rankId = options_.rankId;
    info.pid = HybmDevLegacySegment::pid_;
    HybmDevLegacySegment::GetDeviceInfo(info.sdid, info.serverId, info.superPodId);
    auto ret = LiteralExInfoTranslater<HbmExportDeviceInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }

    BM_LOG_DEBUG("export device info(sdid=" << sdid_ << " pid=" << pid_ << " rank=" << info.rankId
                                            << " deviceId=" << devicePhyId_ << ")");
    return BM_OK;
}

Result AsymmetricMemSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    auto pos = registerSlices_.find(slice->index_);
    if (pos == registerSlices_.end()) {
        BM_LOG_ERROR("export slice not exist: index=" << slice->index_);
        return BM_INVALID_PARAM;
    }
    return ExportSlice(pos->second, exInfo);
}

Result AsymmetricMemSegment::ExportSlice(const RegisterSlice &regSlice, std::string &exInfo) noexcept
{
    uint32_t sdId;
    UserSliceExportInfo info;
    info.segmentType =
        (regSlice.slice->memType_ == HYBM_MEM_TYPE_DEVICE) ? SEGMENT_TYPE_USER_DEV : SEGMENT_TYPE_USER_DRAM;
    // gvaOffset is taken against the constant origin, so it is the same number in every process.
    info.gvaOffset = regSlice.slice->gva_ - reinterpret_cast<uint64_t>(globalVirtualAddress_);
    info.address = regSlice.slice->vAddress_;
    info.size = regSlice.slice->size_;
    info.devicePhyId = static_cast<uint32_t>(devicePhyId_);
    info.rankId = options_.rankId;
    HybmDevLegacySegment::GetDeviceInfo(sdId, info.serverId, info.superPodId);
    // HBM shared 携带带类型标记的定长共享名（129B，VMM share_handle 可含 \0，必须整段拷贝防截断）；
    // DRAM/非 shared 为空名，info.name 保持全零
    if (!regSlice.name.empty()) {
        std::memcpy(info.name, regSlice.name.data(), USER_HBM_NAME_MAX_LEN);
    }
    auto ret = LiteralExInfoTranslater<UserSliceExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export user slice failed: rankId=" << options_.rankId << " index=" << regSlice.slice->index_
                                                         << " ret=" << ret);
        return BM_ERROR;
    }

    BM_LOG_DEBUG("export user slice success. type:" << info.segmentType << " addr:" << VaToStr(info.address) << " size:"
                                                    << VaToStr(info.size) << " name:" << UserMemNamePreview(info.name)
                                                    << " rank:" << options_.rankId);
    return BM_OK;
}

Result AsymmetricMemSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(UserSliceExportInfo);
    return BM_OK;
}

void AsymmetricMemSegment::RollbackImportedSlices(const std::vector<MemSlicePtr> &slices) noexcept
{
    if (slices.empty()) {
        return;
    }
    for (auto &slice : slices) {
        // 按登记类型回滚（IPC close / VMM unmap+release）；未登记或空名条目为 no-op，
        // DRAM 与 GVA-only 条目不会被误传给 RtIpcCloseMemory（依赖 remoteSlices_，须先于账本清理调用）
        UserMemUnImportName(slice->index_);
    }
    {
        std::unique_lock<std::mutex> uniqueLock{mutex_};
        for (auto &slice : slices) {
            auto rIt = remoteSlices_.find(slice->index_);
            if (rIt == remoteSlices_.end()) {
                continue;
            }
            // 仅 HBM 条目登记过导入账本；DRAM 条目账本本就为空，erase 按键为 no-op
            importedSliceInfo_.erase(rIt->second.name);
            remoteSlices_.erase(rIt);
        }
        // rank 关系无条件清理：DRAM 条目不在 importedSliceInfo_，不能以账本查找结果为前提；
        // 清空后的 rank 键一并移除，保证回滚后导入重试可完整重新执行
        for (auto it = rankToRemoteSlices_.begin(); it != rankToRemoteSlices_.end();) {
            auto &vec = it->second;
            for (auto &slice : slices) {
                vec.erase(std::remove(vec.begin(), vec.end(), slice), vec.end());
            }
            it = vec.empty() ? rankToRemoteSlices_.erase(it) : std::next(it);
        }
    }
    for (auto &slice : slices) {
        // VA 记录同步注销：否则导入重试会被 shareName 短路复用已回滚的映射
        HybmVaManager::GetInstance().RemoveOneVaInfo(slice->gva_, slice->vAddress_, 0);
    }
}

bool AsymmetricMemSegment::ResolveRemoteGva(uint64_t gva, uint64_t &lva)
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto &it : remoteSlices_) {
        const auto &slice = it.second.slice;
        if (slice == nullptr || slice->vAddress_ == 0 || slice->size_ == 0) {
            continue; // 未本地映射（device_rdma 等路径）或空 slice：无 lva 可给
        }
        if (gva >= slice->gva_ && gva < slice->gva_ + slice->size_) {
            lva = slice->vAddress_ + (gva - slice->gva_);
            return true;
        }
    }
    return false;
}

Result AsymmetricMemSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[],
                                    uint32_t overrideRank) noexcept
{
    if (allExInfo.empty()) {
        return BM_OK;
    }

    Result ret = BM_ERROR;
    uint32_t index = 0U;
    std::vector<MemSlicePtr> importedSlices; // 本次成功导入的远端 slice，失败时用于回滚 IPC 映射
    for (auto &info : allExInfo) {
        MemSlicePtr rms;
        auto magic = *reinterpret_cast<const uint64_t *>(info.data());
        if (magic == ENTITY_EXPORT_INFO_MAGIC) {
            ret = ImportDeviceInfo(info, overrideRank);
        } else if (magic == USER_MEM_SLICE_EXPORT_INFO_MAGIC) {
            ret = ImportSliceInfo(info, rms, overrideRank);
        } else {
            BM_LOG_ERROR("invalid import magic : " << magic);
            ret = BM_INVALID_PARAM;
        }
        if (rms != nullptr) {
            importedSlices.emplace_back(rms);
        }
        if (addresses == nullptr) {
            if (ret != BM_OK) {
                break;
            }
            // kv trans addresses is null need continue
            continue;
        }
        if (ret != BM_OK) {
            // rollback：仅针对真实打开过的 IPC 映射；addresses[] 中是 gva/nullptr，不能传给 RtIpcCloseMemory
            RollbackImportedSlices(importedSlices);
            break;
        }

        void *address = nullptr;
        if (rms != nullptr) {
            address = (void *)(ptrdiff_t)(rms->gva_);
        }
        addresses[index++] = address;
    }

    return ret;
}

Result AsymmetricMemSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rankId : ranks) {
        importedDeviceInfo_.erase(rankId);
        RemoveSliceInfo(rankId);
    }
    uniqueLock.unlock();
    return BM_OK;
}

void AsymmetricMemSegment::RemoveSliceInfo(const uint32_t rankId) noexcept
{
    // Clear Imported SliceInfo
    auto it = rankToRemoteSlices_.find(rankId);
    if (it == rankToRemoteSlices_.end()) {
        return;
    }
    auto &remoteSliceVec = it->second;
    for (auto &remoteSlice : remoteSliceVec) {
        // 同 gva 的 imported 记录可能多实体共存（多 store 组窗口号撞车），必须携带自己的 dva 精确注销，
        // 避免按行内容级联误减共存方的 DVA 行
        HybmVaManager::GetInstance().RemoveOneVaInfo(remoteSlice->gva_, remoteSlice->vAddress_, 0);
        auto rIt = remoteSlices_.find(remoteSlice->index_);
        if (rIt == remoteSlices_.end()) {
            continue;
        }
        if (options_.shared) {
            // 按登记类型解除映射（IPC close / VMM unmap+release）；未实际打开的条目 vAddress_=0 为 no-op
            UserMemUnImportName(remoteSlice->index_);
        }
        BM_LOG_INFO("RemoveSliceInfo, rankId=" << rankId << ", remoteSlice->index_=" << remoteSlice->index_
                                               << ", slice name " << UserMemNamePreview(rIt->second.name));
        importedSliceInfo_.erase(rIt->second.name);
        remoteSlices_.erase(remoteSlice->index_);
    }
    rankToRemoteSlices_.erase(rankId);
}

Result AsymmetricMemSegment::Mmap() noexcept
{
    BM_LOG_ERROR("AsymmetricMemSegment NOT SUPPORT Mmap");
    return BM_NOT_SUPPORTED;
}

MemSlicePtr AsymmetricMemSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    MemSlicePtr target;
    auto index = MemSlice::GetIndexFrom(slice);
    auto pos = registerSlices_.find(index);
    if (pos != registerSlices_.end()) {
        target = pos->second.slice;
    } else if ((pos = remoteSlices_.find(index)) != remoteSlices_.end()) {
        target = pos->second.slice;
    } else {
        if (quiet) {
            BM_LOG_DEBUG("cannot get slice: " << slice);
        } else {
            BM_LOG_ERROR("cannot get slice: " << slice);
        }
        return nullptr;
    }

    if (!target->ValidateId(slice)) {
        return nullptr;
    }

    return target;
}

Result AsymmetricMemSegment::Unmap() noexcept
{
    BM_LOG_INFO("AsymmetricMemSegment NOT SUPPORT Unmap");
    return BM_NOT_SUPPORTED;
}

Result AsymmetricMemSegment::ImportDeviceInfo(const std::string &info, uint32_t overrideRank) noexcept
{
    HbmExportDeviceInfo deviceInfo;
    LiteralExInfoTranslater<HbmExportDeviceInfo> translator;
    auto ret = translator.Deserialize(info, deviceInfo);
    if (ret != 0) {
        BM_LOG_ERROR("deserialize device info failed: " << ret);
        return ret;
    }

    if (deviceInfo.devicePhyId >= MAX_DEVICE_COUNT) {
        BM_LOG_ERROR("Invalid deviceInfo device id: " << deviceInfo.devicePhyId
                                                      << ", max: " << static_cast<int>(MAX_DEVICE_COUNT));
        return BM_ERROR;
    }

    // RtEnableP2P 仅本机设备间语义：同机对端才使能本地 P2P；跨机对端走下方 superPodPid 白名单通路
    if (CanLocalHostReaches(deviceInfo.superPodId, deviceInfo.serverId, deviceInfo.devicePhyId)) {
        ret = EnsurePeerAccess(deviceInfo.devicePhyId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("ensure peer access failed, rankId=" << options_.rankId << " remoteRank=" << deviceInfo.rankId
                                                              << " peerDev=" << deviceInfo.devicePhyId
                                                              << " ret=" << ret);
            return ret;
        }
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    if (options_.shared) {
        for (auto &it : registerSlices_) {
            if (it.second.name.empty()) {
                continue; // DRAM/非 shared 注册内存无共享名；VMM 已设 NO_WLIST 免白名单，helper 内 no-op
            }
            ret = UserMemSetSuperPodPid(it.second.name.c_str(), deviceInfo.sdid, deviceInfo.pid);
            if (ret != BM_OK) {
                // 根因日志已在 UserMemSetSuperPodPid 内记录
                return ret;
            }
        }
    }

    // trans 场景 rank 覆盖：用导入方本地分配的 rank 替换导出端 rank，隔离各 uniqueId 连接的设备信息
    const uint32_t rankId = (overrideRank != HYBM_IMPORT_RANK_INVALID) ? overrideRank : deviceInfo.rankId;
    importedDeviceInfo_.emplace(rankId, deviceInfo);
    uniqueLock.unlock();
    return BM_OK;
}

Result AsymmetricMemSegment::ImportSliceInfo(const std::string &info, MemSlicePtr &remoteSlice,
                                             uint32_t overrideRank) noexcept
{
    UserSliceExportInfo sliceInfo;
    LiteralExInfoTranslater<UserSliceExportInfo> translator;
    auto ret = translator.Deserialize(info, sliceInfo);
    if (ret != 0) {
        BM_LOG_ERROR("deserialize user slice info failed: " << ret);
        return ret;
    }

    if (sliceInfo.devicePhyId >= MAX_DEVICE_COUNT) {
        BM_LOG_ERROR("Invalid sliceInfo device id: " << sliceInfo.devicePhyId
                                                     << ", max: " << static_cast<int>(MAX_DEVICE_COUNT));
        return BM_ERROR;
    }

    if (overrideRank != HYBM_IMPORT_RANK_INVALID) {
        // trans 场景 rank 覆盖：导出端 gva 布局为 gvaOffset = origin + rankId * maxSize + delta，
        // 将窗口从导出端 rank 平移到本地覆盖 rank 窗口，避免多对端 slice 落入同一窗口与本地内存冲突
        const uint64_t exporterWindow = static_cast<uint64_t>(sliceInfo.rankId) * options_.maxSize;
        if (sliceInfo.gvaOffset < exporterWindow) {
            BM_LOG_ERROR("invalid gvaOffset=" << sliceInfo.gvaOffset << " rankId=" << sliceInfo.rankId
                                              << " maxSize=" << options_.maxSize);
            return BM_INVALID_PARAM;
        }
        sliceInfo.gvaOffset += static_cast<uint64_t>(overrideRank) * options_.maxSize - exporterWindow;
        sliceInfo.rankId = overrideRank;
    }

    // 介质判型：segmentType 由导出端按 slice 实际内存类型写入
    if (sliceInfo.segmentType == SEGMENT_TYPE_USER_DRAM) {
        return ImportDramSlice(sliceInfo, remoteSlice);
    }
    return ImportHbmSlice(sliceInfo, remoteSlice);
}

// 导出名是否携带有效的共享句柄。仅 shared/VMM 会写入非全 0 的共享名；
// 非 shared（HBM 未走 VMM/IPC 共享名）name 恒为全 0（见 RegisterMemory 内的 RegisterSlice name 构造）。
static bool HasShareName(const char *name, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (name[i] != '\0') {
            return true;
        }
    }
    return false;
}

MemSlicePtr AsymmetricMemSegment::FindReusableImport(uint64_t expectGva) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto &it : remoteSlices_) {
        const auto &slice = it.second.slice;
        if (slice != nullptr && slice->gva_ == expectGva) {
            return slice;
        }
    }
    BM_LOG_INFO("no reusable import for gva, fallback to full import: rankId=" << options_.rankId
                                                                               << " expectGva=" << VaToStr(expectGva));
    return nullptr;
}

Result AsymmetricMemSegment::ImportHbmSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept
{
    Result ret = BM_OK;
    void *address = nullptr;
    // name 定长构造：VMM share_handle 为二进制，隐式 strlen 会在首个 \0 截断导致 find/erase 失配
    const std::string shareName(sliceInfo.name, USER_HBM_NAME_MAX_LEN);
    // 幂等短路仅对 shared/VMM 生效。非 shared（name 全 0）没有
    // 可区分不同对端的字段（rankId 只在单 store rank 空间内 injective、多 D 场景各 D 常同为 0，
    // 描述符在多 server 同规格部署下也完全相同），据此去重会让不同对端互相命中并塌缩到同一 GVA，
    // 因此非 shared 一律不做幂等短路，逐 slice 独立导入并各自登记 GVA。
    const uint64_t expectGva = sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_);
    auto reused = HasShareName(sliceInfo.name, USER_HBM_NAME_MAX_LEN) ? FindReusableImport(expectGva) : nullptr;
    if (reused != nullptr) {
        // 幂等短路：重复导入复用既有 slice 与映射，避免重复 ReserveLva/map 与 va_manager 重复计数
        remoteSlice = reused;
        BM_LOG_INFO("import hbm slice replay reuse: gva=" << VaToStr(remoteSlice->gva_)
                                                          << " size=" << VaToStr(remoteSlice->size_));
        return BM_OK;
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    if (options_.shared && CanSdmaReaches(sliceInfo.superPodId, sliceInfo.serverId, sliceInfo.devicePhyId)) {
        // RtEnableP2P/RtGetP2PStatus 均为本机设备间语义，仅同机对端执行；同超节点跨机由
        // RtIpc(share name + superPodPid 白名单) 打通，跳过本地 P2P 使能与查询
        if (CanLocalHostReaches(sliceInfo.superPodId, sliceInfo.serverId, sliceInfo.devicePhyId) &&
            devicePhyId_ != static_cast<int>(sliceInfo.devicePhyId)) {
            ret = EnsurePeerAccess(sliceInfo.devicePhyId);
            if (ret != BM_OK) {
                BM_LOG_ERROR("ensure peer access failed, rankId=" << options_.rankId
                                                                  << " remoteRank=" << sliceInfo.rankId << " peerDev="
                                                                  << sliceInfo.devicePhyId << " ret=" << ret);
                return ret;
            }

            // 检查 P2P 双向就绪：本端已 EnsurePeerAccess(EnableRemotePeerAccess)，但对端可能还没使能完。
            // 不阻塞等待：未就绪返回专用码 BM_P2P_NOT_READY，由上层状态机重试；该码在上层只保留本处 WARN，
            // 不再逐层打 ERROR。查询接口本身失败（含符号缺失 BM_UNDER_API_UNLOAD）不做拦截，交实际导入决定。
            uint32_t p2pStatus = 0;
            auto p2pRet = DlAclApi::RtGetP2PStatus(static_cast<uint32_t>(deviceId_),
                                                   static_cast<uint32_t>(sliceInfo.devicePhyId), p2pStatus);
            if (p2pRet == BM_OK && p2pStatus != 1) {
                BM_LOG_WARN("trans P2P not ready, rankId=" << options_.rankId << " remoteRank=" << sliceInfo.rankId
                                                           << " peerDev=" << sliceInfo.devicePhyId << " ret=" << p2pRet
                                                           << " status=" << p2pStatus << ", will retry later");
                return BM_P2P_NOT_READY;
            }
        }

        // IPC 名打开映射；VMM share_handle 导入 + 预留 LVA + 映射（根因日志均在 helper 内记录）
        ret = UserMemImportAndMapName(&address, sliceInfo);
        if (ret != BM_OK) {
            return ret;
        }
    } else if (options_.dataOpType &
               (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) {
        address = nullptr;
    }

    remoteSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM,
                                             sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_),
                                             reinterpret_cast<uint64_t>(address), sliceInfo.size);
    rankToRemoteSlices_[sliceInfo.rankId].push_back(remoteSlice);
    remoteSlices_.emplace(remoteSlice->index_, RegisterSlice{remoteSlice, shareName});
    importedSliceInfo_.emplace(shareName, sliceInfo);
    uniqueLock.unlock();
    ret = HybmVaManager::GetInstance().AddVaInfo(
        {remoteSlice->gva_, remoteSlice->vAddress_, 0, remoteSlice->size_, HYBM_MEM_TYPE_DEVICE}, false,
        AddressOwnership::Imported);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);
    return BM_OK;
}

Result AsymmetricMemSegment::ImportDramSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept
{
    Result ret = BM_OK;
    // 远端 host 内存本地不可映射，不做 RtEnableP2P/RtIpcOpenMemory，数据面走 transport MR
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    remoteSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM,
                                             sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_), 0,
                                             sliceInfo.size);
    rankToRemoteSlices_[sliceInfo.rankId].push_back(remoteSlice);
    remoteSlices_.emplace(remoteSlice->index_, RegisterSlice{remoteSlice, "", false});
    uniqueLock.unlock();
    ret = HybmVaManager::GetInstance().AddVaInfo({remoteSlice->gva_, 0, 0, remoteSlice->size_, HYBM_MEM_TYPE_HOST},
                                                 false, AddressOwnership::Imported);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK,
                             "AddVaInfo failed: rankId=" << options_.rankId << " remoteRankId=" << sliceInfo.rankId
                                                         << " gva=" << VaToStr(remoteSlice->gva_) << " ret=" << ret,
                             ret);
    BM_LOG_INFO("import dram slice success: rankId=" << sliceInfo.rankId << " gva=" << VaToStr(remoteSlice->gva_)
                                                     << " size=" << VaToStr(sliceInfo.size));
    return BM_OK;
}

void AsymmetricMemSegment::CloseMemory() noexcept
{
    if (options_.shared) {
        for (auto &it : remoteSlices_) {
            // 按登记类型解除映射（IPC close / VMM unmap+release）
            UserMemUnImportName(it.first);
        }
    }
    // 导入记录的 VA 行同步注销（多实体同址共存时按各自 dva 精确回收，防止僵尸记录驻留进程级台账）
    for (auto &it : remoteSlices_) {
        HybmVaManager::GetInstance().RemoveOneVaInfo(it.second.slice->gva_, it.second.slice->vAddress_, 0);
    }
    remoteSlices_.clear();
    for (auto &it : registerSlices_) {
        // 段级销毁兜底：hostMapped DRAM slice 对称解除设备映射；与 ReleaseSliceMemory 相同的
        // 引用计数门控——仅当本次注销使记录涉及的所有 VA 行归零回收（无别名存活）时才释放映射
        bool freed = HybmVaManager::GetInstance().RemoveOneVaInfo(it.second.slice->gva_);
        if (it.second.hostMapped && freed) {
            ReleaseHostMapping(it.second.slice->vAddress_);
        }
    }
    if (globalVirtualAddress_ != nullptr) {
        HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uint64_t>(globalVirtualAddress_));
    }
    globalVirtualAddress_ = lvaBase_ = nullptr;
    totalVirtualSize_ = 0;
    BM_LOG_INFO("close memory finish.");
}

bool AsymmetricMemSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < lvaBase_) {
        return false;
    }

    if (reinterpret_cast<const uint8_t *>(begin) + size > lvaBase_ + totalVirtualSize_) {
        return false;
    }

    return true;
}

bool AsymmetricMemSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    auto pos = importedDeviceInfo_.find(rankId);
    if (pos == importedDeviceInfo_.end()) {
        return false;
    }

    uint32_t sdId;
    uint32_t serverId;
    uint32_t superPodId;
    HybmDevLegacySegment::GetDeviceInfo(sdId, serverId, superPodId);

    if (pos->second.serverId == serverId) {
        return true;
    }

    if (pos->second.superPodId == invalidSuperPodId || superPodId == invalidSuperPodId) {
        return false;
    }

    return pos->second.superPodId == superPodId;
}

// ====================== IPC/VMM 双路径共享 helper 实现 ======================

Result AsymmetricMemSegment::UserMemExportName(const void *ptr, uint64_t len, char *name) noexcept
{
    //   A3：HBM 地址窗口内的用户态设备内存（torch/acl 直接分配，LOCK_DEV 型，
    //   驱动不支持 HalMemRetainAllocationHandle）走 IPC 名路径；窗口外的 host 内存
    //   （offload 池等，GVM 窗口 0x28 开头，可 retain+export）走 VMM share_handle 路径。
    //   注意 A3 驱动对 host 用户态内存的 DrvMemGetAttribute 也返回 LOCK_DEV(0x10)，
    //   属性查询无法区分介质，只能按地址窗口判型。
    const bool useVmm = !IsHbmAddr(reinterpret_cast<uint64_t>(ptr));
    name[0] = static_cast<char>(useVmm ? USER_HBM_NAME_TYPE_VMM : USER_HBM_NAME_TYPE_IPC);
    if (!useVmm) {
        auto ret = DlAclApi::RtIpcSetMemoryName(ptr, len, name + 1, DEVICE_SHM_NAME_SIZE + 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("set memory name failed: rankId=" << options_.rankId
                                                           << " addr=" << VaToStr(reinterpret_cast<uint64_t>(ptr))
                                                           << " size=" << len << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        return BM_OK;
    }
    return ExportVmmShareName(ptr, name);
}

Result AsymmetricMemSegment::ExportVmmShareName(const void *ptr, char *name) noexcept
{
    const auto addrStr = VaToStr(reinterpret_cast<uint64_t>(ptr));
    ShareHandleStr shareInfo;
    // 本地已登记该地址的 share handle 时直接复用并跳过 retain/export：SHARED 池仅分配 rank
    // 能 export 成功，其他 rank 的同 VA 是导入映射（句柄在导入时登记），对其 export 会报错
    if (HybmVaManager::GetInstance().GetHandle(ptr, shareInfo) == 0) {
        BM_LOG_INFO("reuse registered share handle, skip export: rankId=" << options_.rankId << " addr=" << addrStr);
        std::memcpy(name + 1, shareInfo.data(), shareInfo.size());
        return BM_OK;
    }
    drv_mem_handle_t *handle = nullptr;
    auto ret = DlHalApi::HalMemRetainAllocationHandle(&handle, const_cast<void *>(ptr));
    if (ret != BM_OK || handle == nullptr) {
        BM_LOG_ERROR("HalMemRetainAllocationHandle failed: rankId=" << options_.rankId << " addr=" << addrStr
                                                                    << " ret=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    // halMemExport 的 flags 实测必须传 0：传 ACL 的 DISABLE_PID_VALIDATION(0x1) 返回 65534 被拒，
    // 该 flag 语义仅在 ACL 层 aclrtMemExportToShareableHandleV2 支持；免白名单当前以
    // SetAttribute 降级（WARN）换取链路验证，白名单拦截时再切 ACL 层 export 链路
    MemShareHandle shareHandle{}; // 驱动出参，HalMemExport 成功后填充
    ret = DlHalApi::HalMemExport(handle, MEM_HANDLE_TYPE_FABRIC, 0, &shareHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemExport failed: rankId=" << options_.rankId << " addr=" << addrStr << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        return BM_DL_FUNCTION_FAILED;
    }
    // 实测：export 后立即 release 会导致同 allocation 的后续 slice retain/export 全部 PARA_ERROR，
    // share_handle 疑似依赖 retain 引用存活，故 retain 引用保持不释放（销毁对称待段级统一处理）；
    // trans/setAttr 失败路径仍对称释放
    uint64_t shareable = 0U;
    uint32_t sId = 0;
    ret = DlHalApi::HalMemTransShareableHandle(MEM_HANDLE_TYPE_FABRIC, &shareHandle, &sId, &shareable);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemTransShareableHandle failed: rankId=" << options_.rankId << " addr=" << addrStr
                                                                  << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        return BM_ERROR;
    }
    BM_LOG_INFO("trans shareable ok: rankId=" << options_.rankId << " shareable=0x" << std::hex << shareable
                                              << " serverId=" << std::dec << sId);
    struct ShareHandleAttr attr = {.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE, .rsv = {0}};
    ret = DlHalApi::HalMemShareHandleSetAttribute(shareable, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
    if (ret != BM_OK) {
        // A5 上 ACL 物理分配来源的 shareable 可能不被 HAL 属性接口接受（实测 PARA_ERROR）；
        // 免白名单已由 export flag 承担，此处降级为告警不阻断；若对端 import 被白名单拒绝再回退本步
        BM_LOG_WARN("HalMemShareHandleSetAttribute degraded: rankId=" << options_.rankId << " shareable=0x" << std::hex
                                                                      << shareable << " ret=" << std::dec << ret);
    }
    std::memcpy(name + 1, shareHandle.share_info, MEM_SHARE_HANDLE_LEN);
    return BM_OK;
}

Result AsymmetricMemSegment::UserMemSetSuperPodPid(const char *name, uint32_t sdid, uint32_t pid) noexcept
{
    // 仅 IPC 名需要 superPod/pid 白名单；VMM 共享已设 NO_WLIST 免白名单，空名（DRAM/非 shared）无需处理
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_IPC) {
        return BM_OK;
    }
    int32_t pidArg = static_cast<int32_t>(pid);
    auto ret = DlAclApi::RtSetIpcMemorySuperPodPid(name + 1, sdid, &pidArg, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set shm(" << name + 1 << ") for sdid=" << sdid << " pid=" << pid << " failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("set shm(" << name + 1 << ") for sdid=" << sdid << " pid=" << pid << " success.");
    return BM_OK;
}

namespace {
// reserve flags 候选（按优先级）：该驱动（25.7.0/ascendhal 7.35.23）实测 reserve 的 flags 为页型
// 标志（1=HUGE）而非旧驱动的 MEM_RSV_TYPE_REMOTE_MAP 语义，且 expectPtr 可能为空才被接受；
// 逐候选探测，失败降级。useExpect=false 时采用驱动自选地址。
struct ReserveFlagCandidate {
    uint64_t flags;
    bool useExpect;
};
const ReserveFlagCandidate kReserveCandidates[] = {
    {MEM_RSV_TYPE_REMOTE_MAP, true}, // 旧语义：REMOTE_MAP + 指定 VaManager 预留地址
    {1, true},                       // 页型(HUGE) + 指定地址
    {1, false},                      // 页型(HUGE) + 驱动自选地址（放弃 VaManager 预留）
    {0, false},
};

// 单次 reserve 尝试：成功且地址符合期望返回 lva；地址不符时释放驱动侧预留返回 0 表示换候选
uint64_t TryHalReserve(uint64_t size, uint64_t expectLva, uint64_t flags, bool useExpect)
{
    void *lva = nullptr;
    void *expect = useExpect ? reinterpret_cast<void *>(expectLva) : nullptr;
    auto ret = DlHalApi::HalMemAddressReserve(&lva, size, 0, expect, flags);
    if (ret != 0) {
        return 0;
    }
    const auto got = reinterpret_cast<uint64_t>(lva);
    if (useExpect && got != expectLva) {
        (void)DlHalApi::HalMemAddressFree(lva); // 驱动返回了非请求地址：释放该预留，尝试下一候选
        return 0;
    }
    return got;
}
} // namespace

uint64_t AsymmetricMemSegment::ReserveLva(const UserSliceExportInfo &info) noexcept
{
    auto reserved = HybmVaManager::GetInstance().AllocReserveLva(info.size, HVM_DVA, HYBM_MEM_TYPE_DEVICE);
    const auto reservedLva = reserved.va[HVM_DVA];
    if (reservedLva == 0) {
        BM_LOG_ERROR("AllocReserveLva failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                       << " size=" << info.size);
        return 0;
    }
    for (const auto &candidate : kReserveCandidates) {
        auto lva = TryHalReserve(info.size, reservedLva, candidate.flags, candidate.useExpect);
        if (lva == 0) {
            continue;
        }
        if (!candidate.useExpect) {
            // 驱动自选地址：归还 VaManager 的预留，后续映射/登记均采用驱动地址
            HybmVaManager::GetInstance().FreeReserveLva(reservedLva, HVM_DVA);
        }
        BM_LOG_INFO("reserve lva ok: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                              << " lva=" << VaToStr(lva) << " flags=0x" << std::hex << candidate.flags
                                              << " useExpect=" << std::dec << candidate.useExpect);
        return lva;
    }
    BM_LOG_ERROR("HalMemAddressReserve failed for all candidates: rankId=" << options_.rankId << " remoteRank="
                                                                           << info.rankId << " size=" << info.size
                                                                           << " reservedVa=" << VaToStr(reservedLva));
    HybmVaManager::GetInstance().FreeReserveLva(reservedLva, HVM_DVA);
    return 0;
}

// 507899（ACL_RT_INTERNAL_ERROR，定义见 dl_acl_api.h）在 IPC open 上覆盖两类对端未就绪的
// 瞬时拒绝：白名单未设置与节点不可见，初始化竞争期常见，对端白名单落地后重试即成功。此处
// 按可重试码上抛，复用上层既有的 WARN + 服务端重试管线；其余失败码仍按硬错误处理

Result AsymmetricMemSegment::UserMemImportAndMapName(void **ptr, const UserSliceExportInfo &info) noexcept
{
    const uint16_t type = UserMemNameType(info.name);
    if (type == USER_HBM_NAME_TYPE_IPC) {
        auto ret = DlAclApi::RtIpcOpenMemory(ptr, info.name + 1);
        if (ret != BM_OK) {
            // 对端 IPC 白名单/节点尚未就绪属瞬时竞争，按可重试码上抛，交上层状态机重试
            if (ret == ACL_RT_INTERNAL_ERROR) {
                BM_LOG_WARN("IpcOpenMemory(" << info.name + 1 << ") not ready, will retry: rankId=" << options_.rankId
                                             << " sdid=" << sdid_ << " pid=" << pid_ << " deviceId=" << devicePhyId_
                                             << " remoteDeviceId=" << info.devicePhyId << " ret=" << ret);
                return BM_P2P_NOT_READY;
            }
            BM_LOG_ERROR("IpcOpenMemory(" << info.name + 1 << ") failed: rankId=" << options_.rankId
                                          << " sdid=" << sdid_ << " pid=" << pid_ << " deviceId=" << devicePhyId_
                                          << " remoteDeviceId=" << info.devicePhyId << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_INFO("IpcOpenMemory(" << info.name + 1 << ") success: rankId=" << options_.rankId
                                     << " deviceId=" << devicePhyId_);
        return BM_OK;
    }
    if (type != USER_HBM_NAME_TYPE_VMM) {
        // 未知类型（含 0）：exchange 数据损坏或对端版本不匹配，必须显式报错而非静默跳过
        BM_LOG_ERROR("unknown user mem name type: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                           << " type=" << type);
        return BM_INVALID_PARAM;
    }
    return ImportAndMapVmmName(ptr, info);
}

Result AsymmetricMemSegment::ImportAndMapVmmName(void **ptr, const UserSliceExportInfo &info) noexcept
{
    MemShareHandle shareHandle{};
    std::memcpy(shareHandle.share_info, info.name + 1, MEM_SHARE_HANDLE_LEN);
    drv_mem_handle_t *handle = nullptr;
    auto ret = DlHalApi::HalMemImport(MEM_HANDLE_TYPE_FABRIC, &shareHandle, logicDeviceId_, &handle);
    if (ret != BM_OK || handle == nullptr) {
        BM_LOG_ERROR("HalMemImport failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                    << " deviceId=" << logicDeviceId_ << " ret=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    const auto lva = ReserveLva(info);
    if (lva == 0) {
        // 预留失败的根因已在 ReserveLva 记录；imported handle 不能泄漏
        ReleaseVmmHandleQuietly(handle);
        return BM_ERROR;
    }
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(lva), info.size, 0, handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                 << " lva=" << VaToStr(lva) << " size=" << info.size << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
        return BM_DL_FUNCTION_FAILED;
    }
    // 授权本地设备对导入内存的读写权限：A5 上 import 映射默认无写权限，
    // SDMA 写会被硬件静默丢弃（实测 ret=0 但数据未落地），须显式 SetAccess
    struct drv_mem_access_desc accessDesc[1] = {};
    accessDesc[0].location.side = MEM_DEV_SIDE;
    accessDesc[0].location.id = logicDeviceId_;
    accessDesc[0].type = MEM_ACCESS_TYPE_READWRITE;
    ret = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(lva), info.size, accessDesc, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemSetAccess failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId << " lva="
                                                       << VaToStr(lva) << " size=" << info.size << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
        return BM_DL_FUNCTION_FAILED;
    }
    vmmLvaToHandle_[lva] = handle;
    *ptr = reinterpret_cast<void *>(lva);
    BM_LOG_INFO("import vmm slice success: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                    << " lva=" << VaToStr(lva) << " size=" << info.size);
    return BM_OK;
}

void AsymmetricMemSegment::CloseRemoteIpcMemory(const RegisterSlice &reg) noexcept
{
    // RtIpcOpenMemory 返回的地址高 16 位带标记位，close 时需掩码还原（历史行为，保持不变）
    void *address = reinterpret_cast<void *>(static_cast<ptrdiff_t>(reg.slice->vAddress_ << 16 >> 16));
    auto ret = DlAclApi::RtIpcCloseMemory(address);
    if (ret != 0) {
        BM_LOG_WARN("RtIpcCloseMemory failed: deviceId=" << devicePhyId_ << " addr=" << VaToStr(reg.slice->vAddress_)
                                                         << " ret=" << ret << ", This may affect future registration.");
    }
}

void AsymmetricMemSegment::UnmapRemoteVmmMemory(const RegisterSlice &reg) noexcept
{
    const auto lva = reg.slice->vAddress_;
    auto ret = DlHalApi::HalMemUnmap(reinterpret_cast<void *>(static_cast<ptrdiff_t>(lva)));
    if (ret != 0) {
        BM_LOG_WARN("HalMemUnmap failed: deviceId=" << devicePhyId_ << " lva=" << VaToStr(lva) << " ret=" << ret);
    }
    HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
    // 同 name 可在多个 rank 窗口各有一片：按 name 取会释放别人的 handle，自己的落到 "may leak"
    auto it = vmmLvaToHandle_.find(lva);
    if (it == vmmLvaToHandle_.end()) {
        BM_LOG_WARN("vmm handle not found on unimport, may leak: deviceId=" << devicePhyId_ << " lva=" << VaToStr(lva));
        return;
    }
    ReleaseVmmHandleQuietly(it->second);
    vmmLvaToHandle_.erase(it);
}

void AsymmetricMemSegment::UserMemUnImportName(uint32_t sliceId) noexcept
{
    auto rIt = remoteSlices_.find(sliceId);
    if (rIt == remoteSlices_.end()) {
        return; // 未登记条目（含尚未登记的回滚场景）为 no-op
    }
    const auto &reg = rIt->second;
    if (reg.slice == nullptr || reg.slice->vAddress_ == 0) {
        return; // 未实际打开映射（不可达/非 shared/DRAM），无资源可释放
    }
    const auto type = UserMemNameType(reg.name);
    if (type == USER_HBM_NAME_TYPE_IPC) {
        CloseRemoteIpcMemory(reg);
    } else if (type == USER_HBM_NAME_TYPE_VMM) {
        UnmapRemoteVmmMemory(reg);
    }
}

// 107002（ACL_RT_IPC_NAME_NOT_FOUND，定义见 dl_acl_api.h）：共享名不存在（对端先行退出/
// 名字已销毁）时 teardown 期属预期场景，降级 WARN；其余失败仍按 ERROR 记录（可能泄漏，
// 需人工跟进）

Result AsymmetricMemSegment::UserMemDestroyName(const char *name) noexcept
{
    // 仅 IPC 名有可销毁的共享名；VMM 导出侧 retain 引用已在导出时释放，无资源可清理
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_IPC) {
        return BM_OK;
    }
    auto ret = DlAclApi::RtIpcDestroyMemoryName(name + 1);
    if (ret != BM_OK) {
        if (ret == ACL_RT_IPC_NAME_NOT_FOUND) {
            BM_LOG_WARN("destroy memory name skipped (already gone): rankId=" << options_.rankId << " name=" << name + 1
                                                                              << " ret=" << ret);
        } else {
            BM_LOG_ERROR("destroy memory name failed: rankId=" << options_.rankId << " name=" << name + 1
                                                               << " ret=" << ret);
        }
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

} // namespace mf
} // namespace ock
