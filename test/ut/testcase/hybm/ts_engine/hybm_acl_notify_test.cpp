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

#define private   public
#define protected public
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "dl_hal_api_def.h"
#include "dl_rt_api.h"
#include "hybm_acl_notify.h"
#undef private
#undef protected

using namespace ock::mf;

namespace {
constexpr uint64_t kTestNotifyHandle = 0x9000ULL;
constexpr uint64_t kTestAclStream = 0x6000ULL;
constexpr uint64_t kTestRegBlockBase = 0x12C180000000ULL;
constexpr uint32_t kTestNotifyId = 11U;
constexpr uint64_t kTestHalOffset = 0x1000ULL;
constexpr uint64_t kTestRtAddr = 0xAAAA0000ULL;
constexpr uint32_t kTestRtLen = 8U;

uint32_t g_destroyCount = 0U;
uint32_t g_waitCount = 0U;
uint32_t g_halOffset = static_cast<uint32_t>(kTestHalOffset);

int32_t FakeCreateNotifyOk(void **notify, uint64_t)
{
    if (notify != nullptr) {
        *notify = reinterpret_cast<void *>(kTestNotifyHandle);
    }
    return 0;
}

int32_t FakeCreateNotifyFail(void **notify, uint64_t)
{
    if (notify != nullptr) {
        *notify = nullptr;
    }
    return -1;
}

int32_t FakeGetNotifyIdOk(void *, uint32_t *notifyId)
{
    if (notifyId != nullptr) {
        *notifyId = kTestNotifyId;
    }
    return 0;
}

int32_t FakeGetNotifyIdFail(void *, uint32_t *)
{
    return -1;
}

int32_t FakeDestroyNotifyOk(void *)
{
    g_destroyCount++;
    return 0;
}

int32_t FakeWaitAndResetNotifyOk(void *, void *, uint32_t)
{
    g_waitCount++;
    return 0;
}

int32_t FakeWaitAndResetNotifyFail(void *, void *, uint32_t)
{
    return -1;
}

int32_t FakeAclrtSynchronizeStreamOk(void *)
{
    return 0;
}

int32_t FakeAclrtSynchronizeStreamFail(void *)
{
    return -1;
}

int32_t FakeHalDetailQueryOk(uint32_t, struct halResourceIdInputInfo *, struct halResourceDetailInfo *info)
{
    if (info != nullptr) {
        info->value0 = g_halOffset;
    }
    return 0;
}

int32_t FakeHalDetailQueryFail(uint32_t, struct halResourceIdInputInfo *, struct halResourceDetailInfo *)
{
    return -1;
}

int32_t FakeDrvNotifyIdAddrOffsetOk(uint32_t, struct drvNotifyInfo *info)
{
    if (info != nullptr) {
        info->devAddrOffset = g_halOffset;
    }
    return 0;
}

int32_t FakeDrvNotifyIdAddrOffsetFail(uint32_t, struct drvNotifyInfo *)
{
    return -1;
}

int32_t FakeRtGetDevResAddressOk(rtDevResInfo *, rtDevResAddrInfo *info)
{
    if (info != nullptr) {
        *info->resAddress = kTestRtAddr;
        *info->len = kTestRtLen;
    }
    return 0;
}

int32_t FakeRtGetDevResAddressFail(rtDevResInfo *, rtDevResAddrInfo *)
{
    return -1;
}

int32_t FakeRtGetDevResAddressZero(rtDevResInfo *, rtDevResAddrInfo *info)
{
    if (info != nullptr) {
        *info->resAddress = 0U;
        *info->len = 0U;
    }
    return 0;
}

struct DlApiFnGuard {
    aclrtCreateNotifyFunc oldCreateNotify{DlAclApi::pAclrtCreateNotify};
    aclrtGetNotifyIdFunc oldGetNotifyId{DlAclApi::pAclrtGetNotifyId};
    aclrtDestroyNotifyFunc oldDestroyNotify{DlAclApi::pAclrtDestroyNotify};
    aclrtWaitAndResetNotifyFunc oldWaitAndResetNotify{DlAclApi::pAclrtWaitAndResetNotify};
    aclrtSynchronizeStreamFunc oldSynchronizeStream{DlAclApi::pAclrtSynchronizeStream};
    rtGetDevResAddressFunc oldGetDevResAddress{DlRtApi::pRtGetDevResAddress};
    halResourceDetailQueryFunc oldHalDetailQuery{DlHalApi::pHalResourceDetailQuery};
    drvNotifyIdAddrOffsetFunc oldDrvNotifyIdAddrOffset{DlHalApi::pDrvNotifyIdAddrOffset};

    DlApiFnGuard()
    {
        DlAclApi::pAclrtCreateNotify = &FakeCreateNotifyOk;
        DlAclApi::pAclrtGetNotifyId = &FakeGetNotifyIdOk;
        DlAclApi::pAclrtDestroyNotify = &FakeDestroyNotifyOk;
        DlAclApi::pAclrtWaitAndResetNotify = &FakeWaitAndResetNotifyOk;
        DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamOk;
        DlRtApi::pRtGetDevResAddress = &FakeRtGetDevResAddressOk;
        DlHalApi::pHalResourceDetailQuery = &FakeHalDetailQueryOk;
        DlHalApi::pDrvNotifyIdAddrOffset = &FakeDrvNotifyIdAddrOffsetOk;
    }

    ~DlApiFnGuard()
    {
        DlAclApi::pAclrtCreateNotify = oldCreateNotify;
        DlAclApi::pAclrtGetNotifyId = oldGetNotifyId;
        DlAclApi::pAclrtDestroyNotify = oldDestroyNotify;
        DlAclApi::pAclrtWaitAndResetNotify = oldWaitAndResetNotify;
        DlAclApi::pAclrtSynchronizeStream = oldSynchronizeStream;
        DlRtApi::pRtGetDevResAddress = oldGetDevResAddress;
        DlHalApi::pHalResourceDetailQuery = oldHalDetailQuery;
        DlHalApi::pDrvNotifyIdAddrOffset = oldDrvNotifyIdAddrOffset;
    }
};

void ResetCounters()
{
    g_destroyCount = 0U;
    g_waitCount = 0U;
    g_halOffset = static_cast<uint32_t>(kTestHalOffset);
}
} // namespace

TEST(HybmAclNotifyTest, InitResolvesRegAddrByHalOffset)
{
    DlApiFnGuard guard;
    ResetCounters();

    HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
    EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
    EXPECT_TRUE(notify.Inited());
    EXPECT_EQ(notify.GetNotifyId(), kTestNotifyId);
    EXPECT_EQ(notify.GetOffset(), kTestHalOffset);
    EXPECT_EQ(notify.GetRegAddr(), kTestRegBlockBase + kTestHalOffset);
    EXPECT_EQ(notify.GetRegLen(), 0U);
    EXPECT_EQ(notify.GetStream(), reinterpret_cast<void *>(kTestAclStream));

    // 重复 Init 幂等，不重新创建
    EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
    EXPECT_EQ(g_destroyCount, 0U);
}

TEST(HybmAclNotifyTest, InitResolvesRegAddrByRt)
{
    DlApiFnGuard guard;
    ResetCounters();

    HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
    EXPECT_EQ(notify.Init(0U), BM_OK); // regBlockBase=0 -> rtGetDevResAddress 路径
    EXPECT_EQ(notify.GetRegAddr(), kTestRtAddr);
    EXPECT_EQ(notify.GetRegLen(), kTestRtLen);
    EXPECT_EQ(notify.GetOffset(), 0U);
}

TEST(HybmAclNotifyTest, InitFailurePathsReleaseNotify)
{
    DlApiFnGuard guard;
    ResetCounters();

    // 1) create 失败
    DlAclApi::pAclrtCreateNotify = &FakeCreateNotifyFail;
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_NE(notify.Init(kTestRegBlockBase), BM_OK);
        EXPECT_FALSE(notify.Inited());
    }
    EXPECT_EQ(g_destroyCount, 0U);

    // 2) get notify id 失败：创建出来的 notify 必须释放
    DlAclApi::pAclrtCreateNotify = &FakeCreateNotifyOk;
    DlAclApi::pAclrtGetNotifyId = &FakeGetNotifyIdFail;
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_NE(notify.Init(kTestRegBlockBase), BM_OK);
        EXPECT_FALSE(notify.Inited());
    }
    EXPECT_EQ(g_destroyCount, 1U);

    // 3) 地址解析失败（HAL 查询与 drv 回落都失败）：notify 也要释放
    DlAclApi::pAclrtGetNotifyId = &FakeGetNotifyIdOk;
    DlHalApi::pHalResourceDetailQuery = &FakeHalDetailQueryFail;
    DlHalApi::pDrvNotifyIdAddrOffset = &FakeDrvNotifyIdAddrOffsetFail;
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_NE(notify.Init(kTestRegBlockBase), BM_OK);
        EXPECT_FALSE(notify.Inited());
    }
    EXPECT_EQ(g_destroyCount, 2U);

    // 4) HAL 查询失败但 drv 回落成功：仍然可用
    DlHalApi::pDrvNotifyIdAddrOffset = &FakeDrvNotifyIdAddrOffsetOk;
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
        EXPECT_EQ(notify.GetRegAddr(), kTestRegBlockBase + kTestHalOffset);
    }

    // 5) rt 路径拿到 0 地址：判为失败
    DlRtApi::pRtGetDevResAddress = &FakeRtGetDevResAddressZero;
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_NE(notify.Init(0U), BM_OK);
        EXPECT_FALSE(notify.Inited());
    }
}

TEST(HybmAclNotifyTest, WaitBranches)
{
    DlApiFnGuard guard;
    ResetCounters();

    // 未 Init：直接失败
    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_NE(notify.Wait(), BM_OK);
    }

    // Init 成功但没有 ACL 流：无法入队等待
    {
        HybmAclNotify notify(nullptr);
        EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
        EXPECT_NE(notify.Wait(), BM_OK);
    }

    HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
    EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);

    // wait&reset 失败
    DlAclApi::pAclrtWaitAndResetNotify = &FakeWaitAndResetNotifyFail;
    EXPECT_NE(notify.Wait(), BM_OK);

    // wait 成功但本流同步失败
    DlAclApi::pAclrtWaitAndResetNotify = &FakeWaitAndResetNotifyOk;
    DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamFail;
    EXPECT_NE(notify.Wait(), BM_OK);

    // 全部成功
    DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamOk;
    EXPECT_EQ(notify.Wait(), BM_OK);
    EXPECT_EQ(g_waitCount, 2U);
}

TEST(HybmAclNotifyTest, ResetIsIdempotentAndMoveTransfersOwnership)
{
    DlApiFnGuard guard;
    ResetCounters();

    {
        HybmAclNotify notify(reinterpret_cast<void *>(kTestAclStream));
        EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
        notify.Reset();
        EXPECT_FALSE(notify.Inited());
        notify.Reset(); // 幂等
        EXPECT_EQ(g_destroyCount, 1U);

        // 移动：所有权转移，源对象复位
        EXPECT_EQ(notify.Init(kTestRegBlockBase), BM_OK);
        HybmAclNotify moved(std::move(notify));
        EXPECT_TRUE(moved.Inited());
        EXPECT_EQ(moved.GetNotifyId(), kTestNotifyId);
        EXPECT_FALSE(notify.Inited());
        moved.Reset();
        EXPECT_EQ(g_destroyCount, 2U);
    }
    EXPECT_EQ(g_destroyCount, 2U); // 析构不再重复释放
}
