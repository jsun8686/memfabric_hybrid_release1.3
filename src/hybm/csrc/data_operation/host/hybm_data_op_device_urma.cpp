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
#include "hybm_data_op_device_urma.h"

#include <sys/mman.h>
#include <cstdint>
#include <new>

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "dl_hcomm_api.h"
#include "hybm_copy_direction.h"
#include "hybm_def.h"
#include "hybm_define.h"
#include "hybm_logger.h"
#include "hybm_types.h"
#include "hybm_ptracer.h"
#include "hybm_gva.h"
#include "hybm_stream_manager.h"
#include "hybm_va_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace {
constexpr uint64_t URMA_SWAP_SPACE_SIZE = 0;

bool IsSupportedUrmaDirection(hybm_data_copy_direction direction)
{
    const auto value = static_cast<int32_t>(direction);
    return value >= static_cast<int32_t>(HYBM_LOCAL_HOST_TO_GLOBAL_HOST) &&
           value < static_cast<int32_t>(HYBM_DATA_COPY_DIRECTION_AUTO);
}
} // namespace

namespace ock {
namespace mf {

DataOpDeviceURMA::DataOpDeviceURMA(uint32_t rankId, std::shared_ptr<transport::TransportManager> tm) noexcept
    : rankId_{rankId}, transportManager_{std::move(tm)}
{}

Result DataOpDeviceURMA::Initialize() noexcept
{
    if (inited_) {
        return BM_OK;
    }
    urmaSwapSpaceSize_ =
        MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_URMA_SWAP_SPACE_SIZE, URMA_SWAP_SPACE_SIZE) * MB;
    if (urmaSwapSpaceSize_ == 0) {
        BM_LOG_INFO("HYBM_URMA_SWAP_SPACE_SIZE is 0, skip swap memory allocation");
        inited_ = true;
        return BM_OK;
    }
    auto ret = AllocSwapMemory();
    if (ret != BM_OK) {
        return ret;
    }
    transport::TransportMemoryRegion input;
    input.addr = reinterpret_cast<uint64_t>(urmaSwapBaseAddr_);
    input.size = urmaSwapSpaceSize_;
    input.flags = transport::REG_MR_FLAG_HBM; // 先使用hbm swap把dram/hbm池全部调通
    if (transportManager_ != nullptr) {
        ret = transportManager_->RegisterMemoryRegion(input);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to register urma swap memory, size: " << urmaSwapSpaceSize_);
            FreeSwapMemory();
            return BM_MALLOC_FAILED;
        }
    }
    urmaSwapMemoryAllocator_ = std::make_shared<RbtreeRangePool>((uint8_t *)urmaSwapBaseAddr_, urmaSwapSpaceSize_);
    inited_ = true;
    return BM_OK;
}

void DataOpDeviceURMA::UnInitialize() noexcept
{
    FreeSwapMemory();
    inited_ = false;
}

void DataOpDeviceURMA::TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept
{
    if (UNLIKELY(src == nullptr && dst == nullptr)) {
        return;
    }
    // 对于本地内存, DRAM需要输入host va, HBM需要输入device va, transport才能识别
    // 对于远端内存, transport仅记录的gva, TransformVa输出应当为0
    uint64_t out;
    uint32_t oType = (HybmDirectionSrcMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;
    out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(src), HVM_GVA, oType);
    if (out != 0) {
        src = reinterpret_cast<void *>(out);
    }

    oType = (HybmDirectionDestMemType[direction] == HYBM_MEM_TYPE_HOST) ? HVM_HVA : HVM_DVA;
    out = HybmVaManager::GetInstance().TransformVa(reinterpret_cast<uint64_t>(dst), HVM_GVA, oType);
    if (out != 0) {
        dst = reinterpret_cast<void *>(out);
    }
}

Result DataOpDeviceURMA::AllocSwapMemory()
{
    void *ptr = nullptr;
    auto ret = DlAclApi::AclrtMalloc(
        &ptr, urmaSwapSpaceSize_,
        static_cast<aclrtMemMallocPolicy>(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_ONLY));
    if (ret != 0) {
        BM_LOG_ERROR("Failed to AclrtMallocHost urma swap memory, size: " << urmaSwapSpaceSize_);
        return BM_MALLOC_FAILED;
    }
    ret = HybmVaManager::GetInstance().AddVaInfo(
        {0, reinterpret_cast<uint64_t>(ptr), 0, urmaSwapSpaceSize_, HYBM_MEM_TYPE_DEVICE});
    if (ret != 0) {
        BM_LOG_ERROR("add va info failed, va:" << ptr << " ret:" << ret);
        const auto freeRet = DlAclApi::AclrtFree(ptr);
        if (freeRet != 0) {
            BM_LOG_ERROR("Failed to AclrtFree urma swap memory, ret: " << freeRet);
        }
        return ret;
    }

    urmaSwapBaseAddr_ = ptr;
    return BM_OK;
}

void DataOpDeviceURMA::FreeSwapMemory()
{
    if (urmaSwapBaseAddr_ != nullptr) {
        if (transportManager_ != nullptr) {
            const auto ret = transportManager_->UnregisterMemoryRegion((uint64_t)urmaSwapBaseAddr_);
            if (ret != 0) {
                BM_LOG_ERROR("Failed to UnregisterMemoryRegion, ret: " << ret);
            }
        }
        const auto ret = DlAclApi::AclrtFree(urmaSwapBaseAddr_);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to AclrtFreeHost swap memory, ret: " << ret);
        }
        HybmVaManager::GetInstance().RemoveOneVaInfo(reinterpret_cast<uint64_t>(urmaSwapBaseAddr_), HVM_DVA);
        urmaSwapBaseAddr_ = nullptr;
    }
}

DataOpDeviceURMA::~DataOpDeviceURMA()
{
    FreeSwapMemory();
    inited_ = false;
}

Result DataOpDeviceURMA::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                  const ock::mf::ExtOptions &options) noexcept
{
    if (!IsSupportedUrmaDirection(direction)) {
        BM_LOG_ERROR("URMA data copy invalid direction: " << direction << " localRank: " << rankId_
                                                          << " dataSize: " << params.dataSize);
        return BM_INVALID_PARAM;
    }

    Result ret;
    // only convert local-side addresses; remote side stays in GVA.
    TransformVa(params.src, params.dest, direction);
    switch (direction) {
        case HYBM_LOCAL_HOST_TO_GLOBAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_LH_TO_GH);
            ret = CopyLH2GH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_LH_TO_GH, ret);
            break;
        }
        case HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_LH_TO_GD);
            ret = CopyLH2GD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_LH_TO_GD, ret);
            break;
        }
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_LD_TO_GH);
            ret = CopyLD2GH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_LD_TO_GH, ret);
            break;
        }
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_LD_TO_GD);
            ret = CopyLD2GD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_LD_TO_GD, ret);
            break;
        }
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GD_TO_GD);
            ret = CopyGD2GD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GD_TO_GD, ret);
            break;
        }
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GD_TO_GH);
            ret = CopyGD2GH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GD_TO_GH, ret);
            break;
        }
        case HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GH_TO_GD);
            ret = CopyGH2GD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GH_TO_GD, ret);
            break;
        }
        case HYBM_GLOBAL_HOST_TO_GLOBAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GH_TO_GH);
            ret = CopyGH2GH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GH_TO_GH, ret);
            break;
        }
        case HYBM_GLOBAL_HOST_TO_LOCAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GH_TO_LH);
            ret = CopyGH2LH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GH_TO_LH, ret);
            break;
        }
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GD_TO_LH);
            ret = CopyGD2LH(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GD_TO_LH, ret);
            break;
        }
        case HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GH_TO_LD);
            ret = CopyGH2LD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GH_TO_LD, ret);
            break;
        }
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE: {
            TP_TRACE_BEGIN(TP_HYBM_URMA_GD_TO_LD);
            ret = CopyGD2LD(params.src, params.dest, params.dataSize, options);
            TP_TRACE_END(TP_HYBM_URMA_GD_TO_LD, ret);
            break;
        }
        default:
            BM_LOG_ERROR("data copy invalid direction: " << direction);
            ret = BM_INVALID_PARAM;
    }
    return ret;
}

Result DataOpDeviceURMA::CopyLH2LH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    auto ret = DlAclApi::AclrtMemcpy(destVA, length, srcVA, length, ACL_MEMCPY_HOST_TO_HOST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy failed, ret: " << ret << " Src=" << VaToInfo(srcVA) << " dest=" << VaToInfo(destVA)
                                                 << " length=" << length);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}
Result DataOpDeviceURMA::CopyLD2LD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    auto ret = DlAclApi::AclrtMemcpy(destVA, length, srcVA, length, ACL_MEMCPY_DEVICE_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy failed, ret: " << ret << " Src=" << VaToInfo(srcVA) << " dest=" << VaToInfo(destVA)
                                                 << " length=" << length);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result DataOpDeviceURMA::CopyLH2LD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    auto ret = DlAclApi::AclrtMemcpy(destVA, length, srcVA, length, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy failed, ret: " << ret << " Src=" << VaToInfo(srcVA) << " dest=" << VaToInfo(destVA)
                                                 << " length=" << length);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result DataOpDeviceURMA::CopyLD2LH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    auto ret = DlAclApi::AclrtMemcpy(destVA, length, srcVA, length, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy failed, ret: " << ret << " Src=" << VaToInfo(srcVA) << " dest=" << VaToInfo(destVA)
                                                 << " length=" << length);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result DataOpDeviceURMA::CopyLH2GH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.destRankId == rankId_) {
        ret = CopyLH2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafePut(srcVA, destVA, length, options, true);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyLH2GD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.destRankId == rankId_) {
        ret = CopyLH2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafePut(srcVA, destVA, length, options, true);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyLD2GH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.destRankId == rankId_) {
        ret = CopyLD2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafePut(srcVA, destVA, length, options, false);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyLD2GD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.destRankId == rankId_) {
        ret = CopyLD2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafePut(srcVA, destVA, length, options, false);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyURMA(const void *srcVA, void *destVA, uint64_t length,
                                  const ock::mf::ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    auto src = (uint64_t)(ptrdiff_t)srcVA;
    auto dest = (uint64_t)(ptrdiff_t)destVA;
    Result ret;
    if (options.srcRankId == rankId_) {
        ret = transportManager_->WriteRemote(options.destRankId, src, dest, length);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to write src to dest", ret);
    } else if (options.destRankId == rankId_) {
        ret = transportManager_->ReadRemote(options.srcRankId, dest, src, length);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to read src to dest", ret);
    } else {
        BM_LOG_ERROR("Invalid param, local rank:" << rankId_ << ", srcId: " << options.srcRankId
                                                  << ", dstId: " << options.destRankId);
        return BM_INVALID_PARAM;
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGH2GH(const void *srcVA, void *destVA, uint64_t length,
                                   const ock::mf::ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_ && options.destRankId == rankId_) {
        ret = CopyLH2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to urma src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGD2GH(const void *srcVA, void *destVA, uint64_t length,
                                   const ock::mf::ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_ && options.destRankId == rankId_) {
        ret = CopyLD2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to urma src to dest", ret);
    }
    return ret;
}
Result DataOpDeviceURMA::CopyGH2GD(const void *srcVA, void *destVA, uint64_t length,
                                   const ock::mf::ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_ && options.destRankId == rankId_) {
        ret = CopyLH2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to urma src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGD2GD(const void *srcVA, void *destVA, uint64_t length,
                                   const ock::mf::ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_ && options.destRankId == rankId_) {
        ret = CopyLD2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to urma src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGH2LH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_) {
        ret = CopyLH2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafeGet(srcVA, destVA, length, options, true);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGD2LH(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_) {
        ret = CopyLD2LH(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafeGet(srcVA, destVA, length, options, true);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGH2LD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_) {
        ret = CopyLH2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafeGet(srcVA, destVA, length, options, false);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyGD2LD(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options) noexcept
{
    BM_LOG_DEBUG("SrcVA=" << VaToInfo(srcVA) << ", destVA=" << VaToInfo(destVA) << ", length=" << length);
    Result ret;
    if (options.srcRankId == rankId_) {
        ret = CopyLD2LD(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    } else {
        ret = SafeGet(srcVA, destVA, length, options, false);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to dest", ret);
    }
    return ret;
}

Result DataOpDeviceURMA::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                       const ExtOptions &options) noexcept
{
    BM_LOG_ERROR("DataOpDeviceURMA::DataCopyAsync Not Supported!");
    return BM_ERROR;
}

Result DataOpDeviceURMA::Wait(int32_t waitId) noexcept
{
    // Since DataOpDeviceURMA::DataCopyAsync is not supported, Wait should do nothing for now.
    return BM_OK;
}

Result DataOpDeviceURMA::BatchDataCopyDefault(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                              const ExtOptions &options) noexcept
{
    Result ret;
    TP_TRACE_BEGIN(TP_HYBM_URMA_BATCH_DEFAULT);
    for (uint32_t i = 0; i < params.batchSize; i++) {
        hybm_copy_params pm = {params.sources[i], params.destinations[i], params.dataSizes[i]};
        ret = DataCopy(pm, direction, options);
        if (ret != BM_OK) {
            break;
        }
    }
    TP_TRACE_END(TP_HYBM_URMA_BATCH_DEFAULT, ret);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "[BatchDataCopy] Failed to copy src to dest", ret);
    return BM_OK;
}

Result DataOpDeviceURMA::BatchDataCopyLocal(hybm_batch_copy_params &params, int32_t direction,
                                            const ock::mf::ExtOptions &options) noexcept
{
    switch (direction) {
        case HYBM_LOCAL_HOST_TO_GLOBAL_HOST:
        case HYBM_GLOBAL_HOST_TO_GLOBAL_HOST:
        case HYBM_GLOBAL_HOST_TO_LOCAL_HOST:
            return BatchDataCopyLocalSync(params, ACL_MEMCPY_HOST_TO_HOST, options);
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE:
            return BatchDataCopyLocalAsync(params, ACL_MEMCPY_DEVICE_TO_DEVICE, options);
        case HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE:
        case HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE:
            return BatchDataCopyLocalAsync(params, ACL_MEMCPY_HOST_TO_DEVICE, options);
        case HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST:
        case HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST:
            return BatchDataCopyLocalAsync(params, ACL_MEMCPY_DEVICE_TO_HOST, options);
        case HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE:
            return BatchDataCopyLocalBatch(params, ACL_MEMCPY_HOST_TO_DEVICE, options);
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST:
            return BatchDataCopyLocalBatch(params, ACL_MEMCPY_DEVICE_TO_HOST, options);
        default:
            BM_LOG_ERROR("Failed to BatchDataCopyLocal not support direct:" << direction);
            return -1;
    }
}

Result DataOpDeviceURMA::BatchDataCopyLocalSync(hybm_batch_copy_params &params, int32_t direction,
                                                const ExtOptions &options) noexcept
{
    for (size_t i = 0; i < params.batchSize; ++i) {
        auto destAddr = params.destinations[i];
        auto srcAddr = params.sources[i];
        auto count = params.dataSizes[i];
        auto ret = DlAclApi::AclrtMemcpy(destAddr, count, srcAddr, count, direction);
        if (ret != 0) {
            BM_LOG_ERROR("AclrtMemcpy failed, ret: " << ret << " direct: " << direction << std::hex
                                                     << " src: " << srcAddr << " dst: " << destAddr << std::dec
                                                     << " size: " << count);
            return BM_DL_FUNCTION_FAILED;
        }
    }
    return BM_OK;
}

Result DataOpDeviceURMA::BatchDataCopyLocalAsync(hybm_batch_copy_params &params, int32_t direction,
                                                 const ExtOptions &options) noexcept
{
    void *st = options.stream;
    auto ret = 0;
    uint32_t batchNum = params.batchSize;
    if (st == nullptr) {
        st = HybmStreamManager::GetThreadAclStream();
    }

    for (size_t i = 0; i < batchNum; ++i) {
        auto destAddr = params.destinations[i];
        auto srcAddr = params.sources[i];
        auto count = params.dataSizes[i];
        ret = DlAclApi::AclrtMemcpyAsync(destAddr, count, srcAddr, count, direction, st);
        if (ret != 0) {
            (void)DlAclApi::AclrtSynchronizeStream(st);
            BM_LOG_ERROR("copy memory on local failed: " << ret << " stream:" << reinterpret_cast<uintptr_t>(st)
                                                         << " direct:" << direction << std::hex << " src:" << srcAddr
                                                         << " dst:" << destAddr);
            return BM_DL_FUNCTION_FAILED;
        }
    }
    ret = DlAclApi::AclrtSynchronizeStream(st);
    if (ret != 0) {
        BM_LOG_ERROR("aclrtSynchronizeStream failed: " << ret << " stream:" << reinterpret_cast<uintptr_t>(st));
    }
    return ret;
}

Result DataOpDeviceURMA::BatchDataCopyLocalBatch(hybm_batch_copy_params &params, int32_t direction,
                                                 const ExtOptions &options) noexcept
{
    uint32_t batchNum = params.batchSize;
    std::vector<aclrtMemcpyBatchAttr> attrs(batchNum);
    std::vector<size_t> attrsIds(batchNum);
    std::vector<size_t> sizes(batchNum);
    size_t idx = 0;
    auto deviceLoc = aclrtMemLocation{static_cast<uint32_t>(HybmGetInitDeviceId()),
                                      aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_DEVICE};
    auto hostLoc = aclrtMemLocation{0, aclrtMemLocationType::ACL_MEM_LOCATION_TYPE_HOST};
    for (size_t i = 0; i < batchNum; i++) {
        if (direction == ACL_MEMCPY_HOST_TO_DEVICE) {
            attrs[i] = aclrtMemcpyBatchAttr{deviceLoc, hostLoc, {}};
        } else {
            attrs[i] = aclrtMemcpyBatchAttr{hostLoc, deviceLoc, {}};
        }
        attrsIds[i] = idx++;
        sizes[i] = params.dataSizes[i];
    }
    size_t fail_idx = 0;
    TP_TRACE_BEGIN(TP_HYBM_URMA_BATCH_DATA_COPY_LOCAL_ASYNC);
    auto ret = BatchDataCopyLocalAsync(params, direction, options);
    TP_TRACE_END(TP_HYBM_URMA_BATCH_DATA_COPY_LOCAL_ASYNC, ret);

    if (ret != 0) {
        TP_TRACE_BEGIN(TP_HYBM_URMA_ACLRT_MEMCPY_BATCH);
        ret = DlAclApi::AclrtMemcpyBatch(params.destinations, sizes.data(), params.sources, sizes.data(), sizes.size(),
                                         attrs.data(), attrsIds.data(), attrs.size(), &fail_idx);
        TP_TRACE_END(TP_HYBM_URMA_ACLRT_MEMCPY_BATCH, ret);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyMultiRankLocal(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                            const ExtOptions &options,
                                            const std::vector<uint32_t> &localIndices) noexcept
{
    if (localIndices.empty()) {
        return BM_OK;
    }
    std::vector<void *> sources;
    std::vector<void *> destinations;
    std::vector<uint64_t> sizes;
    sources.reserve(localIndices.size());
    destinations.reserve(localIndices.size());
    sizes.reserve(localIndices.size());
    for (uint32_t index : localIndices) {
        if (index >= params.batchSize) {
            BM_LOG_ERROR("local batch index out of range, index: " << index << " batchSize: " << params.batchSize
                                                                   << " localRank: " << rankId_);
            return BM_INVALID_PARAM;
        }
        void *src = params.sources[index];
        void *dest = params.destinations[index];
        TransformVa(src, dest, direction);
        sources.push_back(src);
        destinations.push_back(dest);
        sizes.push_back(params.dataSizes[index]);
    }
    ExtOptions copyOptions{};
    copyOptions.srcRankId = rankId_;
    copyOptions.destRankId = rankId_;
    copyOptions.stream = options.stream;
    copyOptions.flags = options.flags;
    hybm_batch_copy_params localParams{sources.data(), destinations.data(), sizes.data(),
                                       static_cast<uint32_t>(sizes.size())};
    TP_TRACE_BEGIN(TP_HYBM_URMA_BATCH_LOCAL);
    auto ret = BatchDataCopyLocal(localParams, direction, copyOptions);
    TP_TRACE_END(TP_HYBM_URMA_BATCH_LOCAL, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("multi-rank local batch copy failed, ret: " << ret << " localRank: " << rankId_
                                                                 << " batchSize: " << localParams.batchSize
                                                                 << " direction: " << direction);
    }
    return ret;
}

Result DataOpDeviceURMA::CopyMultiRankUnregistered(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                                   const ExtOptions &options,
                                                   const transport::RankGroupMap &unregisteredGroups) noexcept
{
    for (const auto &[p2pInfo, indices] : unregisteredGroups) {
        ExtOptions copyOptions{};
        copyOptions.srcRankId = p2pInfo.first;
        copyOptions.destRankId = p2pInfo.second;
        copyOptions.stream = options.stream;
        copyOptions.flags = options.flags;
        for (uint32_t index : indices) {
            hybm_copy_params copyParam{params.sources[index], params.destinations[index], params.dataSizes[index]};
            auto ret = DataCopy(copyParam, direction, copyOptions);
            if (ret != BM_OK) {
                BM_LOG_ERROR("multi-rank fallback copy failed, ret: " << ret << " index: " << index
                                                                      << " srcRank: " << p2pInfo.first
                                                                      << " destRank: " << p2pInfo.second);
                return ret;
            }
        }
    }
    return BM_OK;
}

Result DataOpDeviceURMA::BatchDataCopyMultiRank(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                                const ExtOptions &options) noexcept
{
    if (params.batchSize == 0 || params.sources == nullptr || params.destinations == nullptr ||
        params.dataSizes == nullptr) {
        BM_LOG_ERROR("invalid multi-rank batch params, batchSize: " << params.batchSize);
        return BM_INVALID_PARAM;
    }
    //按照kernel一次可以下发的最大数切分
    const uint32_t sliceCount = (params.batchSize - 1U) / HCOMM_BATCH_TRANSFER_MAX_DESC_NUM + 1U;
    std::vector<transport::RankGroupMap> sliceGroupMaps;
    TP_TRACE_BEGIN(TP_HYBM_URMA_BUILD_SLICE_GROUP_MAPS);
    try {
        sliceGroupMaps.resize(sliceCount);
        for (const auto &[p2pInfo, indices] : options.groupMap) {
            if (p2pInfo.first != rankId_ && p2pInfo.second != rankId_) {
                BM_LOG_ERROR("multi-rank batch group excludes local rank, localRank: "
                             << rankId_ << " srcRank: " << p2pInfo.first << " destRank: " << p2pInfo.second);
                TP_TRACE_END(TP_HYBM_URMA_BUILD_SLICE_GROUP_MAPS, BM_INVALID_PARAM);
                return BM_INVALID_PARAM;
            }
            for (uint32_t index : indices) {
                if (index >= params.batchSize) {
                    BM_LOG_ERROR("multi-rank batch index out of range, index: "
                                 << index << " batchSize: " << params.batchSize << " srcRank: " << p2pInfo.first
                                 << " destRank: " << p2pInfo.second);
                    TP_TRACE_END(TP_HYBM_URMA_BUILD_SLICE_GROUP_MAPS, BM_INVALID_PARAM);
                    return BM_INVALID_PARAM;
                }
                const uint32_t sliceIndex = index / HCOMM_BATCH_TRANSFER_MAX_DESC_NUM;
                const uint32_t sliceOffset = sliceIndex * HCOMM_BATCH_TRANSFER_MAX_DESC_NUM;
                sliceGroupMaps[sliceIndex][p2pInfo].push_back(index - sliceOffset);
            }
        }
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("multi-rank batch build slices failed, batchSize: " << params.batchSize
                                                                         << " sliceCount: " << sliceCount);
        TP_TRACE_END(TP_HYBM_URMA_BUILD_SLICE_GROUP_MAPS, BM_MALLOC_FAILED);
        return BM_MALLOC_FAILED;
    }
    TP_TRACE_END(TP_HYBM_URMA_BUILD_SLICE_GROUP_MAPS, BM_OK);
    for (uint32_t sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        const uint32_t sliceOffset = sliceIndex * HCOMM_BATCH_TRANSFER_MAX_DESC_NUM;
        const uint32_t sliceSize = std::min(HCOMM_BATCH_TRANSFER_MAX_DESC_NUM, params.batchSize - sliceOffset);
        hybm_batch_copy_params sliceParams{params.sources + sliceOffset, params.destinations + sliceOffset,
                                           params.dataSizes + sliceOffset, sliceSize};
        const auto &sliceGroupMap = sliceGroupMaps[sliceIndex];

        std::vector<uint32_t> localIndices;
        transport::RankGroupMap unregisteredGroups;
        std::set<uint32_t> batchRanks;
        // 以一个slice来调用kernel
        auto submitRet = transportManager_->TransferRemoteBatchAsync(sliceParams, direction, sliceGroupMap,
                                                                     localIndices, unregisteredGroups, batchRanks);
        if (submitRet != BM_OK) {
            BM_LOG_ERROR("multi-rank batch slice transfer failed, offset: " << sliceOffset << " count: " << sliceSize
                                                                            << " groupNum: " << sliceGroupMap.size()
                                                                            << " ret: " << submitRet);
            return submitRet;
        }

        Result copyRet = BM_OK;
        //本地&未注册
        if (!localIndices.empty()) {
            copyRet = CopyMultiRankLocal(sliceParams, direction, options, localIndices);
        }
        if (copyRet == BM_OK && !unregisteredGroups.empty()) {
            copyRet = CopyMultiRankUnregistered(sliceParams, direction, options, unregisteredGroups);
        }
        //同步，batchRanks是当前slice的ranks
        const Result syncRet = transportManager_->SynchronizeRanks(batchRanks);
        if (syncRet != BM_OK) {
            BM_LOG_ERROR("multi-rank batch slice synchronize failed, offset: " << sliceOffset << " count: " << sliceSize
                                                                               << " rankNum: " << batchRanks.size()
                                                                               << " ret: " << syncRet);
        }
        if (copyRet != BM_OK) {
            BM_LOG_ERROR("multi-rank batch slice copy failed, offset: " << sliceOffset << " count: " << sliceSize
                                                                        << " ret: " << copyRet);
            return copyRet;
        }
        if (syncRet != BM_OK) {
            return syncRet;
        }
    }
    return BM_OK;
}

Result DataOpDeviceURMA::SafePut(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options,
                                 bool srcIsHost)
{
    Result ret = 0;
    uintptr_t srcBase = reinterpret_cast<uintptr_t>(srcVA);
    uintptr_t destBase = reinterpret_cast<uintptr_t>(destVA);
    uint64_t remainingLength = length;
    uint64_t offset = 0;
    if (transportManager_->QueryHasRegistered(srcBase, length)) {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy urma", ret);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(urmaSwapMemoryAllocator_ != nullptr,
                             "urmaSwapMemoryAllocator_ is not initialized, swap space size: " << urmaSwapSpaceSize_,
                             BM_ERROR);
    while (remainingLength > 0) {
        uint64_t currentChunkSize = std::min(remainingLength, urmaSwapSpaceSize_);
        auto tmpRdmaMemory = urmaSwapMemoryAllocator_->Allocate(currentChunkSize);
        auto tmpSwap = tmpRdmaMemory.Address();
        BM_ASSERT_LOG_AND_RETURN(tmpSwap != nullptr, "Failed to malloc temp buffer", BM_MALLOC_FAILED);
        const void *currentSrc = reinterpret_cast<const void *>(srcBase + offset);
        void *currentDest = reinterpret_cast<void *>(destBase + offset);
        if (srcIsHost) {
            ret = CopyLH2LD(currentSrc, tmpSwap, currentChunkSize, options);
        } else {
            ret = CopyLD2LD(currentSrc, tmpSwap, currentChunkSize, options);
        }
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy src to swap", ret);
        ret = CopyURMA(tmpSwap, currentDest, currentChunkSize, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy swap to dest", ret);
        offset += currentChunkSize;
        remainingLength -= currentChunkSize;
    }
    return 0;
}

Result DataOpDeviceURMA::SafeGet(const void *srcVA, void *destVA, uint64_t length, const ExtOptions &options,
                                 bool destIsHost)
{
    Result ret = 0;
    uintptr_t srcBase = reinterpret_cast<uintptr_t>(srcVA);
    uintptr_t destBase = reinterpret_cast<uintptr_t>(destVA);
    uint64_t remainingLength = length;
    uint64_t offset = 0;
    if (transportManager_->QueryHasRegistered(destBase, length)) {
        ret = CopyURMA(srcVA, destVA, length, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to copy urma", ret);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(urmaSwapMemoryAllocator_ != nullptr,
                             "urmaSwapMemoryAllocator_ is not initialized, swap space size: " << urmaSwapSpaceSize_,
                             BM_ERROR);
    while (remainingLength > 0) {
        uint64_t currentChunkSize = std::min(remainingLength, urmaSwapSpaceSize_);
        auto tmpRdmaMemory = urmaSwapMemoryAllocator_->Allocate(currentChunkSize);
        auto tmpSwap = tmpRdmaMemory.Address();
        BM_ASSERT_LOG_AND_RETURN(tmpSwap != nullptr, "[CopyGD2LH] Failed to malloc temp buffer", BM_MALLOC_FAILED);
        const void *currentSrc = reinterpret_cast<const void *>(srcBase + offset);
        void *currentDest = reinterpret_cast<void *>(destBase + offset);
        ret = CopyURMA(currentSrc, tmpSwap, currentChunkSize, options);
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "[CopyGD2LH] Failed to copy src to swap", ret);
        if (destIsHost) {
            ret = CopyLD2LH(tmpSwap, currentDest, currentChunkSize, options);
        } else {
            ret = CopyLD2LD(tmpSwap, currentDest, currentChunkSize, options);
        }
        BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "[CopyGD2LH] Failed to copy swap to dest", ret);
        offset += currentChunkSize;
        remainingLength -= currentChunkSize;
    }
    return 0;
}

Result DataOpDeviceURMA::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                       const ExtOptions &options) noexcept
{
    if (!IsSupportedUrmaDirection(direction)) {
        BM_LOG_ERROR("URMA batch data copy invalid direction: " << direction << " localRank: " << rankId_
                                                                << " batchSize: " << params.batchSize
                                                                << " groupNum: " << options.groupMap.size());
        return BM_INVALID_PARAM;
    }
    if (options.groupMap.empty()) {
        BM_LOG_ERROR("URMA batch data copy requires rank groups, localRank: " << rankId_ << " direction: " << direction
                                                                              << " batchSize: " << params.batchSize);
        return BM_INVALID_PARAM;
    }

    TP_TRACE_BEGIN(TP_HYBM_URMA_BATCH_MULTI_RANK);
    auto ret = BatchDataCopyMultiRank(params, direction, options);
    TP_TRACE_END(TP_HYBM_URMA_BATCH_MULTI_RANK, ret);
    return ret;
}
} // namespace mf
} // namespace ock
