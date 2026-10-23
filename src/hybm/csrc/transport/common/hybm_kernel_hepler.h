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

#ifndef MF_HYBM_KERNEL_HEPLER_H
#define MF_HYBM_KERNEL_HEPLER_H

#include "dl_acl_api.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

struct DeviceFuncHandles {
    aclrtFuncHandle batchRead{nullptr};
    aclrtFuncHandle batchWrite{nullptr};
    aclrtFuncHandle batchTransfer{nullptr};
};

// Load AICPU kernel JSON and resolve function handles.
// @param funcRead      kernel function name for read  (e.g. "HybmBatchRead")
// @param funcWrite     kernel function name for write (e.g. "HybmBatchWrite")
// @param binHandle     [in/out] cached bin handle; if nullptr, loads from JSON
// @param funcHandles   [out] resolved function handles
// JSON path: ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/hybm/config/libcann_hybm_kernel.json
Result LoadDeviceKernelAndGetHandles(const char *funcRead, const char *funcWrite, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles);

Result LoadDeviceKernelAndGetHandles(const char *funcTransfer, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles);

// Load AICPU kernel and validate the resolved handles are non-null.
// Shared by device_rdma_hcomm and device_urma transport managers.
// @return BM_OK on success; otherwise a result code with error log.
Result LoadDeviceKernelAndValidate(const char *funcRead, const char *funcWrite, aclrtBinHandle &binHandle,
                                   DeviceFuncHandles &funcHandles);

// Load a single named AICPU kernel into the given aclrtFuncHandle (native RDMA path).
// @param binHandle  [in/out] cached bin handle; if nullptr, loads from JSON
// @param funcHandle [out] resolved function handle
// @param funcName   kernel function name (e.g. "HybmRdmaAicpuPostSend")
Result LoadDeviceKernelAndGetHandles(aclrtBinHandle &binHandle, aclrtFuncHandle &funcHandle, const char *funcName);

// Common AICPU kernel launch sequence: ArgsInit/Append/Finalize + LaunchKernelWithConfig.
// Shared by the HCOMM/URMA batch path (device_kernel_helper.h) and the native RDMA path
// (rdma_aicpu_helper.h). Returns the first non-OK ret without logging; callers log with context.
Result LaunchAicpuKernel(aclrtFuncHandle funcHandle, void *stream, const void *args, size_t argsBytes);

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_KERNEL_HEPLER_H
