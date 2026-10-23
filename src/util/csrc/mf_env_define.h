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
#ifndef MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
#define MEMFABRIC_HYBRID_MF_ENV_CONFIG_H

#include <cstdlib>
#include <string>

#include "mf_out_logger.h"

namespace ock {
namespace mf {
namespace env {

static std::string GetEnvStr(const char *name, const char *deprecatedName = nullptr) noexcept
{
    if (const char *val = std::getenv(name); val != nullptr) {
        return std::string(val);
    }
    if (deprecatedName != nullptr) {
        if (const char *depVal = std::getenv(deprecatedName); depVal != nullptr) {
            MF_OUT_LOG("[MF ", WARN_LEVEL,
                       "Environment variable '" << deprecatedName << "' is deprecated, please use '" << name
                                                << "' instead.");
            return std::string(depVal);
        }
    }
    return std::string();
}

inline const std::string CUDA_HOME = GetEnvStr("CUDA_HOME");

inline const std::string ASCEND_HOME_PATH = GetEnvStr("ASCEND_HOME_PATH");
inline const std::string ASCEND_RT_VISIBLE_DEVICES = GetEnvStr("ASCEND_RT_VISIBLE_DEVICES");

inline const std::string HCOM_MAX_SLICE_SIZE = GetEnvStr("HCOM_MAX_SLICE_SIZE");
inline const std::string HCOM_RECV_DATA_SIZE = GetEnvStr("HCOM_RECV_DATA_SIZE");

inline const std::string MF_HCOM_CQ_DEPTH = GetEnvStr("MF_HCOM_CQ_DEPTH");
inline const std::string MF_HCOM_SQ_SIZE = GetEnvStr("MF_HCOM_SQ_SIZE");
inline const std::string MF_HCOM_RQ_SIZE = GetEnvStr("MF_HCOM_RQ_SIZE");
inline const std::string MF_HCOM_PREPOST_SIZE = GetEnvStr("MF_HCOM_PREPOST_SIZE");
inline const std::string MF_HCOM_MAX_SEND_RECV_DATA_CNT = GetEnvStr("MF_HCOM_MAX_SEND_RECV_DATA_CNT");
inline const std::string MF_HYBM_RDMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "HYBM_RDMA_SWAP_SPACE_SIZE");
inline const std::string MF_HYBM_URMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_URMA_SWAP_SPACE_SIZE", "HYBM_URMA_SWAP_SPACE_SIZE");
inline const std::string MF_HYBM_RDMA_FORCE_UNREGISTERED =
    GetEnvStr("MF_HYBM_RDMA_FORCE_UNREGISTERED", "HYBM_RDMA_FORCE_UNREGISTERED");
// HCOMM 开关（默认关闭）：关闭时 DEVICE_RDMA 一律走 native；打开后按 CANN 版本判断
inline const std::string MF_HYBM_RDMA_USE_HCOMM = GetEnvStr("MF_HYBM_RDMA_USE_HCOMM", "HYBM_RDMA_USE_HCOMM");
// AICPU 直发开关（默认关闭）：打开时 DEVICE_RDMA 使用 AI QP，队列信息下发 device 供 AICPU kernel 直接下发任务
inline const std::string MF_HYBM_RDMA_USE_AICPU = GetEnvStr("MF_HYBM_RDMA_USE_AICPU", "HYBM_RDMA_USE_AICPU");
inline const std::string MF_HYBM_ENABLE_4K_PAGE = GetEnvStr("MF_HYBM_ENABLE_4K_PAGE", "HYBM_ENABLE_4K_PAGE");
inline const std::string MF_HOST_SHM_USE_MEMFD = GetEnvStr("MF_HOST_SHM_USE_MEMFD");
inline const std::string MF_LOG_LEVEL = GetEnvStr("MF_LOG_LEVEL", "ASCEND_MF_LOG_LEVEL");
inline const std::string MF_SOCKET_URL = GetEnvStr("MF_SOCKET_URL");
inline const std::string MF_TRANSPORT_MANAGER = GetEnvStr("MF_TRANSPORT_MANAGER", "TRANSPORT_MANAGER");
inline const std::string TEST_MF_TRANSPORT_FULL_MESH = GetEnvStr("TEST_MF_TRANSPORT_FULL_MESH");
inline const std::string MF_CONFIG_STORE_URL = GetEnvStr("MF_CONFIG_STORE_URL", "ASCEND_MF_STORE_URL");
inline const std::string MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S = GetEnvStr("MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S");
inline const std::string MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S = GetEnvStr("MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S");
inline const std::string MF_CONFIG_STORE_PORT_START =
    GetEnvStr("MF_CONFIG_STORE_PORT_START", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START");
inline const std::string MF_CONFIG_STORE_PORT_END =
    GetEnvStr("MF_CONFIG_STORE_PORT_END", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_END");
inline const std::string MF_GROUP_JOIN_MAX_TIMEOUT = GetEnvStr("MF_GROUP_JOIN_MAX_TIMEOUT");
inline const std::string MF_GROUP_RETRY_TIME = GetEnvStr("MF_GROUP_RETRY_TIME", "MF_SMEM_GROUP_RETRY_TIME");
inline const std::string MF_QP_READY_CHECK_TIMEOUT_BASE = GetEnvStr("MF_QP_READY_CHECK_TIMEOUT_BASE");
inline const std::string MF_ACC_CHECK_PERIOD_HOURS =
    GetEnvStr("MF_ACC_CHECK_PERIOD_HOURS", "ACCLINK_CHECK_PERIOD_HOURS");
inline const std::string MF_ACC_CERT_CHECK_AHEAD_DAYS =
    GetEnvStr("MF_ACC_CERT_CHECK_AHEAD_DAYS", "ACCLINK_CERT_CHECK_AHEAD_DAYS");
inline const std::string MF_DEVICE_RDMA_TC = GetEnvStr("HCCL_RDMA_TC");
inline const std::string MF_DEVICE_RDMA_SL = GetEnvStr("HCCL_RDMA_SL");
inline const std::string MF_NPU_RDMA_SEND_CQ_DEPTH = GetEnvStr("MF_NPU_RDMA_SEND_CQ_DEPTH");
inline const std::string MF_NPU_RDMA_RECV_CQ_DEPTH = GetEnvStr("MF_NPU_RDMA_RECV_CQ_DEPTH");
inline const std::string MF_NPU_RDMA_MAX_SEND_WR = GetEnvStr("MF_NPU_RDMA_MAX_SEND_WR");
inline const std::string MF_NPU_RDMA_MAX_RECV_WR = GetEnvStr("MF_NPU_RDMA_MAX_RECV_WR");

} // namespace env
} // namespace mf
} // namespace ock

#endif // MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
