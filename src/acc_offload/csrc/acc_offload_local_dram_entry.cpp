/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "hybm_big_mem.h"
#include "acc_offload_launch.h"
#include "acc_offload_local_dram_entry.h"

namespace ock {
namespace offload {

using namespace ock::mf;

constexpr uint64_t KB = 1024ULL;
constexpr uint64_t MB = KB * 1024ULL;
constexpr uint64_t GB = MB * 1024ULL;

static uint64_t AlignUp(uint64_t value, uint64_t align) noexcept
{
    return (value + align - 1) & ~(align - 1);
}

int32_t AccOffloadLocalDramEntry::Initialize(const offload_config_t &config)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (inited_) {
        return OFFLOAD_OK;
    }

    uint32_t flags = 0;
    int32_t ret = hybm_init(config.deviceId, flags);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("init hybm failed, result: " << ret);
        hybm_uninit();
        return OFFLOAD_ERROR;
    }

    uint64_t alignedReserveSize = AlignUp(config.reserveSize, GB);
    uint64_t alignedAllocSize = AlignUp(config.allocSize, GB);
    if (alignedReserveSize != alignedAllocSize) {
        OFFLOAD_LOG_ERROR("local dram requires reserveSize == allocSize, reserveSize: "
                          << config.reserveSize << ", allocSize: " << config.allocSize);
        hybm_uninit();
        return OFFLOAD_ERROR;
    }
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.memType = HYBM_MEM_TYPE_HOST;
    options.bmDataOpType = HYBM_DOP_TYPE_MTE;
    options.rankCount = 1;
    options.rankId = 0;
    options.devId = config.deviceId;
    options.maxDRAMSize = alignedReserveSize;
    options.hostVASpace = alignedAllocSize;
    options.scene = HYBM_SCENE_OFFLOAD; // offload 独立 VA 区段(常规 GVM 区最前2T)，与 BM 物理隔离
    options.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    options.dramShmFd = -1;

    options.flags |=
        (config.flags & OFFLOAD_FLAG_GIANT_PAGE) ? HYBM_FLAG_OFFLOAD_GIANT_PAGE : HYBM_FLAG_UNRESTRICTED_MEM;
    do {
        ret = AccOffloadLaunchApi::TryLoadLibrary();
        if (ret != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("offload launch load library failed");
            ret = OFFLOAD_ERROR;
            break;
        }

        entity_ = hybm_create_entity(HYBM_ENTITY_ID_OFFLOAD_BASE, &options, flags);
        if (entity_ == nullptr) {
            OFFLOAD_LOG_ERROR("create entity failed");
            ret = OFFLOAD_ERROR;
            break;
        }

        ret = hybm_reserve_mem_space(entity_, flags);
        if (ret != 0) {
            OFFLOAD_LOG_ERROR("reserve mem failed, result: " << ret);
            ret = OFFLOAD_ERROR;
            break;
        }

        if (options.maxDRAMSize > 0) {
            slice_ = hybm_alloc_local_memory(entity_, HYBM_MEM_TYPE_HOST, options.hostVASpace, flags);
            if (slice_ == nullptr) {
                OFFLOAD_LOG_ERROR("alloc local host mem failed, size: " << options.hostVASpace);
                ret = OFFLOAD_ERROR;
                break;
            }
        }

        base_ = reinterpret_cast<uint8_t *>(hybm_get_slice_va(entity_, slice_, 0));
        if (base_ == nullptr) {
            OFFLOAD_LOG_ERROR("get slice va failed");
            ret = OFFLOAD_ERROR;
            break;
        }
    } while (0);

    size_ = alignedReserveSize;
    memMng_ = std::make_shared<AccOffloadMemManager>(base_, size_);
    inited_ = true;

    if (ret != 0 || memMng_ == nullptr) {
        UnInitialize();
        return ret;
    }

    return ret;
}

void AccOffloadLocalDramEntry::UnInitialize()
{
    if (!inited_) {
        return;
    }

    uint32_t flags = 0;
    if (entity_ != nullptr) {
        if (slice_ != nullptr) {
            hybm_free_local_memory(entity_, slice_, 1, flags);
        }
        hybm_unreserve_mem_space(entity_, flags);
        hybm_destroy_entity(entity_, flags);
    }

    AccOffloadLaunchApi::CleanupLibrary();

    hybm_uninit();
    entity_ = nullptr;
    inited_ = false;
}

void *AccOffloadLocalDramEntry::MallocHost(size_t size)
{
    if (memMng_ == nullptr) {
        OFFLOAD_LOG_ERROR("mem manager is nullptr, malloc failed");
        return nullptr;
    }

    OFFLOAD_LOG_DEBUG("malloc host size: " << size);
    return memMng_->Allocate(size);
}

void AccOffloadLocalDramEntry::FreeHost(void *ptr)
{
    if (memMng_ == nullptr || ptr == nullptr) {
        OFFLOAD_LOG_ERROR("mem manager is nullptr or ptr is nullptr, free failed");
        return;
    }

    OFFLOAD_LOG_DEBUG("free host ptr: " << reinterpret_cast<uint64_t>(ptr));
    memMng_->Release(ptr);
}

int32_t AccOffloadLocalDramEntry::GetDva(uint64_t hostPtr, uint64_t *dvaPtr)
{
    auto ret = hybm_gva_to_va(hostPtr, HYBM_MEM_TYPE_DEVICE, dvaPtr);
    if (ret != 0) {
        OFFLOAD_LOG_ERROR("hybm_gva_to_va failed, hostPtr: 0x" << std::hex << hostPtr << ", ret: " << ret);
        return OFFLOAD_ERROR;
    }

    OFFLOAD_LOG_INFO("get dva: hva=0x" << std::hex << hostPtr << " -> dva=0x" << *dvaPtr);
    return OFFLOAD_OK;
}

int32_t AccOffloadLocalDramEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                                             uint8_t devIdx)
{
    OFFLOAD_LOG_DEBUG("sparse copy, src: " << reinterpret_cast<uint64_t>(srcPtrs)
                                           << ", dst: " << reinterpret_cast<uint64_t>(dstPtrs) << ", len: "
                                           << reinterpret_cast<uint64_t>(lenPtrs) << ", devIdx: " << devIdx);

    return AccOffloadLaunchApi::AccOffloadSparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, devIdx);
}

int32_t AccOffloadLocalDramEntry::GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                uint32_t *numLocalExpertPtr, int64_t *groupList,
                                                int64_t *packedGroupList, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadGroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList,
                                                        packedGroupList, devIdx);
}

int32_t AccOffloadLocalDramEntry::KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
{
    return AccOffloadLaunchApi::AccOffloadKvExchange(metaPtr, devIdx);
}

} // namespace offload
} // namespace ock
