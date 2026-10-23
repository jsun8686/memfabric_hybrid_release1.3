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

#ifndef MF_HYBM_CORE_DL_ACL_API_H
#define MF_HYBM_CORE_DL_ACL_API_H

#include "hybm_common_include.h"

namespace ock {
namespace mf {

enum class aclrtMemLocationType {
    ACL_MEM_LOCATION_TYPE_HOST = 0, // Host内存
    ACL_MEM_LOCATION_TYPE_DEVICE,   // Device内存
};

using aclrtMemLocation = struct aclrtMemLocation;
struct aclrtMemLocation {
    uint32_t id;
    aclrtMemLocationType type; // 内存所在位置
};

using aclrtMemcpyBatchAttr = struct aclrtMemcpyBatchAttr;
struct aclrtMemcpyBatchAttr {
    aclrtMemLocation dstLoc;
    aclrtMemLocation srcLoc;
    uint8_t rsv[16];
};

typedef enum {
    ACL_STREAM_ATTR_FAILURE_MODE = 1,
    ACL_STREAM_ATTR_FLOAT_OVERFLOW_CHECK = 2,
    ACL_STREAM_ATTR_USER_CUSTOM_TAG = 3,
    ACL_STREAM_ATTR_CACHE_OP_INFO = 4,
} aclrtStreamAttr;

typedef union {
    uint64_t failureMode;
    uint32_t overflowSwitch;
    uint32_t userCustomTag;
    uint32_t cacheOpInfoSwitch;
    uint32_t reserve[4];
} aclrtStreamAttrValue;

using aclError = int32_t;
constexpr aclError ACL_SUCCESS = 0;

// CANN runtime 通用内部错误码：rtIpcOpenMemory 等接口返回。plog 证据表明该码在 IPC open 上
// 覆盖两类对端未就绪的瞬时拒绝：白名单未设置（drv: Find wlist fail / Wlist verify fail）
// 与节点不可见（drv: Query ipc node attr fail）；调用方可按可重试处理
constexpr aclError ACL_RT_INTERNAL_ERROR = 507899;

// 驱动 IPC 名字节点不存在：RtIpcDestroyMemoryName 对已销毁/对端先行退出的共享名返回，
// teardown 期属预期场景
constexpr aclError ACL_RT_IPC_NAME_NOT_FOUND = 107002;

// stream 被 aclrtStreamAbort 中止后，对该 stream 的同步接口（aclrtSynchronizeStream 等）返回该码。
constexpr aclError ACL_ERROR_RT_STREAM_ABORT = 107023;
using aclrtBinHandle = void *;
using aclrtFuncHandle = void *;
using aclrtArgsHandle = void *;
using aclrtParamHandle = void *;

enum class aclrtBinaryLoadOptionType : int32_t {
    ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD = 1,
    ACL_RT_BINARY_LOAD_OPT_LAZY_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_MAGIC = 2,
    ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE = 3,
};

union aclrtBinaryLoadOptionValue {
    uint32_t isLazyLoad;
    uint32_t magic;
    int32_t cpuKernelMode;
    uint32_t rsv[4];
};

struct aclrtBinaryLoadOption {
    aclrtBinaryLoadOptionType type;
    aclrtBinaryLoadOptionValue value;
};

struct aclrtBinaryLoadOptions {
    aclrtBinaryLoadOption *options;
    size_t numOpt;
};

typedef enum aclrtLaunchKernelAttrId {
    ACL_RT_LAUNCH_KERNEL_ATTR_SCHEM_MODE = 1, // 调度模式
    ACL_RT_LAUNCH_KERNEL_ATTR_DYN_UBUF_SIZE = 2, // 用于指定SIMT算子执行时需要的VECTOR CORE内部UB buffer的大小
    ACL_RT_LAUNCH_KERNEL_ATTR_ENGINE_TYPE = 3,     // 算子执行引擎
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCKDIM_OFFSET,     // numBlocks偏移量
    ACL_RT_LAUNCH_KERNEL_ATTR_BLOCK_TASK_PREFETCH, // 任务下发时，是否阻止硬件预取本任务的信息
    ACL_RT_LAUNCH_KERNEL_ATTR_DATA_DUMP,           // 是否开启Dump
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT,             // 任务调度器等待任务执行的超时时间，单位秒
    ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT_US = 8,      // 任务调度器等待任务执行的超时时间，单位微秒
} aclrtLaunchKernelAttrId;

struct aclrtTimeoutUs {
    uint32_t timeoutLow;
    uint32_t timeoutHigh;
};

union aclrtLaunchKernelAttrValue {
    uint8_t schemMode;
    uint32_t localMemorySize;
    uint32_t engineType;
    uint32_t numBlocksOffset;
    uint8_t isBlockTaskPrefetch;
    uint8_t isDataDump;
    uint16_t timeout;
    aclrtTimeoutUs timeoutUs;
    uint32_t rsv[4];
};

struct aclrtLaunchKernelAttr {
    aclrtLaunchKernelAttrId id;
    aclrtLaunchKernelAttrValue value;
};

struct aclrtLaunchKernelCfg {
    aclrtLaunchKernelAttr *attrs;
    size_t numAttrs;
};

typedef enum aclrtMemMallocPolicy {
    ACL_MEM_MALLOC_HUGE_FIRST,
    ACL_MEM_MALLOC_HUGE_ONLY,
    ACL_MEM_MALLOC_NORMAL_ONLY,
    ACL_MEM_MALLOC_HUGE_FIRST_P2P,
    ACL_MEM_MALLOC_HUGE_ONLY_P2P,
    ACL_MEM_MALLOC_NORMAL_ONLY_P2P,
    ACL_MEM_MALLOC_HUGE1G_ONLY,
    ACL_MEM_MALLOC_HUGE1G_ONLY_P2P,
    ACL_MEM_TYPE_LOW_BAND_WIDTH = 0x0100U,
    ACL_MEM_TYPE_HIGH_BAND_WIDTH = 0x1000U,
    ACL_MEM_ACCESS_USER_SPACE_READONLY = 0x100000U,
} aclrtMemMallocPolicy;

constexpr uint64_t ACL_NOTIFY_FLAG_DEVICE_ONLY = 0x00000001U;

using aclrtSetDeviceFunc = int32_t (*)(int32_t);
using aclrtGetDeviceFunc = int32_t (*)(int32_t *);
using aclrtDeviceEnablePeerAccessFunc = int32_t (*)(int32_t, uint32_t);
using aclrtCreateStreamFunc = int (*)(void **);
using aclrtCreateStreamWithConfigFunc = int (*)(void **, int32_t, uint32_t);
using aclrtStreamGetIdFunc = int (*)(void *, int32_t *);
using rtStreamGetSqidFunc = int32_t (*)(const void *, uint32_t *);
using rtStreamGetCqidFunc = int32_t (*)(const void *, uint32_t *, uint32_t *);
using aclrtCreateNotifyFunc = int (*)(void **, uint64_t);
using aclrtGetNotifyIdFunc = int (*)(void *, uint32_t *);
using aclrtDestroyNotifyFunc = int (*)(void *);
using aclrtWaitAndResetNotifyFunc = int (*)(void *, void *, uint32_t);
using aclrtGetCurrentContextFunc = int (*)(void **);
using aclrtSetStreamAttributeFunc = int (*)(void *, aclrtStreamAttr, aclrtStreamAttrValue *);
using aclrtDestroyStreamFunc = int (*)(void *);
using aclrtSynchronizeStreamFunc = int (*)(void *);
using aclrtStreamAbortFunc = int (*)(void *);
using aclrtMallocFunc = int32_t (*)(void **, size_t, uint32_t);
using aclrtFreeFunc = int (*)(void *);
using aclrtMallocHostFunc = int32_t (*)(void **, size_t);
using aclrtFreeHostFunc = int (*)(void *);
using aclrtMemcpyFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t);
using aclrtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtMemcpy2dFunc = int32_t (*)(void *, size_t, const void *, size_t, size_t, size_t, uint32_t);
using aclrtMemcpy2dAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, size_t, size_t, uint32_t, void *);
using aclrtMemsetFunc = int32_t (*)(void *, size_t, int32_t, size_t);
using rtDeviceGetBareTgidFunc = int32_t (*)(uint32_t *);
using rtGetDeviceInfoFunc = int32_t (*)(uint32_t, int32_t, int32_t, int64_t *val);
using rtIpcSetMemoryNameFunc = int32_t (*)(const void *, uint64_t, char *, uint32_t);
using rtSetIpcMemorySuperPodPidFunc = int32_t (*)(const char *, uint32_t, int32_t *, int32_t);
using rtIpcDestroyMemoryNameFunc = int32_t (*)(const char *);
using rtEnableP2PFunc = int32_t (*)(uint32_t, uint32_t, uint32_t);
using rtDisableP2PFunc = int32_t (*)(uint32_t, uint32_t);
using rtGetP2PStatusFunc = int32_t (*)(uint32_t, uint32_t, uint32_t *);
using rtGetLogicDevIdByUserDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using aclrtGetPhyDevIdByLogicDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using rtIpcOpenMemoryFunc = int32_t (*)(void **, const char *);
using rtIpcCloseMemoryFunc = int32_t (*)(const void *);
using rtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtGetSocNameFunc = const char *(*)();
using aclrtGetVersionFunc = int32_t (*)(int32_t *, int32_t *, int32_t *);
using aclsysGetVersionStrFunc = int32_t (*)(char *, char *); // (pkgName, versionStr)
using aclrtMemcpyBatchFunc = int32_t (*)(void **, size_t *, void **, size_t *, size_t, aclrtMemcpyBatchAttr *, size_t *,
                                         size_t, size_t *);
using aclrtBinaryLoadFromFileFunc = int32_t (*)(const char *, aclrtBinaryLoadOptions *, aclrtBinHandle *);
using aclrtBinaryGetFunctionFunc = int32_t (*)(const aclrtBinHandle, const char *, aclrtFuncHandle *);
using aclrtKernelArgsInitFunc = int32_t (*)(aclrtFuncHandle, aclrtArgsHandle *);
using aclrtKernelArgsAppendFunc = int32_t (*)(aclrtArgsHandle, void *, size_t, aclrtParamHandle *);
using aclrtKernelArgsFinalizeFunc = int32_t (*)(aclrtArgsHandle);
using aclrtLaunchKernelWithConfigFunc = int32_t (*)(aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *,
                                                    aclrtArgsHandle, void *);

class DlAclApi {
public:
    static Result LoadLibrary(const std::string &libDirPath);
    static void CleanupLibrary();
    static AscendSocType GetAscendSocType();
    static int32_t AclrtGetVersion(int32_t *major, int32_t *minor, int32_t *patch);
    static bool AclsysGetVersionStr(const std::string &pkgName, std::string &versionStr);
    static bool IsCannGE(int32_t major, int32_t minor);

    static inline Result AclrtSetDevice(int32_t deviceId, bool force = false)
    {
        if (pAclrtSetDevice == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        if (force) {
            return pAclrtSetDevice(deviceId);
        }
        int32_t nowDeviceId = -1;
        if (AclrtGetDevice(&nowDeviceId) == 0 && nowDeviceId == deviceId) {
            return BM_OK;
        } else {
            return pAclrtSetDevice(deviceId);
        }
    }

    static inline Result AclrtGetDevice(int32_t *deviceId)
    {
        if (pAclrtGetDevice == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetDevice(deviceId);
    }

    static inline Result AclrtDeviceEnablePeerAccess(int32_t peerDeviceId, uint32_t flags)
    {
        if (pAclrtDeviceEnablePeerAccess == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDeviceEnablePeerAccess(peerDeviceId, flags);
    }

    static inline Result AclrtCreateStream(void **stream)
    {
        if (pAclrtCreateStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateStream(stream);
    }

    static inline Result AclrtCreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config)
    {
        if (pAclrtCreateStreamWithConfig == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateStreamWithConfig(stream, prot, config);
    }

    static inline Result AclrtStreamGetId(void *stream, int32_t *streamId)
    {
        if (pAclrtStreamGetId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtStreamGetId(stream, streamId);
    }

    static inline Result RtStreamGetSqid(void *stream, uint32_t *sqId)
    {
        if (pRtStreamGetSqid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtStreamGetSqid(stream, sqId);
    }

    static inline Result RtStreamGetCqid(void *stream, uint32_t *cqId, uint32_t *logicCqId)
    {
        if (pRtStreamGetCqid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtStreamGetCqid(stream, cqId, logicCqId);
    }

    static inline Result AclrtCreateNotify(void **notify, uint64_t flag)
    {
        if (pAclrtCreateNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtCreateNotify(notify, flag);
    }

    static inline Result AclrtGetNotifyId(void *notify, uint32_t *notifyId)
    {
        if (pAclrtGetNotifyId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetNotifyId(notify, notifyId);
    }

    static inline Result AclrtDestroyNotify(void *notify)
    {
        if (pAclrtDestroyNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDestroyNotify(notify);
    }

    static inline Result AclrtWaitAndResetNotify(void *notify, void *stream, uint32_t timeout)
    {
        if (pAclrtWaitAndResetNotify == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtWaitAndResetNotify(notify, stream, timeout);
    }

    static inline Result AclrtGetCurrentContext(void **context)
    {
        if (pAclrtGetCurrentContext == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetCurrentContext(context);
    }

    static inline Result AclrtSetStreamAttribute(void *stream, aclrtStreamAttr stmAttrType, aclrtStreamAttrValue *value)
    {
        if (pAclrtSetStreamAttribute == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtSetStreamAttribute(stream, stmAttrType, value);
    }

    static inline Result AclrtDestroyStream(void *stream)
    {
        if (pAclrtDestroyStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtDestroyStream(stream);
    }

    static inline Result AclrtSynchronizeStream(void *stream)
    {
        if (pAclrtSynchronizeStream == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtSynchronizeStream(stream);
    }

    // 停止 stream 上正在执行/已下发未执行的任务；平台或 CANN 版本不支持时返回 BM_UNDER_API_UNLOAD。
    // 调用后，对该 stream 的同步接口会返回 ACL_ERROR_RT_STREAM_ABORT(107023)。
    static inline Result AclrtStreamAbort(void *stream)
    {
        if (pAclrtStreamAbort == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtStreamAbort(stream);
    }

    static inline Result AclrtBinaryLoadFromFile(const char *binPath, aclrtBinaryLoadOptions *options,
                                                 aclrtBinHandle *binHandle)
    {
        if (pAclrtBinaryLoadFromFile == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtBinaryLoadFromFile(binPath, options, binHandle);
    }

    static inline Result AclrtBinaryGetFunction(aclrtBinHandle binHandle, const char *kernelName,
                                                aclrtFuncHandle *funcHandle)
    {
        if (pAclrtBinaryGetFunction == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtBinaryGetFunction(binHandle, kernelName, funcHandle);
    }

    static inline Result AclrtKernelArgsInit(aclrtFuncHandle funcHandle, aclrtArgsHandle *argsHandle)
    {
        if (pAclrtKernelArgsInit == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsInit(funcHandle, argsHandle);
    }

    static inline Result AclrtKernelArgsAppend(aclrtArgsHandle argsHandle, void *param, size_t paramSize,
                                               aclrtParamHandle *paramHandle)
    {
        if (pAclrtKernelArgsAppend == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsAppend(argsHandle, param, paramSize, paramHandle);
    }

    static inline Result AclrtKernelArgsFinalize(aclrtArgsHandle argsHandle)
    {
        if (pAclrtKernelArgsFinalize == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtKernelArgsFinalize(argsHandle);
    }

    static inline Result AclrtLaunchKernelWithConfig(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                                     aclrtLaunchKernelCfg *cfg, aclrtArgsHandle argsHandle,
                                                     void *reserved)
    {
        if (pAclrtLaunchKernelWithConfig == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtLaunchKernelWithConfig(funcHandle, blockDim, stream, cfg, argsHandle, reserved);
    }

    static inline Result AclrtMalloc(void **ptr, size_t count, uint32_t type)
    {
        if (pAclrtMalloc == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMalloc(ptr, count, type);
    }

    static inline Result AclrtFree(void *ptr)
    {
        if (pAclrtFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtFree(ptr);
    }

    static inline Result AclrtMallocHost(void **ptr, size_t count)
    {
        if (pAclrtMallocHost == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMallocHost(ptr, count);
    }

    static inline Result AclrtFreeHost(void *ptr)
    {
        if (pAclrtFreeHost == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtFreeHost(ptr);
    }

    static inline Result AclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
    {
        if (pAclrtMemcpy == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy(dst, destMax, src, count, kind);
    }

    static inline Result AclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                          void *stream)
    {
        if (pAclrtMemcpyAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpyAsync(dst, destMax, src, count, kind, stream);
    }

    static inline Result AclrtMemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches,
                                          aclrtMemcpyBatchAttr *attrs, size_t *attrsIndexes, size_t numAttrs,
                                          size_t *failIndex)
    {
        if (pAclrtMemcpyBatch == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpyBatch(dsts, destMax, srcs, sizes, numBatches, attrs, attrsIndexes, numAttrs, failIndex);
    }

    static inline Result AclrtMemcpy2d(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width,
                                       size_t height, uint32_t kind)
    {
        if (pAclrtMemcpy2d == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy2d(dst, dpitch, src, spitch, width, height, kind);
    }

    static inline Result AclrtMemcpy2dAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width,
                                            size_t height, uint32_t kind, void *stream)
    {
        if (pAclrtMemcpy2dAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemcpy2dAsync(dst, dpitch, src, spitch, width, height, kind, stream);
    }

    static inline Result AclrtMemset(void *dst, size_t destMax, int32_t value, size_t count)
    {
        if (pAclrtMemset == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtMemset(dst, destMax, value, count);
    }

    static inline Result RtDeviceGetBareTgid(uint32_t *pid)
    {
        if (pRtDeviceGetBareTgid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtDeviceGetBareTgid(pid);
    }

    static inline Result RtGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val)
    {
        if (pRtGetDeviceInfo == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetDeviceInfo(deviceId, moduleType, infoType, val);
    }

    static inline Result RtSetIpcMemorySuperPodPid(const char *name, uint32_t sdid, int32_t pid[], int32_t num)
    {
        if (pRtSetIpcMemorySuperPodPid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtSetIpcMemorySuperPodPid(name, sdid, pid, num);
    }

    static inline Result RtIpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
    {
        if (pRtIpcSetMemoryName == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcSetMemoryName(ptr, byteCount, name, len);
    }

    static inline Result RtIpcDestroyMemoryName(const char *name)
    {
        if (pRtIpcDestroyMemoryName == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcDestroyMemoryName(name);
    }

    static inline Result RtIpcOpenMemory(void **ptr, const char *name)
    {
        if (pRtIpcOpenMemory == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcOpenMemory(ptr, name);
    }

    static inline Result RtIpcCloseMemory(const void *ptr)
    {
        if (pRtIpcCloseMemory == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtIpcCloseMemory(ptr);
    }

    static inline const char *AclrtGetSocName()
    {
#ifdef NO_XPU
        return nullptr;
#endif
        if (pAclrtGetSocName == nullptr) {
            BM_LOG_ERROR("pAclrtGetSocName is nullptr, bm under api unload.");
            return nullptr;
        }
        return pAclrtGetSocName();
    }

    static inline Result RtEnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag)
    {
        if (pRtEnableP2P == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtEnableP2P(devIdDes, phyIdSrc, flag);
    }

    static inline Result RtDisableP2P(uint32_t devIdDes, uint32_t phyIdSrc)
    {
        if (pRtDisableP2P == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtDisableP2P(devIdDes, phyIdSrc);
    }

    static inline Result RtGetP2PStatus(uint32_t devId, uint32_t peerDevId, uint32_t &status) noexcept
    {
        if (pRtGetP2PStatus == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetP2PStatus(devId, peerDevId, &status);
    }

    static inline Result RtGetLogicDevIdByUserDevId(const int32_t userDevId, int32_t *const logicDevId)
    {
        if (pRtGetLogicDevIdByUserDevId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetLogicDevIdByUserDevId(userDevId, logicDevId);
    }

    static inline Result AclrtGetPhyDevIdByLogicDevId(const int32_t logicDevId, int32_t *const phyDevId)
    {
        if (pAclrtGetPhyDevIdByLogicDevId == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pAclrtGetPhyDevIdByLogicDevId(logicDevId, phyDevId);
    }

    static inline Result RtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                       void *stream)
    {
        if (pRtMemcpyAsync == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtMemcpyAsync(dst, destMax, src, count, kind, stream);
    }

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *rtHandle;
    static const char *gAscendAclLibName;

    static aclrtSetDeviceFunc pAclrtSetDevice;
    static aclrtGetDeviceFunc pAclrtGetDevice;
    static aclrtDeviceEnablePeerAccessFunc pAclrtDeviceEnablePeerAccess;
    static aclrtCreateStreamFunc pAclrtCreateStream;
    static aclrtCreateStreamWithConfigFunc pAclrtCreateStreamWithConfig;
    static aclrtStreamGetIdFunc pAclrtStreamGetId;
    static rtStreamGetSqidFunc pRtStreamGetSqid;
    static rtStreamGetCqidFunc pRtStreamGetCqid;
    static aclrtCreateNotifyFunc pAclrtCreateNotify;
    static aclrtGetNotifyIdFunc pAclrtGetNotifyId;
    static aclrtDestroyNotifyFunc pAclrtDestroyNotify;
    static aclrtWaitAndResetNotifyFunc pAclrtWaitAndResetNotify;
    static aclrtGetCurrentContextFunc pAclrtGetCurrentContext;
    static aclrtSetStreamAttributeFunc pAclrtSetStreamAttribute;
    static aclrtDestroyStreamFunc pAclrtDestroyStream;
    static aclrtSynchronizeStreamFunc pAclrtSynchronizeStream;
    static aclrtStreamAbortFunc pAclrtStreamAbort;
    static aclrtBinaryLoadFromFileFunc pAclrtBinaryLoadFromFile;
    static aclrtBinaryGetFunctionFunc pAclrtBinaryGetFunction;
    static aclrtKernelArgsInitFunc pAclrtKernelArgsInit;
    static aclrtKernelArgsAppendFunc pAclrtKernelArgsAppend;
    static aclrtKernelArgsFinalizeFunc pAclrtKernelArgsFinalize;
    static aclrtLaunchKernelWithConfigFunc pAclrtLaunchKernelWithConfig;
    static aclrtMallocFunc pAclrtMalloc;
    static aclrtFreeFunc pAclrtFree;
    static aclrtMallocHostFunc pAclrtMallocHost;
    static aclrtFreeHostFunc pAclrtFreeHost;
    static aclrtMemcpyFunc pAclrtMemcpy;
    static aclrtMemcpyBatchFunc pAclrtMemcpyBatch;
    static aclrtMemcpyAsyncFunc pAclrtMemcpyAsync;
    static aclrtMemcpy2dFunc pAclrtMemcpy2d;
    static aclrtMemcpy2dAsyncFunc pAclrtMemcpy2dAsync;
    static aclrtMemsetFunc pAclrtMemset;
    static rtDeviceGetBareTgidFunc pRtDeviceGetBareTgid;
    static rtGetDeviceInfoFunc pRtGetDeviceInfo;
    static rtSetIpcMemorySuperPodPidFunc pRtSetIpcMemorySuperPodPid;
    static rtIpcSetMemoryNameFunc pRtIpcSetMemoryName;
    static rtIpcDestroyMemoryNameFunc pRtIpcDestroyMemoryName;
    static rtIpcOpenMemoryFunc pRtIpcOpenMemory;
    static rtIpcCloseMemoryFunc pRtIpcCloseMemory;
    static aclrtGetSocNameFunc pAclrtGetSocName;
    static aclrtGetVersionFunc pAclrtGetVersion;
    static aclsysGetVersionStrFunc pAclsysGetVersionStr;
    static rtEnableP2PFunc pRtEnableP2P;
    static rtDisableP2PFunc pRtDisableP2P;
    static rtGetP2PStatusFunc pRtGetP2PStatus;
    static rtMemcpyAsyncFunc pRtMemcpyAsync;
    static rtGetLogicDevIdByUserDevIdFunc pRtGetLogicDevIdByUserDevId;
    static aclrtGetPhyDevIdByLogicDevIdFunc pAclrtGetPhyDevIdByLogicDevId;
};
} // namespace mf
} // namespace ock

#endif // MF_HYBM_CORE_DL_ACL_API_H
