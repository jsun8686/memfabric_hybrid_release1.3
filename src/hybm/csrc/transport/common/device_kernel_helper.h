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

#ifndef MF_HYBM_DEVICE_KERNEL_HELPER_H
#define MF_HYBM_DEVICE_KERNEL_HELPER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "hybm_logger.h"
#include "dl_acl_api.h"
#include "hybm_kernel_hepler.h"
#include "hybm_transport_common.h"
#include "hybm_batch_transfer.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

// ─────────────────────────────────────────────────────────────────
// DeviceTransferBuffers — AICPU kernel batch buffer tracking
// Allocated via AclrtMalloc, freed after Synchronize.
// ─────────────────────────────────────────────────────────────────
struct DeviceTransferBuffers {
    void *dstList{nullptr};
    void *srcList{nullptr};
    void *lenList{nullptr};
};

inline void ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    if (buffers.dstList != nullptr) {
        (void)DlAclApi::AclrtFree(buffers.dstList);
    }
    buffers.dstList = nullptr;
    buffers.srcList = nullptr;
    buffers.lenList = nullptr;
}

// ─────────────────────────────────────────────────────────────────
// KernelLaunchConfig — parameters for LaunchDeviceKernel
// ─────────────────────────────────────────────────────────────────
struct KernelLaunchConfig {
    aclrtFuncHandle funcHandle{nullptr};
    ThreadHandle thread{0};
    ChannelHandle channel{0};
    void *stream{nullptr};
    bool isRead{false};
    uint32_t batchSize{0};
    const uint64_t *localAddrs{nullptr};
    const uint64_t *remoteAddrs{nullptr};
    const uint64_t *sizes{nullptr};
    uint64_t remoteFlagAddr{0};
    uint64_t localFlagAddr{0};
    uint32_t flagSize{0};
};

/**
 * @brief Allocate device buffer and copy host data for kernel launch.
 */
inline Result PrepareLaunchBuffer(const KernelLaunchConfig &config, void *&dstListDev)
{
    const auto ptrBytes = config.batchSize * sizeof(void *);
    const auto totalBytes = ptrBytes * 2UL + config.batchSize * sizeof(uint64_t);
    std::vector<uint8_t> hostBuf(totalBytes);
    auto ret = DlAclApi::AclrtMalloc(&dstListDev, totalBytes, 0);
    if (ret != BM_OK || dstListDev == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel AclrtMalloc failed, size=" << totalBytes << " ret=" << ret);
        return ret;
    }
    auto *dstBase = reinterpret_cast<void **>(hostBuf.data());
    auto *srcBase = reinterpret_cast<void **>(hostBuf.data() + ptrBytes);
    auto *lenBase = reinterpret_cast<uint64_t *>(hostBuf.data() + ptrBytes * 2UL);
    for (uint32_t i = 0; i < config.batchSize; ++i) {
        dstBase[i] = reinterpret_cast<void *>(config.isRead ? config.localAddrs[i] : config.remoteAddrs[i]);
        srcBase[i] = reinterpret_cast<void *>(config.isRead ? config.remoteAddrs[i] : config.localAddrs[i]);
        lenBase[i] = config.sizes[i];
    }
    ret = DlAclApi::AclrtMemcpy(dstListDev, totalBytes, hostBuf.data(), totalBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchDeviceKernel AclrtMemcpy H2D failed, ret=" << ret);
        (void)DlAclApi::AclrtFree(dstListDev);
    }
    return ret;
}

/**
 * @brief Set up kernel args (HybmOneSideOpParam) and launch AICPU kernel.
 */
inline Result LaunchDeviceKernel(const KernelLaunchConfig &config, DeviceTransferBuffers &outBuffers)
{
    if (config.batchSize == 0) {
        return BM_OK;
    }
    if (config.localAddrs == nullptr || config.remoteAddrs == nullptr || config.sizes == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: null address arrays");
        return BM_INVALID_PARAM;
    }
    if (config.funcHandle == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: funcHandle not loaded");
        return BM_DL_FUNCTION_FAILED;
    }
    if (config.stream == nullptr) {
        BM_LOG_ERROR("LaunchDeviceKernel: null stream");
        return BM_INVALID_PARAM;
    }

    void *dstListDev = nullptr;
    auto ret = PrepareLaunchBuffer(config, dstListDev);
    if (ret != BM_OK) {
        return ret;
    }
    const auto ptrBytes = config.batchSize * sizeof(void *);

    HybmOneSideOpParam args{};
    args.thread = config.thread;
    args.channel = config.channel;
    args.list_num = config.batchSize;
    args.dst_buf_addr_list = reinterpret_cast<void **>(dstListDev);
    args.src_buf_addr_list = reinterpret_cast<void **>(static_cast<uint8_t *>(dstListDev) + ptrBytes);
    args.len_list = reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(dstListDev) + ptrBytes * 2UL);
    args.remote_flag_addr = config.remoteFlagAddr;
    args.local_flag_addr = config.localFlagAddr;
    args.flag_size = config.flagSize;

    ret = LaunchAicpuKernel(config.funcHandle, config.stream, &args, sizeof(args));
    if (ret != BM_OK) {
        BM_LOG_ERROR("LaunchDeviceKernel launch failed, kernel=" << (config.isRead ? "HybmBatchRead" : "HybmBatchWrite")
                                                                 << " ret=" << ret);
        (void)DlAclApi::AclrtFree(dstListDev);
        return ret;
    }

    outBuffers.dstList = dstListDev;
    outBuffers.srcList = static_cast<uint8_t *>(dstListDev) + ptrBytes;
    outBuffers.lenList = static_cast<uint8_t *>(dstListDev) + ptrBytes * 2UL;
    BM_LOG_INFO("LaunchDeviceKernel success, isRead=" << config.isRead << " batchSize=" << config.batchSize);
    return BM_OK;
}

/**
 * @brief Query device info via RtGetDeviceInfo into output, with error log.
 */
inline Result QueryDeviceInfo(int32_t userId, int32_t infoType, uint32_t &output, const char *typeName)
{
    int64_t infoValue = 0;
    auto ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, infoType, &infoValue);
    if (ret != 0) {
        BM_LOG_ERROR("RtGetDeviceInfo(" << typeName << ") return=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    output = static_cast<uint32_t>(infoValue);
    return BM_OK;
}

/**
 * @brief 获取本地设备信息，包括 phyDeviceId, sdid, serverId, superPodId。
 */
inline Result InitLocalDeviceInfo(uint32_t &phyDeviceId, uint32_t &sdid, uint32_t &serverId, uint32_t &superPodId)
{
    int32_t userId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    if (ret != 0 || userId < 0) {
        BM_LOG_ERROR("AclrtGetDevice() return=" << ret << ", output deviceId=" << userId);
        return BM_DL_FUNCTION_FAILED;
    }
    int32_t phyId = 0;
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    if (ret != 0) {
        BM_LOG_ERROR("AclrtGetPhyDevIdByLogicDevId() return=" << ret << ", userDeviceId=" << userId);
        return BM_DL_FUNCTION_FAILED;
    }
    phyDeviceId = static_cast<uint32_t>(phyId);
    ret = QueryDeviceInfo(userId, INFO_TYPE_SDID, sdid, "INFO_TYPE_SDID");
    if (ret != BM_OK) {
        return ret;
    }
    ret = QueryDeviceInfo(userId, INFO_TYPE_SERVER_ID, serverId, "INFO_TYPE_SERVER_ID");
    if (ret != BM_OK) {
        return ret;
    }
    ret = QueryDeviceInfo(userId, INFO_TYPE_SUPER_POD_ID, superPodId, "INFO_TYPE_SUPER_POD_ID");
    if (ret != BM_OK) {
        return ret;
    }
    BM_LOG_INFO("local device info: phyId=" << phyDeviceId << " sdid=" << sdid << " serverId=" << serverId
                                            << " superPodId=" << superPodId);
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_DEVICE_KERNEL_HELPER_H
