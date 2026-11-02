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

#include <unistd.h>

#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include "acc_tcp_port_util.h"
#include "smem_trans/smem_trans_connection.h"

// 说明：uniqueId(ip:port) 解析与 MF_CONFIG_STORE_PORT_START/END 端口范围的环境变量
// 解析逻辑已内聚为 smem_trans_entry.cpp 的文件内静态函数（由 SmemTransTest 的 fork 用例
// 端到端覆盖）；此处覆盖可直测的公开接口：ParseTransName 与 AccFindAvailableTcpPort。

using namespace ock::smem;

namespace {
constexpr uint16_t K_PORT_ZERO = 0;
constexpr uint16_t K_RANGE_MIN = 9000;
constexpr uint16_t K_RANGE_MAX = 65535;
constexpr uint16_t K_ENV_MIN = 49300;
constexpr uint16_t K_ENV_MAX = 49350;
constexpr int K_INVALID_FD = -1;
} // namespace

// ===== SmemTransConnection::ParseTransName: trans uniqueId (ip:port) parsing =====

class ParseTransNameTest : public testing::Test {};

TEST_F(ParseTransNameTest, Ipv4WithPort)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_TRUE(SmemTransConnection::ParseTransName("127.0.0.1:10000", ip, port));
    EXPECT_EQ(ip.type, ock::mf::IpV4);
    EXPECT_EQ(port, 10000);
}

TEST_F(ParseTransNameTest, Ipv4PortMaxBoundary)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_TRUE(SmemTransConnection::ParseTransName("10.0.0.1:65535", ip, port));
    EXPECT_EQ(ip.type, ock::mf::IpV4);
    EXPECT_EQ(port, 65535);
}

TEST_F(ParseTransNameTest, Ipv6BracketedWithPort)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_TRUE(SmemTransConnection::ParseTransName("[::1]:10001", ip, port));
    EXPECT_EQ(ip.type, ock::mf::IpV6);
    EXPECT_EQ(port, 10001);
}

TEST_F(ParseTransNameTest, Ipv4WithoutPortRejected)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_FALSE(SmemTransConnection::ParseTransName("127.0.0.1", ip, port));
}

TEST_F(ParseTransNameTest, PrivilegedPortRejected)
{
    // url 解析要求端口 > 1024（与 smem HA 层约定一致）
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_FALSE(SmemTransConnection::ParseTransName("127.0.0.1:80", ip, port));
}

TEST_F(ParseTransNameTest, PortOverflowRejected)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_FALSE(SmemTransConnection::ParseTransName("10.0.0.1:65536", ip, port));
}

TEST_F(ParseTransNameTest, InvalidIpRejected)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_FALSE(SmemTransConnection::ParseTransName("host:10000", ip, port));
}

TEST_F(ParseTransNameTest, EmptyInputRejected)
{
    ock::mf::net_addr_t ip{};
    uint16_t port = 1;
    EXPECT_FALSE(SmemTransConnection::ParseTransName("", ip, port));
}

// ===== acc::AccFindAvailableTcpPort: tcp port probing =====

class AccFindAvailableTcpPortTest : public testing::Test {
public:
    void SetUp() override
    {
        sockfd_ = K_INVALID_FD;
    }

    void TearDown() override
    {
        if (sockfd_ >= 0) {
            close(sockfd_);
            sockfd_ = K_INVALID_FD;
        }
    }

    int sockfd_ = K_INVALID_FD;
};

TEST_F(AccFindAvailableTcpPortTest, FindsPortInGivenRange)
{
    auto port = ock::acc::AccFindAvailableTcpPort(sockfd_, K_RANGE_MIN, K_RANGE_MAX);
    EXPECT_GE(port, K_RANGE_MIN);
    EXPECT_LE(port, K_RANGE_MAX);
    EXPECT_GE(sockfd_, 0); // 探测 socket 保持绑定，由调用方持有
}

TEST_F(AccFindAvailableTcpPortTest, RespectsNarrowRange)
{
    auto port = ock::acc::AccFindAvailableTcpPort(sockfd_, K_ENV_MIN, K_ENV_MAX);
    EXPECT_GE(port, K_ENV_MIN);
    EXPECT_LE(port, K_ENV_MAX);
}

TEST_F(AccFindAvailableTcpPortTest, ReversedRangeRejected)
{
    EXPECT_EQ(ock::acc::AccFindAvailableTcpPort(sockfd_, 20000, 10000), K_PORT_ZERO);
}

TEST_F(AccFindAvailableTcpPortTest, ZeroMinRejected)
{
    EXPECT_EQ(ock::acc::AccFindAvailableTcpPort(sockfd_, 0, 10000), K_PORT_ZERO);
}
