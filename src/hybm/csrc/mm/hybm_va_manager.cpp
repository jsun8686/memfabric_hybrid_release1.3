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

#include <sstream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <set>

#include "dl_hal_api.h"
#include "hybm_def.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {

uint8_t HybmVaManager::directionLut[BIT_LUT_SIZE];

void HybmVaManager::InitDirectionLut()
{
    for (int except = 0; except < BIT_LUT_SIZE; except++) {
        uint8_t d = HYBM_DATA_COPY_DIRECTION_BUTT;
        for (int i = 0; i < HYBM_DATA_COPY_DIRECTION_BUTT - 1; i++) {
            if ((dirMask[i] & except) == dirMask[i]) {
                d = static_cast<uint8_t>(i);
                break;
            }
        }
        directionLut[except] = d;
    }
}

static constexpr uint64_t MB_OFFSET = 20UL;
Result HybmVaManager::Initialize(AscendSocType socType) noexcept
{
#if defined(ASCEND_NPU)
    if (socType == AscendSocType::ASCEND_UNKNOWN) {
        BM_LOG_ERROR("soc type is unknown.");
        return BM_INVALID_PARAM;
    }
#endif
    soc_ = socType;
    return BM_OK;
}

Result HybmVaManager::AddVaInfo(const AllocatedGvaInfo &info, bool onlyGva)
{
    if (info.base.size == 0) {
        BM_LOG_INFO("gva:" << info.base.va[HVM_GVA] << " size:0, skip add va info");
        return BM_OK;
    }
    if (info.base.memType >= HYBM_MEM_TYPE_BUTT) {
        BM_LOG_ERROR("AddVaInfo failed: invalid memType=" << info.base.memType);
        return BM_ERROR;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // onlyGva=true (sdma/device_rdma/vmm, etc.) records the GVA identity only and does not enter the DVA/HVA
    // translation tables, so "several entities sharing the same dva/hva pair" cannot happen and the check
    // reduces to a plain GVA overlap test.
    const auto ret = onlyGva ? ValidateOnlyGvaAdd(info) : ValidateAddVaInfo(info);
    if (ret != BM_OK) {
        return ret;
    }

    const uint64_t gva = info.base.va[HVM_GVA];
    if (onlyGva) {
        AddRowRef(HVM_GVA, gva);
        allocatedMap_[HVM_GVA][gva] = info;
        return BM_OK;
    }

    // A GVA row corresponds one-to-one with a registration record and is always written; the DVA/HVA tables are
    // first-wins, so when an earlier registration (the owner) already holds that key it is not overwritten and
    // this registration only records its new gva identity. Every touched row gets a per-table lifetime reference.
    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        const uint64_t key = info.base.va[i];
        if (key == 0) {
            continue;
        }
        auto rowIt = allocatedMap_[i].find(key);
        if (rowIt != allocatedMap_[i].end() && rowIt->second == info) {
            // 完全相同的记录再次登记：幂等重放或不同物理内存的等描述符共存（多 store 组）。
            // 引用计数照加（每次登记都有对应的注销），仅告警提示存在重放路径
            BM_LOG_WARN("identical record re-added, key=" << VaToStr(key) << " type=" << i
                                                          << " - replay path or identical coexistence");
        }
        AddRowRef(i, key);
        if (i == HVM_GVA || rowIt == allocatedMap_[i].end()) {
            allocatedMap_[i][key] = info;
        }
    }
    return BM_OK;
}

Result HybmVaManager::ValidateOnlyGvaAdd(const AllocatedGvaInfo &info)
{
    if (info.base.va[HVM_GVA] == 0) {
        return BM_OK;
    }
    auto old = CheckOverlap(info.base.va[HVM_GVA], info.base.size, HVM_GVA);
    if (!old.first || old.second == info) {
        return BM_OK;
    }
    BM_LOG_ERROR("AddVaInfo failed: address overlap. gva=" << VaToStr(info.base.va[0])
                                                           << ", size=" << VaToStr(info.base.size) << ", memType="
                                                           << info.base.memType << ", old: " << old.second);
    return BM_ERROR;
}

Result HybmVaManager::ValidateAddVaInfo(const AllocatedGvaInfo &info)
{
    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        if (info.base.va[i] == 0) {
            continue;
        }
        auto old = CheckOverlap(info.base.va[i], info.base.size, i);
        if (!old.first || old.second == info) {
            continue;
        }
        // Replay registration of the same local physical memory (identical dva/hva/size/memType, only gva differs):
        // allowed, and the caller records the new gva identity first-wins without overwriting the dva/hva owner row.
        // Imported records never take part in this judgement.
        const AllocatedGvaInfo &prev = old.second;
        if (prev.base.va[HVM_DVA] == info.base.va[HVM_DVA] && prev.base.va[HVM_HVA] == info.base.va[HVM_HVA] &&
            prev.base.size == info.base.size && prev.base.memType == info.base.memType &&
            prev.ownership == AddressOwnership::Local && info.ownership == AddressOwnership::Local) {
            continue;
        }
        // Imported rows are classification-only entries (the data plane resolves remote addresses per connection
        // via remoteSlices_/TransformAddr) and never occupy local address space. Under multi-store numbering
        // (PD multi-P) a peer window number can legitimately coincide with a local window, so cross-ownership
        // overlap is allowed instead of failing the registration; same-ownership overlap keeps the rules above.
        if (info.IsImported() != prev.IsImported()) {
            BM_LOG_INFO("AddVaInfo cross-ownership overlap allowed: gva="
                        << VaToStr(info.base.va[0]) << " va:" << VaToStr(info.base.va[i]) << ", size="
                        << VaToStr(info.base.size) << ", ownership=" << (info.IsImported() ? "imported" : "local"));
            continue;
        }
        // 同 gva 的 imported↔imported 共存：多 store 组下不同对端（如各自 store 的 0 号 rank）窗口号相同，
        // 属合法拓扑。imported 行 classification-only，共存记录的 ownership/memType 一致，分类结果不受影响；
        // 生命周期由 rowRefCnt_ 保证（最后一条注销才回收行），数据面按连接经实体域解析、不经此行寻址
        if (info.IsImported() && prev.IsImported()) {
            BM_LOG_INFO("AddVaInfo imported-over-imported overlap allowed: gva=" << VaToStr(info.base.va[0])
                                                                                 << " size=" << VaToStr(info.base.size)
                                                                                 << " memType=" << info.base.memType);
            continue;
        }
        BM_LOG_ERROR("AddVaInfo failed: address overlap. gva="
                     << VaToStr(info.base.va[0]) << " va:" << VaToStr(info.base.va[i])
                     << ", size=" << VaToStr(info.base.size) << ", vaType=" << i << ", memType=" << info.base.memType
                     << ", old: " << old.second);
        return BM_ERROR;
    }
    return BM_OK;
}

Result HybmVaManager::AddVaInfo(const BaseAllocatedGvaInfo &baseInfo, bool onlyGva, AddressOwnership ownership)
{
    AllocatedGvaInfo info(baseInfo, ownership);
    const auto ret = AddVaInfo(info, onlyGva);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);
    BM_LOG_INFO("AddVaInfo success: " << info);
    return BM_OK;
}

bool HybmVaManager::RemoveOneVaInfo(uint64_t va, uint32_t type)
{
    if (va == 0 || type >= HVM_BUTT) {
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = allocatedMap_[type].find(va);
    if (it == allocatedMap_[type].end()) {
        BM_LOG_WARN("Unable to RemoveOneVaInfo: address not found. va=" << VaToStr(va) << " type=" << type);
        return false;
    }
    const AllocatedGvaInfo record = it->second;
    BM_LOG_INFO("Removing VaInfo: " << record);
    // 共享 GVA 行（多实体 imported 同址共存）时行 content 是最后写者，按内容级联会误减共存方的 DVA 行；
    // 此类调用方必须改用 (gva, dva, hva) 精确重载并携带自己的 dva
    auto cntIt = rowRefCnt_[type].find(va);
    if (type == HVM_GVA && cntIt != rowRefCnt_[type].end() && cntIt->second > 1U && record.base.va[HVM_DVA] != 0) {
        BM_LOG_WARN("RemoveOneVaInfo cascades on a shared GVA row, dva=" << VaToStr(record.base.va[HVM_DVA])
                                                                         << " may belong to a coexisting record");
    }
    bool allFreed = true;
    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        if (record.base.va[i] == 0) {
            continue;
        }
        if (!RemoveRowRef(i, record.base.va[i])) {
            allFreed = false;
        }
    }
    BM_LOG_DEBUG("RemoveOneGvaInfo success: gva=" << VaToStr(va) << " type=" << type << " allFreed=" << allFreed);
    return allFreed;
}

bool HybmVaManager::RemoveOneVaInfo(uint64_t gva, uint64_t dva, uint64_t hva)
{
    if (gva == 0 && dva == 0 && hva == 0) {
        BM_LOG_WARN("RemoveOneVaInfo: all keys are zero, nothing to remove");
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    bool allFreed = true;
    bool removedAny = false;
    uint64_t keys[HVM_BUTT] = {};
    keys[HVM_GVA] = gva;
    keys[HVM_DVA] = dva;
    keys[HVM_HVA] = hva;
    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        if (keys[i] == 0 || rowRefCnt_[i].find(keys[i]) == rowRefCnt_[i].end()) {
            continue;
        }
        removedAny = true;
        if (!RemoveRowRef(i, keys[i])) {
            allFreed = false;
        }
    }
    if (!removedAny) {
        BM_LOG_WARN("RemoveOneVaInfo: none of the keys has a live row, gva=" << VaToStr(gva) << " dva=" << VaToStr(dva)
                                                                             << " hva=" << VaToStr(hva));
        return false;
    }
    return allFreed;
}

void HybmVaManager::AddRowRef(uint32_t type, uint64_t key)
{
    if (key == 0 || type >= HVM_BUTT) {
        return;
    }
    auto &cnt = rowRefCnt_[type][key];
    if (allocatedMap_[type].find(key) != allocatedMap_[type].end()) {
        ++cnt;
    } else {
        cnt = 1;
    }
}

bool HybmVaManager::RemoveRowRef(uint32_t type, uint64_t key)
{
    if (key == 0 || type >= HVM_BUTT) {
        return false;
    }
    auto cntIt = rowRefCnt_[type].find(key);
    if (cntIt == rowRefCnt_[type].end()) {
        return false;
    }
    if (--cntIt->second == 0) {
        rowRefCnt_[type].erase(cntIt);
        allocatedMap_[type].erase(key);
        return true;
    }
    return false;
}

uint64_t HybmVaManager::TransformVa(uint64_t va, uint32_t inputType, uint32_t outputType)
{
    BM_VALIDATE_RETURN(va > 0 && inputType < HVM_BUTT && outputType < HVM_BUTT,
                       "input is invalid, va=" << VaToStr(va) << " Itype=" << inputType << " Otype=" << outputType, 0);

    if (inputType == outputType) {
        return va;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (allocatedMap_[inputType].empty()) {
        BM_LOG_WARN("No allocated spaces found for " << inputType << ". input_va=" << VaToStr(va));
        return 0;
    }
    auto it = allocatedMap_[inputType].upper_bound(va);
    if (it != allocatedMap_[inputType].begin()) {
        --it;
        if (it->second.Contains(va, inputType) && it->second.base.va[outputType] != 0) {
            // Calculate the corresponding GVA
            uint64_t offset = va - it->second.base.va[inputType];
            uint64_t outputVa = it->second.base.va[outputType] + offset;
            BM_LOG_DEBUG("GetGvaByVa range mapping: input_va=" << VaToStr(va) << " -> output_va=" << VaToStr(outputVa)
                                                               << " (offset=" << VaToStr(offset) << ")");
            return outputVa;
        }
    }
    BM_LOG_DEBUG("TransformVa: no mapping found for va=" << VaToStr(va));
    return 0;
}

hybm_mem_type HybmVaManager::GetGvaMemType(uint64_t va)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (allocatedMap_[HVM_GVA].empty()) {
        return HYBM_MEM_TYPE_BUTT;
    }
    auto it = allocatedMap_[HVM_GVA].upper_bound(va);
    if (it != allocatedMap_[HVM_GVA].begin()) {
        --it;
    }
    if (it->second.Contains(va, HVM_GVA)) {
        BM_LOG_DEBUG("GetMemType: va=" << VaToStr(va) << " memType=" << it->second.base.memType);
        return it->second.base.memType;
    }
    BM_LOG_DEBUG("GetMemType: va=" << VaToStr(va) << " not found, returning default type");
    return HYBM_MEM_TYPE_BUTT;
}

AddrType HybmVaManager::ClassifyAddress(const uint64_t va)
{
    uint8_t mask = ClassifyAddressMask(va);
    if (mask & BIT_LOCAL_HOST) {
        return LOCAL_HOST;
    }
    if (mask & BIT_GLOBAL_HOST) {
        return GLOBAL_HOST;
    }
    if (mask & BIT_LOCAL_DEVICE) {
        return LOCAL_DEVICE;
    }
    if (mask & BIT_GLOBAL_DEVICE) {
        return GLOBAL_DEVICE;
    }
    return LOCAL_HOST;
}

Result HybmVaManager::GetLocalMemoryType(uint64_t va, hybm_mem_type &memType) const noexcept
{
    memType = HYBM_MEM_TYPE_BUTT;
    if (va == 0) {
        BM_LOG_ERROR("GetLocalMemoryType failed: va=0 is invalid");
        return BM_INVALID_PARAM;
    }

    // On A3 (910C) the VA of NPU-pinned host memory may fall into the HBM address range, where the
    // address-range check misclassifies it and breaks RDMA/SDMA registration. Query the driver attribute
    // on every 910C transport (A3 memcache error fix); other SoCs keep the address-range check.
    const bool queryDrvAttr = soc_ == ASCEND_950 || soc_ == ASCEND_910C;
    if (queryDrvAttr) {
        DVattribute attr{};
        auto ret = DlHalApi::DrvMemGetAttribute(static_cast<DVdeviceptr>(va), &attr);
        if (ret == BM_OK) {
            memType = (attr.memType == DV_MEM_SVM_DEVICE || attr.memType == DV_MEM_LOCK_DEV ||
                       attr.memType == DV_MEM_LOCK_DEV_DVPP)
                          ? HYBM_MEM_TYPE_DEVICE
                          : HYBM_MEM_TYPE_HOST;
            return BM_OK;
        }
        if (soc_ == ASCEND_950) {
            BM_LOG_ERROR("GetLocalMemoryType failed: interface=DrvMemGetAttribute ret=" << ret
                                                                                        << ", va=" << VaToStr(va));
            return ret;
        }
        // 910C if driver attr unavailable, fallback to address check
        BM_LOG_DEBUG("GetLocalMemoryType: DrvMemGetAttribute unavailable on 910C, fallback to address check, ret="
                     << ret << ", va=" << VaToStr(va));
    }

    memType = (va >= HYBM_HBM_START_ADDR && va < HYBM_HBM_END_ADDR) ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    return BM_OK;
}

bool HybmVaManager::TryGetLocalRegisteredMemTypeLocked(uint64_t va, hybm_mem_type &memType) const
{
    const uint32_t localTypes[] = {HVM_HVA, HVM_DVA};
    bool found = false;
    for (auto type : localTypes) {
        const auto &allocations = allocatedMap_[type];
        if (allocations.empty()) {
            continue;
        }
        auto it = allocations.upper_bound(va);
        if (it == allocations.begin()) {
            continue;
        }
        --it;
        if (!it->second.Contains(va, type)) {
            continue;
        }
        if (found && memType != it->second.base.memType) {
            return false;
        }
        memType = it->second.base.memType;
        found = true;
    }
    return found;
}

bool HybmVaManager::ClassifyAddressMaskLocked(uint64_t va, uint8_t &mask) const
{
    auto r = QueryAddrLocked(va);
    // 已 import → 单 GLOBAL
    if (r.inAllocGva && r.imported) {
        mask = (r.memType == HYBM_MEM_TYPE_DEVICE) ? BIT_GLOBAL_DEVICE : BIT_GLOBAL_HOST;
        return true;
    }
    // reservedMap_ → GVA，已在 allocatedMap_ 中才设有效位
    if (!reservedMap_[HVM_GVA].empty()) {
        auto it = reservedMap_[HVM_GVA].upper_bound(va);
        if (it != reservedMap_[HVM_GVA].begin()) {
            --it;
        }
        if (it->second.Contains(va)) {
            if (!r.inAllocGva) {
                mask = 0; // 未 join → 无效
                return true;
            }
            if (r.memType == HYBM_MEM_TYPE_DEVICE ||
                (r.memType == HYBM_MEM_TYPE_BUTT && it->second.memType == HYBM_MEM_TYPE_DEVICE)) {
                mask = BIT_LOCAL_DEVICE | BIT_GLOBAL_DEVICE;
            } else {
                mask = BIT_LOCAL_HOST | BIT_GLOBAL_HOST;
            }
            return true;
        }
    }
    if (r.inAllocGva) {
        mask = (r.memType == HYBM_MEM_TYPE_DEVICE) ? BIT_LOCAL_DEVICE : BIT_LOCAL_HOST;
        return true;
    }
    hybm_mem_type registeredMemType = HYBM_MEM_TYPE_BUTT;
    if (TryGetLocalRegisteredMemTypeLocked(va, registeredMemType)) {
        mask = (registeredMemType == HYBM_MEM_TYPE_DEVICE) ? BIT_LOCAL_DEVICE : BIT_LOCAL_HOST;
        return true;
    }
    return false;
}

uint8_t HybmVaManager::ClassifyAddressMask(const uint64_t va)
{
    uint8_t mask = 0U;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ClassifyAddressMaskLocked(va, mask)) {
            return mask;
        }
    }

    hybm_mem_type memType;
    auto ret = GetLocalMemoryType(va, memType);
    if (ret != BM_OK) {
        return 0; // 无效
    }
    return (memType == HYBM_MEM_TYPE_DEVICE) ? BIT_LOCAL_DEVICE : BIT_LOCAL_HOST;
}

void HybmVaManager::ClassifyAddressMask(uint64_t srcVa, uint64_t destVa, uint8_t &srcMask, uint8_t &destMask)
{
    bool srcResolved = false;
    bool destResolved = false;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        srcResolved = ClassifyAddressMaskLocked(srcVa, srcMask);
        destResolved = ClassifyAddressMaskLocked(destVa, destMask);
    }

    auto classifyLocal = [this](uint64_t va, uint8_t &mask) {
        hybm_mem_type memType = HYBM_MEM_TYPE_BUTT;
        auto ret = GetLocalMemoryType(va, memType);
        if (ret != BM_OK) {
            mask = 0U;
            return;
        }
        mask = (memType == HYBM_MEM_TYPE_DEVICE) ? BIT_LOCAL_DEVICE : BIT_LOCAL_HOST;
    };
    if (!srcResolved) {
        classifyLocal(srcVa, srcMask);
    }
    if (!destResolved) {
        classifyLocal(destVa, destMask);
    }
}

hybm_data_copy_direction HybmVaManager::InferCopyDirection(uint64_t srcVa, uint64_t dstVa)
{
    auto src = static_cast<int>(ClassifyAddress(srcVa));
    auto dst = static_cast<int>(ClassifyAddress(dstVa));
    if (src >= 0 && src < ADDRESS_CATEGORY_BUTT && dst >= 0 && dst < ADDRESS_CATEGORY_BUTT) {
        auto dir = COPY_DIRECTION_TABLE[src][dst];
        if (dir != HYBM_DATA_COPY_DIRECTION_BUTT) {
            return dir;
        }
        // 方向表 BUTT 兜底：L2G 和 H2GD 已在表中，仅 LOCAL→LOCAL 需要
        if (src == LOCAL_HOST && dst == LOCAL_HOST) {
            // 检查哪个地址在 managed 范围内，就让它作为被检查的一方
            if (dstVa >= HYBM_GLOBAL_GVM_START_ADDR && dstVa < HYBM_GVM_END_ADDR) {
                return HYBM_LOCAL_HOST_TO_GLOBAL_HOST; // g_checkMap[0]={false,true} 查 dest
            }
            if (srcVa >= HYBM_GLOBAL_GVM_START_ADDR && srcVa < HYBM_GVM_END_ADDR) {
                return HYBM_GLOBAL_HOST_TO_LOCAL_HOST; // g_checkMap[6]={true,false} 查 src
            }
        }
    }
    return HYBM_DATA_COPY_DIRECTION_BUTT;
}

bool HybmVaManager::IsValidAddr(uint64_t va)
{
    auto r = QueryAddr(va);
    return r.inAllocGva;
}

ReservedGvaInfo HybmVaManager::AllocReserveGva(const GvaReserveOptions &options)
{
    ReservedGvaInfo result;
    uint32_t t = HVM_DVA; // reserve device va all
    BM_VALIDATE_RETURN(options.size != 0, "size must > 0.", result);
    BM_VALIDATE_RETURN(options.localSize != 0 || options.enable56BitsGva,
                       "local size must > 0 when 56-bit GVA is not enabled.", result);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    uint64_t lva = 0;
    // The A5 branch bumps localSize, so operate on the copy instead of on options.localSize.
    uint64_t localSize = options.localSize;
    if (localSize > 0) {
        if (soc_ == ASCEND_950) {
            localSize += GB; // A5被底软额外占用了1G
        }
        lva = AllocReserveLvaInner(localSize, t, options.scene);
        if (lva == 0) {
            return result;
        }
    }

    uint64_t gva = lva;
    if (options.enable56BitsGva) {
        uint64_t startAddr = HYBM_56BITS_GVA_START_ADDR;
        uint64_t endAddr = HYBM_56BITS_GVA_END_ADDR;
        if (options.scene == HYBM_SCENE_TRANS) {
            startAddr = HYBM_TRANS_GVA_START_ADDR;
            endAddr = HYBM_TRANS_GVA_END_ADDR;
        }
        uint64_t upperLimit = endAddr - startAddr;
        if (options.size > upperLimit) {
            BM_LOG_ERROR("Failed to reserve size:" << options.size << ", upper limit size:" << upperLimit);
            return result;
        }

        BM_LOG_DEBUG("enable56BitsGva searching in GVA range " << VaToStr(startAddr) << "-" << VaToStr(endAddr));
        // Find free space
        auto [freeAddr, found] = FindFreeSpace(startAddr, endAddr, options.size, HVM_GVA);
        if (!found) {
            BM_LOG_ERROR("AllocReserveGva failed: no free space found for size=" << VaToStr(options.size));
            return result;
        }
        gva = freeAddr;
    }

    result.va[HVM_GVA] = gva;
    result.va[t] = lva;
    result.size = options.size;
    result.memType = options.memType;
    reservedMap_[HVM_GVA][result.va[HVM_GVA]] = result;
    // 启用 56 位 GVA 时 lvaSize 与 gvaSize 不一致
    auto lvaResult = result;
    lvaResult.size = localSize;
    reservedMap_[t][lvaResult.va[t]] = lvaResult;
    BM_LOG_DEBUG("AllocReserveGva success: " << result);
    return result;
}

ReservedGvaInfo HybmVaManager::AllocReserveLva(uint64_t size, uint32_t type, hybm_mem_type memType)
{
    ReservedGvaInfo result;
    BM_VALIDATE_RETURN(size != 0, "size must > 0.", result);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto lva = AllocReserveLvaInner(size, type);
    if (lva == 0) {
        return result;
    }
    result.va[type] = lva;
    result.size = size;
    result.memType = memType;
    reservedMap_[type][result.va[type]] = result;
    BM_LOG_DEBUG("AllocReserveLva success: " << result);
    return result;
}

uint64_t HybmVaManager::AllocReserveLvaInner(uint64_t size, uint32_t type, hybm_scene scene)
{
    if (size == 0) {
        BM_LOG_ERROR("AllocReserveLva failed: size=0");
        return 0;
    }
    // VA 区段选择：常规 GVM 区按 scene 物理隔离。
    // HYBM_SCENE_OFFLOAD       → 搜索 [OFFLOAD_VA_START, OFFLOAD_VA_END)（最前2T）
    // 其他(BM/SHM/Trans/...)   → 搜索 [GVM_START_ADDR, GVM_END)，HYBM_GVM_START_ADDR 已等于 OFFLOAD_VA_END_ADDR
    uint64_t startAddr;
    uint64_t endAddr;
    if (scene == HYBM_SCENE_OFFLOAD) {
        startAddr = (soc_ == ASCEND_950) ? HYBM_OFFLOAD_VA_START_ADDR_A5 : HYBM_OFFLOAD_VA_START_ADDR;
        endAddr = (soc_ == ASCEND_950) ? HYBM_OFFLOAD_VA_END_ADDR_A5 : HYBM_OFFLOAD_VA_END_ADDR;
    } else {
        startAddr = (soc_ == ASCEND_950) ? HYBM_GVM_START_ADDR_A5 : HYBM_GVM_START_ADDR;
        endAddr = (soc_ == ASCEND_950) ? HYBM_GVM_END_ADDR_A5 : HYBM_GVM_END_ADDR;
    }

    uint64_t upperLimit = endAddr - startAddr;
    if (size > upperLimit) {
        BM_LOG_ERROR("Failed to reserve size:" << size << ", upper limit size:" << upperLimit << ", scene=" << scene);
        return 0;
    }

    BM_LOG_DEBUG("AllocReserveLva, searching in range " << VaToStr(startAddr) << "-" << VaToStr(endAddr)
                                                        << ", scene=" << scene);
    // Find free space
    auto [freeAddr, found] = FindFreeSpace(startAddr, endAddr, size, type);
    if (!found) {
        BM_LOG_ERROR("AllocReserveLva failed: no free space found for size=" << VaToStr(size));
        return 0;
    }

    BM_LOG_DEBUG("AllocReserveLva success: " << VaToStr(freeAddr) << " size: " << size);
    return freeAddr;
}

void HybmVaManager::FreeReserveGva(uint64_t addr)
{
    if (addr == 0) {
        BM_LOG_WARN("Unable to FreeReserveGva: invalid addr=0");
        return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = reservedMap_[HVM_GVA].find(addr);
    if (it == reservedMap_[HVM_GVA].end()) {
        BM_LOG_WARN("Unable to FreeReserveGva: reserved space not found at addr=" << VaToStr(addr));
        return;
    }
    const ReservedGvaInfo &info = it->second;
    BM_LOG_DEBUG("FreeReserveGva: " << info);
    auto type = (info.va[HVM_DVA] != 0 ? HVM_DVA : HVM_HVA);
    reservedMap_[type].erase(info.va[type]);
    reservedMap_[HVM_GVA].erase(it);
    BM_LOG_DEBUG("FreeReserveGva success: addr=" << VaToStr(addr));
}

void HybmVaManager::FreeReserveLva(uint64_t addr, uint32_t type)
{
    if (addr == 0 || (type != HVM_DVA && type != HVM_HVA)) {
        BM_LOG_WARN("Unable to FreeReserveLva: invalid addr:" << addr << ", type:" << type);
        return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = reservedMap_[type].find(addr);
    if (it == reservedMap_[type].end()) {
        BM_LOG_WARN("FreeReserveDva failed: reserved space not found at addr=" << VaToStr(addr));
        return;
    }
    const ReservedGvaInfo &info = it->second;
    BM_LOG_DEBUG("FreeReserveLva: " << info);
    reservedMap_[type].erase(addr);
    BM_LOG_DEBUG("FreeReserveLva success: addr=" << VaToStr(addr));
}

void HybmVaManager::DumpReservedGvaInfo() const
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    BM_LOG_DEBUG("Total reserved spaces: " << reservedMap_[HVM_GVA].size());
    if (reservedMap_[HVM_GVA].empty()) {
        BM_LOG_WARN("No reserved spaces found.");
        return;
    }
    int index = 1;
    uint64_t totalSizeMB = 0;
    for (const auto &pair : reservedMap_[HVM_GVA]) {
        const ReservedGvaInfo &info = pair.second;
        BM_LOG_DEBUG(index << ". " << info);
        totalSizeMB += (info.size >> MB_OFFSET);
        index++;
    }
    BM_LOG_DEBUG("Total reserved size: " << totalSizeMB << "MB");
}

void HybmVaManager::DumpAllocatedGvaInfo() const
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    BM_LOG_DEBUG("Total allocated spaces: " << allocatedMap_[HVM_GVA].size());
    if (allocatedMap_[HVM_GVA].empty()) {
        BM_LOG_DEBUG("No allocated spaces found.");
        return;
    }
    int index = 1;
    uint64_t totalSizeMB = 0;
    for (const auto &pair : allocatedMap_[HVM_GVA]) {
        const AllocatedGvaInfo &info = pair.second;
        BM_LOG_DEBUG(index << ". " << info);
        totalSizeMB += (info.base.size >> MB_OFFSET);
        index++;
    }
    BM_LOG_DEBUG("Total allocated size: " << totalSizeMB << "MB");
    std::map<hybm_mem_type, uint64_t> sizeByType;
    std::map<hybm_mem_type, size_t> countByType;
    for (const auto &pair : allocatedMap_[HVM_GVA]) {
        const AllocatedGvaInfo &info = pair.second;
        sizeByType[info.base.memType] += (info.base.size >> MB_OFFSET);
        countByType[info.base.memType]++;
    }
    for (const auto &pair : sizeByType) {
        hybm_mem_type memType = pair.first;
        uint64_t sizeMB = pair.second;
        size_t count = countByType[memType];
        BM_LOG_DEBUG(memType << ": " << count << " allocations, total size: " << sizeMB << "MB");
    }
}

std::pair<bool, AllocatedGvaInfo> HybmVaManager::CheckOverlap(const uint64_t va, const uint64_t size, uint32_t type)
{
    const uint64_t end = va + size;
    if (allocatedMap_[type].empty()) {
        return {false, {}};
    }
    auto it = allocatedMap_[type].upper_bound(va);
    if (it != allocatedMap_[type].end()) {
        uint64_t st = it->first;
        uint64_t ed = it->second.base.size + st;
        if ((st <= va && va < ed) || (va <= st && st < end)) {
            return {true, it->second};
        }
    }

    if (it != allocatedMap_[type].begin()) {
        it--;
        uint64_t st = it->first;
        uint64_t ed = it->second.base.size + st;
        if ((st <= va && va < ed) || (va <= st && st < end)) {
            return {true, it->second};
        }
    }
    return {false, {}};
}

std::pair<uint64_t, bool> HybmVaManager::FindFreeSpace(uint64_t start, uint64_t end, uint64_t size, uint32_t type)
{
    if (size == 0 || start >= end || size > (end - start)) {
        BM_LOG_ERROR("FindFreeSpace: invalid parameters. start=" << VaToStr(start) << ", end=" << VaToStr(end)
                                                                 << ", size=" << VaToStr(size));
        return {0, false};
    }
    std::vector<std::pair<uint64_t, uint64_t>> usedRanges;
    for (const auto &pair : reservedMap_[type]) {
        const ReservedGvaInfo &info = pair.second;
        if (info.va[type] >= start && info.va[type] < end) {
            usedRanges.emplace_back(info.va[type], info.va[type] + info.size);
        }
    }
    BM_LOG_DEBUG("FindFreeSpace: found " << usedRanges.size() << " used ranges in target area");
    if (usedRanges.empty()) {
        BM_LOG_DEBUG("FindFreeSpace: no used ranges, using start address " << VaToStr(start));
        return {start, true};
    }
    std::sort(usedRanges.begin(), usedRanges.end());
    uint64_t current = start;
    for (const auto &range : usedRanges) {
        if (range.first > current) {
            uint64_t freeSize = range.first - current;
            if (freeSize >= size) {
                BM_LOG_DEBUG("FindFreeSpace: found free space at " << VaToStr(current)
                                                                   << ", size=" << VaToStr(freeSize));
                return {current, true};
            }
            BM_LOG_DEBUG("FindFreeSpace: free space at " << VaToStr(current) << " too small (size=" << VaToStr(freeSize)
                                                         << ", needed=" << VaToStr(size) << ")");
        }
        if (range.second > current) {
            current = range.second;
        }
    }
    BM_LOG_DEBUG("FindFreeSpace: current:" << VaToStr(current) << " end:" << VaToStr(end) << " size:" << VaToStr(size));
    if (current <= end && size <= end - current) {
        BM_LOG_DEBUG("FindFreeSpace: found free space at end, addr=" << VaToStr(current));
        return {current, true};
    }
    BM_LOG_ERROR("FindFreeSpace: no suitable free space found, size: " << VaToStr(size));
    return {0, false};
}

std::pair<AllocatedGvaInfo, bool> HybmVaManager::FindAllocByVa(uint64_t va, uint32_t type) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (allocatedMap_[type].empty()) {
        BM_LOG_DEBUG("No allocated spaces found.");
        return {AllocatedGvaInfo{}, false};
    }
    auto it = allocatedMap_[type].upper_bound(va);
    if (it != allocatedMap_[type].begin()) {
        --it;
    }
    if (it->second.Contains(va, type)) {
        return {it->second, true};
    }
    return {AllocatedGvaInfo{}, false};
}

size_t HybmVaManager::GetAllocCount() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    size_t count = allocatedMap_[HVM_GVA].size();
    BM_LOG_DEBUG("GetAllocCount: " << count);
    return count;
}

size_t HybmVaManager::GetReservedCount() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    size_t count = reservedMap_[HVM_GVA].size();
    BM_LOG_DEBUG("GetReservedCount: " << count);
    return count;
}

void HybmVaManager::ClearAll()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    BM_LOG_DEBUG("Alloc count before clear: " << allocatedMap_[HVM_GVA].size());
    BM_LOG_DEBUG("Reserved count before clear: " << reservedMap_[HVM_GVA].size());
    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        allocatedMap_[i].clear();
        reservedMap_[i].clear();
        rowRefCnt_[i].clear();
    }
}

const AllocatedGvaInfo *HybmVaManager::FindAllocRecordLocked(uint64_t addr) const
{
    for (uint32_t type : {HVM_HVA, HVM_DVA}) {
        auto &vaMap = allocatedMap_[type];
        if (vaMap.empty()) {
            continue;
        }
        auto it = vaMap.upper_bound(addr);
        if (it == vaMap.begin()) {
            continue;
        }
        --it;
        if (it->second.Contains(addr, type)) {
            return &it->second;
        }
    }
    return nullptr;
}

AllocatedGvaInfo *HybmVaManager::FindAllocRecordLocked(uint64_t addr)
{
    return const_cast<AllocatedGvaInfo *>(static_cast<const HybmVaManager *>(this)->FindAllocRecordLocked(addr));
}

Result HybmVaManager::AddHandle(const void *ptr, const ShareHandleStr &shareInfo)
{
    const auto addr = reinterpret_cast<uint64_t>(ptr);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto *record = FindAllocRecordLocked(addr);
    if (record == nullptr) {
        BM_LOG_ERROR("AddHandle failed: no alloc record covers addr=" << VaToStr(addr));
        return 1;
    }
    record->SetShareHandle(shareInfo);
    BM_LOG_DEBUG("AddHandle success: addr=" << VaToStr(addr) << " base=" << VaToStr(record->base.va[HVM_DVA])
                                            << " size=" << VaToStr(record->base.size));
    return 0;
}

Result HybmVaManager::GetHandle(const void *ptr, ShareHandleStr &shareInfo) const
{
    const auto addr = reinterpret_cast<uint64_t>(ptr);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto *record = FindAllocRecordLocked(addr);
    // 未登记属预期路径（如本 rank 首次导出），调用方会回退 retain/export，不记 ERROR
    if (record == nullptr || !record->GetShareHandle(shareInfo)) {
        BM_LOG_DEBUG("GetHandle miss: addr=" << VaToStr(addr));
        return 1;
    }
    return 0;
}
} // namespace mf
} // namespace ock
