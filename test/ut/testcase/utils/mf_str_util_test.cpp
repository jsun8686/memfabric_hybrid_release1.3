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
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <limits>

#include "mf_str_util.h"

using namespace ock::mf;

class MFStrUtilTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// StrUtil::StrTrim
// ============================================================
TEST_F(MFStrUtilTest, StrTrim_Empty)
{
    EXPECT_EQ(StrUtil::StrTrim(""), "");
}

TEST_F(MFStrUtilTest, StrTrim_LeadingSpaces)
{
    EXPECT_EQ(StrUtil::StrTrim("  hello"), "hello");
}

TEST_F(MFStrUtilTest, StrTrim_TrailingSpaces)
{
    EXPECT_EQ(StrUtil::StrTrim("hello  "), "hello");
}

TEST_F(MFStrUtilTest, StrTrim_BothSides)
{
    EXPECT_EQ(StrUtil::StrTrim("  hello world  "), "hello world");
}

TEST_F(MFStrUtilTest, StrTrim_NoSpaces)
{
    EXPECT_EQ(StrUtil::StrTrim("hello"), "hello");
}

// ============================================================
// StrUtil::Split
// ============================================================
TEST_F(MFStrUtilTest, Split_Basic)
{
    auto parts = StrUtil::Split("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3U);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c"); // 2
}

TEST_F(MFStrUtilTest, Split_EmptyString)
{
    auto parts = StrUtil::Split("", ',');
    EXPECT_TRUE(parts.empty());
}

TEST_F(MFStrUtilTest, Split_NoDelimiter)
{
    auto parts = StrUtil::Split("hello", ',');
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0], "hello");
}

TEST_F(MFStrUtilTest, Split_TrailingDelimiter)
{
    // std::getline stops extracting at EOF, trailing delimiter yields no extra empty token
    auto parts = StrUtil::Split("a,b,", ',');
    ASSERT_EQ(parts.size(), 2U);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
}

// ============================================================
// StrUtil::StartWith
// ============================================================
TEST_F(MFStrUtilTest, StartWith_Exact)
{
    EXPECT_TRUE(StrUtil::StartWith("hello", "hello"));
}

TEST_F(MFStrUtilTest, StartWith_Prefix)
{
    EXPECT_TRUE(StrUtil::StartWith("hello world", "hello"));
}

TEST_F(MFStrUtilTest, StartWith_PrefixLongerThanStr)
{
    EXPECT_FALSE(StrUtil::StartWith("hi", "hello"));
}

TEST_F(MFStrUtilTest, StartWith_EmptyPrefix)
{
    EXPECT_TRUE(StrUtil::StartWith("hello", ""));
}

TEST_F(MFStrUtilTest, StartWith_DoesNotStart)
{
    EXPECT_FALSE(StrUtil::StartWith("world", "hello"));
}

// ============================================================
// StrUtil::String2Uint
// ============================================================
TEST_F(MFStrUtilTest, String2Uint_Basic)
{
    uint64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("12345", val));
    EXPECT_EQ(val, 12345U);
}

TEST_F(MFStrUtilTest, String2Uint_Empty)
{
    uint64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("", val));
}

TEST_F(MFStrUtilTest, String2Uint_Negative)
{
    uint64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("-1", val));
}

TEST_F(MFStrUtilTest, String2Uint_LeadingZeros)
{
    uint64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("00123", val));
    EXPECT_EQ(val, 123U);
}

TEST_F(MFStrUtilTest, String2Uint_Overflow)
{
    uint64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("999999999999999999999999999999999999", val));
}

TEST_F(MFStrUtilTest, String2Uint_NonNumeric)
{
    uint64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("abc", val));
}

TEST_F(MFStrUtilTest, String2Uint_TrailingSpaces)
{
    uint64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("123  ", val));
    EXPECT_EQ(val, 123U);
}

TEST_F(MFStrUtilTest, String2Uint_MixedChars)
{
    uint64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("123abc", val));
}

TEST_F(MFStrUtilTest, String2Uint_Zero)
{
    uint64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("0", val));
    EXPECT_EQ(val, 0U);
}

TEST_F(MFStrUtilTest, String2Uint_Uint8Max)
{
    uint8_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("255", val));
    EXPECT_EQ(val, 255); // 255
}

TEST_F(MFStrUtilTest, String2Uint_Uint8Overflow)
{
    uint8_t val = 0;
    EXPECT_FALSE(StrUtil::String2Uint("256", val));
}

TEST_F(MFStrUtilTest, String2Uint_ExceedsTypeMax)
{
    uint16_t val = 0;
    EXPECT_TRUE(StrUtil::String2Uint("65535", val));
    EXPECT_EQ(val, 65535); // 65535
    EXPECT_FALSE(StrUtil::String2Uint("65536", val));
}

// ============================================================
// StrUtil::String2Int
// ============================================================
TEST_F(MFStrUtilTest, String2Int_Basic)
{
    int64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Int("12345", val));
    EXPECT_EQ(val, 12345); // 12345
}

TEST_F(MFStrUtilTest, String2Int_Negative)
{
    int64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Int("-12345", val));
    EXPECT_EQ(val, -12345); // -12345
}

TEST_F(MFStrUtilTest, String2Int_Empty)
{
    int64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("", val));
}

TEST_F(MFStrUtilTest, String2Int_NonNumeric)
{
    int64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("abc", val));
}

TEST_F(MFStrUtilTest, String2Int_Overflow)
{
    int64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("999999999999999999999999999999999999", val));
}

TEST_F(MFStrUtilTest, String2Int_Int8Max)
{
    int8_t val = 0;
    EXPECT_TRUE(StrUtil::String2Int("127", val));
    EXPECT_EQ(val, 127); // 127
}

TEST_F(MFStrUtilTest, String2Int_Int8Min)
{
    int8_t val = 0;
    EXPECT_TRUE(StrUtil::String2Int("-128", val));
    EXPECT_EQ(val, -128); // 128
}

TEST_F(MFStrUtilTest, String2Int_Int8Overflow)
{
    int8_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("128", val));
}

TEST_F(MFStrUtilTest, String2Int_Int8Underflow)
{
    int8_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("-129", val));
}

TEST_F(MFStrUtilTest, String2Int_TrailingSpaces)
{
    int64_t val = 0;
    EXPECT_TRUE(StrUtil::String2Int("  -123  ", val));
    EXPECT_EQ(val, -123); // 123
}

TEST_F(MFStrUtilTest, String2Int_MixedChars)
{
    int64_t val = 0;
    EXPECT_FALSE(StrUtil::String2Int("123abc", val));
}

// ============================================================
// StrUtil::GetNowTime
// ============================================================
TEST_F(MFStrUtilTest, GetNowTime_ReturnsNonZero)
{
    auto t = StrUtil::GetNowTime();
    EXPECT_GT(t, 0);
}

TEST_F(MFStrUtilTest, GetNowTime_Increases)
{
    auto t1 = StrUtil::GetNowTime();
    auto t2 = StrUtil::GetNowTime();
    EXPECT_GE(t2, t1);
}

TEST_F(MFStrUtilTest, GetNowTime_UsesMonotonicMilliseconds)
{
    const auto before =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto value = StrUtil::GetNowTime();
    const auto after =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();

    EXPECT_GE(value, before);
    EXPECT_LE(value, after);
}
