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

/* This sample demonstrates the smem_trans_* batch copy flow defined in
 * src/smem/include/host/smem_trans.h.
 *
 * Flow:
 *   1. The main process creates 2 child processes via fork().
 *   2. Each child calls smem_trans_init + smem_trans_create with deviceId 0 / 1,
 *      the url uses ip 127.0.0.1 and port 0 (auto detected by the config store).
 *   3. Each child allocates 2M-aligned 1G host memory (posix_memalign) and
 *      registers it via smem_trans_register_mem.
 *   4. The two children synchronize and exchange the registered base address and
 *      the actual url (127.0.0.1:<port>) through a unix socketpair.
 *   5. Both children call smem_trans_batch_copy to write 2M from their own
 *      registered memory to the peer's exchanged address, at different offsets
 *      (rank 0 writes offset 0, rank 1 writes offset 2M). Before the copy, the
 *      source 2M is filled with a rank/position pattern directly in host memory.
 *   6. Each child polls smem_trans_batch_status_query until the write is done.
 *   7. After both writes finish, they synchronize again.
 *   8. Each child checks that the data written by the peer matches the expected
 *      pattern.
 *   9. Each child also submits a local copy whose destUrl is its own url and
 *      dest is its own registered address (offset 8M + rank*2M), exercising the
 *      self-connection path, then waits and verifies the local result.
*/

#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "smem.h"
#include "smem_trans.h"

namespace {
constexpr uint32_t RANK_COUNT = 2;
constexpr uint64_t ALIGN_SIZE = 2ULL * 1024ULL * 1024ULL;             /* 2M, NPU huge page size */
constexpr uint64_t MEM_CAPACITY = 1ULL * 1024ULL * 1024ULL * 1024ULL; /* 1G */
constexpr uint64_t COPY_SIZE = 2ULL * 1024ULL * 1024ULL;              /* 2M, size of each copy */
constexpr int POLL_INTERVAL_US = 100 * 1000;                          /* 100ms */
constexpr int POLL_MAX_RETRY = 1200;                                  /* 120s timeout */
/* smem_trans_batch_status_query return codes (not exported in public headers): 0 = task finished and
 * its result was written to the out param, SM_NEED_RETRY = still pending, other negative = error. */
constexpr int SM_TRANS_QUERY_OK = 0;
constexpr int SM_TRANS_QUERY_PENDING = -2015;
constexpr int SYNC_TIMEOUT_SEC = 120;

/* Info exchanged between the two children so that each can reach the peer. */
struct PeerInfo {
    uint64_t addr; /* local registered base address (mapped by the peer via trans) */
    char url[64];  /* actual url after auto port detection, format: ip:port */
};

static void LogInfo(uint32_t rank, const char *fmt, ...)
{
    printf("[rank %u] ", rank);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void LogErr(uint32_t rank, const char *fmt, ...)
{
    printf("[rank %u][ERR] ", rank);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static bool WriteAll(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, static_cast<const char *>(buf) + done, len - done);
        if (n < 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static bool ReadAll(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, static_cast<char *>(buf) + done, len - done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

/* Simple barrier: both sides send and receive a byte token. */
static int BarrierPeer(int fd, uint32_t rank)
{
    uint8_t token = static_cast<uint8_t>(rank + 1);
    if (!WriteAll(fd, &token, 1)) {
        LogErr(rank, "barrier write failed, fd %d, errno %d (%s)", fd, errno, strerror(errno));
        return -1;
    }
    uint8_t peer = 0;
    if (!ReadAll(fd, &peer, 1)) {
        LogErr(rank, "barrier read failed, fd %d, errno %d (%s)", fd, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int ExchangePeerInfo(int fd, uint32_t rank, const PeerInfo &mine, PeerInfo &peer)
{
    if (!WriteAll(fd, &mine, sizeof(mine))) {
        LogErr(rank, "exchange write failed, fd %d, errno %d (%s)", fd, errno, strerror(errno));
        return -1;
    }
    if (!ReadAll(fd, &peer, sizeof(peer))) {
        LogErr(rank, "exchange read failed, fd %d, errno %d (%s)", fd, errno, strerror(errno));
        return -1;
    }
    LogInfo(rank, "exchanged peer info, addr 0x%lx, url %s", static_cast<unsigned long>(peer.addr), peer.url);
    return 0;
}

static void SetRecvTimeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = SYNC_TIMEOUT_SEC;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Pattern rule: element i = ((rank + 1) << 48) | i, encodes both rank and position. */
static void FillPattern(uint64_t *host, uint32_t rank, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        host[i] = (static_cast<uint64_t>(rank + 1) << 48) | static_cast<uint64_t>(i);
    }
}

static int VerifyPattern(const uint64_t *host, uint32_t peerRank, size_t count, uint32_t rank)
{
    for (size_t i = 0; i < count; i++) {
        uint64_t expected = (static_cast<uint64_t>(peerRank + 1) << 48) | static_cast<uint64_t>(i);
        if (host[i] != expected) {
            LogErr(rank, "verify failed at index %zu, peerRank %u, expected 0x%lx, got 0x%lx", i, peerRank,
                   static_cast<unsigned long>(expected), static_cast<unsigned long>(host[i]));
            return -1;
        }
    }
    LogInfo(rank, "verify success, %zu elements match peer(rank %u) pattern", count, peerRank);
    return 0;
}

/* Init smem trans. On failure returns non-zero and leaves cleanup to the caller. */
static int SetupTrans(uint32_t rank, uint32_t deviceId, smem_trans_t &handle, char (&myUrl)[64])
{
    smem_set_conf_store_tls(false, nullptr, 0);
    smem_set_log_level(2); /* WARN, keeps the output clean */

    smem_trans_config_t config;
    int32_t ret = smem_trans_config_init(&config);
    if (ret != 0) {
        LogErr(rank, "smem_trans_config_init failed, ret %d", ret);
        return ret;
    }
    config.role = SMEM_TRANS_BOTH; /* both sides send and receive */
    config.deviceId = deviceId;
    config.dataOpType = SMEMB_DATA_OP_DEVICE_RDMA;                 /* device SDMA, no HCOM/RDMA dependency */
    (void)snprintf(config.url, sizeof(config.url), "127.0.0.1:0"); /* port 0 => auto detection */

    ret = smem_trans_init(deviceId, 0);
    if (ret != 0) {
        LogErr(rank, "smem_trans_init failed, ret %d, deviceId %u", ret, deviceId);
        return ret;
    }
    handle = smem_trans_create(&config, 0);
    if (handle == nullptr) {
        LogErr(rank, "smem_trans_create failed, url %s, deviceId %u", config.url, deviceId);
        return -1;
    }
    uint16_t port = 0;
    ret = smem_trans_get_rpc_port(handle, &port);
    if (ret != 0) {
        LogErr(rank, "smem_trans_get_rpc_port failed, ret %d", ret);
        return ret;
    }
    (void)snprintf(myUrl, sizeof(myUrl), "127.0.0.1:%u", port);
    LogInfo(rank, "trans created, url %s, deviceId %u", myUrl, deviceId);
    return 0;
}

/* Allocate 2M-aligned 1G host memory and register it. posix_memalign guarantees
 * the returned pointer is 2M-aligned, so raw == base. */
static int AllocAndRegister(uint32_t rank, smem_trans_t handle, void *&raw, void *&base)
{
    int rc = posix_memalign(&raw, ALIGN_SIZE, MEM_CAPACITY);
    if (rc != 0) {
        LogErr(rank, "posix_memalign failed, rc %d, align %zu, size %zu", rc, ALIGN_SIZE, MEM_CAPACITY);
        return -1;
    }
    base = raw; /* posix_memalign already guarantees ALIGN_SIZE alignment */
    int32_t ret = smem_trans_register_mem(handle, base, MEM_CAPACITY, 0);
    if (ret != 0) {
        LogErr(rank, "smem_trans_register_mem failed, ret %d, base %p, size %zu", ret, base, MEM_CAPACITY);
        return -1;
    }
    LogInfo(rank, "alloc and register done, base %p size %zu", base, MEM_CAPACITY);
    return 0;
}

/* Fill the source 2M with the rank pattern directly in host memory and submit
 * an async batch write to the peer. */
static int FillAndCopy(uint32_t rank, smem_trans_t handle, void *localBase, void *peerBase, const char *peerUrl,
                       uint64_t &batchId)
{
    const size_t count = COPY_SIZE / sizeof(uint64_t);
    void *src = static_cast<uint8_t *>(localBase) + rank * COPY_SIZE;
    FillPattern(static_cast<uint64_t *>(src), rank, count);

    void *dst = static_cast<uint8_t *>(peerBase) + rank * COPY_SIZE;
    void *srcList[1] = {src};
    void *dstList[1] = {dst};
    size_t sizeList[1] = {COPY_SIZE};
    smem_trans_batch_params_t params{};
    params.destUrl = peerUrl;
    params.srcList = srcList;
    params.destList = dstList;
    params.sizeList = sizeList;
    params.batchSize = 1;
    params.opcode = SMEM_TRANS_OP_WRITE;
    params.stream = nullptr; /* synchronous in the executor, status reflects real completion */
    params.flags = 0;
    int32_t ret = smem_trans_batch_copy(handle, &params, &batchId);
    if (ret != 0) {
        LogErr(rank, "smem_trans_batch_copy failed, ret %d, destUrl %s", ret, peerUrl);
        return -1;
    }
    LogInfo(rank, "batch copy submitted, batchId %lu, src %p dst %p size %zu", static_cast<unsigned long>(batchId), src,
            dst, COPY_SIZE);
    return 0;
}

static int WaitForDone(uint32_t rank, smem_trans_t handle, uint64_t batchId)
{
    for (int i = 0; i < POLL_MAX_RETRY; i++) {
        int32_t result = 0;
        int32_t ret = smem_trans_batch_status_query(handle, batchId, &result, 0);
        if (ret == SM_TRANS_QUERY_OK) {
            if (result != 0) {
                LogErr(rank, "batch %lu failed, result %d", static_cast<unsigned long>(batchId), result);
                return -1;
            }
            LogInfo(rank, "batch %lu done", static_cast<unsigned long>(batchId));
            return 0;
        }
        if (ret != SM_TRANS_QUERY_PENDING) {
            LogErr(rank, "batch status query failed, ret %d, batchId %lu", ret, static_cast<unsigned long>(batchId));
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }
    LogErr(rank, "batch %lu timeout", static_cast<unsigned long>(batchId));
    return -1;
}

/* Verify the peer's written data directly from the registered host memory. */
static int VerifyPeerData(uint32_t rank, void *base, uint32_t peerRank)
{
    const size_t count = COPY_SIZE / sizeof(uint64_t);
    void *seg = static_cast<uint8_t *>(base) + peerRank * COPY_SIZE;
    return VerifyPattern(static_cast<const uint64_t *>(seg), peerRank, count, rank);
}

/* Local copy slot layout inside the registered 1G buffer (peer copy uses slots 0/1):
 *   src slot = (RANK_COUNT + rank) * COPY_SIZE      (offset 4M + rank*2M)
 *   dst slot = (2 * RANK_COUNT + rank) * COPY_SIZE  (offset 8M + rank*2M)
 * so the local copies never overlap the peer-copy segments nor each other. */
static uint64_t LocalCopySrcOffset(uint32_t rank)
{
    return (RANK_COUNT + rank) * COPY_SIZE;
}

static uint64_t LocalCopyDstOffset(uint32_t rank)
{
    return (2 * RANK_COUNT + rank) * COPY_SIZE;
}

/* Fill the local source 2M with the own pattern and submit a batch write whose
 * destUrl is the own url and dest is the own registered address: the copy goes
 * through the self-connection path (dest transformed to the local slice gva). */
static int FillAndCopyLocal(uint32_t rank, smem_trans_t handle, void *base, const char *myUrl, uint64_t &batchId)
{
    const size_t count = COPY_SIZE / sizeof(uint64_t);
    void *src = static_cast<uint8_t *>(base) + LocalCopySrcOffset(rank);
    void *dst = static_cast<uint8_t *>(base) + LocalCopyDstOffset(rank);
    memset(dst, 0, COPY_SIZE);
    FillPattern(static_cast<uint64_t *>(src), rank, count);

    void *srcList[1] = {src};
    void *dstList[1] = {dst};
    size_t sizeList[1] = {COPY_SIZE};
    smem_trans_batch_params_t params{};
    params.destUrl = myUrl; /* own url: dest address is local */
    params.srcList = srcList;
    params.destList = dstList;
    params.sizeList = sizeList;
    params.batchSize = 1;
    params.opcode = SMEM_TRANS_OP_WRITE;
    params.stream = nullptr;
    params.flags = 0;
    int32_t ret = smem_trans_batch_copy(handle, &params, &batchId);
    if (ret != 0) {
        LogErr(rank, "local smem_trans_batch_copy failed, ret %d, destUrl %s", ret, myUrl);
        return -1;
    }
    LogInfo(rank, "local batch copy submitted, batchId %lu, src %p dst %p size %zu",
            static_cast<unsigned long>(batchId), src, dst, COPY_SIZE);
    return 0;
}

/* Verify the local copy result directly from the registered host memory. */
static int VerifyLocalData(uint32_t rank, void *base)
{
    const size_t count = COPY_SIZE / sizeof(uint64_t);
    void *seg = static_cast<uint8_t *>(base) + LocalCopyDstOffset(rank);
    return VerifyPattern(static_cast<const uint64_t *>(seg), rank, count, rank);
}

/* Self copy phase: write to the own registered address via the own url, wait
 * for completion and verify the local result. No peer interaction needed. */
static int RunLocalCopyPhase(uint32_t rank, smem_trans_t handle, void *base, const char *myUrl)
{
    uint64_t batchId = 0;
    int ret = FillAndCopyLocal(rank, handle, base, myUrl, batchId);
    if (ret != 0) {
        return ret;
    }
    ret = WaitForDone(rank, handle, batchId);
    if (ret != 0) {
        return ret;
    }
    return VerifyLocalData(rank, base);
}

/* Exchange addresses, run the batch copy, wait for it, sync, and verify the peer data. */
static int RunCopyPhase(uint32_t rank, smem_trans_t handle, void *base, const char *myUrl, int sock)
{
    PeerInfo mine = {};
    PeerInfo peer = {};
    mine.addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(base));
    (void)snprintf(mine.url, sizeof(mine.url), "%s", myUrl);
    if (BarrierPeer(sock, rank) != 0) {
        return -1;
    }
    if (ExchangePeerInfo(sock, rank, mine, peer) != 0) {
        return -1;
    }
    uint64_t batchId = 0;
    int ret = FillAndCopy(rank, handle, base, reinterpret_cast<void *>(peer.addr), peer.url, batchId);
    if (ret != 0) {
        return ret;
    }
    ret = WaitForDone(rank, handle, batchId);
    if (ret != 0) {
        return ret;
    }
    if (BarrierPeer(sock, rank) != 0) {
        return -1;
    }
    return VerifyPeerData(rank, base, RANK_COUNT - 1 - rank);
}

static int RunChild(uint32_t rank, uint32_t deviceId, int sock)
{
    smem_trans_t handle = nullptr;
    void *raw = nullptr;
    void *base = nullptr;
    char myUrl[64] = {0};
    int ret = SetupTrans(rank, deviceId, handle, myUrl);
    if (ret != 0) {
        goto trans;
    }
    ret = AllocAndRegister(rank, handle, raw, base);
    if (ret != 0) {
        goto mem;
    }
    ret = RunCopyPhase(rank, handle, base, myUrl, sock);
    if (ret == 0) {
        ret = RunLocalCopyPhase(rank, handle, base, myUrl);
    }

mem:
    if (raw != nullptr) {
        free(raw);
    }
trans:
    if (handle != nullptr) {
        smem_trans_destroy(handle, 0);
    }
    smem_trans_uninit(0);
    return ret;
}

/* Fork one child that keeps keepFd and closes closeFd. Returns child pid, or -1 on fork error. */
static pid_t ForkChild(uint32_t rank, uint32_t deviceId, int keepFd, int closeFd)
{
    pid_t pid = fork();
    if (pid < 0) {
        LogErr(rank, "fork failed, errno %d (%s)", errno, strerror(errno));
        return -1;
    }
    if (pid != 0) {
        return pid;
    }
    close(closeFd);
    int ret = RunChild(rank, deviceId, keepFd);
    close(keepFd);
    _exit(ret == 0 ? 0 : 1);
}

static void WaitChild(pid_t pid, const char *name, int &exitCode)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[main] waitpid %s failed\n", name);
        exitCode = -1;
        return;
    }
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
        printf("[main] %s exited with code %d\n", name, exitCode);
    } else {
        exitCode = -1;
        fprintf(stderr, "[main] %s abnormal exit, status %d\n", name, status);
    }
}
} /* namespace */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    (void)signal(SIGPIPE, SIG_IGN); /* writes return EPIPE instead of killing the process */

    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fprintf(stderr, "[main] socketpair failed\n");
        return 1;
    }
    SetRecvTimeout(sv[0]);
    SetRecvTimeout(sv[1]);

    pid_t pid0 = ForkChild(0, 0, sv[0], sv[1]);
    if (pid0 < 0) {
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    pid_t pid1 = ForkChild(1, 1, sv[1], sv[0]);
    if (pid1 < 0) {
        close(sv[0]);
        close(sv[1]);
        kill(pid0, SIGTERM);
        waitpid(pid0, nullptr, 0);
        return 1;
    }
    /* parent: no longer needs the socketpair, both children hold their own end */
    close(sv[0]);
    close(sv[1]);

    int code0 = 0;
    int code1 = 0;
    WaitChild(pid0, "child0(device0)", code0);
    WaitChild(pid1, "child1(device1)", code1);

    if (code0 == 0 && code1 == 0) {
        printf("[main] ===== batch copy demo PASSED =====\n");
        return 0;
    }
    fprintf(stderr, "[main] ===== batch copy demo FAILED (code0=%d, code1=%d) =====\n", code0, code1);
    return 1;
}
