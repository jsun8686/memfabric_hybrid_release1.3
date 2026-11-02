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
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <thread>
#include <chrono>
#include "smem_types.h"
#include "smem_store_factory.h"
#include "smem_trans.h"
#include "dl_acl_api.h"
#include "dl_api.h"
#include "hybm_big_mem.h"
#include "hybm_entity_default.h"
#include "hybm_asymmetric_mem_segment.h"
#include "hybm_mem_segment.h"
#define private public // NOLINT(google-runtime-int) test hack: expose private members
#include "smem_trans/smem_trans_entry.h"
#include "smem_trans/smem_trans_entry_manager.h"
#undef private
#include "hybm_va_manager.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::smem;
using namespace ock::mf;
using ClassifyAddressMaskMemberFn = uint8_t (HybmVaManager::*)(const uint64_t);

const char STORE_URL[] = "tcp://127.0.0.1:5432";
const char UNIQUE_ID[] = "127.0.0.1:5321";
const char STORE_URL_IPV6[] = "tcp://[::1]:5432";
const char UNIQUE_IPV6_ID[] = "[::1]:5321";
constexpr size_t REGISTER_MEM_ELEM_COUNT = 10;
constexpr int MAX_TEST_ATTEMPTS = 10;
constexpr int TRANS_RETRY_COUNT = 120;
constexpr int TRANS_RETRY_INTERVAL_MS = 100;
constexpr uint64_t K_SLICE1_ADDR = 0x1000;
constexpr uint64_t K_SLICE1_SIZE = 4096;
constexpr uint64_t K_SLICE2_ADDR = 0x2000;
constexpr uint64_t K_SLICE2_SIZE = 8192;
constexpr uint32_t K_PEER_RANK_ONE = 1;
constexpr uint32_t K_PEER_RANK_TWO = 2;
constexpr uint32_t K_HALF_DIVISOR = 2;
constexpr int STORE_CREATE_FAILED_EXIT_CODE = 10;
constexpr int STORE_CREATE_RETRY_FAILED_EXIT_CODE = 11;
const smem_trans_config_t g_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
const uint8_t MASK_MOCK_VAL = 3;    // BIT_LOCAL_HOST | BIT_GLOBAL_HOST → H2GH 方向（host transport 数据面）
constexpr int DUMMY_MEM_VALUE = 42; // dummy address value for deregister tests
constexpr int TRANS_SYNC_WAIT_MS = 500; // sleep for cross-process sync
// 单次尝试内等待对端就绪的上限：一次失败的 write/submit 内部会按 store 连接重试并阻塞数十秒，
// 只靠重试次数（TRANS_RETRY_COUNT）兜底会把失败尝试拖到几十分钟，故叠加墙钟上限
constexpr int TRANS_PEER_WAIT_TIMEOUT_MS = 30000;
// 父进程等待两个子 rank 的上限：超时强杀并回收，保证用例有界，且不残留占用固定端口的子进程
constexpr int TRANS_RANK_EXIT_TIMEOUT_MS = 60000;
constexpr int TRANS_RANK_WAIT_POLL_MS = 20;

// Build a transfer config for the current API (config carries the instance url; the
// per-instance config store server is created internally by smem_trans_create).
static smem_trans_config_t MakeTransConfig(smem_trans_role_t role, const char *url, uint32_t deviceId = 0,
                                           uint32_t dataOpType = 0)
{
    smem_trans_config_t config;
    (void)smem_trans_config_init(&config);
    config.role = role;
    config.initTimeout = SMEM_DEFAUT_WAIT_TIME;
    config.deviceId = deviceId;
    config.dataOpType = static_cast<smem_bm_data_op_type>(dataOpType);
    if (url != nullptr) {
        (void)snprintf(config.url, sizeof(config.url), "%s", url);
    }
    return config;
}

// Wait for a one-byte pipe signal with a wall-clock bound (poll based, so a peer that exits without
// signalling is detected through EOF instead of blocking forever).
// Returns 1 when the expected byte arrived, 0 when the peer closed the pipe, -1 on timeout/error.
static int WaitPipeSignal(int fd, char expected, uint32_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return -1;
        }
        auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd = {fd, POLLIN, 0};
        int rc = poll(&pfd, 1, static_cast<int>(remainMs));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        char byte = 0;
        auto n = read(fd, &byte, 1);
        if (n == 1) {
            return (byte == expected) ? 1 : 0;
        }
        if (n == 0) {
            return 0; // 对端已退出
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

// Wait for the two forked ranks of one attempt with a wall-clock bound: as soon as any rank exits
// abnormally the other one is killed (it would otherwise keep waiting for a dead peer), and if the
// deadline expires both are killed and reaped. Returns true only when both ranks exited 0 in time.
// exitCodes carries each rank's exit status (-1 when signalled or killed after the deadline).
static bool WaitRanksForAttempt(const pid_t *pids, uint32_t rankSize, uint32_t timeoutMs, std::array<int, 2> &exitCodes)
{
    for (auto &code : exitCodes) {
        code = -1;
    }
    std::vector<bool> reaped(rankSize, false);
    uint32_t reapedCount = 0;
    bool allZero = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (reapedCount < rankSize) {
        for (uint32_t i = 0; i < rankSize; ++i) {
            if (reaped[i]) {
                continue;
            }
            int status = 0;
            if (waitpid(pids[i], &status, WNOHANG) != pids[i]) {
                continue;
            }
            reaped[i] = true;
            ++reapedCount;
            if (WIFEXITED(status)) {
                exitCodes[i] = WEXITSTATUS(status);
            }
            if (exitCodes[i] != 0) {
                allZero = false;
            }
        }
        if (!allZero || reapedCount == rankSize) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            allZero = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(TRANS_RANK_WAIT_POLL_MS));
    }
    if (reapedCount < rankSize) {
        for (uint32_t i = 0; i < rankSize; ++i) {
            if (reaped[i]) {
                continue;
            }
            int status = 0;
            kill(pids[i], SIGKILL);
            waitpid(pids[i], &status, 0);
            reaped[i] = true;
            exitCodes[i] = -1;
            // 强杀避免残留子进程继续占用固定端口（否则会污染后续尝试与后续用例）
            std::cout << "[trans] rank " << i << " force killed, not exited within " << timeoutMs << " ms" << std::endl;
        }
        allZero = false;
    }
    return allZero;
}

// The host transport (HCOM/HOST_TCP) derives its data-plane port from a process-local entity id
// (11000 + entityId), so co-located trans processes would otherwise bind the same port. Reserve the
// ids belonging to the lower ranks so every rank's two entities get disjoint ports.
static void ReserveRankEntityIds(uint32_t rank)
{
    for (uint32_t skip = 0; skip < rank * 2U; ++skip) {
        uint32_t skippedId = 0;
        (void)SmemTransEntryManager::Instance().AllocEntityId(skippedId);
    }
}

class SmemTransTest : public testing::Test {
public:
    static void SetUpTestCase();
    static void TearDownTestCase();
    void SetUp() override;
    void TearDown() override;
};

void SmemTransTest::SetUpTestCase() {}

void SmemTransTest::TearDownTestCase() {}

void SmemTransTest::SetUp() {}

void SmemTransTest::TearDown()
{
    GlobalMockObject::verify();
    GlobalMockObject::reset();
}

TEST_F(SmemTransTest, smem_trans_config_init_success)
{
    smem_trans_config_t config;
    EXPECT_EQ(smem_trans_config_init(&config), SM_OK);
}

TEST_F(SmemTransTest, smem_trans_config_init_failed_invalid_param)
{
    EXPECT_EQ(smem_trans_config_init(nullptr), SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_create_success)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        smem_tls_config tls;
        tls.tlsEnable = false;
        ock::smem::StoreFactory::SetTlsInfo(tls);

        int32_t ret = smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        if (ret != 0) {
            _exit(1u);
        }

        ret = smem_trans_init(0, 0);
        if (ret != 0) {
            _exit(2u);
        }

        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);
        if (handle == nullptr) {
            _exit(3u);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(0);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_create_success_ipv6)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        smem_tls_config tls;
        tls.tlsEnable = false;
        ock::smem::StoreFactory::SetTlsInfo(tls);

        int32_t ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        if (ret != 0) {
            exit(1);
        }

        ret = smem_trans_init(0, 0);
        if (ret != 0) {
            exit(2);
        }

        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_IPV6_ID), 0);
        if (handle == nullptr) {
            exit(3);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);

        exit(0);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_create_failed_invalid_param)
{
    // not initialized
    smem_trans_config_t validCfg = MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID);
    EXPECT_EQ(smem_trans_create(&validCfg, 0), nullptr);

    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, 0);

    // config == nullptr
    EXPECT_EQ(smem_trans_create(nullptr, 0), nullptr);
    // config->url is empty
    smem_trans_config_t emptyCfg = MakeTransConfig(SMEM_TRANS_SENDER, nullptr);
    EXPECT_EQ(smem_trans_create(&emptyCfg, 0), nullptr);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_invalid_param)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        EXPECT_EQ(ret, 0);
        // client connect to server when initializing
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);

        // handle = nullptr
        ret = smem_trans_register_mem(nullptr, address, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 1;
            goto cleanup;
        }
        // address = nullptr
        ret = smem_trans_register_mem(handle, nullptr, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // size = 0
        ret = smem_trans_register_mem(handle, address, 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }

    cleanup:
        delete[] address;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_invalid_param_ipv6)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        EXPECT_EQ(ret, 0);
        // client connect to server when initializing
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_IPV6_ID), 0);

        // handle = nullptr
        ret = smem_trans_register_mem(nullptr, address, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 1;
            goto cleanup;
        }
        // address = nullptr
        ret = smem_trans_register_mem(handle, nullptr, size, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // size = 0
        ret = smem_trans_register_mem(handle, address, 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }

    cleanup:
        delete[] address;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_failed_not_initialized)
{
    int *address = new int[REGISTER_MEM_ELEM_COUNT];
    size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);
    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);

    EXPECT_EQ(smem_trans_register_mem(handle, address, size, 0), SM_INVALID_PARAM);

    delete[] address;
}

TEST_F(SmemTransTest, smem_trans_register_mem_invalid_handle_nonnull)
{
    smem_trans_config_t config = g_trans_options;
    ASSERT_EQ(smem_trans_init(config.deviceId, 0), 0);

    int *address = new int[REGISTER_MEM_ELEM_COUNT];
    size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0x12345678);

    int32_t ret = smem_trans_register_mem(invalid_handle, address, size, 0);

    EXPECT_NE(ret, SM_OK);

    delete[] address;
    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_register_mem_duplicate_address)
{
    pid_t pid = fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        if (ret != 0) {
            _exit(1);
        }
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);
        if (handle == nullptr) {
            _exit(2);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret != SM_OK) {
            _exit(3);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret != SM_OK) {
            _exit(4); // 重复注册应复用既有 slice 并返回成功
        }
        smem_trans_deregister_mem(handle, address);
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        delete[] address;
        _exit(0);
    }

    int status;
    ASSERT_NE(waitpid(pid, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "Child exited with error code: " << WEXITSTATUS(status);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_success)
{
    pid_t pid = fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        int *address = new int[REGISTER_MEM_ELEM_COUNT];
        size_t size = REGISTER_MEM_ELEM_COUNT * sizeof(int);

        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        if (ret != 0) {
            _exit(1);
        }
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);
        if (handle == nullptr) {
            _exit(2);
        }
        ret = smem_trans_register_mem(handle, address, size, 0);
        if (ret != SM_OK) {
            _exit(3);
        }
        ret = smem_trans_deregister_mem(handle, address);
        if (ret != SM_OK) {
            _exit(4);
        }
        delete[] address;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        _exit(0);
    }

    int status;
    ASSERT_NE(waitpid(pid, &status, 0), -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "Child exited with error code: " << WEXITSTATUS(status);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_not_initialized)
{
    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);
    int dummy = 42;

    EXPECT_EQ(smem_trans_deregister_mem(handle, &dummy), SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    int dummy = 42;

    EXPECT_EQ(smem_trans_deregister_mem(nullptr, &dummy), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_deregister_mem_failed_null_address)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x12345678);

    EXPECT_EQ(smem_trans_deregister_mem(handle, nullptr), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_not_initialized)
{
    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_EQ(smem_trans_read(reinterpret_cast<smem_trans_t>(0x1000), local_buf, REMOTE_ID, remote_buf, 1, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_read_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_EQ(smem_trans_read(nullptr, local_buf, REMOTE_ID, remote_buf, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    char local_buf[1];
    const char remote_buf[1] = {0};
    auto handle = reinterpret_cast<smem_trans_t>(0x1000);

    EXPECT_EQ(smem_trans_read(handle, local_buf, nullptr, remote_buf, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_failed_get_entry_failed)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADBEEF);
    char local_buf[1];
    const char remote_buf[1] = {0};
    const char REMOTE_ID[] = "remote:123";

    EXPECT_NE(smem_trans_read(invalid_handle, local_buf, REMOTE_ID, remote_buf, 1, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_not_initialized)
{
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read(reinterpret_cast<smem_trans_t>(0x2000), local_addrs, REMOTE_ID, remote_addrs,
                                    data_sizes, 1, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read(nullptr, local_addrs, REMOTE_ID, remote_addrs, data_sizes, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x2000);
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};

    EXPECT_EQ(smem_trans_batch_read(handle, local_addrs, nullptr, remote_addrs, data_sizes, 1, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_failed_get_entry_failed)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xBADBAD);
    void *local_addrs[1] = {nullptr};
    const void *remote_addrs[1] = {nullptr};
    size_t data_sizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_NE(smem_trans_batch_read(invalid_handle, local_addrs, REMOTE_ID, remote_addrs, data_sizes, 1, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_not_initialized)
{
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(reinterpret_cast<smem_trans_t>(0x3000), data, REMOTE_ID, remote, 4, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(nullptr, data, REMOTE_ID, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x3000);
    const char data[] = "test";
    char remote[10] = {0};
    void *stream = reinterpret_cast<void *>(0x1234);

    EXPECT_EQ(smem_trans_write_submit(handle, data, nullptr, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_null_stream)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x3000);
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_write_submit(handle, data, REMOTE_ID, remote, 4, nullptr, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_submit_failed_get_entry_failed)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADCAFE);
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x5678);

    EXPECT_NE(smem_trans_write_submit(invalid_handle, data, REMOTE_ID, remote, 4, stream, 0), SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_not_initialized)
{
    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    int32_t ret =
        smem_trans_write(reinterpret_cast<smem_trans_t>(0x3000), data, REMOTE_ID, remote, sizeof(data) - 1, 0);

    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_write_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";

    ret = smem_trans_write(nullptr, data, REMOTE_ID, remote, sizeof(data) - 1, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    auto dummy_handle = reinterpret_cast<smem_trans_t>(0x3000);

    ret = smem_trans_write(dummy_handle, data, nullptr, remote, sizeof(data) - 1, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_write_failed_get_entry_failed)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    const char data[] = "test";
    char remote[10] = {0};
    const char REMOTE_ID[] = "remote";
    auto invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADCAFE);

    ret = smem_trans_write(invalid_handle, data, REMOTE_ID, remote, sizeof(data) - 1, 0);
    EXPECT_NE(ret, SM_OK);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_not_initialized)
{
    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1111);

    EXPECT_EQ(smem_trans_read_submit(reinterpret_cast<smem_trans_t>(0x4000), local, REMOTE_ID, remote, 4, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2222);

    EXPECT_EQ(smem_trans_read_submit(nullptr, local, REMOTE_ID, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x4000);
    char local[10] = {0};
    const char remote[] = "data";
    void *stream = reinterpret_cast<void *>(0x3333);

    EXPECT_EQ(smem_trans_read_submit(handle, local, nullptr, remote, 4, stream, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_failed_null_stream)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x4000);
    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_read_submit(handle, local, REMOTE_ID, remote, 4, nullptr, 0), SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    char local[10] = {0};
    const char remote[] = "data";
    const char REMOTE_ID[] = "127.0.0.1:9999";
    void *stream = reinterpret_cast<void *>(0x5555);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xDEADBEEF);

    int32_t result = smem_trans_read_submit(invalid_handle, local, REMOTE_ID, remote, sizeof(remote) - 1, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_not_initialized)
{
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x1111);

    EXPECT_EQ(smem_trans_batch_write_submit(reinterpret_cast<smem_trans_t>(0x6000), localAddrs, REMOTE_ID, remoteAddrs,
                                            dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2222);

    EXPECT_EQ(smem_trans_batch_write_submit(nullptr, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x6000);
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    void *stream = reinterpret_cast<void *>(0x3333);

    EXPECT_EQ(smem_trans_batch_write_submit(handle, localAddrs, nullptr, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_failed_null_stream)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0x6000);
    const void *localAddrs[1] = {nullptr};
    void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_write_submit(handle, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, nullptr, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    const size_t batchSize = 2;
    int localBuf0[10] = {1, 2, 3};
    int localBuf1[10] = {4, 5, 6};
    const void *localAddrs[batchSize] = {localBuf0, localBuf1};

    int remoteBuf0[10] = {0};
    int remoteBuf1[10] = {0};
    void *remoteAddrs[batchSize] = {remoteBuf0, remoteBuf1};

    size_t dataSizes[batchSize] = {sizeof(int) * 3, sizeof(int) * 3};
    const char *remoteUniqueId = "127.0.0.1:8888";
    void *stream = reinterpret_cast<void *>(0x7777);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xBADCAFE);

    int32_t result = smem_trans_batch_write_submit(invalid_handle, localAddrs, remoteUniqueId, remoteAddrs, dataSizes,
                                                   batchSize, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid (but non-null) handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_not_initialized)
{
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2001);

    EXPECT_EQ(smem_trans_batch_read_submit(reinterpret_cast<smem_trans_t>(0xA000), localAddrs, REMOTE_ID, remoteAddrs,
                                           dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_handle)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";
    void *stream = reinterpret_cast<void *>(0x2002);

    EXPECT_EQ(smem_trans_batch_read_submit(nullptr, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_remote_unique_id)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0xA001);
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    void *stream = reinterpret_cast<void *>(0x2003);

    EXPECT_EQ(smem_trans_batch_read_submit(handle, localAddrs, nullptr, remoteAddrs, dataSizes, 1, stream, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_failed_null_stream)
{
    int ret = smem_trans_init(0, 0);
    ASSERT_EQ(ret, SM_OK);

    auto handle = reinterpret_cast<smem_trans_t>(0xA002);
    void *localAddrs[1] = {nullptr};
    const void *remoteAddrs[1] = {nullptr};
    size_t dataSizes[1] = {0};
    const char REMOTE_ID[] = "remote";

    EXPECT_EQ(smem_trans_batch_read_submit(handle, localAddrs, REMOTE_ID, remoteAddrs, dataSizes, 1, nullptr, 0),
              SM_INVALID_PARAM);

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_read_submit_invalid_nonnull_handle)
{
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, SM_OK);

    const uint32_t batchSize = 2;
    int localBuf0[10] = {0};
    int localBuf1[10] = {0};
    void *localAddrs[batchSize] = {localBuf0, localBuf1};

    const int remoteBuf0[10] = {1, 2, 3};
    const int remoteBuf1[10] = {4, 5, 6};
    const void *remoteAddrs[batchSize] = {remoteBuf0, remoteBuf1};

    size_t dataSizes[batchSize] = {sizeof(int) * 3, sizeof(int) * 3};
    const char *remoteUniqueId = "127.0.0.1:9999";
    void *stream = reinterpret_cast<void *>(0x8888);

    smem_trans_t invalid_handle = reinterpret_cast<smem_trans_t>(0xF00DBEEF);

    int32_t result = smem_trans_batch_read_submit(invalid_handle, localAddrs, remoteUniqueId, remoteAddrs, dataSizes,
                                                  batchSize, stream, 0);

    EXPECT_NE(result, SM_OK) << "Expected failure due to invalid (non-null) handle";

    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_read_write)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    size_t capacities = 500 * sizeof(int);
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options, std::vector<int *> addrPtrs,
                   size_t capacities, const std::array<const char *, 2> unique_ids, const int *syncPipe) {
        trans_options.dataOpType = SMEMB_DATA_OP_HOST_TCP;
        auto classifyAddressMaskPtr = static_cast<ClassifyAddressMaskMemberFn>(&HybmVaManager::ClassifyAddressMask);
        MOCKER_CPP(classifyAddressMaskPtr, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(trans_options.deviceId, 0);
        if (ret != 0) {
            _exit(1);
        }
        ReserveRankEntityIds(rank);
        void *handle = nullptr;
        constexpr int createMaxRetry = 300;
        constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
        for (int retry = 0; retry < createMaxRetry; ++retry) {
            handle = smem_trans_create(
                &MakeTransConfig(trans_options.role, unique_ids[rank], trans_options.deviceId, SMEMB_DATA_OP_HOST_TCP),
                0);
            if (handle != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
        }
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_register_mem(handle, addrPtrs[rank], capacities, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        // trans 仅支持「先注册内存再 join」：接收端先发布已注册切片，发送端在其完成后再建链（join），
        // 这样对端切片由 join 携带，而不依赖 join 之后被忽略的 ADD_SLICES 广播。
        if (rank == 1) {
            char done = 'r';
            (void)write(syncPipe[1], &done, 1);
        }
        if (rank == 0) {
            char done = 0;
            ssize_t n = 0;
            do {
                n = read(syncPipe[0], &done, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1) {
                _exit(7);
            }
            // 留出时间让 server 持久化接收端切片，再发起 join
            std::this_thread::sleep_for(std::chrono::milliseconds(TRANS_SYNC_WAIT_MS));
        }

        if (rank == 0) {
            // Receiver registration and session publication are asynchronous across processes.
            // Retry briefly to avoid transient "session not found" flakiness.
            // 每次失败内部会按 store 连接阻塞重试数十秒，故以墙钟上限兜底，避免失败尝试拖到几十分钟
            constexpr int maxRetry = 120;
            const auto peerDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(TRANS_PEER_WAIT_TIMEOUT_MS);
            for (int retry = 0; retry < maxRetry && std::chrono::steady_clock::now() < peerDeadline; ++retry) {
                ret = smem_trans_write(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
            // 通知接收端传输已完成（rank0 提前失败时写端随进程退出关闭，接收端读到 EOF 同样能退出）
            char fin = 'd';
            (void)write(syncPipe[3], &fin, 1);
        }
        if (rank == 1) {
            // The receiver must stay registered while the sender's write retries run: the HOST_TCP
            // data plane writes over a live hcom channel, which is torn down as soon as the receiver
            // deregisters and exits, making every sender retry fail with "rankId is not connect".
            // 故改为等发送端的完成信号（固定 sleep 覆盖不到发送端建链/重试的耗时）；rank0 提前
            // 失败时其写端关闭，这里同样能通过 EOF 及时退出，不会把用例拖成假挂死。
            auto sig = WaitPipeSignal(syncPipe[2], 'd', TRANS_PEER_WAIT_TIMEOUT_MS);
            if (sig <= 0) {
                std::cout << "[trans] rank 1 wait peer done: sig=" << sig << std::endl;
            }
        }

        ret = smem_trans_deregister_mem(handle, addrPtrs[rank]);
        if (ret != SM_OK) {
            _exit(5);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"127.0.0.1:5321", "127.0.0.1:5322"}};
    std::vector<int *> addrPtrs = {sender_buffer, recv_buffer};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int maxAttempts = MAX_TEST_ATTEMPTS;
    for (int attempt = 0; attempt < maxAttempts && !testSuccess; ++attempt) {
        // syncPipe[0..1]: 接收端注册完成 → 发送端；syncPipe[2..3]: 发送端传输完成 → 接收端
        int syncPipe[4] = {-1, -1, -1, -1};
        if (pipe(&syncPipe[0]) != 0 || pipe(&syncPipe[2]) != 0) {
            continue;
        }
        pid_t pids[rankSize] = {0};
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                (void)prctl(PR_SET_PDEATHSIG, SIGKILL); // 父进程被强杀时不残留占用固定端口的子进程
                if (i == 0) {
                    close(syncPipe[1]); // rank0: 只保留 ready 读端与 done 写端
                    close(syncPipe[2]);
                } else {
                    close(syncPipe[0]); // rank1: 只保留 ready 写端与 done 读端
                    close(syncPipe[3]);
                }
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int storeCreateMaxRetry = TRANS_RETRY_COUNT;
                    constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
                    for (int retry = 0; retry < storeCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids, syncPipe);
                _exit(0);
            }
        }
        close(syncPipe[0]);
        close(syncPipe[1]);
        close(syncPipe[2]);
        close(syncPipe[3]);

        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }

        std::array<int, 2> exitCodes = {{-1, -1}};
        testSuccess = WaitRanksForAttempt(pids, rankSize, TRANS_RANK_EXIT_TIMEOUT_MS, exitCodes);
        if (!testSuccess) {
            std::cout << "[trans] attempt " << attempt << " failed: rank0 exit=" << exitCodes[0]
                      << " rank1 exit=" << exitCodes[1] << std::endl;
        }
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_write_ipv6)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    size_t capacities = 500 * sizeof(int);
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options, std::vector<int *> addrPtrs,
                   size_t capacities, const std::array<const char *, 2> unique_ids, const int *syncPipe) {
        trans_options.dataOpType = SMEMB_DATA_OP_HOST_TCP;
        auto classifyAddressMaskPtr = static_cast<ClassifyAddressMaskMemberFn>(&HybmVaManager::ClassifyAddressMask);
        MOCKER_CPP(classifyAddressMaskPtr, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(trans_options.deviceId, 0);
        if (ret != 0) {
            _exit(1);
        }
        ReserveRankEntityIds(rank);
        constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
        void *handle = nullptr;
        {
            constexpr int createMaxRetry = 120;
            for (int retry = 0; retry < createMaxRetry; ++retry) {
                handle = smem_trans_create(&MakeTransConfig(trans_options.role, unique_ids[rank],
                                                            trans_options.deviceId, SMEMB_DATA_OP_HOST_TCP),
                                           0);
                if (handle != nullptr) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
        }
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_register_mem(handle, addrPtrs[rank], capacities, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        // trans 仅支持「先注册内存再 join」：接收端先发布已注册切片，发送端等待后再建链（join），
        // 对端切片由 join 携带，不依赖 join 之后被忽略的 ADD_SLICES 广播。
        if (rank == 1) {
            char done = 'r';
            (void)write(syncPipe[1], &done, 1);
        }
        if (rank == 0) {
            char done = 0;
            ssize_t n = 0;
            do {
                n = read(syncPipe[0], &done, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1) {
                _exit(7);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(TRANS_SYNC_WAIT_MS));
        }

        if (rank == 0) {
            // 失败重试内部可能阻塞数十秒，故叠加墙钟上限，保证失败尝试有界退出
            constexpr int writeMaxRetry = TRANS_RETRY_COUNT;
            const auto peerDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(TRANS_PEER_WAIT_TIMEOUT_MS);
            for (int retry = 0; retry < writeMaxRetry && std::chrono::steady_clock::now() < peerDeadline; ++retry) {
                ret = smem_trans_write(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }

            uint64_t stream_cnt = 0;
            ret = smem_trans_write_submit(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities,
                                          reinterpret_cast<void *>(&stream_cnt), 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            // Note: the old "stream_cnt == 1" assertion relied on the mock CANN stub
            // (aclrtMemcpyAsync increments *stream). The HOST_TCP data plane goes
            // through ubs-comm TCP instead and is synchronous, so no async task is
            // ever submitted to an acl stream and the counter stays 0.

            ret = smem_trans_read(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities, 0);
            if (ret != SM_OK) {
                _exit(8);
            }
            stream_cnt = 0;
            ret = smem_trans_read_submit(handle, addrPtrs[0], unique_ids[1], addrPtrs[1], capacities,
                                         reinterpret_cast<void *>(&stream_cnt), 0);
            if (ret != SM_OK) {
                _exit(9);
            }
            // Same as above: HOST_TCP submit is synchronous, no stream counter.
            // 通知接收端传输已完成（rank0 提前失败时写端随进程退出关闭，接收端读到 EOF 同样能退出）
            char fin = 'd';
            (void)write(syncPipe[3], &fin, 1);
        }
        if (rank == 1) {
            // 等发送端完成信号再注销（rank0 提前失败时通过 EOF 退出）：固定 sleep 覆盖不到发送端
            // 建链/重试耗时，接收端过早退出会让发送端重试全部失败
            auto sig = WaitPipeSignal(syncPipe[2], 'd', TRANS_PEER_WAIT_TIMEOUT_MS);
            if (sig <= 0) {
                std::cout << "[trans] rank 1 wait peer done: sig=" << sig << std::endl;
            }
        }

        ret = smem_trans_deregister_mem(handle, addrPtrs[rank]);
        if (ret != SM_OK) {
            _exit(5);
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"[::]:5321", "[::]:5322"}};
    std::vector<int *> addrPtrs = {sender_buffer, recv_buffer};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int maxAttempts = MAX_TEST_ATTEMPTS;
    for (int attempt = 0; attempt < maxAttempts && !testSuccess; ++attempt) {
        // syncPipe[0..1]: 接收端注册完成 → 发送端；syncPipe[2..3]: 发送端传输完成 → 接收端
        int syncPipe[4] = {-1, -1, -1, -1};
        if (pipe(&syncPipe[0]) != 0 || pipe(&syncPipe[2]) != 0) {
            continue;
        }
        pid_t pids[rankSize] = {0};
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                (void)prctl(PR_SET_PDEATHSIG, SIGKILL); // 父进程被强杀时不残留占用固定端口的子进程
                if (i == 0) {
                    close(syncPipe[1]); // rank0: 只保留 ready 读端与 done 写端
                    close(syncPipe[2]);
                } else {
                    close(syncPipe[0]); // rank1: 只保留 ready 写端与 done 读端
                    close(syncPipe[3]);
                }
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int storeCreateMaxRetry = TRANS_RETRY_COUNT;
                    constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
                    for (int retry = 0; retry < storeCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids, syncPipe);
                _exit(0);
            }
        }
        close(syncPipe[0]);
        close(syncPipe[1]);
        close(syncPipe[2]);
        close(syncPipe[3]);

        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }
        std::array<int, 2> exitCodes = {{-1, -1}};
        testSuccess = WaitRanksForAttempt(pids, rankSize, TRANS_RANK_EXIT_TIMEOUT_MS, exitCodes);
        if (!testSuccess) {
            std::cout << "[trans] attempt " << attempt << " failed: rank0 exit=" << exitCodes[0]
                      << " rank1 exit=" << exitCodes[1] << std::endl;
        }
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_read_write)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    std::vector<void *> sender_addrPtrs = {sender_buffer};
    std::vector<void *> recv_addrPtrs = {recv_buffer};
    std::vector<size_t> capacities = {500 * sizeof(int)};
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options,
                   std::vector<std::vector<void *>> addrPtrs, std::vector<size_t> capacities,
                   const std::array<const char *, 2> unique_ids, const int *syncPipe) {
        trans_options.dataOpType = SMEMB_DATA_OP_HOST_TCP;
        auto classifyAddressMaskPtr = static_cast<ClassifyAddressMaskMemberFn>(&HybmVaManager::ClassifyAddressMask);
        MOCKER_CPP(classifyAddressMaskPtr, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(trans_options.deviceId, 0);
        if (ret != 0) {
            _exit(1);
        }
        ReserveRankEntityIds(rank);
        void *handle = nullptr;
        {
            constexpr int createMaxRetry = TRANS_RETRY_COUNT;
            constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
            for (int retry = 0; retry < createMaxRetry; ++retry) {
                handle = smem_trans_create(&MakeTransConfig(trans_options.role, unique_ids[rank],
                                                            trans_options.deviceId, SMEMB_DATA_OP_HOST_TCP),
                                           0);
                if (handle != nullptr) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
        }
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_batch_register_mem(handle, addrPtrs[rank].data(), capacities.data(), 1, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        // trans 仅支持「先注册内存再 join」：接收端先发布已注册切片，发送端等待后再建链（join），
        // 对端切片由 join 携带，不依赖 join 之后被忽略的 ADD_SLICES 广播。
        if (rank == 1) {
            char done = 'r';
            (void)write(syncPipe[1], &done, 1);
        }
        if (rank == 0) {
            char done = 0;
            ssize_t n = 0;
            do {
                n = read(syncPipe[0], &done, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1) {
                _exit(7);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(TRANS_SYNC_WAIT_MS));
        }

        if (rank == 0) {
            const void *srcAddr[] = {addrPtrs[0][0]};
            // 失败重试内部可能阻塞数十秒，故叠加墙钟上限，保证失败尝试有界退出
            constexpr int writeMaxRetry = TRANS_RETRY_COUNT;
            constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
            const auto peerDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(TRANS_PEER_WAIT_TIMEOUT_MS);
            for (int retry = 0; retry < writeMaxRetry && std::chrono::steady_clock::now() < peerDeadline; ++retry) {
                ret =
                    smem_trans_batch_write(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }

            uint64_t stream_cnt = 0;
            ret = smem_trans_batch_write_submit(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(),
                                                1, reinterpret_cast<void *>(&stream_cnt), 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            // Note: the old "stream_cnt == 1" assertion relied on the mock CANN stub
            // (aclrtMemcpyAsync increments *stream). The HOST_TCP data plane goes
            // through ubs-comm TCP instead and is synchronous, so no async task is
            // ever submitted to an acl stream and the counter stays 0.

            for (int retry = 0; retry < writeMaxRetry && std::chrono::steady_clock::now() < peerDeadline; ++retry) {
                ret =
                    smem_trans_batch_read(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
            stream_cnt = 0;
            ret = smem_trans_batch_read_submit(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1,
                                               reinterpret_cast<void *>(&stream_cnt), 0);
            if (ret != SM_OK) {
                _exit(6);
            }
            // Same as above: HOST_TCP submit is synchronous, no stream counter.
            // 通知接收端传输已完成（rank0 提前失败时写端随进程退出关闭，接收端读到 EOF 同样能退出）
            char fin = 'd';
            (void)write(syncPipe[3], &fin, 1);
        }
        if (rank == 1) {
            // 等发送端完成信号再注销（rank0 提前失败时通过 EOF 退出）：固定 sleep 覆盖不到发送端
            // 建链/重试耗时，接收端过早退出会让发送端重试全部失败
            auto sig = WaitPipeSignal(syncPipe[2], 'd', TRANS_PEER_WAIT_TIMEOUT_MS);
            if (sig <= 0) {
                std::cout << "[trans] rank 1 wait peer done: sig=" << sig << std::endl;
            }
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        _exit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"127.0.0.1:5321", "127.0.0.1:5322"}};
    std::vector<std::vector<void *>> addrPtrs = {sender_addrPtrs, recv_addrPtrs};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool needKillOthers = false;
    auto createStoreWithRetry = [](const char *storeUrl) {
        constexpr int storeCreateMaxRetry = TRANS_RETRY_COUNT;
        constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
        for (int retry = 0; retry < storeCreateMaxRetry; ++retry) {
            if (smem_create_config_store(storeUrl, SMEM_STORE_SKIP_RECOVER) == 0) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
        }
        _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
    };
    {
        // syncPipe[0..1]: 接收端注册完成 → 发送端；syncPipe[2..3]: 发送端传输完成 → 接收端
        int syncPipe[4] = {-1, -1, -1, -1};
        if (pipe(&syncPipe[0]) != 0) {
            syncPipe[0] = -1;
            syncPipe[1] = -1;
        }
        if (pipe(&syncPipe[2]) != 0) {
            syncPipe[2] = -1;
            syncPipe[3] = -1;
        }
        pid_t pids[rankSize];
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            EXPECT_NE(pids[i], -1);
            if (pids[i] == -1) {
                maxProcess = i;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                (void)prctl(PR_SET_PDEATHSIG, SIGKILL); // 父进程被强杀时不残留占用固定端口的子进程
                if (i == 0) {
                    close(syncPipe[1]); // rank0: 只保留 ready 读端与 done 写端
                    close(syncPipe[2]);
                } else {
                    close(syncPipe[0]); // rank1: 只保留 ready 写端与 done 读端
                    close(syncPipe[3]);
                }
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    createStoreWithRetry(STORE_URL);
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids, syncPipe);
                _exit(0);
            }
        }

        close(syncPipe[0]);
        close(syncPipe[1]);
        close(syncPipe[2]);
        close(syncPipe[3]);

        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                int status = 0;
                kill(pids[i], SIGKILL);
                waitpid(pids[i], &status, 0);
            }
            ASSERT_NE(needKillOthers, true);
        }

        std::array<int, 2> exitCodes = {{-1, -1}};
        bool allExitedZero = WaitRanksForAttempt(pids, rankSize, TRANS_RANK_EXIT_TIMEOUT_MS, exitCodes);
        EXPECT_EQ(allExitedZero, true) << "rank0 exit=" << exitCodes[0] << " rank1 exit=" << exitCodes[1];
    }
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_write_ipv6)
{
    uint32_t rankSize = 2;
    int *sender_buffer = new int[500];
    int *recv_buffer = new int[500];
    std::vector<void *> sender_addrPtrs = {sender_buffer};
    std::vector<void *> recv_addrPtrs = {recv_buffer};
    std::vector<size_t> capacities = {500 * sizeof(int)};
    smem_trans_config_t sender_trans_options = {SMEM_TRANS_SENDER, SMEM_DEFAUT_WAIT_TIME, 0, 0};
    smem_trans_config_t recv_trans_options = {SMEM_TRANS_RECEIVER, SMEM_DEFAUT_WAIT_TIME, 1, 0};

    auto func = [](uint32_t rank, uint32_t rankCount, smem_trans_config_t trans_options,
                   std::vector<std::vector<void *>> addrPtrs, std::vector<size_t> capacities,
                   const std::array<const char *, 2> unique_ids, const int *syncPipe) {
        trans_options.dataOpType = SMEMB_DATA_OP_HOST_TCP;
        auto classifyAddressMaskPtr = static_cast<ClassifyAddressMaskMemberFn>(&HybmVaManager::ClassifyAddressMask);
        MOCKER_CPP(classifyAddressMaskPtr, uint8_t(*)(HybmVaManager *, const uint64_t))
            .stubs()
            .will(returnValue(MASK_MOCK_VAL));
        int ret = smem_trans_init(trans_options.deviceId, 0);
        if (ret != 0) {
            _exit(1);
        }
        ReserveRankEntityIds(rank);
        constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
        void *handle = nullptr;
        {
            constexpr int createMaxRetry = 120;
            for (int retry = 0; retry < createMaxRetry; ++retry) {
                handle = smem_trans_create(&MakeTransConfig(trans_options.role, unique_ids[rank],
                                                            trans_options.deviceId, SMEMB_DATA_OP_HOST_TCP),
                                           0);
                if (handle != nullptr) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
        }
        if (handle == nullptr) {
            _exit(2);
        }

        ret = smem_trans_batch_register_mem(handle, addrPtrs[rank].data(), capacities.data(), 1, 0);
        if (ret != SM_OK) {
            _exit(3);
        }

        // trans 仅支持「先注册内存再 join」：接收端先发布已注册切片，发送端等待后再建链（join），
        // 对端切片由 join 携带，不依赖 join 之后被忽略的 ADD_SLICES 广播。
        if (rank == 1) {
            char done = 'r';
            (void)write(syncPipe[1], &done, 1);
        }
        if (rank == 0) {
            char done = 0;
            ssize_t n = 0;
            do {
                n = read(syncPipe[0], &done, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1) {
                _exit(7);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(TRANS_SYNC_WAIT_MS));
        }

        if (rank == 0) {
            const void *srcAddr[] = {addrPtrs[0][0]};
            // 失败重试内部可能阻塞数十秒，故叠加墙钟上限，保证失败尝试有界退出
            constexpr int writeMaxRetry = TRANS_RETRY_COUNT;
            const auto peerDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(TRANS_PEER_WAIT_TIMEOUT_MS);
            for (int retry = 0; retry < writeMaxRetry && std::chrono::steady_clock::now() < peerDeadline; ++retry) {
                ret =
                    smem_trans_batch_write(handle, srcAddr, unique_ids[1], addrPtrs[1].data(), capacities.data(), 1, 0);
                if (ret == SM_OK) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
            }
            if (ret != SM_OK) {
                _exit(4);
            }
            // 通知接收端传输已完成（rank0 提前失败时写端随进程退出关闭，接收端读到 EOF 同样能退出）
            char fin = 'd';
            (void)write(syncPipe[3], &fin, 1);
        }
        if (rank == 1) {
            // 等发送端完成信号再注销（rank0 提前失败时通过 EOF 退出）：固定 sleep 覆盖不到发送端
            // 建链/重试耗时，接收端过早退出会让发送端重试全部失败
            auto sig = WaitPipeSignal(syncPipe[2], 'd', TRANS_PEER_WAIT_TIMEOUT_MS);
            if (sig <= 0) {
                std::cout << "[trans] rank 1 wait peer done: sig=" << sig << std::endl;
            }
        }

        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
    };

    const std::array<const char *, 2> unique_ids = {{"[::]:5321", "[::]:5322"}};
    std::vector<std::vector<void *>> addrPtrs = {sender_addrPtrs, recv_addrPtrs};
    std::vector<smem_trans_config_t> trans_options = {sender_trans_options, recv_trans_options};

    bool testSuccess = false;
    constexpr int maxAttempts = 5;
    for (int attempt = 0; attempt < maxAttempts && !testSuccess; ++attempt) {
        // syncPipe[0..1]: 接收端注册完成 → 发送端；syncPipe[2..3]: 发送端传输完成 → 接收端
        int syncPipe[4] = {-1, -1, -1, -1};
        if (pipe(&syncPipe[0]) != 0 || pipe(&syncPipe[2]) != 0) {
            continue;
        }
        pid_t pids[rankSize] = {0};
        bool needKillOthers = false;
        uint32_t maxProcess = rankSize;
        for (uint32_t i = 0; i < rankSize; ++i) {
            pids[i] = fork();
            if (pids[i] == -1) {
                maxProcess = i;
                needKillOthers = true;
                break;
            }
            if (pids[i] == 0) {
                (void)prctl(PR_SET_PDEATHSIG, SIGKILL); // 父进程被强杀时不残留占用固定端口的子进程
                if (i == 0) {
                    close(syncPipe[1]); // rank0: 只保留 ready 读端与 done 写端
                    close(syncPipe[2]);
                } else {
                    close(syncPipe[0]); // rank1: 只保留 ready 写端与 done 读端
                    close(syncPipe[3]);
                }
                smem_set_conf_store_tls(false, nullptr, 0);
                if (i == 0) {
                    int ret = -1;
                    constexpr int storeCreateMaxRetry = TRANS_RETRY_COUNT;
                    constexpr int retryIntervalMs = TRANS_RETRY_INTERVAL_MS;
                    for (int retry = 0; retry < storeCreateMaxRetry; ++retry) {
                        ret = smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
                        if (ret == 0) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMs));
                    }
                    if (ret != 0) {
                        _exit(STORE_CREATE_RETRY_FAILED_EXIT_CODE);
                    }
                }
                func(i, rankSize, trans_options[i], addrPtrs, capacities, unique_ids, syncPipe);
                _exit(0);
            }
        }
        close(syncPipe[0]);
        close(syncPipe[1]);
        close(syncPipe[2]);
        close(syncPipe[3]);

        if (needKillOthers) {
            for (uint32_t i = 0; i < maxProcess; ++i) {
                if (pids[i] > 0) {
                    int status = 0;
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], &status, 0);
                }
            }
            continue;
        }
        std::array<int, 2> exitCodes = {{-1, -1}};
        testSuccess = WaitRanksForAttempt(pids, rankSize, TRANS_RANK_EXIT_TIMEOUT_MS, exitCodes);
        if (!testSuccess) {
            std::cout << "[trans] attempt " << attempt << " failed: rank0 exit=" << exitCodes[0]
                      << " rank1 exit=" << exitCodes[1] << std::endl;
        }
    }
    EXPECT_EQ(testSuccess, true);
    delete[] sender_buffer;
    delete[] recv_buffer;
}

TEST_F(SmemTransTest, smem_trans_batch_write_failed_invalid_param)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *srcPtr1 = new int[1000];
        int *srcPtr2 = new int[2000];
        std::vector<const void *> srcAddrPtrs = {srcPtr1, srcPtr2};
        int *destPtr1 = new int[5000];
        int *destPtr2 = new int[6000];
        std::vector<void *> destAddrPtrs = {destPtr1, destPtr2};
        std::vector<size_t> dataSizes = {128U, 128U};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);

        // handle = nullptr
        ret = smem_trans_batch_write(nullptr, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // srcAddresses = nullptr
        ret = smem_trans_batch_write(handle, nullptr, UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }
        // destUniqueId = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), nullptr, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 4;
            goto cleanup;
        }
        // destAddresses = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, nullptr, dataSizes.data(), dataSizes.size(),
                                     0);
        if (ret != SM_INVALID_PARAM) {
            flag = 5;
            goto cleanup;
        }
        // dataSizes = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), nullptr,
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 6;
            goto cleanup;
        }
        // batchSize = 0
        ret =
            smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_ID, destAddrPtrs.data(), dataSizes.data(), 0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 7;
            goto cleanup;
        }

    cleanup:
        delete[] srcPtr1;
        delete[] srcPtr2;
        delete[] destPtr1;
        delete[] destPtr2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        smem_destroy_config_store(STORE_URL);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_batch_write_failed_invalid_param_ipv6)
{
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *srcPtr1 = new int[1000];
        int *srcPtr2 = new int[2000];
        std::vector<const void *> srcAddrPtrs = {srcPtr1, srcPtr2};
        int *destPtr1 = new int[5000];
        int *destPtr2 = new int[6000];
        std::vector<void *> destAddrPtrs = {destPtr1, destPtr2};
        std::vector<size_t> dataSizes = {128U, 128U};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_IPV6_ID), 0);

        // handle = nullptr
        ret = smem_trans_batch_write(nullptr, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 2;
            goto cleanup;
        }
        // srcAddresses = nullptr
        ret = smem_trans_batch_write(handle, nullptr, UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 3;
            goto cleanup;
        }
        // destUniqueId = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), nullptr, destAddrPtrs.data(), dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 4;
            goto cleanup;
        }
        // destAddresses = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, nullptr, dataSizes.data(),
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 5;
            goto cleanup;
        }
        // dataSizes = nullptr
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), nullptr,
                                     dataSizes.size(), 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 6;
            goto cleanup;
        }
        // batchSize = 0
        ret = smem_trans_batch_write(handle, srcAddrPtrs.data(), UNIQUE_IPV6_ID, destAddrPtrs.data(), dataSizes.data(),
                                     0, 0);
        if (ret != SM_INVALID_PARAM) {
            flag = 7;
            goto cleanup;
        }

    cleanup:
        delete[] srcPtr1;
        delete[] srcPtr2;
        delete[] destPtr1;
        delete[] destPtr2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        smem_destroy_config_store(STORE_URL);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mems_success_receiver)
{
    smem_trans_config_t trans_options = g_trans_options;
    trans_options.role = SMEM_TRANS_RECEIVER;
    pid_t pid = fork();
    EXPECT_NE(pid, -1);

    if (pid == 0) {
        uint8_t flag = 0;
        int *address1 = new int[1000];
        int *address2 = new int[2000];
        std::vector<void *> addrPtrs = {address1, address2};
        std::vector<size_t> capacities = {1000 * sizeof(int), 2000 * sizeof(int)};

        // first create server
        smem_set_conf_store_tls(false, nullptr, 0);
        smem_create_config_store(STORE_URL, SMEM_STORE_SKIP_RECOVER);
        int ret = smem_trans_init(0, 0);
        EXPECT_EQ(ret, 0);

        // client connect to server when initializing
        auto handle = smem_trans_create(&MakeTransConfig(trans_options.role, UNIQUE_ID, trans_options.deviceId), 0);

        ret = smem_trans_batch_register_mem(handle, addrPtrs.data(), capacities.data(), capacities.size(), 0);
        if (ret != SM_OK) {
            flag = 2;
            goto cleanup;
        }

    cleanup:
        delete[] address1;
        delete[] address2;
        smem_trans_destroy(handle, 0);
        smem_trans_uninit(0);
        exit(flag);
    }

    int status;
    EXPECT_NE(waitpid(pid, &status, 0), -1);

    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(SmemTransTest, smem_trans_register_mems_success_receiver_ipv6)
{
    smem_trans_config_t trans_options = g_trans_options;
    trans_options.role = SMEM_TRANS_RECEIVER;

    int *address1 = new int[1000];
    int *address2 = new int[2000];
    std::vector<void *> addrPtrs = {address1, address2};
    std::vector<size_t> capacities = {1000 * sizeof(int), 2000 * sizeof(int)};

    // first create server
    smem_set_conf_store_tls(false, nullptr, 0);
    smem_create_config_store(STORE_URL_IPV6, SMEM_STORE_SKIP_RECOVER);
    int ret = smem_trans_init(0, 0);
    EXPECT_EQ(ret, 0);

    // client connect to server when initializing
    auto handle = smem_trans_create(&MakeTransConfig(trans_options.role, UNIQUE_IPV6_ID, trans_options.deviceId), 0);

    ret = smem_trans_batch_register_mem(handle, addrPtrs.data(), capacities.data(), capacities.size(), 0);
    EXPECT_EQ(ret, SM_OK);

    delete[] address1;
    delete[] address2;
    smem_trans_destroy(handle, 0);
    smem_trans_uninit(0);
}

TEST_F(SmemTransTest, smem_trans_batch_status_query_failed_invalid_param)
{
    uint32_t deviceId = 0U;

    int32_t ret = smem_trans_init(deviceId, 0);
    ASSERT_EQ(ret, SM_OK);
    auto handle = smem_trans_create(&MakeTransConfig(SMEM_TRANS_SENDER, UNIQUE_ID), 0);
    ASSERT_NE(handle, nullptr);

    uint32_t batchId = 0U;
    ret = smem_trans_batch_status_query(handle, batchId, nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);

    smem_trans_destroy(handle, 0);
    smem_trans_uninit(0);
}
// ===== CombineMemories / AlignMemory unit tests =====

namespace {
constexpr uint64_t NPU_PAGE_SIZE_UT = 2ULL * 1024ULL * 1024ULL; // 2MB, matches AlignMemory
using MemPair = std::pair<const void *, size_t>;

// Verify merge result: count and each (start, size) pair
void ExpectMerged(const std::vector<MemPair> &got, const std::vector<std::pair<uint64_t, uint64_t>> &expect)
{
    ASSERT_EQ(got.size(), expect.size());
    for (size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ(reinterpret_cast<uint64_t>(got[i].first), expect[i].first) << "idx " << i;
        EXPECT_EQ(got[i].second, expect[i].second) << "idx " << i;
    }
}

// 裸构造的 SmemTransConnection（仅成员初始化，不做 Initialize）：store_ 为空时任何时机触发
// 析构都会在 ClearStoreCallbacks 解引用空 store，故以 new 出的进程级容器持有引用且不主动
// 释放，UT 进程退出时故意泄漏以避免静态析构崩溃
SmemTransConnectionPtr MakeBareTransConn(const smem_trans_config_t &config, hybm_entity_t entity = nullptr)
{
    static auto *keepAlive = new std::vector<SmemTransConnectionPtr>(); // NOLINT: 故意泄漏，见上
    StorePtr emptyStore;
    // 裸构造不建链，对端 name 用同一占位串（解析失败仅表示不缓存，不影响本用例关注的成员行为）
    auto conn = SmMakeRef<SmemTransConnection>(
        config, "ut-conn", "ut-conn", 0, entity, emptyStore, WorkerUniqueId{},
        [](const std::string &, uint32_t &rank) {
            rank = K_PEER_RANK_ONE;
            return SM_OK;
        },
        [](const std::string &) {});
    keepAlive->push_back(conn);
    return conn;
}
} // namespace

// Already 2MB-aligned address/size stays unchanged
TEST_F(SmemTransTest, AlignMemory_aligned_unchanged)
{
    const void *addr = reinterpret_cast<const void *>(0x100000000ULL);
    uint64_t size = NPU_PAGE_SIZE_UT * 10;
    SmemTransEntry::AlignMemory(addr, size);
    EXPECT_EQ(reinterpret_cast<uint64_t>(addr), 0x100000000ULL);
    EXPECT_EQ(size, NPU_PAGE_SIZE_UT * 10);
}

// Unaligned address: start rounds down, size rounds up and covers the prefix
TEST_F(SmemTransTest, AlignMemory_unaligned_rounded)
{
    const void *addr = reinterpret_cast<const void *>(0x100001000ULL); // page offset 0x1000
    uint64_t size = NPU_PAGE_SIZE_UT;                                  // spans two 2MB pages
    SmemTransEntry::AlignMemory(addr, size);
    EXPECT_EQ(reinterpret_cast<uint64_t>(addr), 0x100000000ULL);
    // diff(0x1000) + size(0x200000) = 0x201000, rounded up to 2MB = 0x400000
    EXPECT_EQ(size, NPU_PAGE_SIZE_UT * 2);
}

// Exact adjacency (end == next.start): NOT merged -- core fix of the issue
TEST_F(SmemTransTest, CombineMemories_adjacent_not_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 100},
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 100), NPU_PAGE_SIZE_UT * 50},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ExpectMerged(got, {{0x100000000ULL, NPU_PAGE_SIZE_UT * 100},
                       {0x100000000ULL + NPU_PAGE_SIZE_UT * 100, NPU_PAGE_SIZE_UT * 50}});
}

// Real overlap (end > next.start): merged into one segment
TEST_F(SmemTransTest, CombineMemories_overlap_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 4},                        // [0, 4MB)
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 2), NPU_PAGE_SIZE_UT * 4}, // [2MB, 6MB)
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 6); // [0, 6MB)
}

// With a gap: NOT merged
TEST_F(SmemTransTest, CombineMemories_disjoint_not_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 2},
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 10), NPU_PAGE_SIZE_UT * 2},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    EXPECT_EQ(got.size(), 2U);
}

// Unsorted input: sorted internally then overlapping segments merged
TEST_F(SmemTransTest, CombineMemories_unsorted_overlap_merged)
{
    std::vector<MemPair> input = {
        {reinterpret_cast<const void *>(0x100000000ULL + NPU_PAGE_SIZE_UT * 2), NPU_PAGE_SIZE_UT * 4},
        {reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 4},
    };
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 6);
}

// Single input: returned as-is
TEST_F(SmemTransTest, CombineMemories_single_input)
{
    std::vector<MemPair> input = {{reinterpret_cast<const void *>(0x100000000ULL), NPU_PAGE_SIZE_UT * 3}};
    auto got = SmemTransEntry::CombineMemories(input);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), 0x100000000ULL);
    EXPECT_EQ(got[0].second, NPU_PAGE_SIZE_UT * 3);
}

// Regression: simulate SGLang PD K/V exactly adjacent on a 2MB boundary.
// After alignment K_end == V_start; CombineMemories must yield two independent registrations
// instead of merging them into one segment crossing the heap boundary.
TEST_F(SmemTransTest, CombineMemories_pd_kv_boundary_regression)
{
    // K half: already 2MB-aligned
    const uint64_t kPtr = 0x12c9c0000000ULL;
    uint64_t kSize = 18069061632ULL; // K half size from the issue (unaligned)
    const void *kAddr = reinterpret_cast<const void *>(kPtr);
    SmemTransEntry::AlignMemory(kAddr, kSize);

    // V half: starts right after K's aligned end
    const uint64_t vPtr = kPtr + kSize;
    uint64_t vSize = 2258632704ULL; // V half size from the issue
    const void *vAddr = reinterpret_cast<const void *>(vPtr);
    SmemTransEntry::AlignMemory(vAddr, vSize);

    // Exactly adjacent after alignment
    EXPECT_EQ(reinterpret_cast<uint64_t>(kAddr) + kSize, reinterpret_cast<uint64_t>(vAddr));

    std::vector<MemPair> input = {{kAddr, kSize}, {vAddr, vSize}};
    auto got = SmemTransEntry::CombineMemories(input);

    // Expect: two independent segments; K must not cover V (merging would make size ~= 19.13GiB,
    // exceeding K's heap ~= 17GiB)
    ASSERT_EQ(got.size(), 2U) << "adjacent K/V must not merge, otherwise cross-heap registration fails";
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[0].first), reinterpret_cast<uint64_t>(kAddr));
    EXPECT_EQ(got[0].second, kSize);
    EXPECT_EQ(reinterpret_cast<uint64_t>(got[1].first), reinterpret_cast<uint64_t>(vAddr));
    EXPECT_EQ(got[1].second, vSize);
    // The merged K segment must not cover V's start
    EXPECT_LT(reinterpret_cast<uint64_t>(got[0].first) + got[0].second, reinterpret_cast<uint64_t>(got[1].first) + 1);
}

/* ================================================================== */
/*  OnEstablishConnection/OnAddToWhitelist: register external slices    */
/*  (ESTABLISH + ADDWLST paths, in-process; the ipv6 tests exercise     */
/*  the same code via fork)                                            */
/* ================================================================== */
TEST_F(SmemTransTest, OnEstablishConnection_RegistersExternalSlices)
{
    smem_trans_config_t config = g_trans_options; // SMEM_TRANS_SENDER
    auto conn = MakeBareTransConn(config);

    MOCKER_CPP(&hybm_import, int32_t(*)(hybm_entity_t, const hybm_exchange_info[], uint32_t, void *[], uint32_t))
        .stubs()
        .will(returnValue(BM_OK));
    MOCKER_CPP(&hybm_mmap, int32_t(*)(hybm_entity_t, uint32_t)).stubs().will(returnValue(BM_OK));
    MOCKER_CPP(&hybm_transport_connect, int32_t(*)(hybm_entity_t, const uint32_t *, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(SMEM_OK));

    // baseInfo carries the peer session; role is stored in the reserved field (wire format [u][descLen][desc])
    SmemTransExchangeInfo base{};
    base.u.session.reserved = static_cast<uint16_t>(SMEM_TRANS_RECEIVER);
    std::vector<uint8_t> baseBytes;
    SerializeTransExchangeInfo(baseBytes, base);

    SmemTransExchangeInfo slice1{};
    slice1.u.address = LocalMapAddress(reinterpret_cast<void *>(K_SLICE1_ADDR), K_SLICE1_SIZE);
    std::vector<uint8_t> slice1Bytes;
    SerializeTransExchangeInfo(slice1Bytes, slice1);
    SmemTransExchangeInfo slice2{};
    slice2.u.address = LocalMapAddress(reinterpret_cast<void *>(K_SLICE2_ADDR), K_SLICE2_SIZE);
    std::vector<uint8_t> slice2Bytes;
    SerializeTransExchangeInfo(slice2Bytes, slice2);

    RankFullInfo peer;
    peer.rankId = 1;
    peer.baseInfo = baseBytes;
    peer.externalInfo.push_back(slice1Bytes);

    // ESTABLISH: alloc the peer override-rank window and connect transport
    EXPECT_EQ(conn->OnEstablishConnection(1, {peer}, 0), SMEM_OK);
    // ADD whitelist: import entity + slices and record the local->global mapping
    EXPECT_EQ(conn->OnAddToWhitelist(1, {peer}, 0), SM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        ASSERT_EQ(conn->remoteSlices_.size(), 1U);
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 1U);
    }

    // repeated ESTABLISH/ADD with identical slices: idempotent, no duplicate records
    EXPECT_EQ(conn->OnEstablishConnection(1, {peer}, 0), SMEM_OK);
    EXPECT_EQ(conn->OnAddToWhitelist(1, {peer}, 0), SM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 1U);
    }

    // a new slice appears: imported additionally and recorded
    peer.externalInfo.push_back(slice2Bytes);
    EXPECT_EQ(conn->OnAddToWhitelist(1, {peer}, 0), SM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 2U);
    }
}

/* ADD_SLICES: slices registered after the ADDWLST snapshot must still reach the importer */
TEST_F(SmemTransTest, OnAddSlices_ImportsIncrementalSlices)
{
    smem_trans_config_t config = g_trans_options; // SMEM_TRANS_SENDER
    auto conn = MakeBareTransConn(config, reinterpret_cast<hybm_entity_t>(0x1));

    MOCKER_CPP(&hybm_import, int32_t(*)(hybm_entity_t, const hybm_exchange_info[], uint32_t, void *[], uint32_t))
        .stubs()
        .will(returnValue(BM_OK));
    MOCKER_CPP(&hybm_mmap, int32_t(*)(hybm_entity_t, uint32_t)).stubs().will(returnValue(BM_OK));
    MOCKER_CPP(&hybm_transport_connect, int32_t(*)(hybm_entity_t, const uint32_t *, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(SMEM_OK));

    SmemTransExchangeInfo base{};
    base.u.session.reserved = static_cast<uint16_t>(SMEM_TRANS_RECEIVER);
    std::vector<uint8_t> baseBytes;
    SerializeTransExchangeInfo(baseBytes, base);

    SmemTransExchangeInfo slice1{};
    slice1.u.address = LocalMapAddress(reinterpret_cast<void *>(K_SLICE1_ADDR), K_SLICE1_SIZE);
    std::vector<uint8_t> slice1Bytes;
    SerializeTransExchangeInfo(slice1Bytes, slice1);

    RankFullInfo peer;
    peer.rankId = K_PEER_RANK_ONE;
    peer.baseInfo = baseBytes;
    peer.externalInfo.push_back(slice1Bytes);

    // snapshot path: only slice1 is known when ADDWLST fires
    EXPECT_EQ(conn->OnEstablishConnection(K_PEER_RANK_ONE, {peer}, 0), SMEM_OK);
    EXPECT_EQ(conn->OnAddToWhitelist(K_PEER_RANK_ONE, {peer}, 0), SM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        ASSERT_EQ(conn->remoteSlices_.size(), 1U);
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 1U);
    }

    // slice2 only ever arrives through ADD_SLICES: it must be imported into the same peer window
    SmemTransExchangeInfo slice2{};
    slice2.u.address = LocalMapAddress(reinterpret_cast<void *>(K_SLICE2_ADDR), K_SLICE2_SIZE);
    std::vector<uint8_t> slice2Bytes;
    SerializeTransExchangeInfo(slice2Bytes, slice2);
    MultiBytes incremental;
    incremental.push_back(slice2Bytes);
    EXPECT_EQ(conn->OnAddSlices(K_PEER_RANK_ONE, incremental, 0), SMEM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        ASSERT_EQ(conn->remoteSlices_.size(), 1U) << "incremental slice must reuse the peer entry";
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 2U);
    }

    // re-delivery of the same batch stays idempotent
    EXPECT_EQ(conn->OnAddSlices(K_PEER_RANK_ONE, incremental, 0), SMEM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        EXPECT_EQ(conn->remoteSlices_.begin()->second.size(), 2U);
    }

    // a rank that this connection never registered belongs to another connection: skip it
    EXPECT_EQ(conn->OnAddSlices(K_PEER_RANK_TWO, incremental, 0), SMEM_OK);
    {
        ock::mf::ReadGuard locker(conn->remoteSliceRwMutex_);
        EXPECT_EQ(conn->remoteSlices_.size(), 1U);
    }

    // empty batch is a no-op
    EXPECT_EQ(conn->OnAddSlices(K_PEER_RANK_ONE, MultiBytes{}, 0), SMEM_OK);
}

/* ADD_SLICES 回调注册：漏注册时 SmemGroupManagerClient 只打 INFO 后 return 0，增量片静默丢弃。
 * ADD_SLICES 无 ack 也不参与 link 推进，漏注册没有任何兜底通道，故必须有用例守住注册本身 */
TEST_F(SmemTransTest, Callbacks_WireAddSlicesIntoDispatcher)
{
    smem_trans_config_t config = g_trans_options;
    auto conn = MakeBareTransConn(config, reinterpret_cast<hybm_entity_t>(0x1));

    SmemGroupCommandAsyncDispatcher asyncMgr(nullptr);
    ASSERT_EQ(conn->RegisterAsyncCallbacks(&asyncMgr), SMEM_OK);
    EXPECT_TRUE(static_cast<bool>(asyncMgr.onAddToWhitelist_));
    EXPECT_TRUE(static_cast<bool>(asyncMgr.onEstablishConnection_));
    EXPECT_TRUE(static_cast<bool>(asyncMgr.onAddSlices_));

    auto executor = SmMakeRef<SmemGroupManagerClient>();
    ASSERT_EQ(conn->RegisterExecutorCallbacks(executor.Get(), &asyncMgr), SMEM_OK);
    ASSERT_TRUE(static_cast<bool>(executor->onAddSlices_));

    // 端到端：store 下发的 ADD_SLICES 经 executor 回调进入 dispatcher 队列，而不是静默丢弃
    MultiBytes slices;
    slices.emplace_back(Bytes(4U, 0U));
    EXPECT_EQ(executor->AddSlices(K_PEER_RANK_ONE, slices, 0), 0);
    EXPECT_EQ(asyncMgr.addSlicesQueue_.size(), 1U);
}
