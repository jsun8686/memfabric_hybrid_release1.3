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
#ifndef MF_HYBRID_SMEM_TRANS_DEF_H
#define MF_HYBRID_SMEM_TRANS_DEF_H

#include <stdint.h>
#include "smem_bm_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMEM_TRANS_RANK_COUNT_MAX (1024U)

typedef void *smem_trans_t;

/*
 * @brief Transfer role, i.e. sender/receiver
 */
typedef enum {
    SMEM_TRANS_NONE = 0, /* no role */
    SMEM_TRANS_SENDER,   /* sender */
    SMEM_TRANS_RECEIVER, /* receiver */
    SMEM_TRANS_BOTH,     /* both sender and receiver */
    SMEM_TRANS_BUTT
} smem_trans_role_t;

/**
 * @brief Transfer config
 */
typedef struct {
    smem_trans_role_t role;          /* transfer role */
    uint32_t initTimeout;            /* func timeout, default 120 seconds */
    uint32_t deviceId;               /* npu device id */
    uint32_t flags;                  /* optional flags */
    smem_bm_data_op_type dataOpType; /* data operation type; see smem_bm_data_op_type */
    char nic[64];                    /* host NIC addr for host_rdma, format: tcp://ip:port */
    char url[64];                    /* unique id of this instance, format: ip:port, port 0 means auto detection */
    smem_tls_config hcomTlsConfig;   /* hcom TLS config for host_rdma */
} smem_trans_config_t;

/**
 * @brief Batch copy operation type, i.e. read/write
 */
typedef enum {
    SMEM_TRANS_OP_READ = 0,  /* read data from remote to local */
    SMEM_TRANS_OP_WRITE = 1, /* write data from local to remote */
} smem_trans_opcode_t;

/**
 * @brief Asynchronous batch copy params, submitted by smem_trans_batch_copy
 */
typedef struct {
    const char *destUrl;        /* remote unique id, format: ip:port */
    void **srcList;             /* WRITE: local source addrs; READ: local received addrs */
    void **destList;            /* WRITE: remote target addrs; READ: remote source addrs */
    size_t *sizeList;           /* byte counts of each batch copy */
    uint32_t batchSize;         /* number of batch copy tasks */
    smem_trans_opcode_t opcode; /* distinguishes read/write */
    void *stream;               /* if not null, submit task on this stream async */
    uint32_t flags;             /* reserved flags */
} smem_trans_batch_params_t;

#ifdef __cplusplus
}
#endif

#endif // MF_HYBRID_SMEM_TRANS_DEF_H
