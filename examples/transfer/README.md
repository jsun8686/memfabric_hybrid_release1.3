# memfabric_hybrid 数据传输示例

## 目录说明

本目录包含基于 memfabric_hybrid 传输引擎（mf_adapter / TransferEngine）的跨节点数据传输示例，适用于基于
昇腾 NPU 的分布式计算场景。示例通过内存注册与直接内存访问实现节点间数据的高效传输，避免了传统 IO 操作的
性能开销。

各脚本均以 **Prefill**（数据生产方）与 **Decode**（数据消费方）两类角色运行，每个实例内部会按自身会话 ID
（`src-unique-id`）创建配置存储服务（config store），对端按会话 ID 直连该服务完成发现与建链，支持大规模张量
数据（torch.Tensor）的传输。

| 脚本 | 传输模式 | 说明 |
|------|---------|------|
| test_transfer_engine.py | push | 基础跨节点张量传输：Prefill 生成数据并主动写入 Decode |
| test_transfer_engine_rd2h.py | pull | SFA RD2H 场景 PD KV Cache 拉取传输，支持 Decode 侧多 rank |

## test_transfer_engine.py：基础跨节点传输（push 模式）

### 功能说明

工具包含两种工作角色：

* **Prefill 角色**：负责生成数据并将数据传输到目标节点

* **Decode 角色**：负责接收并处理 Prefill 节点传输的数据

两者各自在本地创建配置存储服务（config store），Prefill 按 Decode 的会话 ID（`ip:port`）直连其 config store，
完成信息交换与建链。

### 使用方法

基本命令格式：

```bash
python test_transfer_engine.py [参数]
```

### 参数说明

| 参数名             | 必选 | 说明                                               |
|-----------------|----|--------------------------------------------------|
| --role          | 是  | 工作角色，可选值：`Prefill` 或 `Decode`                    |
| --src-unique-id | 是  | 当前节点的会话 ID，同时也是本实例内部 config store 的监听地址，格式：`ip:port` |
| --store-url     | 是  | 兼容参数（脚本仍要求传入，可传任意合法 `tcp://ip:port`）：当前版本config store协议固定为tcp且由各实例自行创建，该参数不再参与对端发现 |
| --npu-id        | 否  | NPU 设备 ID，默认值：0                                  |
| --dst-unique-id | 否  | 目标节点会话 ID（仅 Prefill 角色需要），格式：`ip:port`           |
| --log-level     | 否  | 日志级别：0 (debug)、1 (info)、2 (warn)、3 (error)，默认值：0 |

### 运行步骤

#### 1. 启动 Decode 节点

```bash
python test_transfer_engine.py \
    --role Decode \
    --src-unique-id "127.0.0.1:50051" \
    --store-url tcp://127.0.0.1:8000 \
    --npu-id 0 \
    --log-level 1
```

#### 2. 启动 Prefill 节点

```bash
python test_transfer_engine.py \
    --role Prefill \
    --src-unique-id "127.0.0.1:50052" \
    --store-url tcp://127.0.0.1:8000 \
    --npu-id 1 \
    --dst-unique-id "127.0.0.1:50051" \
    --log-level 1
```

### 工作流程说明

1. **初始化阶段**：

    * 两个节点分别启动并通过`engine.initialize()`初始化 TransferEngine，`src-unique-id`即本实例内部config store的监听地址

    * 对端发现不再依赖`store_url`：Prefill 按 Decode 的`src-unique-id`直连其config store，因此 Prefill 节点的`dst-unique-id`必须与
      Decode 节点的`src-unique-id`完全一致（包括端口号）

2. **数据传输阶段**：

    * Decode 节点创建接收缓冲区（大小为`(10, 50, 40, 20, 60)`的 float16 张量）并注册内存地址

    * Prefill 节点生成随机张量数据，注册内存后通过`transfer_sync_write`方法将数据传输到 Decode 节点

    * 数据通过直接内存访问方式传输，避免数据拷贝

### 故障排查

1. **初始化失败**：

    * 确认`src-unique-id`的地址/端口在所有节点可访问（对端会直连该地址的config store，可使用`telnet ip port`验证端口连通性），
      建议使用固定端口而非自动分配端口

    * 确认`src-unique-id`格式正确且未被占用

    * 验证 NPU 设备是否正常工作（可通过`npu-smi info`命令检查）

2. **数据传输失败**：

    * 检查 Prefill 节点的`dst-unique-id`是否与 Decode 节点的`src-unique-id`完全一致

    * 确认两个节点的内存注册成功（日志中包含`register success`）

    * 查看 debug 级日志（`--log-level 0`）获取详细错误信息

## test_transfer_engine_rd2h.py：SFA RD2H PD KV Cache 拉取传输（pull 模式）

### 简介

仿照 vllm-ascend `SfaRemoteD2HConnector` + `SparseKVOffloadManager` 的生产用法，模拟 SFA 场景下
Prefill/Decode 分离部署的 main KV Cache 拉取（pull）传输：

* **Prefill 侧**：按 MLA/SFA main cache 布局（bf16）生成每层 KV，仅 `register_memory` 注册源内存，从不主动写数据
* **Decode 侧多 rank**：每 rank 一个进程模拟多卡，共享同一 DRAM offload 池（仅 rank0 分配、其余挂接），池基地址经
  **HCCL broadcast** 同步（等价生产 `tp_group.broadcast(gvas)`，不经 Prefill 中继）
* 各 rank 独立初始化 TransferEngine，按 `tp_block_range` 均分区间拉取各自分片；完成后 rank0 用同 seed 重放期望
  KV 全量校验，并演示 `sparse_copy` 将 decode 新增 KV 直写 CPU 池（D2H）

与 test_transfer_engine.py 的主要差异：

| 维度 | test_transfer_engine.py | test_transfer_engine_rd2h.py |
|------|-------------------------|-------------------------------|
| 传输模式 | push（P 主动写 D） | pull（D 各 rank 主动读 P） |
| 数据布局 | 任意张量 | MLA/SFA main KV cache（k/v 分离 + block table） |
| Decode 并行 | 单进程 | 多 rank（共享池 + HCCL 广播 + 块区间切分） |
| 目的内存 | D 侧注册的接收缓冲 | D 侧共享 offload DRAM 池（无需注册） |
| 正确性校验 | 传输后比对 | seed 重放逐块全量校验 + D2H 演示 |
| 性能统计 | 无 | warmup/iterations 多轮纯传输带宽（均值/最好） |

默认参数（32 层 × 128 块）下单层 K+V 为 18 MiB、每轮约 576 MiB；默认 1 轮 warmup + 3 轮测量，单次约 2.25 GiB。

### 参数说明

| 参数名 | 必选 | 说明 |
|-------|----|------|
| --role | 是 | 工作角色：`Prefill` 或 `Decode` |
| --local-ip | 否 | 本机 IPv4，默认 `127.0.0.1`；跨机须传对端可达的本机网卡 IP |
| --prefill-ip | 否 | Prefill 机器 IPv4，默认 `127.0.0.1`；跨机须传真实 Prefill IP |
| --npu-id | 否 | launcher 模式：Decode 起始卡号（rank r 用 `起始+r`，默认 0）；手动 `--tp-rank` 模式：可选，默认等于 rank，可指定任意卡号 |
| --tp-size | 否 | Decode TP rank 数，默认 1 |
| --tp-rank | 否 | Decode TP rank；缺省时一条命令拉起全部 rank 子进程 |
| --decode-tp-size | 否 | 期望的 Decode rank 数（仅 Prefill 侧），默认 1 |
| --control-port | 否 | TCP 控制通道端口，默认 29598 |
| --dist-port | 否 | Decode HCCL 进程组 TCPStore 端口，默认 29599 |
| --num-layers | 否 | KV cache 层数，默认 32 |
| --num-blocks | 否 | 每层物理块数，默认 128 |
| --block-size | 否 | 每块 token 数，默认 128 |
| --kv-lora-rank | 否 | MLA kv_lora_rank（k 维），默认 512 |
| --qk-rope-head-dim | 否 | MLA qk_rope_head_dim（v 维），默认 64 |
| --dram-pool-gb | 否 | Decode 共享 offload DRAM 池大小（GiB），默认 2 |
| --warmup | 否 | 预热轮数（不计入统计），默认 1 |
| --iterations | 否 | 测量轮数（报均值/最好带宽），默认 3 |
| --seed | 否 | mock 数据随机种子，默认 2026 |
| --log-level | 否 | 日志级别：0 (debug)、1 (info)、2 (warn)、3 (error)，默认 2 |
| --timeout | 否 | 控制通道超时（秒），默认 120 |

约束：`num_layers * 2` 不得超过 256（底层 HCCL 注册 region 上限）；Decode 多 rank 必须在同一台机器上
（共享 DRAM 池要求）。

### 运行步骤

单机快速验证两侧均可省略 `--local-ip` 与 `--prefill-ip`；跨机部署两侧都须传对端可达的 `--local-ip`（P 侧地址会
进入 session id 供 D 回连），Decode 侧还须传真实 `--prefill-ip`。

#### 1. 启动 Prefill 侧（store server）

```bash
python test_transfer_engine_rd2h.py --role Prefill --decode-tp-size 2 --npu-id 0
```

#### 2. 启动 Decode 侧（一条命令拉起全部 rank）

Decode 与 Prefill 不能共用同一张卡，需通过 `--npu-id` 指定起始卡号（须与 Prefill 所用卡不同，例如 Prefill 用
`--npu-id 0`，Decode 用 `--npu-id 1`，rank r 实际使用 `--npu-id + r`）：

```bash
python test_transfer_engine_rd2h.py --role Decode --npu-id 1 --tp-size 2
```

手动指定 rank 单独启动（等价上一条命令）或调整测量轮数：

```bash
python test_transfer_engine_rd2h.py --role Decode --npu-id 1 --tp-rank 1 --tp-size 2                # 指定 rank
python test_transfer_engine_rd2h.py --role Decode --npu-id 1 --tp-size 2 --warmup 2 --iterations 10 # 调轮数
```

> 说明：`--npu-id` 在 launcher 模式（不传 `--tp-rank`）作为起始卡号，rank r 用 `--npu-id + r`；手动 `--tp-rank` 模式下
> 可选，默认等于 rank，也可显式指定任意卡号（rank 与卡号不绑定）。

#### 3. 观察结果

两侧均输出 `PASS` 即成功，例如：

```text
[P] PASS: layers=32, decode_ranks=2, bytes=603979776/round, iterations=3,
    transfer_bw=23.81 GiB/s (best 24.35), d2h_demo_ok=True
[D1] PASS: pulled bytes=301989888/round, blocks=[64,128), transfer_bw=23.81 GiB/s (best 24.35), d2h_demo_ok=True
```

* `bytes/round`：单轮拉取总字节数（各 rank 分片之和）
* `transfer_bw`：纯传输带宽（仅计时 `batch_transfer_sync_read`，不含描述符构建），多轮取均值，括号内为最好一轮
* P 侧会打印 `desc X.XXXs, transfer X.XXXs per round`，可观察描述符构建开销占比；校验失败以 `verification failed`
  报错退出，不会输出 PASS

### 工作流程说明

控制协议（P 与每个 D rank 之间的 TCP JSON 通道）：

```text
hello → init → pool_ready → meta → pull_done(各轮) → verify → final
```

1. **握手**：各 D rank 连接 P 并上报 tp_rank；P 校验 rank 集合完整
2. **共享池建立**：P 下发 `init`；各 rank 初始化 offload 共享池（内部 GroupBarrier 会合），rank0 分配 CPU 池张量，
   基地址经 HCCL broadcast 同步到所有 rank，随后 rank0 向 P 上报 `pool_ready`
3. **元数据下发**：P 将每层 KV 基地址、块长、非连续 block table（seed 确定的 randperm）等打包为 `meta` 广播下发
4. **分片拉取**：各 rank 按 `tp_block_range` 划分的块区间构造读描述符（按 block table 映射源地址，连续块自动合并），
   调用 `batch_transfer_sync_read` 从 P 的 HBM 拉入共享池；warmup 轮静默执行，测量轮分段计时
5. **全量校验**：所有轮完成后 rank0 用相同 seed 重放的期望 KV 对整个共享池逐层逐块 `torch.equal` 比对，
   并运行 `sparse_copy` D2H 演示（失败仅告警）
6. **收尾**：P 汇总各 rank 各轮纯传输用时（按轮取最慢 rank），计算均值/最好带宽并下发 `final`
