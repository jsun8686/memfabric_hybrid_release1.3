# 环境变量说明

本文档列出 MemFabric_Hybrid 使用的所有环境变量及其用途。

## 运行时环境变量

以下环境变量在程序运行时读取，用于控制运行时行为：

| 环境变量名 | 默认值                                                | 说明                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
|-----------|----------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `CUDA_HOME` | 无（必填）                                              | GPU环境下CUDA安装路径，用于加载CUDA动态库。NPU环境无需设置。                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `ASCEND_HOME_PATH` | 无（必填）                                              | NPU环境下Ascend安装路径，用于加载CANN动态库。GPU环境无需设置。                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `MF_HYBM_USE_HCOMM_AICPU` | false                                              | 设为`true`时绕过SoC判断，在任意SoC自动安装HCOMM AICPU kernel。                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `ASCEND_RT_VISIBLE_DEVICES` | 无                                                  | 设备可见性控制，用于将物理设备ID映射为逻辑设备ID。格式如`0,1,2,3`。                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `HCOM_MAX_SLICE_SIZE` | NPU: 1MB<br>其他: 1GB                                | HCOM传输最大切片大小（字节），控制单次传输数据分片上限。适用于HOST_RDMA/HOST_TCP/HOST_URMA传输模式。                                                                                                                                                                                                                                                                                                                                                                                                        |
| `HCOM_RECV_DATA_SIZE` | NPU: 1MB+1KB<br>其他: 1MB+1024                       | HCOM接收数据缓冲区大小（字节）。建议设置为`HCOM_MAX_SLICE_SIZE + 1024`。                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `MEMFABRIC_HYBRID_EXTEND_LIB_PATH` | 无                                                  | 扩展库路径，用于加载自定义的`libmf_hybm_copy_extend.so`库。                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `MF_HYBM_RDMA_SWAP_SPACE_SIZE` | device_rdma: 0<br>host_rdma: 1GB(NPU/GPU),4GB(CPU) | RDMA交换空间大小（单位MB），用于host_rdma/device_rdma数据传输的中转内存。                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `MF_HYBM_URMA_SWAP_SPACE_SIZE` | 0                                                  | URMA交换空间大小（单位MB），用于device_urma数据传输未注册内存的中转内存。设为0时跳过交换空间分配，未注册内存路径将直接返回错误。                                                                                                                                                                                                                                                                                                                                                                                                 |
| `MF_HYBM_RDMA_FORCE_UNREGISTERED` | 0                                                  | 强制RDMA跳过内存注册检查路径。设为非0值时，`BatchDataCopy`直接走未注册路径发起RDMA读写。                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `MF_HYBM_ENABLE_4K_PAGE` | 0                                                  | RMDA场景默认拦截4K页，设为非0值时，开放支持。                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `MF_HOST_SHM_USE_MEMFD` | 0                                                  | HOST_SHM段本端内存申请开关。设为`1`时优先尝试`memfd_create(MFD_HUGETLB)`申请匿名大页（容器内无需挂载`/dev/hugepages`，但跨进程import需经`/proc/<pid>/fd/<fd>`打开，要求各rank共享PID命名空间，如容器以`--pid=host`启动且运行uid一致）。 |
| `MF_DEVICE_UB_QOS` | 0                                                  | device_urma/device_uboe 的 HCOMM channel QoS，取值范围0-7，数值越大优先级越高；非法值回退为0。                                                                                                                                                                                                                                                                                                                                                                                                    |
| `MF_HYBM_RDMA_USE_HCOMM` | 0                                                  | HCOMM传输开关（仅 `DEVICE_RDMA` 路径生效）。<br>**默认关闭（0）**：无论CANN版本，`DEVICE_RDMA`一律走native RDMA传输（`RdmaTransportManager`），动态注册内存天然支持，无需中转。<br>**置1**：恢复CANN版本判断——CANN ≥ 9.1（ACL ≥ 1.17）时走HCOMM传输（`DeviceRdmaHcommTransportManager`），否则仍走native RDMA。<br>**限制**：A2（Ascend 910B）走HCOMM时，channel MR表为创建时快照（`AicpuTsRoceChannel` 不支持运行时 `UpdateMemInfo`），join后动态注册的内存（如vLLM KV cache）暂不支持直传，此时**必须同时设置 `MF_HYBM_RDMA_FORCE_UNREGISTERED=1` 走swap中转**。A5（Ascend 950）URMA通道支持动态MR更新，不受此限制。 |
| `MF_LOG_LEVEL` | 无                                                  | MemFabric日志级别，取值范围0-4（0:DEBUG, 1:INFO, 2:WARN, 3:ERROR, 4:OFF）。**仅Python接口下生效，bm/shm场景不生效。**                                                                                                                                                                                                                                                                                                                                                                              |
| `MF_CONFIG_STORE_URL` | 无（必填）                                              | MemFabric Store URL，用于Transfer Engine初始化时连接配置存储。格式如`tcp://ip:port`。                                                                                                                                                                                                                                                                                                                                                                                                       |
| `MF_CONFIG_STORE_PORT_START` | 9000                                               | Config Store可用端口范围起始值，与`MF_CONFIG_STORE_PORT_END`配合使用。TransferEngine在`session_id`未指定端口（如`ip`/`ip:0`）时自动选端口亦使用此范围。                                                                                                                                                                                                                                                                                                                                                         |
| `MF_CONFIG_STORE_PORT_END` | 65535                                              | Config Store可用端口范围结束值。                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `MF_CONFIG_STORE_HEARTBEAT_TIMEOUT_S` | 60                                                 | Config Store 服务端心跳超时（秒），范围 0～3600（1 小时）；0 表示永不超时。 |
| `MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S` | 2                                                  | Config Store 服务端链路中间态（EXCHANGING/EXCHANGED/CONNECTING）重发超时（秒），范围 1～600（10 分钟）；等待超过该时间仍未收到 ACK 则重发该链路，最多 10 次后降级为 IDLE。 |
| `MF_SOCKET_URL` | 无                                                  | 调试用的socket URL，覆盖 config store 的连接地址。                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `MF_TRANSPORT_MANAGER` | 无                                                  | 传输管理器选择，用于调试指定传输层实现。                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `MF_TRANS_THREAD_NUM` | 4                                                  | TRANS异步批量拷贝任务的后台执行线程数；取值为0或非法值时回退为默认值4。                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `MF_GROUP_JOIN_MAX_TIMEOUT` | 600                                                | 集群加入最大超时时间（秒），适用于BM和Transfer模块的Join操作。                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `MF_GROUP_RETRY_TIME` | 5                                                  | 集群更新（GroupUpdate）重试次数，适用于BM和Transfer模块。                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `MF_QP_READY_CHECK_TIMEOUT_BASE` | 100                                                | Device RDMA QP（Queue Pair）就绪检查超时基础值（秒）。在Device RDMA传输模式下，等待所有rank的QP就绪的总超时基数。                                                                                                                                                                                                                                                                                                                                                                                             |
| `MF_NPU_RDMA_SEND_CQ_DEPTH` | 16384                                              | Device RDMA 发送CQ（Completion Queue）深度，有效范围64-32768。                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `MF_NPU_RDMA_RECV_CQ_DEPTH` | 16384                                              | Device RDMA 接收CQ深度，有效范围64-32768。                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `MF_NPU_RDMA_MAX_SEND_WR` | 32767                                              | Device RDMA 发送WQ深度（max_send_wr），有效范围64-32767。                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `MF_NPU_RDMA_MAX_RECV_WR` | 32767                                              | Device RDMA 接收WQ深度（max_recv_wr），有效范围64-32767。                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `MF_ACC_CHECK_PERIOD_HOURS` | 168                                                | 控制路径SSL证书定期检查周期（小时），有效范围24-720。                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `MF_ACC_CERT_CHECK_AHEAD_DAYS` | 30                                                 | 证书提前检查天数，在证书过期前多少天开始告警，有效范围7-180。                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `MF_HCOM_CQ_DEPTH` | 无                                                  | HCOM完成队列深度，设置后覆盖默认值。                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `MF_HCOM_SQ_SIZE` | 无                                                  | HCOM发送队列大小，设置后覆盖默认值。                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `MF_HCOM_RQ_SIZE` | 无                                                  | HCOM接收队列大小，设置后覆盖默认值。                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `MF_HCOM_PREPOST_SIZE` | 无                                                  | HCOM预投递大小，设置后覆盖默认值。                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| `MF_HCOM_MAX_SEND_RECV_DATA_CNT` | 无                                                  | HCOM最大发送接收数据计数，设置后覆盖默认值。                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `MF_HYBM_RDMA_USE_AICPU` | 0                                                  | 设置为1时，device_rdma协议使用AICPU算子下发方式进行传输，设置为0时走非AICPU传输通道。注意：当同时设置MF_HYBM_RDMA_USE_HCOMM时，优先走HCOMM传输                                                                                                                                                                                                                                                                                                                                                                          |

## 构建时环境变量

以下环境变量在编译构建时读取，用于控制构建行为：

| 环境变量名 | 默认值 | 说明 |
|-----------|-------|------|
| `MEMFABRIC_VERSION` | 无（必填） | 构建版本号，用于生成whl包版本。**仅构建Python whl包时需要，C++构建不需要。** |
| `BUILD_OPEN_ABI` | OFF | 是否使用开放ABI（`_GLIBCXX_USE_CXX11_ABI`）。ON表示使用C++11 ABI。 |
| `BUILD_MODE` | RELEASE | 构建模式。可选值：RELEASE、DEBUG、ASAN。 |
| `ENABLE_PTRACER` | ON | 是否启用ptracer性能打点工具。 |
| `XPU_TYPE` | NPU | 异构设备类型。可选值：NPU、GPU、NONE。 |
| `MF_UT_BUILD_TYPE` | ASAN | `run_ut.sh` 的 UT 构建类型（即 `CMAKE_BUILD_TYPE`）。**aarch64 上 ASAN 不稳**（mockcpp `JmpCode` 钩子 + 假栈导致 `stack-use-after-return` 误报、函数入口栈插桩 SEGV），本地迭代建议 `MF_UT_BUILD_TYPE=DEBUG`（仍带 gcov 覆盖率门槛，sanitizer 留给 CI）。 |

## 安装脚本环境变量

| 环境变量名 | 说明 |
|-----------|------|
| `ASCEND_TOOLKIT_HOME` | Ascend Toolkit安装路径，安装脚本检查用。 |

## 使用示例

### NPU环境运行前设置

```bash
export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit
export MF_LOG_LEVEL=1
export HCOM_MAX_SLICE_SIZE=$((128*1024))
export HCOM_RECV_DATA_SIZE=$((128*1024+128))
export MF_HYBM_RDMA_SWAP_SPACE_SIZE=$((128))
```

### GPU环境运行前设置

```bash
export CUDA_HOME=/usr/local/cuda
export MF_LOG_LEVEL=1
```

### 无卡环境（NONE）运行前设置

```bash
export MF_LOG_LEVEL=1
export HCOM_MAX_SLICE_SIZE=$((128*1024))
export HCOM_RECV_DATA_SIZE=$((128*1024+128))
export MF_HYBM_RDMA_SWAP_SPACE_SIZE=$((128))
```

## DEVICE_RDMA 传输路径选择（HCOMM vs native）

`DEVICE_RDMA` 设备侧传输路径由 `MF_HYBM_RDMA_USE_HCOMM` 开关决定，与CANN版本共同决定实际使用哪条实现：

| `MF_HYBM_RDMA_USE_HCOMM` | CANN版本 | 实际传输实现 | 动态注册内存支持 |
|--------------------------|---------|-------------|----------------|
| 0（默认） | 任意 | native RDMA（`RdmaTransportManager`） | ✅ 天然支持（实时查MR表），无需中转 |
| 1 | ≥ 9.1（ACL ≥ 1.17） | HCOMM（`DeviceRdmaHcommTransportManager`） | ❌ A2需中转（见下） |
| 1 | < 9.1 | native RDMA | ✅ 同上 |

### HCOMM 路径的限制（A2 / Ascend 910B）

- A2 的 HCOMM channel 类为 `AicpuTsRoceChannel`，MR表在channel创建时一次性快照冻结，且不支持运行时 `UpdateMemInfo`（基类空实现）；
- join 后动态注册的内存（如 vLLM 动态 KV cache）不在快照中，暂不支持直传；
- **必须同时设置**：

  ```bash
  export MF_HYBM_RDMA_USE_HCOMM=1
  export MF_HYBM_RDMA_FORCE_UNREGISTERED=1   # 强制走swap中转兜底
  export MF_HYBM_RDMA_SWAP_SPACE_SIZE=2048   # 中转所需的swap空间（MB）
  ```

- native RDMA 路径无此限制，无需设置中转相关变量。

### native RDMA 的 AICPU 直发开关（`MF_HYBM_RDMA_USE_AICPU`）

在 native RDMA（`RdmaTransportManager`，即 `MF_HYBM_RDMA_USE_HCOMM=0`）路径下，可用 `MF_HYBM_RDMA_USE_AICPU` 选择 QP 管理方式：

| `MF_HYBM_RDMA_USE_AICPU` | QP 管理 | 说明 |
|-------------------------|---------|------|
| 0 | `JoinableRanksQpManager` | 普通 RC QP，host 侧填 WQE + STARS 门铃 |
| 1（默认）| `AicpuJoinableRanksQpManager` | **AI QP**（`RaQpAiCreate`），队列信息（`AiQpRMAQueueInfo`）下发 device，供 AICPU kernel 直接构造 WQE 并敲 NIC 门铃 |

  ```bash
  export MF_HYBM_RDMA_USE_AICPU=1
  ```

- 仅在 `HYBM_TYPE_HOST_INITIATE`（非 `HYBM_TYPE_AI_CORE_INITIATE`）路径生效；`AI_CORE_INITIATE` 本身已使用 `FixedRanksQpManager`（同样走 AI QP）。
- 仅作用于 native RDMA；当 `MF_HYBM_RDMA_USE_HCOMM=1` 且 CANN ≥ 9.1 时，`DEVICE_RDMA` 已改走 HCOMM，本开关不生效。
- 初版状态：AI QP 创建与设备侧队列信息下发已实现；数据面（AICPU kernel 下发与完成检测）仍在推进中。

### A5（Ascend 950）URMA 通道

- A5 的 `DEVICE_URMA` 走 `DeviceUrmaTransportManager`，channel 类为 `AicpuTsUrmaChannel`，**支持运行时动态MR更新**（完整实现 `UpdateMemInfo`）；
- 不受 `MF_HYBM_RDMA_USE_HCOMM` 开关影响，无需中转。
