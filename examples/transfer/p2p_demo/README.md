# p2p_demo

一个演示 SMEM TRANS API（`src/smem/include/host/smem_trans.h`）异步批量拷贝流程的 C++
示例。

两个子进程（各自使用不同的 NPU 设备号）通过 `posix_memalign` 分配 2M 对齐的 1G
**主机内存**并注册，互相交换注册地址和 TRANS url，随后各自调用
`smem_trans_batch_copy` 同时向对端写入 2M 模式数据，轮询完成状态，最后校验对端
写入的数据。每个子进程还会额外提交一次**本地拷贝**：`destUrl` 为自身 url、目的地址
为本进程注册的内存地址，用于验证自连接（self-connection）拷贝路径。

## 前置条件

| 依赖 | 环境变量 | 说明 |
|------|---------|------|
| memfabric_hybrid 运行包 | `MEMFABRIC_HYBRID_HOME_PATH` | `bash script/build_and_pack_run.sh` 打出的 `.run` 包解压后的安装根目录 |
| NPU 设备 | 至少 2 张可用 | 两个子进程分别使用设备 0 和设备 1 |

## 编译

```bash
source /usr/local/memfabric_hybrid/set_env.sh
bash build.sh
```

产物安装到 `out/bin/p2p_demo`。

## 运行

```bash
bash run.sh
```

`run.sh` 只依赖 `MEMFABRIC_HYBRID_HOME_PATH`，会自动把 memfabric 的 `lib64` 目录加入
`LD_LIBRARY_PATH`。若运行时找不到驱动动态库，请将 CANN/NPU 驱动库路径一并追加到
`LD_LIBRARY_PATH`。

## 流程

```
主进程
  ├── socketpair(AF_UNIX, SOCK_STREAM)        # 两个子进程间的双向通信通道
  ├── fork child0 -> 设备 0（rank 0）
  ├── fork child1 -> 设备 1（rank 1）
  └── waitpid 两个子进程，汇总 PASS/FAIL

每个子进程（rank r，deviceId r）：
  smem_trans_config_init  -> role=BOTH, dataOpType=SDMA, url="127.0.0.1:0"（自动选端口）
  smem_trans_init(r)
  smem_trans_create(&config)
  smem_trans_get_rpc_port  -> 拼出真实 url "127.0.0.1:<port>"
  posix_memalign(1G, 2M)   -> base            # 2M 对齐的 1G 主机内存
  smem_trans_register_mem(base, 1G)
  ---- barrier（同步） ----
  与对端交换 {base, url}
  向源 2M（base + r*2M）直接写入 rank/位置模式（主机内存直写）
  smem_trans_batch_copy(WRITE, src=base+r*2M, dst=peerBase+r*2M, size=2M) -> batchId
  轮询 smem_trans_batch_status_query(batchId) 直至 DONE
  ---- barrier（同步） ----
  校验对端数据：读 base+(1-r)*2M，与对端模式比对
  向本地源 2M（base+4M+r*2M）直接写入自身模式（主机内存直写）
  smem_trans_batch_copy(WRITE, destUrl=自身 url, src=base+4M+r*2M, dst=base+8M+r*2M, size=2M)
  轮询 smem_trans_batch_status_query(batchId) 直至 DONE
  校验本地数据：读 base+8M+r*2M，与自身模式比对
  destroy / uninit / free
```

rank 0 写偏移 `0`，rank 1 写偏移 `2M`，两次对端写入落在对端 1G 缓冲中两个互不相交
的 2M 段上。本地拷贝再使用两个互不相交的段：源位于偏移 `4M + r*2M`，目的位于偏移
`8M + r*2M`，不会与对端拷贝数据重叠。模式规则为：

```
element[i] = ((peer_rank + 1) << 48) | i
```

其中同时编码了源 rank 与位置信息，校验时可以确认数据来源正确且每个元素顺序无误。

## 预期输出

成功时程序打印（日志有删节）：

```
[rank 0] trans created, url 127.0.0.1:<port0>, deviceId 0
[rank 1] trans created, url 127.0.0.1:<port1>, deviceId 1
[rank 0] alloc and register done, base 0x... size 1073741824
[rank 1] alloc and register done, base 0x... size 1073741824
[rank 0] exchanged peer info, addr 0x..., url 127.0.0.1:<port1>
[rank 1] exchanged peer info, addr 0x..., url 127.0.0.1:<port0>
[rank 0] batch copy submitted, batchId ..., src 0x... dst 0x... size 2097152
[rank 1] batch copy submitted, batchId ..., src 0x... dst 0x... size 2097152
[rank 0] batch ... done
[rank 1] batch ... done
[rank 0] verify success, 262144 elements match peer(rank 1) pattern
[rank 1] verify success, 262144 elements match peer(rank 0) pattern
[rank 0] local batch copy submitted, batchId ..., src 0x... dst 0x... size 2097152
[rank 1] local batch copy submitted, batchId ..., src 0x... dst 0x... size 2097152
[rank 0] batch ... done
[rank 1] batch ... done
[rank 0] verify success, 262144 elements match peer(rank 0) pattern
[rank 1] verify success, 262144 elements match peer(rank 1) pattern
[main] child0(device0) exited with code 0
[main] child1(device1) exited with code 0
[main] ===== batch copy demo PASSED =====
```

退出码非 0 且出现 `FAILED` 行时，可根据日志定位是哪个 rank 失败。
