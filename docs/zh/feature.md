# 特性简介

## 1. Big Memory（BM）

面向大内存使用场景，提供大容量的HBM+DRAM池化空间，提供全局统一的虚拟地址编址和多路径的内存访问能力，支持动态扩缩容。

* 支持创建统一内存空间存储数据。
* 支持同步数据拷贝，包含L2G、G2L、G2H、H2G和G2G。
* 自动分配rank（初始化时用户可以不指定rank，由BM内部自动生成，具体参见smem_bm_init接口）。
* 动态join和leave（参见smem_bm_join和smem_bm_leave接口）。
* 接口支持的语言: C和Python。

Note:

```angular2html
L2G: copy data from local HBM space to global memory space
G2L: copy data from global memory space to local HBM space
H2G: copy data from local DRAM memory to global memory space
G2H: copy data from global memory space to local DRAM memory
G2G: copy data from global memory space to global memory space
```

安装MemFabric后，BM接口头文件位于 `${MEMFABRIC_HYBRID_HOME_PATH}/${arch}-${os}/include/smem/host/smem_bm.h`。

### 使用简介

详情请参见 [bm_example](../../benchmark/bm/README.md)，简单概述如下。

**C接口使用方式**

1. 调用smem_bm_init接口初始化。
2. 调用smem_bm_create接口创建实例。
3. 调用smem_bm_ptr_by_mem_type接口获取gva地址，然后调用smem_bm_copy接口访问对应地址的内存。

**Python接口使用方式**

1. 调用mf_smem.bm.initialize接口初始化。
2. 调用mf_smem.bm.create接口创建实例获得bm_handle。
3. 调用bm_handle.peer_rank_ptr接口获取gva地址，然后调用bm_handle.copy_data接口访问对应地址的内存。

## 2. Transfer（TRANS）

面向大模型推理的数据传输场景，支持对用户注册的HBM内存进行高性能D2D传输。

 * 支持用户注册内存，并对该内存进行跨server共享。
 * 提供对用户注册的内存进行数据拷贝的接口，并提供读写语义。
 * 接口支持的语言: C和Python。

安装MemFabric后，TRANS接口头文件位于`${MEMFABRIC_HYBRID_HOME_PATH}/${arch}-${os}/include/smem/host/smem_trans.h`。

### 使用简介

详情参考[trans_example](../../examples/transfer/README.md)，简单概述如下。

**C接口使用方式**

1. 调用smem_trans_config_init初始化配置，并设置`config.url`（本实例唯一标识，格式`ip:port`；端口填`0`表示自动选择可用端口，可用smem_trans_get_rpc_port获取真实端口）。
2. 调用smem_trans_init接口初始化（入参为deviceId）。
3. 调用smem_trans_create接口创建实例，实例内部会按`config.url`创建自己的config store服务端，无需调用方提前创建config store。
4. 调用smem_trans_register_mem接口将内存注册给memfabric，然后调用smem_trans_write或smem_trans_read接口进行传输；异步批量拷贝场景使用smem_trans_batch_copy提交任务、smem_trans_batch_status_query查询完成状态。

**Python接口使用方式**

1. 调用memfabric_hybrid.TransferEngine接口创建engine。
2. 调用engine.initialize接口初始化（`session_id`同时是本实例内部config store的监听地址，`store_url`为兼容参数，当前版本不再参与对端发现）。
3. 调用engine.register_memory接口将内存注册给memfabric，然后调用engine.transfer_sync_write接口进行传输。

## 3. Share Memory（SHM）

面向算子场景，提供全局共享内存，支持AICORE的细粒度的读写操作。

 * 支持创建统一内存空间。
 * 支持用户传入自定义数据在卡侧访问（参见smem_shm_set_extra_context和smem_shm_get_extra_context_addr接口）。
 * 支持host侧全局barrier和allgather（参见smem_shm_control_barrier和smem_shm_control_allgather接口）。
 * 支持超节点内，卡侧通过MTE直接访问统一内存。
 * 卡侧提供基础copy接口。
 * 接口支持的语言: C和Python。

安装MemFabric后，SHM接口头文件位于`${MEMFABRIC_HYBRID_HOME_PATH}/${arch}-${os}/include/smem/host/smem_shm.h`
和`${MEMFABRIC_HYBRID_HOME_PATH}/${arch}-${os}/include/smem/device/smem_shm_aicore_base_api.h`。

### 使用简介

详情参考[shm_example](../../examples/hbm_share_memory/ShiftPutGet/README.md)，简单概述如下。

**C接口使用方式**

1. 调用smem_shm_init接口初始化。
2. 调用smem_shm_create接口创建实例并获得gva地址。
3. 编写算子直接访问或通过smem_shm_aicore_base_api.h的接口访问gva地址。

## 4. Acc Offload

面向KV Cache卸载等场景，提供DRAM内存池及卡侧稀疏拷贝加速能力，支持单卡本地（LOCAL）与多卡共享（SHARED）两种场景。

 * 支持从预留的DRAM内存池中分配/释放host内存（参见offload_malloc和offload_free接口）。
 * 提供卡侧批量稀疏拷贝接口，加速多个不连续地址间的数据搬移（参见offload_sparse_copy接口）。
 * LOCAL场景：每rank独占一份本地DRAM池。
 * SHARED场景：多rank共享同一份DRAM池。
 * Python侧提供torch友好的empty和sparse_copy封装。
 * 接口支持的语言: C和Python。

### 使用简介

LOCAL场景详情参考[local_dram_offload示例](../../examples/kv_offload/local_dram_offload/local_dram_offload.py)，SHARED场景详情参考[shared_dram_offload示例](../../examples/kv_offload/shared_dram_offload/shared_dram_offload.py)。
简单概述如下。

**Python接口使用方式**

1. `from memfabric_hybrid import offload`。
2. 构造OffloadConfig，调用offload.initialize接口初始化。
   - LOCAL场景：设置device_id、reserve_size、alloc_size（两者需相等），scene保持默认LOCAL。
   - SHARED场景：设置device_id、reserve_size、alloc_size（传实际值）、world_size、rank_id，scene设为offload.Scene.SHARED；如显式指定共享池的config store地址，需再设置store_url（同组各rank保持一致）。
3. 调用offload.empty分配内存并获得torch.Tensor，或调用offload.malloc获得地址。
4. 调用offload.sparse_copy接口在device上执行批量稀疏拷贝。
5. 调用offload.free释放内存，调用offload.uninitialize退出。
    SHARED场景下initialize/uninitialize为集合操作，需所有rank同步进入与退出。
