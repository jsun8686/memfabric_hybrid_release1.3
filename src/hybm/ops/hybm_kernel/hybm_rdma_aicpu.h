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

#ifndef MF_HYBM_OPS_HYBM_KERNEL_HYBM_RDMA_AICPU_H
#define MF_HYBM_OPS_HYBM_KERNEL_HYBM_RDMA_AICPU_H

#include <cstdint>

namespace ock {
namespace mf {
namespace aicpu {

constexpr uint32_t HYBM_RDMA_OP_WRITE = 0U;
constexpr uint32_t HYBM_RDMA_OP_READ = 4U;
constexpr uint32_t HYBM_RDMA_AICPU_ABI_VERSION = 11U;

constexpr uint8_t HYBM_RT_STARS_SQE_TYPE_WRITE_VALUE = 8U;
constexpr uint8_t HYBM_RT_STARS_SQE_INT_DIR_NO = 0U;
constexpr uint8_t HYBM_RT_STARS_DEFAULT_KERNEL_CREDIT = 254U;
constexpr uint8_t HYBM_RT_STARS_WRITE_VALUE_SIZE_TYPE_64BIT = 3U;
constexpr uint32_t HYBM_RT_STARS_WRITE_VALUE_SUB_TYPE_RDMA_DB_SEND = 2U;
constexpr uint32_t HYBM_RT_STARS_SQE_SIZE = 64U;
constexpr uint32_t HYBM_DRV_NORMAL_TYPE = 0U;
constexpr uint32_t HYBM_DRV_SQCQ_PROP_SQ_TAIL = 2U;
constexpr uint32_t HYBM_DRV_SQCQ_PROP_SQ_BASE = 6U;
constexpr uint32_t HYBM_DRV_SQCQ_PROP_SQ_DEPTH = 7U;
constexpr uint32_t HYBM_SQCQ_CONFIG_VALUE_LEN = 8U;
// host/device 共享的 ABI 结构目标字节数：编译期锁定布局，防止字段增删悄悄改变 ABI
constexpr uint32_t HYBM_RT_STARS_SQE_HEADER_SIZE = 8U;
constexpr uint32_t HYBM_HAL_SQCQ_INFO_SIZE = 52U;
constexpr uint32_t HYBM_RDMA_SEGMENT_SIZE = 32U;
constexpr uint32_t HYBM_RDMA_AICPU_OUT_SIZE = 40U;
constexpr uint32_t HYBM_RDMA_AICPU_PARAM_SIZE = 112U;

#pragma pack(push, 1)
struct HybmStarsSqeHeader {
    uint8_t type : 6;
    uint8_t l1_lock : 1;
    uint8_t l1_unlock : 1;

    uint8_t ie : 2;
    uint8_t pre_p : 2;
    uint8_t post_p : 2;
    uint8_t wr_cqe : 1;
    uint8_t reserved : 1;

    uint16_t block_dim;
    uint16_t rt_stream_id;
    uint16_t task_id;
};

struct HybmStarsWriteValueSqe {
    HybmStarsSqeHeader header;
    uint32_t res3;
    uint32_t res4 : 16;
    uint32_t kernel_credit : 8;
    uint32_t res5 : 8;
    uint32_t write_addr_low;
    uint32_t write_addr_high : 17;
    uint32_t res6 : 3;
    uint32_t awsize : 3;
    uint32_t snoop : 1;
    uint32_t awcache : 4;
    uint32_t awprot : 3;
    uint32_t va : 1;
    uint32_t res7;
    uint32_t sub_type;
    uint32_t write_value_part0;
    uint32_t write_value_part1;
    uint32_t write_value_part2;
    uint32_t write_value_part3;
    uint32_t write_value_part4;
    uint32_t write_value_part5;
    uint32_t write_value_part6;
    uint32_t write_value_part7;
};
#pragma pack(pop)

static_assert(sizeof(HybmStarsSqeHeader) == HYBM_RT_STARS_SQE_HEADER_SIZE, "STARS sqe header must be 8 bytes");
static_assert(sizeof(HybmStarsWriteValueSqe) == HYBM_RT_STARS_SQE_SIZE, "STARS write value sqe must be 64 bytes");

struct HybmHalSqCqInfo {
    uint32_t type; // drvSqCqType_t, DRV_NORMAL_TYPE = 0
    uint32_t tsId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t prop; // drvSqCqPropType_t, DRV_SQCQ_PROP_SQ_TAIL = 2
    uint32_t value[HYBM_SQCQ_CONFIG_VALUE_LEN];
};

using HybmHalSqCqConfigInfo = HybmHalSqCqInfo;
using HybmHalSqCqQueryInfo = HybmHalSqCqInfo;

static_assert(sizeof(HybmHalSqCqInfo) == HYBM_HAL_SQCQ_INFO_SIZE, "halSqCq info must be 52 bytes");

struct RdmaSegment {
    uint64_t localAddr;  // 本地 DVA
    uint64_t remoteAddr; // 对端 DVA
    uint64_t len;
    uint32_t localLkey;
    uint32_t remoteRkey;
};

static_assert(sizeof(RdmaSegment) == HYBM_RDMA_SEGMENT_SIZE, "RdmaSegment must be 32 bytes");

struct RdmaAicpuOut {
    uint64_t dbInfo;     // 最后一个已提交 WQE 的 doorbell 值（调试用）
    uint32_t posted;     // 已提交的 WQE 数（host 据此校验提交数量）
    int32_t doorbellRet; // 算子推 SQ tail 的结果（0 成功，host 据此判断算子侧敲铃是否成功）

    // 诊断字段：host 侧无法查 SQ 基址/深度，失败时只能靠算子回传的值定位问题
    uint64_t sqBaseAddr; // halSqCqQuery(SQ_BASE) 查到的 SQ 基址
    uint32_t sqDepth;    // halSqCqQuery(SQ_DEPTH) 查到的 SQ 深度
    uint32_t sqTail;     // 写 SQE 时使用的 SQ tail（写入前的硬件 tail）
    uint32_t localDevId; // drvGetLocalDevIDByHostDevID 换算出的 device-local devId
    uint32_t reserved;
};

static_assert(sizeof(RdmaAicpuOut) == HYBM_RDMA_AICPU_OUT_SIZE, "RdmaAicpuOut must be 40 bytes");

struct RdmaAicpuParam {
    uint32_t abiVersion;

    // AI QP（HccpAiQpInfo.aiQpAddr，device 侧 struct ibv_qp*），供 ibv_exp_post_send 使用
    uint64_t qpHandle;

    uint32_t opcode; // HYBM_RDMA_OP_WRITE / HYBM_RDMA_OP_READ

    // 结果回传缓冲（device VA of RdmaAicpuOut）
    uint64_t outAddr;

    // 数据段一律走 HBM：descAddr 为 device 侧 RdmaSegment[] 地址，descCount 为段数
    uint64_t descAddr;
    uint32_t descCount;

    // notify 尾段（可选）：notifyRemoteAddr != 0 时，内核在数据 WQE 全部提交后再追加一个
    // RDMA READ——从对端 notify 源（notifyRemoteAddr/notifyRemoteRkey）读固定值写入本端
    // notify 寄存器（notifyLocalAddr/notifyLocalLkey），触发硬件完成 notify。
    // 这样"数据 + 完成 notify"由同一次 kernel 下发完成，host 只需敲一次 doorbell 再 Wait。
    uint32_t notifyLen;
    uint32_t notifyLocalLkey;
    uint32_t notifyRemoteRkey;
    uint64_t notifyLocalAddr;
    uint64_t notifyRemoteAddr;

    // 第 i 个数据 WQE 用 wrId+i，notify WQE 用 wrId+descCount，保证全局唯一。
    uint64_t wrId;

    // 算子侧敲 doorbell 所需信息：host 传入专用 doorbell ACL 流的 sqId/streamId 与 doorbell 寄存器地址，
    // SQ 基址/深度/tail 由内核用 device-local devId 现查。
    uint64_t doorbellAddr; // 目标 RoCE doorbell 寄存器 device 地址
    uint32_t sqId;         // 专用 doorbell ACL 流的 SQ id（host 由 rtStreamGetSqid 取得）
    uint32_t tsId;         // 固定 0
    uint32_t devId;        // host 侧 deviceId_，内核用 drvGetLocalDevIDByHostDevID 换算 local
    uint32_t sqStreamId;   // 写入 SQE header.rt_stream_id（host 由 aclrtStreamGetId 取得）
    uint32_t reserved;
};

static_assert(sizeof(RdmaAicpuParam) == HYBM_RDMA_AICPU_PARAM_SIZE, "RdmaAicpuParam size mismatch");

} // namespace aicpu
} // namespace mf
} // namespace ock

extern "C" {
int32_t HybmRdmaAicpuPostSend(ock::mf::aicpu::RdmaAicpuParam *param);
}

#endif // MF_HYBM_OPS_HYBM_KERNEL_HYBM_RDMA_AICPU_H
