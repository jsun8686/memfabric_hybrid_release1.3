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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hybm_rdma_aicpu.h"

#include <cstdint>
#include <dlfcn.h>

#include "hybm_def.h"
#include "hybm_kernel_log.h"

using ock::mf::aicpu::HYBM_DRV_NORMAL_TYPE;
using ock::mf::aicpu::HYBM_DRV_SQCQ_PROP_SQ_BASE;
using ock::mf::aicpu::HYBM_DRV_SQCQ_PROP_SQ_DEPTH;
using ock::mf::aicpu::HYBM_DRV_SQCQ_PROP_SQ_TAIL;
using ock::mf::aicpu::HYBM_RDMA_AICPU_ABI_VERSION;
using ock::mf::aicpu::HYBM_RDMA_OP_READ;
using ock::mf::aicpu::HYBM_RT_STARS_DEFAULT_KERNEL_CREDIT;
using ock::mf::aicpu::HYBM_RT_STARS_SQE_INT_DIR_NO;
using ock::mf::aicpu::HYBM_RT_STARS_SQE_SIZE;
using ock::mf::aicpu::HYBM_RT_STARS_SQE_TYPE_WRITE_VALUE;
using ock::mf::aicpu::HYBM_RT_STARS_WRITE_VALUE_SIZE_TYPE_64BIT;
using ock::mf::aicpu::HYBM_RT_STARS_WRITE_VALUE_SUB_TYPE_RDMA_DB_SEND;
using ock::mf::aicpu::HybmHalSqCqConfigInfo;
using ock::mf::aicpu::HybmHalSqCqQueryInfo;
using ock::mf::aicpu::HybmStarsWriteValueSqe;
using ock::mf::aicpu::RdmaAicpuOut;
using ock::mf::aicpu::RdmaAicpuParam;
using ock::mf::aicpu::RdmaSegment;

// ============================================================================
// AICPU-side native RoCE data path (A2/A3, 910B / 910_93).
//
//   host  : 传入专用 doorbell ACL 流的 sqId/streamId 与 doorbell 寄存器地址
//   device: ibv_exp_post_send() 提交 WQE 并返回 doorbell 值
//   device: halSqCqQuery 现查 SQ 基址/深度/tail（必须用 device-local devId，
//           即先 drvGetLocalDevIDByHostDevID 把 host devId 换算成本地 id）
//   device: 把 WRITE_VALUE / RDMA_DB_SEND SQE 写进该 SQ，并用
//           halSqCqConfig(DRV_SQCQ_PROP_SQ_TAIL) 推进 SQ tail（算子侧敲铃）
//   host  : 只等待硬件 notify（cq_cstm=0：CQ 归 hccp，调用方无法轮询）
// ============================================================================

namespace {

constexpr int IBV_WR_RDMA_WRITE = 0;
constexpr int IBV_WR_RDMA_READ = 4;
constexpr uint32_t IBV_SEND_FENCE = 1U << 0U;
constexpr uint32_t IBV_SEND_SIGNALED = 1U << 1U;

struct ibv_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct ibv_send_wr {
    uint64_t wrId;
    struct ibv_send_wr *next;
    struct ibv_sge *sgList;
    int numSge;
    int opcode;
    int sendFlags;
    uint32_t immData;
    uint64_t rdmaRemoteAddr;
    uint32_t rdmaRkey;
    uint8_t reserve[256 - 52];
};

struct ibv_qp;

struct WrExpRsp {
    uint32_t wqeIndex;
    uint32_t reserve;
    uint64_t dbInfo;
};
using IbvExpPostSend = int (*)(struct ibv_qp *, struct ibv_send_wr *, struct ibv_send_wr **, struct WrExpRsp *);

constexpr int32_t ROCE_ENOMEM = -12;
constexpr uint32_t SQ_FULL_MAX_RETRY = 100000U;
constexpr uint64_t MID_RING_BYTES = 8ULL * 1024ULL * 1024ULL;
constexpr uint32_t MID_RING_WQE_COUNT = 512U;

using HalSqCqConfig = int (*)(uint32_t devId, HybmHalSqCqConfigInfo *info);
using HalSqCqQuery = int (*)(uint32_t devId, HybmHalSqCqQueryInfo *info);
using LocalDevIdQuery = int (*)(uint32_t hostDevId, uint32_t *localDevId);

constexpr const char *kHns17LibName = "libhns-rdmav17.so";
constexpr const char *kHns25LibName = "libhns-rdmav25.so";
constexpr const char *kHrn017LibName = "libhrn0-rdmav17.so";
constexpr const char *kHnsLibCandidates[] = {kHns17LibName, kHns25LibName, kHrn017LibName};
constexpr uint32_t kHnsLibCandidateCount = sizeof(kHnsLibCandidates) / sizeof(kHnsLibCandidates[0U]);
constexpr const char *kHalLibName = "libascend_hal.so";

void *g_hnsHandle = nullptr;
const char *g_hnsLibName = nullptr;
void *g_halHandle = nullptr;
IbvExpPostSend g_ibvExpPostSend = nullptr;
HalSqCqConfig g_halSqCqConfig = nullptr;
HalSqCqQuery g_halSqCqQuery = nullptr;
LocalDevIdQuery g_localDevIdQuery = nullptr;

int32_t ResetApiSymbols()
{
    if (g_hnsHandle != nullptr && dlclose(g_hnsHandle) != 0) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlclose failed, lib=%s error=%s", g_hnsLibName, dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    if (g_halHandle != nullptr && dlclose(g_halHandle) != 0) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlclose failed, lib=%s error=%s", kHalLibName, dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    g_hnsHandle = nullptr;
    g_halHandle = nullptr;
    g_ibvExpPostSend = nullptr;
    g_halSqCqConfig = nullptr;
    g_halSqCqQuery = nullptr;
    g_localDevIdQuery = nullptr;
    return BM_OK;
}

void *LoadRequiredSymbol(void *handle, const char *lib, const char *symbol)
{
    void *addr = dlsym(handle, symbol);
    if (addr == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] dlsym failed, lib=%s symbol=%s error=%s", lib, symbol, dlerror());
    }
    return addr;
}

int32_t LoadRequiredSymbols()
{
    g_ibvExpPostSend =
        reinterpret_cast<IbvExpPostSend>(LoadRequiredSymbol(g_hnsHandle, g_hnsLibName, "ibv_exp_post_send"));
    g_halSqCqConfig = reinterpret_cast<HalSqCqConfig>(LoadRequiredSymbol(g_halHandle, kHalLibName, "halSqCqConfig"));
    g_halSqCqQuery = reinterpret_cast<HalSqCqQuery>(LoadRequiredSymbol(g_halHandle, kHalLibName, "halSqCqQuery"));
    g_localDevIdQuery =
        reinterpret_cast<LocalDevIdQuery>(LoadRequiredSymbol(g_halHandle, kHalLibName, "drvGetLocalDevIDByHostDevID"));
    if (g_ibvExpPostSend == nullptr || g_halSqCqConfig == nullptr || g_halSqCqQuery == nullptr ||
        g_localDevIdQuery == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED, "[hybm] required rdma symbols missing, libs=%s,%s", g_hnsLibName, kHalLibName);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

void *OpenHnsLibrary(const char *&libName)
{
    for (uint32_t i = 0U; i < kHnsLibCandidateCount; ++i) {
        void *handle = dlopen(kHnsLibCandidates[i], RTLD_NOW | RTLD_NODELETE);
        if (handle != nullptr) {
            libName = kHnsLibCandidates[i];
            if (i != 0U) {
                HYBM_LOGI("[hybm] device rdma library fallback, lib=%s index=%u", libName, i);
            }
            return handle;
        }
        HYBM_LOGW("[hybm] device rdma library not loadable, lib=%s index=%u error=%s", kHnsLibCandidates[i], i,
                  dlerror());
    }
    HYBM_LOGE(BM_DL_FUNCTION_FAILED,
              "[hybm] device dlopen failed, tried libs=%s,%s,%s; check device RDMA library deployment", kHns17LibName,
              kHns25LibName, kHrn017LibName);
    return nullptr;
}

int32_t LoadRdmaLibrary()
{
    g_hnsHandle = OpenHnsLibrary(g_hnsLibName);
    if (g_hnsHandle == nullptr) {
        return BM_DL_FUNCTION_FAILED;
    }

    g_halHandle = dlopen(kHalLibName, RTLD_NOW | RTLD_NODELETE);
    if (g_halHandle == nullptr) {
        HYBM_LOGE(BM_DL_FUNCTION_FAILED,
                  "[hybm] device dlopen failed, lib=%s error=%s; check device HAL library deployment", kHalLibName,
                  dlerror());
        return BM_DL_FUNCTION_FAILED;
    }
    (void)dlerror();

    if (LoadRequiredSymbols() != BM_OK) {
        (void)ResetApiSymbols();
        return BM_DL_FUNCTION_FAILED;
    }

    HYBM_LOGI("[hybm] rdma aicpu library loaded, hns=%s hal=%s postSend=%d sqCqConfig=%d sqCqQuery=%d localDevId=%d",
              g_hnsLibName, kHalLibName, static_cast<int32_t>(g_ibvExpPostSend != nullptr),
              static_cast<int32_t>(g_halSqCqConfig != nullptr), static_cast<int32_t>(g_halSqCqQuery != nullptr),
              static_cast<int32_t>(g_localDevIdQuery != nullptr));
    return BM_OK;
}

class RdmaLibrary {
public:
    RdmaLibrary() : result_(LoadRdmaLibrary()) {}

    ~RdmaLibrary()
    {
        (void)ResetApiSymbols();
    }

    RdmaLibrary(const RdmaLibrary &) = delete;
    RdmaLibrary &operator=(const RdmaLibrary &) = delete;

    int32_t Result() const
    {
        return result_;
    }

private:
    const int32_t result_;
};

int32_t EnsureRdmaLoaded()
{
    static const RdmaLibrary library;
    return library.Result();
}

struct DoorbellInfo {
    uint64_t sqBaseAddr = 0U;
    uint32_t sqDepth = 0U;
    uint32_t sqTail = 0U;
    uint32_t localDevId = 0U;
};

int32_t ResolveLocalDevId(const RdmaAicpuParam &param, uint32_t &localDevId)
{
    uint32_t resolved = 0U;
    int32_t ret = g_localDevIdQuery(param.devId, &resolved);
    if (ret != 0) {
        HYBM_LOGW("[hybm] drvGetLocalDevIDByHostDevID failed, wrId=%lu hostDevId=%u ret=%d, fallback to host devId",
                  param.wrId, param.devId, ret);
        return BM_ERROR;
    }
    localDevId = resolved;
    return BM_OK;
}

int32_t QuerySqProp(uint32_t localDevId, const RdmaAicpuParam &param, uint32_t prop, uint32_t &low, uint32_t &high)
{
    HybmHalSqCqQueryInfo query{};
    query.type = HYBM_DRV_NORMAL_TYPE;
    query.tsId = param.tsId;
    query.sqId = param.sqId;
    query.cqId = 0U;
    query.prop = prop;
    int32_t ret = g_halSqCqQuery(localDevId, &query);
    if (ret != 0) {
        HYBM_LOGE(BM_ERROR, "[hybm] halSqCqQuery failed, wrId=%lu localDevId=%u sqId=%u tsId=%u prop=%u ret=%d",
                  param.wrId, localDevId, param.sqId, param.tsId, prop, ret);
        return BM_ERROR;
    }
    low = query.value[0];
    high = query.value[1];
    return BM_OK;
}

int32_t QuerySqDepth(uint32_t localDevId, const RdmaAicpuParam &param, uint32_t &depth)
{
    uint32_t high = 0U;
    return QuerySqProp(localDevId, param, HYBM_DRV_SQCQ_PROP_SQ_DEPTH, depth, high);
}

int32_t QuerySqTail(uint32_t localDevId, const RdmaAicpuParam &param, uint32_t &tail)
{
    uint32_t high = 0U;
    return QuerySqProp(localDevId, param, HYBM_DRV_SQCQ_PROP_SQ_TAIL, tail, high);
}

int32_t QuerySqBase(uint32_t localDevId, const RdmaAicpuParam &param, uint64_t &base)
{
    uint32_t low = 0U;
    uint32_t high = 0U;
    int32_t ret = QuerySqProp(localDevId, param, HYBM_DRV_SQCQ_PROP_SQ_BASE, low, high);
    if (ret != BM_OK) {
        return ret;
    }
    base = (static_cast<uint64_t>(high) << 32U) | low;
    return BM_OK;
}

struct WqeSeg {
    uint64_t localAddr = 0U;
    uint64_t remoteAddr = 0U;
    uint64_t len = 0U;
    uint32_t localLkey = 0U;
    uint32_t remoteRkey = 0U;
};

struct WqeCtx {
    uint64_t qpHandle = 0U;
    uint32_t opcode = 0U;
};

struct WqeBuf {
    struct ibv_sge sge {};
    struct ibv_send_wr wr {};
};

// 本批 WQE 的提交进度 + 中途敲铃要用的 host 侧参数（SQ 三要素由 PushDoorbell 现查）。
struct RingCtx {
    const RdmaAicpuParam *param = nullptr;
    uint64_t lastDbInfo = 0U;      // 最后一个已提交 WQE 的 doorbell 值
    uint64_t postedBytes = 0U;     // 自上次敲铃以来已提交的数据字节数
    uint32_t postedSinceRing = 0U; // 自上次敲铃以来已提交的 WQE 数
    uint32_t posted = 0U;          // 已提交的 WQE 数（数据段 + notify 尾段）
};

int32_t PostRdmaWqe(const WqeCtx &ctx, const WqeSeg &seg, uint64_t wrId, uint32_t sendFlags, WqeBuf &buf,
                    WrExpRsp &expRsp)
{
    buf.sge.addr = seg.localAddr;
    buf.sge.length = static_cast<uint32_t>(seg.len);
    buf.sge.lkey = seg.localLkey;

    buf.wr.wrId = wrId;
    buf.wr.sgList = &buf.sge;
    buf.wr.numSge = 1;
    buf.wr.opcode = (ctx.opcode == 0U) ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
    buf.wr.sendFlags = static_cast<int>(sendFlags);
    buf.wr.rdmaRemoteAddr = seg.remoteAddr;
    buf.wr.rdmaRkey = seg.remoteRkey;

    struct ibv_send_wr *badWr = nullptr;
    int32_t ret = g_ibvExpPostSend(reinterpret_cast<struct ibv_qp *>(ctx.qpHandle), &buf.wr, &badWr, &expRsp);
    __asm__ __volatile__("dsb sy" ::: "memory");
    if (ret == ROCE_ENOMEM) {
        return ROCE_ENOMEM; // SQ 满，交由调用方退避后重试
    }
    if (ret != 0) {
        HYBM_LOGE(BM_ERROR, "[hybm] ibv_exp_post_send failed, wrId=%lu opcode=%d len=%lu ret=%d", wrId, buf.wr.opcode,
                  seg.len, ret);
        return ret;
    }
    if (expRsp.dbInfo == 0U) {
        HYBM_LOGE(BM_ERROR, "[hybm] empty doorbell info, wrId=%lu qpHandle=0x%lx", wrId, ctx.qpHandle);
        return BM_ERROR;
    }
    return 0;
}

void GetWqeSeg(const RdmaAicpuParam &param, uint32_t index, WqeSeg &seg)
{
    const auto *segs = reinterpret_cast<const RdmaSegment *>(param.descAddr);
    seg.localAddr = segs[index].localAddr;
    seg.remoteAddr = segs[index].remoteAddr;
    seg.len = segs[index].len;
    seg.localLkey = segs[index].localLkey;
    seg.remoteRkey = segs[index].remoteRkey;
}

uint64_t WqeLength(const RdmaAicpuParam &param, uint32_t index)
{
    return reinterpret_cast<const RdmaSegment *>(param.descAddr)[index].len;
}

void BackoffForSq()
{
    for (volatile uint32_t i = 0U; i < 2000U; ++i) {
        __asm__ __volatile__("nop");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}

int32_t QuerySqStatic(uint32_t devId, const RdmaAicpuParam &param, DoorbellInfo &di)
{
    di.localDevId = devId;
    int32_t ret = QuerySqDepth(devId, param, di.sqDepth);
    if (ret != BM_OK) {
        return ret;
    }
    if (di.sqDepth == 0U) {
        HYBM_LOGE(BM_ERROR, "[hybm] zero sq depth, wrId=%lu devId=%u sqId=%u", param.wrId, devId, param.sqId);
        return BM_ERROR;
    }
    ret = QuerySqBase(devId, param, di.sqBaseAddr);
    if (ret != BM_OK) {
        return ret;
    }
    if (di.sqBaseAddr == 0U) {
        HYBM_LOGE(BM_ERROR, "[hybm] zero sq base, wrId=%lu devId=%u sqId=%u", param.wrId, devId, param.sqId);
        return BM_ERROR;
    }
    return BM_OK;
}

int32_t ResolveSq(const RdmaAicpuParam &param, DoorbellInfo &di)
{
    uint32_t localDevId = param.devId;
    (void)ResolveLocalDevId(param, localDevId);
    int32_t queryRet = QuerySqStatic(localDevId, param, di);
    if (queryRet != BM_OK && localDevId != param.devId) {
        HYBM_LOGE(BM_ERROR, "[hybm] sq query failed with local devId=%u, retry with host devId=%u, wrId=%lu",
                  localDevId, param.devId, param.wrId);
        queryRet = QuerySqStatic(param.devId, param, di);
    }
    if (queryRet != BM_OK) {
        return queryRet;
    }
    int32_t ret = QuerySqTail(di.localDevId, param, di.sqTail);
    if (ret != BM_OK) {
        return ret;
    }
    if (di.sqTail >= di.sqDepth) {
        HYBM_LOGE(BM_ERROR, "[hybm] sq tail out of range, wrId=%lu localDevId=%u tail=%u depth=%u", param.wrId,
                  di.localDevId, di.sqTail, di.sqDepth);
        return BM_ERROR;
    }
    return BM_OK;
}

HybmStarsWriteValueSqe BuildDoorbellSqe(const RdmaAicpuParam &param, uint64_t dbInfo, uint32_t tail)
{
    HybmStarsWriteValueSqe sqe{};
    sqe.header.type = HYBM_RT_STARS_SQE_TYPE_WRITE_VALUE;
    sqe.header.ie = HYBM_RT_STARS_SQE_INT_DIR_NO;
    sqe.header.pre_p = HYBM_RT_STARS_SQE_INT_DIR_NO;
    sqe.header.post_p = HYBM_RT_STARS_SQE_INT_DIR_NO;
    sqe.header.wr_cqe = 0U;
    sqe.header.rt_stream_id = static_cast<uint16_t>(param.sqStreamId);
    sqe.header.task_id = static_cast<uint16_t>(tail);
    sqe.va = 0U;
    sqe.kernel_credit = HYBM_RT_STARS_DEFAULT_KERNEL_CREDIT;
    sqe.awsize = HYBM_RT_STARS_WRITE_VALUE_SIZE_TYPE_64BIT;
    sqe.sub_type = HYBM_RT_STARS_WRITE_VALUE_SUB_TYPE_RDMA_DB_SEND;
    sqe.write_value_part0 = static_cast<uint32_t>(dbInfo & 0xFFFFFFFFULL);
    sqe.write_value_part1 = static_cast<uint32_t>((dbInfo >> 32U) & 0xFFFFFFFFULL);
    sqe.write_addr_low = static_cast<uint32_t>(param.doorbellAddr & 0xFFFFFFFFULL);
    sqe.write_addr_high = static_cast<uint32_t>((param.doorbellAddr >> 32U) & 0x1FFFFULL);
    return sqe;
}

int32_t WriteAndRingDoorbell(const RdmaAicpuParam &param, const DoorbellInfo &di, const HybmStarsWriteValueSqe &sqe)
{
    // 把 SQE 写进 SQ 的硬件 tail 槽，再用 halSqCqConfig(DRV_SQCQ_PROP_SQ_TAIL) 推进 SQ tail。
    // 等价于 HCOMM A2 AICPU 侧 DispatcherAiCpu::RdmaSend + ConfigSqStatusByType(SQ_TAIL)。
    auto *slot = reinterpret_cast<HybmStarsWriteValueSqe *>(di.sqBaseAddr +
                                                            static_cast<uint64_t>(di.sqTail) * HYBM_RT_STARS_SQE_SIZE);
    __builtin_memcpy(slot, &sqe, sizeof(sqe));
    __asm__ __volatile__("dsb sy" ::: "memory");

    HybmHalSqCqConfigInfo cfg{};
    cfg.type = HYBM_DRV_NORMAL_TYPE;
    cfg.tsId = param.tsId;
    cfg.sqId = param.sqId;
    cfg.cqId = 0U;
    cfg.prop = HYBM_DRV_SQCQ_PROP_SQ_TAIL;
    cfg.value[0] = (di.sqTail + 1U) % di.sqDepth;

    int32_t ret = g_halSqCqConfig(di.localDevId, &cfg);
    if (ret != 0) {
        HYBM_LOGE(BM_ERROR,
                  "[hybm] halSqCqConfig(SQ_TAIL) failed, wrId=%lu localDevId=%u sqId=%u tsId=%u tail=%u ret=%d",
                  param.wrId, di.localDevId, param.sqId, param.tsId, cfg.value[0], ret);
        return BM_ERROR;
    }
    return BM_OK;
}

int32_t PushDoorbell(const RdmaAicpuParam &param, uint64_t dbInfo, DoorbellInfo &di)
{
    if (param.doorbellAddr == 0U) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid doorbell addr, wrId=%lu", param.wrId);
        return BM_INVALID_PARAM;
    }
    int32_t ret = ResolveSq(param, di);
    if (ret != BM_OK) {
        return ret;
    }
    const HybmStarsWriteValueSqe sqe = BuildDoorbellSqe(param, dbInfo, di.sqTail);
    return WriteAndRingDoorbell(param, di, sqe);
}

int32_t WriteOut(const RdmaAicpuParam &param, uint64_t dbInfo, uint32_t posted, int32_t doorbellRet,
                 const DoorbellInfo &di)
{
    if (param.outAddr == 0U) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] out buffer missing, wrId=%lu", param.wrId);
        return BM_INVALID_PARAM;
    }
    auto *out = reinterpret_cast<volatile RdmaAicpuOut *>(param.outAddr);
    out->posted = posted;
    out->dbInfo = dbInfo;
    out->doorbellRet = doorbellRet;
    out->sqBaseAddr = di.sqBaseAddr;
    out->sqDepth = di.sqDepth;
    out->sqTail = di.sqTail;
    out->localDevId = di.localDevId;
    __asm__ __volatile__("dsb sy" ::: "memory");
    return BM_OK;
}

int32_t WriteFailureOut(const RdmaAicpuParam &param, const RingCtx &ring, int32_t kernelRet)
{
    DoorbellInfo di{};
    (void)ResolveSq(param, di); // 尽力拿到 SQ 基址/深度/tail 供 host 定位
    HYBM_LOGE(BM_ERROR, "[hybm] post batch failed, wrId=%lu posted=%u ret=%d localDevId=%u sqDepth=%u sqTail=%u",
              param.wrId, ring.posted, kernelRet, di.localDevId, di.sqDepth, di.sqTail);
    (void)WriteOut(param, ring.lastDbInfo, ring.posted, kernelRet, di);
    return kernelRet;
}

int32_t RingDoorbellMidway(RingCtx &ring, uint64_t wrId)
{
    if (ring.posted == 0U) {
        return BM_OK; // 本批还没有任何 WQE 可敲
    }
    DoorbellInfo di{};
    int32_t ret = PushDoorbell(*ring.param, ring.lastDbInfo, di);
    if (ret != BM_OK) {
        HYBM_LOGE(BM_ERROR, "[hybm] midway doorbell failed, wrId=%lu posted=%u ret=%d", wrId, ring.posted, ret);
        return ret;
    }
    ring.postedBytes = 0U;
    ring.postedSinceRing = 0U;
    return BM_OK;
}

int32_t PostWqeWithRetry(const WqeCtx &ctx, const WqeSeg &seg, uint64_t wrId, uint32_t sendFlags, WqeBuf &buf,
                         WrExpRsp &rsp, RingCtx &ring)
{
    uint32_t retry = 0U;
    int32_t ret = PostRdmaWqe(ctx, seg, wrId, sendFlags, buf, rsp);
    while (ret == ROCE_ENOMEM && retry++ < SQ_FULL_MAX_RETRY) {
        ret = RingDoorbellMidway(ring, wrId);
        if (ret != BM_OK) {
            return ret;
        }
        BackoffForSq();
        ret = PostRdmaWqe(ctx, seg, wrId, sendFlags, buf, rsp);
    }
    if (ret != 0) {
        return ret;
    }
    ring.lastDbInfo = rsp.dbInfo;
    ++ring.posted;
    ++ring.postedSinceRing;
    ring.postedBytes += seg.len;
    // 两个阈值取先到者：每 MID_RING_WQE_COUNT 个 WQE 敲一次（保持 NIC 消费索引紧跟生产者），
    // 或累计字节数达到 MID_RING_BYTES（大段让传输与后续提交重叠）。
    if (ring.postedSinceRing < MID_RING_WQE_COUNT && ring.postedBytes < MID_RING_BYTES) {
        return 0;
    }
    return RingDoorbellMidway(ring, wrId);
}

int32_t PostDataWqes(const RdmaAicpuParam &param, const WqeCtx &ctx, uint32_t total, RingCtx &ring)
{
    WqeBuf buf{};
    for (uint32_t i = 0U; i < total; ++i) {
        const uint64_t len = WqeLength(param, i);
        if (len == 0U) {
            continue;
        }
        if (len > UINT32_MAX) {
            HYBM_LOGE(BM_INVALID_PARAM, "[hybm] length overflow, len=%lu index=%u wrId=%lu", len, i, param.wrId);
            return BM_INVALID_PARAM;
        }
        WqeSeg seg{};
        GetWqeSeg(param, i, seg);

        WrExpRsp rsp{};
        int32_t ret = PostWqeWithRetry(ctx, seg, param.wrId + i, IBV_SEND_SIGNALED, buf, rsp, ring);
        if (ret != 0) {
            HYBM_LOGE(BM_ERROR, "[hybm] post wqe failed at index=%u, descCount=%u wrId=%lu ret=%d", i, param.descCount,
                      param.wrId, ret);
            return ret;
        }
    }
    return BM_OK;
}

int32_t PostNotifyWqe(const RdmaAicpuParam &param, const WqeCtx &ctx, uint32_t total, RingCtx &ring)
{
    if (param.notifyRemoteAddr == 0U || param.notifyLen == 0U) {
        return BM_OK;
    }
    WqeSeg seg{};
    seg.localAddr = param.notifyLocalAddr;
    seg.remoteAddr = param.notifyRemoteAddr;
    seg.len = param.notifyLen;
    seg.localLkey = param.notifyLocalLkey;
    seg.remoteRkey = param.notifyRemoteRkey;

    WqeCtx notifyCtx = ctx;
    notifyCtx.opcode = HYBM_RDMA_OP_READ;
    WqeBuf buf{};
    WrExpRsp rsp{};
    int32_t ret =
        PostWqeWithRetry(notifyCtx, seg, param.wrId + total, IBV_SEND_SIGNALED | IBV_SEND_FENCE, buf, rsp, ring);
    if (ret != 0) {
        HYBM_LOGE(BM_ERROR, "[hybm] post notify wqe failed, wrId=%lu ret=%d", param.wrId, ret);
        return ret;
    }
    return BM_OK;
}

int32_t PostBatchTask(const RdmaAicpuParam &param)
{
    const uint32_t total = param.descCount;
    const WqeCtx ctx{param.qpHandle, param.opcode};
    RingCtx ring{};
    ring.param = &param;
    int32_t ret = PostDataWqes(param, ctx, total, ring);
    if (ret != BM_OK) {
        return WriteFailureOut(param, ring, ret);
    }
    ret = PostNotifyWqe(param, ctx, total, ring);
    if (ret != BM_OK) {
        return WriteFailureOut(param, ring, ret);
    }

    DoorbellInfo di{};
    int32_t dbRet = BM_OK;
    if (ring.posted > 0U) {
        dbRet = PushDoorbell(param, ring.lastDbInfo, di);
    }
    ret = WriteOut(param, ring.lastDbInfo, ring.posted, dbRet, di);
    if (ret != BM_OK) {
        return ret;
    }
    if (ring.posted == 0U) {
        HYBM_LOGI("[hybm] nothing posted, descCount=%u wrId=%lu", param.descCount, param.wrId);
        return BM_OK;
    }
    return dbRet;
}

int32_t ValidateAicpuParam(const RdmaAicpuParam *param)
{
    if (param == nullptr || param->qpHandle == 0U) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] invalid param, param=%p", static_cast<const void *>(param));
        return BM_INVALID_PARAM;
    }
    if (param->abiVersion != HYBM_RDMA_AICPU_ABI_VERSION) {
        HYBM_LOGE(BM_INVALID_PARAM, "[hybm] ABI mismatch, host=%u kernel=%u, rebuild kernel .so", param->abiVersion,
                  HYBM_RDMA_AICPU_ABI_VERSION);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

} // namespace

extern "C" {

int32_t HybmRdmaAicpuPostSend(RdmaAicpuParam *param)
{
    int32_t ret = ValidateAicpuParam(param);
    if (ret != BM_OK) {
        return ret;
    }

    ret = EnsureRdmaLoaded();
    if (ret != BM_OK) {
        return ret;
    }
    return PostBatchTask(*param);
}

} // extern "C"
