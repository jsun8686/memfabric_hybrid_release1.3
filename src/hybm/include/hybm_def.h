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
#ifndef MEM_FABRIC_HYBRID_HYBRID_BIG_MEM_DL_H
#define MEM_FABRIC_HYBRID_HYBRID_BIG_MEM_DL_H

#include <stdint.h>
#include <stdbool.h>

#ifndef __cplusplus
extern "C" {
#endif

typedef void *hybm_entity_t;
typedef void *hybm_mem_slice_t;

#define HYBM_FLAG_EXPORT_ENTITY 1U ///< export/import entity instead of slice
// hybm_get_slice_va 时返回 slice 的 GVA（全局虚拟地址）而非本地虚拟地址
#define HYBM_GET_SLICE_GVA 1U

#define HYBM_TLS_PATH_SIZE 256
// HYBM_BIND_NUMA_FLAG start index When HYBM_PERFORMANCE_MODE_FLAG_INDEX == 1, this field is used
#define HYBM_BIND_NUMA_FLAG_INDEX        0
#define HYBM_BIND_NUMA_FLAG_LEN          7
#define HYBM_PERFORMANCE_MODE_FLAG_INDEX 7
#define HYBM_PERFORMANCE_MODE_FLAG_LEN   1
// Automatic NUMA affinity selection when HYBM_BIND_NUMA_FLAG == HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG
#define HYBM_BIND_NUMA_AUTO_AFFINITY_FLAG ((1U << HYBM_BIND_NUMA_FLAG_LEN) - 1)
// hybm_import 时用本地分配的 rankId 覆盖 exchange info 中的 rankId（trans 场景：
// 单 entity 多连接，每个 uniqueId 连接在导入时映射到本地唯一的覆盖 rank 窗口）
#define HYBM_FLAG_IMPORT_RANK_OVERRIDE (1U << 13)
// 覆盖 rankId 编码在 flags 的 bit16..bit31（预留低 16 位给功能开关）
#define HYBM_IMPORT_RANK_FLAG_INDEX 16U
#define HYBM_IMPORT_RANK_FLAG_LEN   16U
#define HYBM_IMPORT_RANK_MASK       ((1U << HYBM_IMPORT_RANK_FLAG_LEN) - 1U)
// 无效/未指定的覆盖 rankId：uint32_t 最大值。编码时收到该值不置任何覆盖位（等价于不覆盖），
// 解码时未使能覆盖即得到该值；0 是合法的覆盖 rank
#define HYBM_IMPORT_RANK_INVALID UINT32_MAX
#define HYBM_IMPORT_RANK_ENCODE(rank)     \
    (((rank) == HYBM_IMPORT_RANK_INVALID) \
         ? 0U                             \
         : (HYBM_FLAG_IMPORT_RANK_OVERRIDE | (((rank) & HYBM_IMPORT_RANK_MASK) << HYBM_IMPORT_RANK_FLAG_INDEX)))
#define HYBM_IMPORT_RANK_DECODE(flags)  (((flags) >> HYBM_IMPORT_RANK_FLAG_INDEX) & HYBM_IMPORT_RANK_MASK)
#define HYBM_IMPORT_RANK_ENABLED(flags) (((flags) & HYBM_FLAG_IMPORT_RANK_OVERRIDE) != 0U)

#define HYBM_FLAG_CREATE_WITH_SHM (1U << 8)
// HYBM_FLAG_DRAM_MAP_HOST_VA map host virtual address space
#define HYBM_FLAG_DRAM_MAP_HOST_VA (1U << 9)
// HYBM_FLAG_UNRESTRICTED_MEM mem_type == MEM_DDR_TYPE, pg_type == MEM_HUGE_PAGE_TYPE
#define HYBM_FLAG_UNRESTRICTED_MEM   (1U << 11)
#define HYBM_FLAG_OFFLOAD_GIANT_PAGE (1U << 12)
#define HYBM_FLAG_INIT_SHMEM_META    (1ULL << 63)

#define HYBM_PRE_REG_SIZE_THRES (8192U * 1024) // local buffer larger than 8MB maybe preregister to mr

#define HYBM_ENTITY_ID_SEGMENT_SIZE 64U
#define HYBM_ENTITY_ID_SHM_BASE     0U
#define HYBM_ENTITY_ID_BM_BASE      (HYBM_ENTITY_ID_SEGMENT_SIZE * 1U)
#define HYBM_ENTITY_ID_TRANS_BASE   (HYBM_ENTITY_ID_SEGMENT_SIZE * 2U)
#define HYBM_ENTITY_ID_OFFLOAD_BASE (HYBM_ENTITY_ID_SEGMENT_SIZE * 3U)

/* error code define */
#define BM_OK                 (0)
#define BM_ERROR              (-1)
#define BM_INVALID_PARAM      (-2)
#define BM_MALLOC_FAILED      (-3)
#define BM_NEW_OBJECT_FAILED  (-4)
#define BM_FILE_NOT_ACCESS    (-5)
#define BM_DL_FUNCTION_FAILED (-6)
#define BM_TIMEOUT            (-7)
#define BM_UNDER_API_UNLOAD   (-8)
#define BM_NOT_INITIALIZED    (-9)
#define BM_NOT_SUPPORT_FUNC   (-10)
#define BM_P2P_NOT_READY      (-11)
#define BM_NOT_SUPPORTED      (-100)
#define BM_NOT_CONNECTED      (-101)

/**
 * @brief Determine whether the IO initiator is on the host or the device.
 */
typedef enum {
    HYBM_TYPE_AI_CORE_INITIATE = 0, // AI core
    HYBM_TYPE_HOST_INITIATE,        // Host
    HYBM_TYPE_AICPU_INITIATE,       // AICPU
    HYBM_TYPE_BUTT
} hybm_type;

/**
 * @brief data copy type
 */
typedef enum {
    HYBM_DOP_TYPE_DEFAULT = 0U,
    HYBM_DOP_TYPE_MTE = 1U << 0,
    HYBM_DOP_TYPE_SDMA = 1U << 1,
    HYBM_DOP_TYPE_DEVICE_RDMA = 1U << 2,
    HYBM_DOP_TYPE_HOST_RDMA = 1U << 3,
    HYBM_DOP_TYPE_HOST_TCP = 1U << 4,
    HYBM_DOP_TYPE_HOST_URMA = 1U << 5,
    HYBM_DOP_TYPE_HOST_SHM = 1U << 6, /* same-node host shared memory (no network transport) */
    HYBM_DOP_TYPE_AIV_SDMA = 1U << 7,
    HYBM_DOP_TYPE_DEVICE_URMA = 1U << 8,
    HYBM_DOP_TYPE_DEVICE_UBOE = 1U << 9,
    HYBM_DOP_TYPE_BUTT
} hybm_data_op_type;

typedef enum {
    HYBM_MEM_TYPE_DEVICE = 1U << 0,
    HYBM_MEM_TYPE_HOST = 1U << 1,

    HYBM_MEM_TYPE_BUTT
} hybm_mem_type;

typedef enum {
    HYBM_SCENE_DEFAULT = 1U << 0,
    HYBM_SCENE_TRANS = 1U << 1,
    HYBM_SCENE_SHM = 1U << 2,
    HYBM_SCENE_OFFLOAD = 1U << 3, // offload 独立 VA 区段(常规 GVM 区最前2T)
    HYBM_SCENE_BUTT
} hybm_scene;

typedef enum {
    HYBM_ROLE_PEER = 0, // peer to peer connect
    HYBM_ROLE_SENDER,
    HYBM_ROLE_RECEIVER,
    HYBM_ROLE_BUTT
} hybm_role_type;

typedef struct {
    uint8_t *desc;
    uint32_t descLen;
} hybm_exchange_info;

typedef struct {
    bool tlsEnable;
    char caPath[HYBM_TLS_PATH_SIZE];
    char crlPath[HYBM_TLS_PATH_SIZE];
    char certPath[HYBM_TLS_PATH_SIZE];
    char keyPath[HYBM_TLS_PATH_SIZE];
    char keyPassPath[HYBM_TLS_PATH_SIZE];
    char packagePath[HYBM_TLS_PATH_SIZE];
    char decrypterLibPath[HYBM_TLS_PATH_SIZE];
} hybm_tls_config;

typedef struct {
    hybm_type bmType;
    hybm_mem_type memType;
    hybm_data_op_type bmDataOpType;
    uint32_t rankCount;
    uint32_t rankId;
    uint16_t devId;
    uint64_t maxHBMSize;
    uint64_t maxDRAMSize;
    uint64_t deviceVASpace;
    uint64_t hostVASpace;
    hybm_scene scene;
    bool enable56BitsGva;
    hybm_role_type role;
    uint32_t flags;
    char transUrl[64];
    char tag[32];
    char tagOpInfo[256];
    hybm_tls_config tlsOption;
    int dramShmFd;
} hybm_options;

typedef enum {
    HYBM_LOCAL_HOST_TO_GLOBAL_HOST = 0,
    HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE = 1,

    HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST = 2,
    HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE = 3,

    HYBM_GLOBAL_HOST_TO_GLOBAL_HOST = 4,
    HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE = 5,
    HYBM_GLOBAL_HOST_TO_LOCAL_HOST = 6,
    HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE = 7,

    HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST = 8,
    HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE = 9,
    HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST = 10,
    HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE = 11,

    HYBM_DATA_COPY_DIRECTION_AUTO = 12,
    HYBM_DATA_COPY_DIRECTION_BUTT
} hybm_data_copy_direction;

typedef struct {
    void *src;
    void *dest;
    uint64_t dataSize;
} hybm_copy_params;

typedef struct {
    void **sources;
    void **destinations;
    const uint64_t *dataSizes;
    uint32_t batchSize;
} hybm_batch_copy_params;

#ifndef __cplusplus
}
#endif

#endif // MEM_FABRIC_HYBRID_HYBRID_BIG_MEM_DL_H
