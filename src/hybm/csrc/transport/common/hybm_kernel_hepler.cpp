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

#include "hybm_kernel_hepler.h"

#include <climits>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
namespace {
constexpr int32_t kCpuKernelMode = 0;
constexpr const char *kKernelJsonSuffix = "/opp/vendors/cust/op_impl/aicpu/hybm/config/libcann_hybm_kernel.json";
constexpr const char *kDefaultAscendPath = "/usr/local/Ascend/cann";

Result GetKernelFilePath(std::string &jsonPath)
{
    const char *ascendPath = std::getenv("ASCEND_HOME_PATH");
    if (ascendPath == nullptr || ascendPath[0] == '\0') {
        ascendPath = kDefaultAscendPath;
    }
    jsonPath = std::string(ascendPath) + kKernelJsonSuffix;
    BM_LOG_INFO("device_urma kernel json path: " << jsonPath);
    return BM_OK;
}

Result LoadBinaryFromJson(const char *jsonPath, aclrtBinHandle &binHandle)
{
    if (jsonPath == nullptr) {
        BM_LOG_ERROR("device_urma jsonPath is null in LoadBinaryFromJson");
        return BM_INVALID_PARAM;
    }

    char resolvedPath[PATH_MAX] = {};
    if (realpath(jsonPath, resolvedPath) == nullptr) {
        BM_LOG_ERROR("realpath failed for kernel json: " << jsonPath);
        return BM_FILE_NOT_ACCESS;
    }
    if (access(resolvedPath, R_OK) != 0) {
        BM_LOG_ERROR("kernel json is not readable: " << resolvedPath);
        return BM_FILE_NOT_ACCESS;
    }

    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = kCpuKernelMode;
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;

    const auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolvedPath, &loadOptions, &binHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aclrtBinaryLoadFromFile failed, path: " << resolvedPath << " ret: " << ret);
        return ret;
    }
    return BM_OK;
}

Result GetFuncHandle(aclrtBinHandle binHandle, const char *funcName, aclrtFuncHandle &funcHandle)
{
    if (binHandle == nullptr || funcName == nullptr) {
        BM_LOG_ERROR("device_urma binHandle or funcName is null in GetFuncHandle");
        return BM_INVALID_PARAM;
    }
    const auto ret = DlAclApi::AclrtBinaryGetFunction(binHandle, funcName, &funcHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aclrtBinaryGetFunction failed, func: " << funcName << " ret: " << ret);
        return ret;
    }
    if (funcHandle == nullptr) {
        BM_LOG_ERROR("device_urma funcHandle is null after aclrtBinaryGetFunction");
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}
} // namespace

Result LoadDeviceKernelAndGetHandles(const char *funcRead, const char *funcWrite, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles)
{
    funcHandles = DeviceFuncHandles{};
    std::string jsonPath;
    auto ret = GetKernelFilePath(jsonPath);
    if (ret != BM_OK) {
        return ret;
    }
    if (binHandle == nullptr) {
        ret = LoadBinaryFromJson(jsonPath.c_str(), binHandle);
        if (ret != BM_OK) {
            return ret;
        }
    }
    ret = GetFuncHandle(binHandle, funcRead, funcHandles.batchRead);
    if (ret != BM_OK) {
        return ret;
    }
    return GetFuncHandle(binHandle, funcWrite, funcHandles.batchWrite);
}

Result LoadDeviceKernelAndGetHandles(const char *funcTransfer, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles)
{
    funcHandles = DeviceFuncHandles{};
    std::string jsonPath;
    auto ret = GetKernelFilePath(jsonPath);
    if (ret != BM_OK) {
        return ret;
    }
    if (binHandle == nullptr) {
        ret = LoadBinaryFromJson(jsonPath.c_str(), binHandle);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return GetFuncHandle(binHandle, funcTransfer, funcHandles.batchTransfer);
}

Result LoadDeviceKernelAndValidate(const char *funcRead, const char *funcWrite, aclrtBinHandle &binHandle,
                                   DeviceFuncHandles &funcHandles)
{
    auto ret = LoadDeviceKernelAndGetHandles(funcRead, funcWrite, binHandle, funcHandles);
    if (ret != BM_OK) {
        BM_LOG_ERROR("LoadDeviceKernelAndGetHandles failed, ret=" << ret);
        return ret;
    }
    if (funcHandles.batchRead == nullptr || funcHandles.batchWrite == nullptr) {
        BM_LOG_ERROR("invalid device kernel function handles after loading, read: "
                     << funcHandles.batchRead << " write: " << funcHandles.batchWrite);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("AICPU kernel loaded, write=" << funcHandles.batchWrite << " read=" << funcHandles.batchRead);
    return BM_OK;
}

Result LoadDeviceKernelAndGetHandles(aclrtBinHandle &binHandle, aclrtFuncHandle &funcHandle, const char *funcName)
{
    if (funcName == nullptr) {
        BM_LOG_ERROR("AICPU kernel funcName is null");
        return BM_INVALID_PARAM;
    }
    std::string jsonPath;
    auto ret = GetKernelFilePath(jsonPath);
    if (ret != BM_OK) {
        return ret;
    }
    if (binHandle == nullptr) {
        ret = LoadBinaryFromJson(jsonPath.c_str(), binHandle);
        if (ret != BM_OK) {
            return ret;
        }
    }
    ret = GetFuncHandle(binHandle, funcName, funcHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("load AICPU kernel failed, func: " << funcName << " ret: " << ret);
        return ret;
    }
    BM_LOG_INFO("AICPU kernel loaded, func: " << funcName << " handle: " << funcHandle);
    return BM_OK;
}

Result LaunchAicpuKernel(aclrtFuncHandle funcHandle, void *stream, const void *args, size_t argsBytes)
{
    if (funcHandle == nullptr || stream == nullptr) {
        return BM_INVALID_PARAM;
    }
    aclrtArgsHandle argsHandle = nullptr;
    auto ret = DlAclApi::AclrtKernelArgsInit(funcHandle, &argsHandle);
    if (ret != BM_OK) {
        return ret;
    }
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, const_cast<void *>(args), argsBytes, &paramHandle);
    if (ret != BM_OK) {
        return ret;
    }
    ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle);
    if (ret != BM_OK) {
        return ret;
    }
    constexpr uint16_t aicpuKernelTimeoutSec = static_cast<uint16_t>(27U * 68U);
    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = aicpuKernelTimeoutSec;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    return DlAclApi::AclrtLaunchKernelWithConfig(funcHandle, 1U, stream, &cfg, argsHandle, nullptr);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
