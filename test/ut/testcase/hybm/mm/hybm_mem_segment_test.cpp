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

#include <mockcpp/mockcpp.hpp>

#define private   public
#define protected public
#include "hybm_asymmetric_mem_segment.h"
#include "hybm_conn_based_segment.h"
#include "hybm_dev_legacy_segment.h"
#include "hybm_mem_segment.h"
#include "hybm_vmm_based_segment.h"
#include "hybm_asymmetric_mem_segment.h"
#include "dl_acl_api.h"
#undef private
#undef protected

#include "hybm_va_manager.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_gva_version.h"
#include "hybm_host_shm_segment.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

namespace {
constexpr int TEST_ENTITY_ID = 100; // G.CNS.02: 段构造的 entityId 参数具名化
struct MemSegmentAclFnGuard {
    ock::mf::aclrtSetDeviceFunc oldSetDevice{ock::mf::DlAclApi::pAclrtSetDevice};
    ock::mf::rtGetLogicDevIdByUserDevIdFunc oldGetLogicDevId{ock::mf::DlAclApi::pRtGetLogicDevIdByUserDevId};
    ock::mf::aclrtGetPhyDevIdByLogicDevIdFunc oldGetPhyDev{ock::mf::DlAclApi::pAclrtGetPhyDevIdByLogicDevId};
    ock::mf::rtDeviceGetBareTgidFunc oldGetBareTgid{ock::mf::DlAclApi::pRtDeviceGetBareTgid};
    ock::mf::rtGetDeviceInfoFunc oldGetDeviceInfo{ock::mf::DlAclApi::pRtGetDeviceInfo};

    ~MemSegmentAclFnGuard()
    {
        ock::mf::DlAclApi::pAclrtSetDevice = oldSetDevice;
        ock::mf::DlAclApi::pRtGetLogicDevIdByUserDevId = oldGetLogicDevId;
        ock::mf::DlAclApi::pAclrtGetPhyDevIdByLogicDevId = oldGetPhyDev;
        ock::mf::DlAclApi::pRtDeviceGetBareTgid = oldGetBareTgid;
        ock::mf::DlAclApi::pRtGetDeviceInfo = oldGetDeviceInfo;
    }
};

int g_rtGetDeviceInfoCallCount = 0;

struct MemSegmentStaticsGuard {
    bool deviceInfoReady{ock::mf::MemSegment::deviceInfoReady_};
    int deviceId{ock::mf::MemSegment::deviceId_};
    int logicDeviceId{ock::mf::MemSegment::logicDeviceId_};
    int devicePhyId{ock::mf::MemSegment::devicePhyId_};
    uint32_t pid{ock::mf::MemSegment::pid_};
    uint32_t sdid{ock::mf::MemSegment::sdid_};
    uint32_t serverId{ock::mf::MemSegment::serverId_};
    uint32_t superPodId{ock::mf::MemSegment::superPodId_};
    std::string sysBoolId{ock::mf::MemSegment::sysBoolId_};
    uint32_t bootIdHead{ock::mf::MemSegment::bootIdHead_};
    ock::mf::AscendSocType socType{ock::mf::MemSegment::socType_};

    ~MemSegmentStaticsGuard()
    {
        ock::mf::MemSegment::deviceInfoReady_ = deviceInfoReady;
        ock::mf::MemSegment::deviceId_ = deviceId;
        ock::mf::MemSegment::logicDeviceId_ = logicDeviceId;
        ock::mf::MemSegment::devicePhyId_ = devicePhyId;
        ock::mf::MemSegment::pid_ = pid;
        ock::mf::MemSegment::sdid_ = sdid;
        ock::mf::MemSegment::serverId_ = serverId;
        ock::mf::MemSegment::superPodId_ = superPodId;
        ock::mf::MemSegment::sysBoolId_ = sysBoolId;
        ock::mf::MemSegment::bootIdHead_ = bootIdHead;
        ock::mf::MemSegment::socType_ = socType;
    }
};
} // namespace

class HybmMemSegmentTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        auto ret = hybm_init(0, 0);
        // 前置失败必须跳过用例体：否则用例会带着未初始化的全局态继续执行，
        // 一旦命中无保护的下标/迭代器解引用，就会 SIGSEGV 掉整个测试进程，掩盖后续所有 suite。
        ASSERT_EQ(ret, BM_OK) << "hybm_init failed, check ASCEND_HOME_PATH / driver env, ret=" << ret;
        staticsGuard = std::make_unique<MemSegmentStaticsGuard>();
    }

    void TearDown() override
    {
        staticsGuard.reset();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        hybm_uninit();
    }

    std::unique_ptr<MemSegmentStaticsGuard> staticsGuard;
};

// =========================
// 1. MemSegment::Create 选择逻辑
// =========================

/**
 * Create_Rejects_InvalidRank
 *  - rankId 必须 < rankCnt
 */
TEST_F(HybmMemSegmentTest, Create_Rejects_InvalidRank)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 4;
    opt.rankId = 4; // 等于 rankCnt，非法
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    EXPECT_EQ(seg, nullptr);
}

/**
 * Create_Fails_When_InitDeviceInfo_Failed
 */
TEST_F(HybmMemSegmentTest, Create_Fails_When_InitDeviceInfo_Failed)
{
    // 输入参数：rank 合法、HBM 段
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 4;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(-1));

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    EXPECT_EQ(seg, nullptr);
}

/**
 * Create_Hbm_Default_Uses_DevLegacySegment
 *  - 当 GVA 版本不是 V4 或 SoC 不是 910C（默认情况），HBM 段应该走老的设备侧实现 `HybmDevLegacySegment`。
 */
TEST_F(HybmMemSegmentTest, Create_Hbm_Default_Uses_DevLegacySegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    // 模拟 GVA 版本非 V4
    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));

    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910B;

    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    ASSERT_NE(seg, nullptr);
    // 验证实际类型为 HybmDevLegacySegment
    auto devSeg = std::dynamic_pointer_cast<ock::mf::HybmDevLegacySegment>(seg);
    EXPECT_NE(devSeg, nullptr);
    auto vmmSeg = std::dynamic_pointer_cast<ock::mf::HybmVmmBasedSegment>(seg);
    EXPECT_EQ(vmmSeg, nullptr);
}

/**
 * Create_Hbm_V4_910C_NoMte_Uses_VmmBasedSegment
 *  - 当 GVA 版本为 V4 且 SoC 为 910C，且 dataOpType 中未包含 MTE，HBM 段会选择 VMM 实现。
 */
TEST_F(HybmMemSegmentTest, Create_Hbm_V4_910C_NoMte_Uses_VmmBasedSegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_HBM;
    // 没有包含 HYBM_DOP_TYPE_MTE
    opt.dataOpType = HYBM_DOP_TYPE_SDMA;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    // 设置环境为 GVA_V4 + ASCEND_910C
    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V4));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;

    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));

    auto seg = ock::mf::MemSegment::Create(opt, 1);
    ASSERT_NE(seg, nullptr);
    auto vmmSeg = std::dynamic_pointer_cast<ock::mf::HybmVmmBasedSegment>(seg);
    EXPECT_NE(vmmSeg, nullptr);
    auto devSeg = std::dynamic_pointer_cast<ock::mf::HybmDevLegacySegment>(seg);
    EXPECT_EQ(devSeg, nullptr);
}

/**
 * Create_Dram_V4_910C_Uses_VmmBasedSegment
 *  - 对 DRAM 段，当 GVA 版本为 V4 且 SoC 为 910C，会选择 VMM 实现。
 */
TEST_F(HybmMemSegmentTest, Create_Dram_V4_910C_Uses_VmmBasedSegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.dataOpType = HYBM_DOP_TYPE_SDMA;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V4));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;

    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));

    auto seg = ock::mf::MemSegment::Create(opt, 2);
    ASSERT_NE(seg, nullptr);
    auto vmmSeg = std::dynamic_pointer_cast<ock::mf::HybmVmmBasedSegment>(seg);
    EXPECT_NE(vmmSeg, nullptr);
}

TEST_F(HybmMemSegmentTest, Create_Dram_V4_910C_MixedDeviceTransportUsesVmm)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V4));
    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;

    struct TestCase {
        hybm_data_op_type dataOpType;
        bool expectVmm;
    };
    const TestCase testCases[] = {
        {HYBM_DOP_TYPE_DEVICE_RDMA, false},
        {HYBM_DOP_TYPE_DEVICE_URMA, false},
        {HYBM_DOP_TYPE_DEVICE_UBOE, false},
        {static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_RDMA), true},
        {static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_URMA), true},
        {static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_DEVICE_UBOE), true},
        {static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_HOST_TCP), false},
    };
    for (const auto &testCase : testCases) {
        opt.dataOpType = testCase.dataOpType;
        auto segment = ock::mf::MemSegment::Create(opt, 2);
        ASSERT_NE(segment, nullptr);
        EXPECT_EQ(std::dynamic_pointer_cast<ock::mf::HybmVmmBasedSegment>(segment) != nullptr, testCase.expectVmm);
        EXPECT_EQ(std::dynamic_pointer_cast<ock::mf::HybmConnBasedSegment>(segment) != nullptr, !testCase.expectVmm);
    }
}

/**
 * Create_Dram_Default_Uses_ConnBasedSegment
 *  - 在非 V4/910C 情况下，DRAM 段默认走 `HybmConnBasedSegment`。
 */
TEST_F(HybmMemSegmentTest, Create_Dram_Default_Uses_ConnBasedSegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.dataOpType = HYBM_DOP_TYPE_SDMA;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;

    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));

    auto seg = ock::mf::MemSegment::Create(opt, 3);
    ASSERT_NE(seg, nullptr);
    auto connSeg = std::dynamic_pointer_cast<ock::mf::HybmConnBasedSegment>(seg);
    EXPECT_NE(connSeg, nullptr);
}

/**
 * Create_Asymmetric_Uses_AsymmetricMemSegment
 *  - HYBM_MST_ASYMMETRIC 类型始终选择 `AsymmetricMemSegment`（trans 场景用户注册内存段）。
 *  - 这里只检查非空，类型在实现中已经固定。
 */
TEST_F(HybmMemSegmentTest, Create_Asymmetric_Uses_AsymmetricMemSegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 1;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_ASYMMETRIC;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;
    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));

    auto seg = ock::mf::MemSegment::Create(opt, 4);
    ASSERT_NE(seg, nullptr);
    // 具体类型为 AsymmetricMemSegment，这里只验证不会返回空指针
}

// =========================
// 2. ValidateOptions / GetReserveChunkSize 等纯逻辑函数
// =========================

/**
 * DevLegacySegment_ValidateOptions
 *  - 合法条件：segType=HBM / maxSize>0 且对齐大页 / devId>=0 / rankCnt*maxSize 不溢出。
 */
TEST_F(HybmMemSegmentTest, DevLegacySegment_ValidateOptions)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 4;
    opt.devId = 0;

    ock::mf::HybmDevLegacySegment segOk(opt, 0);
    EXPECT_EQ(segOk.ValidateOptions(), BM_OK);

    // 非 HBM 不合法
    opt.segType = ock::mf::HYBM_MST_DRAM;
    ock::mf::HybmDevLegacySegment segBadType(opt, 0);
    EXPECT_EQ(segBadType.ValidateOptions(), BM_INVALID_PARAM);

    // maxSize 不能为 0
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = 0;
    ock::mf::HybmDevLegacySegment segZero(opt, 0);
    EXPECT_EQ(segZero.ValidateOptions(), BM_INVALID_PARAM);

    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE * UINT32_MAX;
    opt.rankCnt = UINT32_MAX;
    ock::mf::HybmDevLegacySegment segMax(opt, 0);
    EXPECT_EQ(segMax.ValidateOptions(), BM_INVALID_PARAM);
}

/**
 * VmmBasedSegment_ValidateOptions
 *  - 只检查 maxSize>0 且对齐大页，rankCnt*maxSize 不溢出。
 */
TEST_F(HybmMemSegmentTest, VmmBasedSegment_ValidateOptions)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::GB;
    opt.rankCnt = 8;

    ock::mf::HybmVmmBasedSegment segOk(opt, 0);
    EXPECT_EQ(segOk.ValidateOptions(), BM_OK);

    opt.maxSize = 0;
    ock::mf::HybmVmmBasedSegment segZero(opt, 0);
    EXPECT_EQ(segZero.ValidateOptions(), BM_INVALID_PARAM);

    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE * UINT32_MAX;
    opt.rankCnt = UINT32_MAX;
    ock::mf::HybmVmmBasedSegment segMax(opt, 0);
    EXPECT_EQ(segMax.ValidateOptions(), BM_INVALID_PARAM);
}

/**
 * ConnBasedSegment_ValidateOptions
 *  - ConnBasedSegment 只支持 DRAM 段，且 maxSize>0 且大页对齐。
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_ValidateOptions)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;

    ock::mf::HybmConnBasedSegment segOk(opt, 0);
    EXPECT_EQ(segOk.ValidateOptions(), BM_OK);

    opt.segType = ock::mf::HYBM_MST_HBM;
    ock::mf::HybmConnBasedSegment segBadType(opt, 0);
    EXPECT_EQ(segBadType.ValidateOptions(), BM_INVALID_PARAM);

    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE * UINT32_MAX;
    opt.rankCnt = UINT32_MAX;
    ock::mf::HybmConnBasedSegment segMax(opt, 0);
    EXPECT_EQ(segMax.ValidateOptions(), BM_INVALID_PARAM);
}

/**
 * DevLegacySegment_GetReserveChunkSize
 *  - 该函数用来计算 GVA 预留时的 chunk 大小，保证：
 *    1) chunk 不超过 128G；
 *    2) totalSize 能被 chunk 整除；
 *    3) chunk 是 singleRankSize 的整数倍。
 */
TEST_F(HybmMemSegmentTest, DevLegacySegment_GetReserveChunkSize_BasicCases)
{
    using HybmSeg = ock::mf::HybmDevLegacySegment;

    // totalSize <= 128G，直接返回 totalSize
    uint64_t total = 16ULL * ock::mf::GB;
    uint64_t single = 4ULL * ock::mf::GB;
    EXPECT_EQ(HybmSeg::GetReserveChunkSize(total, single), total);

    // totalSize > 128G，返回一个不超过 128G 的整数倍 chunk
    total = 256ULL * ock::mf::GB;
    single = 4ULL * ock::mf::GB;
    uint64_t chunk = HybmSeg::GetReserveChunkSize(total, single);
    EXPECT_GT(chunk, 0U);
    EXPECT_LE(chunk, 128ULL * ock::mf::GB);
    EXPECT_EQ(total % chunk, 0U);
    EXPECT_EQ(chunk % single, 0U);

    // 异常场景：入参为 0
    EXPECT_EQ(HybmSeg::GetReserveChunkSize(0, single), 0U);
    EXPECT_EQ(HybmSeg::GetReserveChunkSize(total, 0), 0U);
}

/**
 * HybmDevLegacySegment_Import_WhenShareDisabled
 *  - 当 options_.shared=false 时，Import 会立即返回 BM_OK，不解析任何交换信息。
 */
TEST_F(HybmMemSegmentTest, HybmDevLegacySegment_Import_WhenShareDisabled)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0; // 本地 rank 0

    ock::mf::HybmDevLegacySegment seg(opt, 0);

    // 构造一个来自 rank1 的 HostExportInfo，并序列化
    ock::mf::HbmExportInfo info{};
    info.gva = 0x1000;
    info.sliceIndex = 1;
    info.rankId = 1;
    info.size = 0x2000;
    info.pageTblType = ock::mf::MEM_PT_TYPE_SVM;
    info.memSegType = ock::mf::HYBM_MST_DRAM;
    info.exchangeType = ock::mf::HYBM_INFO_EXG_IN_NODE;

    std::string encoded;
    auto serRet = ock::mf::LiteralExInfoTranslater<ock::mf::HbmExportInfo>{}.Serialize(info, encoded);
    ASSERT_EQ(serRet, BM_OK);

    std::vector<std::string> allExInfo{encoded};

    void *addresses[1]{};
    auto ret = seg.Import(allExInfo, addresses, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_OK);
    // imports_ 应该包含一条记录；必须先确认非空，否则失败时下标访问会 SEGV 掉整个测试进程
    ASSERT_EQ(seg.imports_.size(), 1U) << "Import ret=" << ret;
    EXPECT_EQ(seg.imports_[0].gva, info.gva);

    ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);

    ret = seg.Unmap();
    EXPECT_EQ(ret, BM_OK);

    ret = seg.RemoveImported({1});
    EXPECT_EQ(ret, BM_OK);
}

// =========================
// 3. MemSegment::CanLocalHostReaches / CanSdmaReaches 逻辑
// =========================

/**
 * CanLocalHostReaches_SameServerAndSuperPod
 *  - 本地与远端 serverId / superPodId 完全相同时，始终可达。
 *  - 对 910B 还需额外检查 deviceId 是否在同一“连接组”。
 */
TEST_F(HybmMemSegmentTest, CanLocalHostReaches_SameServerAndSuperPod)
{
    ock::mf::MemSegment::superPodId_ = 0x12;
    ock::mf::MemSegment::serverId_ = 0x34;
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C; // 非 910B，忽略 device 分组
    ock::mf::MemSegment::deviceId_ = 0;

    EXPECT_TRUE(ock::mf::MemSegment::CanLocalHostReaches(0x12, 0x34, 7));
}

/**
 * CanLocalHostReaches_DifferentServerOrSuperPod
 *  - serverId 或 superPodId 不一致时，返回 false。
 */
TEST_F(HybmMemSegmentTest, CanLocalHostReaches_DifferentServerOrSuperPod)
{
    ock::mf::MemSegment::superPodId_ = 0x12;
    ock::mf::MemSegment::serverId_ = 0x34;
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;
    ock::mf::MemSegment::deviceId_ = 0;

    EXPECT_FALSE(ock::mf::MemSegment::CanLocalHostReaches(0x13, 0x34, 0));
    EXPECT_FALSE(ock::mf::MemSegment::CanLocalHostReaches(0x12, 0x35, 0));
}

/**
 * CanSdmaReaches_SameServer_DiffSuperPod
 *  - serverId 相同且 SoC 类型/设备分组满足要求时，认为 SDMA 可达。
 */
TEST_F(HybmMemSegmentTest, CanSdmaReaches_SameServer_DiffSuperPod)
{
    ock::mf::MemSegment::serverId_ = 0x56;
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C; // 非 910B，不受连接分组限制
    ock::mf::MemSegment::deviceId_ = 0;
    ock::mf::MemSegment::superPodId_ = ock::mf::invalidSuperPodId; // 本端 superPodId 未知

    EXPECT_TRUE(ock::mf::MemSegment::CanSdmaReaches(ock::mf::invalidSuperPodId, 0x56, 1));
}

// =========================
// 4. HybmConnBasedSegment 更深入的行为用例
// =========================

/**
 * ConnBasedSegment_ExportSlice_UsesCache
 *  - 当 exportMap_ 中已有该 slice 的导出信息时，Export 直接从缓存返回。
 *  - 这可以帮助理解：第一次导出会构造 HostExportInfo，后续重复导出直接走缓存。
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_ExportSlice_UsesCache)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0;

    ock::mf::HybmConnBasedSegment seg(opt, 0);

    // 构造一个 slice，并放入内部 map
    auto slice =
        std::make_shared<ock::mf::MemSlice>(1, HYBM_MEM_TYPE_HOST, ock::mf::MEM_PT_TYPE_SVM, 0x1000, 0x1000, 0x2000);
    seg.slices_.emplace(slice->index_, ock::mf::MemSliceStatus(slice));

    auto getSlice = seg.GetMemSlice(&slice, true);
    EXPECT_EQ(getSlice, nullptr);
    getSlice = seg.GetMemSlice(&slice, false);
    EXPECT_EQ(getSlice, nullptr);

    // 预置缓存
    std::string cached = "export-info-cache";
    seg.exportMap_[slice->index_] = cached;

    std::string exInfo;
    auto ret = seg.Export(slice, exInfo);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(exInfo, cached);
}

// 第一次导出构造 HostExportInfo
TEST_F(HybmMemSegmentTest, ConnBasedSegment_ExportSlice_Not_UsesCache)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0;

    ock::mf::AllocatedGvaInfo info{};
    info.base.va[ock::mf::HVM_HVA] = 0x1000;
    info.base.size = 0x2000;
    info.base.memType = HYBM_MEM_TYPE_HOST;

    ock::mf::HybmConnBasedSegment seg(opt, 0);
    ock::mf::HybmVaManager::GetInstance().AllocReserveGva(
        {opt.maxSize * opt.rankCnt, opt.maxSize * opt.rankCnt, HYBM_MEM_TYPE_HOST});
    EXPECT_EQ(ock::mf::HybmVaManager::GetInstance().AddVaInfo(info), BM_OK);

    // 构造一个 slice，并放入内部 map
    auto slice =
        std::make_shared<ock::mf::MemSlice>(255, HYBM_MEM_TYPE_HOST, ock::mf::MEM_PT_TYPE_SVM, 0x1000, 0x1000, 0x2000);
    seg.slices_.emplace(slice->index_, ock::mf::MemSliceStatus(slice));

    auto getSlice = seg.GetMemSlice(&slice, true);
    EXPECT_EQ(getSlice, nullptr);
    getSlice = seg.GetMemSlice(&slice, false);
    EXPECT_EQ(getSlice, nullptr);

    std::string exInfo;
    auto ret = seg.Export(slice, exInfo);
    EXPECT_EQ(ret, BM_OK);
    ret = seg.Export(exInfo);
    ock::mf::HybmVaManager::GetInstance().RemoveOneVaInfo(0x1000, ock::mf::HVM_HVA);
    EXPECT_EQ(ret, BM_OK);
}

/**
 * ConnBasedSegment_ExportSlice_InvalidSlice
 *  - 当传入的 slice 不在 slices_ 中时，Export 返回 BM_INVALID_PARAM。
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_ExportSlice_InvalidSlice)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0;

    ock::mf::HybmConnBasedSegment seg(opt, 0);

    auto slice =
        std::make_shared<ock::mf::MemSlice>(2, HYBM_MEM_TYPE_HOST, ock::mf::MEM_PT_TYPE_SVM, 0x2000, 0x2000, 0x1000);
    std::string exInfo;
    auto ret = seg.Export(slice, exInfo);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    // slice 为 空
    ret = seg.Export(nullptr, exInfo);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

/**
 * ConnBasedSegment_Import_AddsRemoteVaInfo
 *  - 通过序列化/反序列化 HostExportInfo 来模拟跨节点导入：
 *    - 本 rank 为 0，导入 rank1 的 HostExportInfo。
 *    - Import calls HybmVaManager::AddVaInfo with AddressOwnership::Imported to register the remote GVA range.
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_Import_AddsRemoteVaInfo)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0; // 本地 rank 0

    ock::mf::HybmConnBasedSegment seg(opt, 0);
    ock::mf::HybmVaManager::GetInstance().AllocReserveGva(
        {opt.maxSize * opt.rankCnt, opt.maxSize * opt.rankCnt, HYBM_MEM_TYPE_HOST});

    // 构造一个来自 rank1 的 HostExportInfo，并序列化
    ock::mf::HostExportInfo info{};
    info.rankId = 1;
    info.gva = opt.maxSize * info.rankId;
    info.sliceIndex = 1;
    info.size = 0x2000;
    info.pageTblType = ock::mf::MEM_PT_TYPE_SVM;
    info.memSegType = ock::mf::HYBM_MST_DRAM;
    info.exchangeType = ock::mf::HYBM_INFO_EXG_IN_NODE;

    std::string encoded;
    auto serRet = ock::mf::LiteralExInfoTranslater<ock::mf::HostExportInfo>{}.Serialize(info, encoded);
    ASSERT_EQ(serRet, BM_OK);

    std::vector<std::string> allExInfo{encoded};

    void *addresses[1]{};
    auto ret = seg.Import(allExInfo, addresses, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_OK);
    // imports_ 应该包含一条记录；必须先确认非空，否则失败时下标访问会 SEGV 掉整个测试进程
    ASSERT_EQ(seg.imports_.size(), 1U) << "Import ret=" << ret;
    EXPECT_EQ(seg.imports_[0].gva, info.gva);
    ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    ret = seg.Unmap();
    EXPECT_EQ(ret, BM_OK);
    ret = seg.RemoveImported({1});
    EXPECT_EQ(ret, BM_OK);
}

/**
 * ConnBasedSegment_MmapAndUnmap_IntegratesWithVaManager
 *  - Mmap：将 imports_ 中除本 rank 外的记录加入 mappedMem_，并清空 imports_。
 *  - Unmap：调用 HybmVaManager::RemoveOneVaInfo 清理所有映射，再清空 mappedMem_。
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_MmapAndUnmap_IntegratesWithVaManager)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0;

    ock::mf::HybmConnBasedSegment seg(opt, 0);

    ock::mf::HostExportInfo local{};
    local.gva = 0x1000;
    local.rankId = 0;
    local.size = 0x1000;

    ock::mf::HostExportInfo remote{};
    remote.gva = 0x2000;
    remote.rankId = 1;
    remote.size = 0x1000;

    seg.imports_.push_back(local);
    seg.imports_.push_back(remote);

    // Mmap 只处理远端 rank
    auto ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.imports_.empty());
    ASSERT_EQ(seg.mappedGvaMem_.size(), 1U) << "Mmap ret=" << ret;
    EXPECT_EQ(*seg.mappedGvaMem_.begin(), remote.gva);

    ret = seg.Unmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.mappedGvaMem_.empty());
}

/**
 * HybmDevLegacySegment_MmapAndUnmap_IntegratesWithVaManager
 *  - Mmap：将 imports_ 中除本 rank 外的记录加入 mappedMem_，并清空 imports_。
 */
TEST_F(HybmMemSegmentTest, HybmDevLegacySegment_MmapAndUnmap_IntegratesWithVaManager)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 2;
    opt.rankId = 0;

    ock::mf::HybmDevLegacySegment seg(opt, 0);

    ock::mf::HbmExportInfo local{};
    local.gva = 0x1000;
    local.rankId = 0;
    local.size = 0x1000;

    ock::mf::HbmExportInfo remote{};
    remote.gva = 0x2000;
    remote.rankId = 1;
    remote.size = 0x1000;
    remote.superPodId = 1;

    seg.imports_.push_back(local);
    seg.imports_.push_back(remote);

    MOCKER_CPP(&ock::mf::MemSegment::CanSdmaReaches, bool (*)(ock::mf::MemSegment *, uint32_t, uint32_t, uint32_t))
        .stubs()
        .will(returnValue(true));

    // Mmap 只处理远端 rank
    auto ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.imports_.empty());
    ASSERT_EQ(seg.mappedGvaMem_.size(), 1U) << "Mmap ret=" << ret;
    EXPECT_EQ(*seg.mappedGvaMem_.begin(), remote.gva);

    ret = seg.Unmap();
    EXPECT_EQ(ret, BM_OK);
    EXPECT_TRUE(seg.mappedGvaMem_.empty());
}

// 测试 MemSegment 内存范围检查
TEST_F(HybmMemSegmentTest, HybmConnBasedSegment_MemoryInRange)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    auto segment = ock::mf::HybmConnBasedSegment::Create(options, 600);
    if (segment) {
        // 测试内存范围检查
        bool inRange = segment->MemoryInRange(nullptr, 0);
        EXPECT_EQ(inRange, true);

        // 测试获取内存类型
        hybm_mem_type memType = segment->GetMemoryType();
        EXPECT_EQ(memType, HYBM_MEM_TYPE_HOST);
    }
}

TEST_F(HybmMemSegmentTest, HybmDevLegacySegment_MemoryInRange)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    auto segment = ock::mf::HybmDevLegacySegment::Create(options, 600);
    if (segment) {
        // 测试内存范围检查
        bool inRange = segment->MemoryInRange(nullptr, 0);
        EXPECT_EQ(inRange, true);

        // 测试获取内存类型
        hybm_mem_type memType = segment->GetMemoryType();
        EXPECT_EQ(memType, HYBM_MEM_TYPE_HOST);
    }
}

/**
 * ConnBasedSegment_GetExportSliceSize_ReturnsStructSize
 */
TEST_F(HybmMemSegmentTest, ConnBasedSegment_GetExportSliceSize_ReturnsStructSize)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;

    ock::mf::HybmConnBasedSegment seg(opt, 0);
    size_t size = 0;
    auto ret = seg.GetExportSliceSize(size);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(size, sizeof(ock::mf::HostExportInfo));
}

// 测试 HybmConnBasedSegment ReserveMemorySpace 功能
TEST_F(HybmMemSegmentTest, HybmConnBasedSegment_ReserveMemorySpace)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.enable56BitsGva = true;

    // 测试构造和参数验证
    ock::mf::HybmConnBasedSegment segment(options, 100);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    // 测试内存预留
    void *address;
    auto reserveRet = segment.ReserveMemorySpace(&address);
    EXPECT_EQ(reserveRet, BM_OK);

    // AllocLocalMemory
    ock::mf::MemSlicePtr slice;
    auto ret = segment.AllocLocalMemory(1, slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    ret = segment.AllocLocalMemory(0, slice);
    EXPECT_EQ(ret, BM_OK);

    ret = segment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);

    // 测试内存释放
    auto unreserveRet = segment.UnReserveMemorySpace();
    EXPECT_EQ(unreserveRet, BM_OK);
}

// 测试 MemSegment SDMA 可达性检查
TEST_F(HybmMemSegmentTest, HybmConnBasedSegment_CheckSdmaReaches)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmConnBasedSegment segment(options, 100);
    // 测试 SDMA 可达性检查
    bool sdmaReaches = segment.CheckSdmaReaches(0);
    EXPECT_EQ(sdmaReaches, false);
}

// =========================
// 5. HybmDevLegacySegment 更深入的导出逻辑
// =========================

/**
 * DevLegacySegment_ExportSlice_UsesCache
 *  - 当 exportMap_ 中已有 slice 导出信息时，Export 直接复用。
 *  - 对 HBM 设备段而言，重复导出会走缓存，以避免重复调用底层 IPC 命名接口。
 */
TEST_F(HybmMemSegmentTest, DevLegacySegment_ExportSlice_UsesCache)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;
    opt.devId = 0;

    ock::mf::HybmDevLegacySegment seg(opt, 0);
    auto slice =
        std::make_shared<ock::mf::MemSlice>(1, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM, 0x3000, 0x3000, 0);
    seg.slices_.emplace(slice->index_, ock::mf::MemSliceStatus(slice));

    ock::mf::HbmExportInfo info{};
    seg.exportMap_[slice->index_] = info;

    std::string exInfo;
    auto ret = seg.Export(slice, exInfo);

    std::string base;
    ock::mf::LiteralExInfoTranslater<ock::mf::HbmExportInfo>{}.Serialize(info, base);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(base, exInfo);
}

// 测试 HybmDevLegacySegment 功能
TEST_F(HybmMemSegmentTest, HybmDevLegacySegment_ValidateOptions)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::HybmDevLegacySegment segment(options, 300);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);
}

// 测试 HybmVmmBasedSegment ReserveMemorySpace 功能
TEST_F(HybmMemSegmentTest, HybmDevLegacySegment_ReserveMemorySpace)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::HybmDevLegacySegment segment(options, 100);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    // 测试内存预留
    void *address;
    auto reserveRet = segment.ReserveMemorySpace(&address);
    EXPECT_EQ(reserveRet, BM_OK);

    // AllocLocalMemory
    ock::mf::MemSlicePtr slice;
    auto ret = segment.AllocLocalMemory(1, slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    ret = segment.AllocLocalMemory(0, slice);
    EXPECT_EQ(ret, BM_OK);

    ret = segment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);

    // 测试内存释放
    auto unreserveRet = segment.UnReserveMemorySpace();
    EXPECT_EQ(unreserveRet, BM_OK);
}

/**
 * DevLegacySegment_ExportSlice_InvalidSlice
 *  - slices_ 中找不到 slice 或句柄不匹配时，返回 BM_INVALID_PARAM。
 */
TEST_F(HybmMemSegmentTest, DevLegacySegment_ExportSlice_InvalidSlice)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;
    opt.devId = 0;

    ock::mf::HybmDevLegacySegment seg(opt, 0);
    auto slice =
        std::make_shared<ock::mf::MemSlice>(2, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM, 0x4000, 0x4000, 0x1000);

    std::string exInfo;
    auto ret = seg.Export(slice, exInfo);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// 测试 MemSegment SDMA 可达性检查
TEST_F(HybmMemSegmentTest, DevLegacySegment_CheckSdmaReaches)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 100);
    ock::mf::HbmExportInfo exportInfo;
    exportInfo.superPodId = 0;
    segment.importMap_[0] = exportInfo;
    segment.superPodId_ = 0;
    // 测试 SDMA 可达性检查
    bool sdmaReaches = segment.CheckSdmaReaches(0);
    EXPECT_EQ(sdmaReaches, true);

    uint64_t addr;
    ock::mf::MemSlicePtr slice;
    auto ret = segment.RegisterMemory(&addr, 0, slice);
    EXPECT_EQ(ret, BM_OK);

    hybm_mem_slice_t memLice;
    auto retGet = segment.GetMemSlice(memLice, true);
    EXPECT_EQ(retGet, nullptr);
}

// =========================
// 6. HybmVmmBasedSegment 较轻量的导入/导出逻辑
// =========================

/**
 * VmmBasedSegment_GetExportSliceSize_ReturnsStructSize
 */
TEST_F(HybmMemSegmentTest, VmmBasedSegment_GetExportSliceSize_ReturnsStructSize)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;

    ock::mf::HybmVmmBasedSegment seg(opt, 0);
    size_t size = 0;
    auto ret = seg.GetExportSliceSize(size);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(size, sizeof(ock::mf::HostSdmaExportInfo));
}

/**
 * VmmBasedSegment_Import_WhenShareDisabled
 *  - 当 options_.shared=false 时，Import 仍会尝试反序列化输入信息，
 *    无效的输入会导致反序列化失败，返回 BM_INVALID_PARAM。
 */
TEST_F(HybmMemSegmentTest, VmmBasedSegment_Import_WhenShareDisabled)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;
    opt.shared = false;

    ock::mf::HybmVmmBasedSegment seg(opt, 0);
    std::vector<std::string> allExInfo = {"dummy-info"};
    void *addresses[1]{};
    auto ret = seg.Import(allExInfo, addresses, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    ret = seg.Mmap();
    EXPECT_EQ(ret, BM_OK);
    ret = seg.Unmap();
    EXPECT_EQ(ret, BM_OK);
    ret = seg.RemoveImported({0});
    EXPECT_EQ(ret, BM_OK);
}

// 测试 HybmVmmBasedSegment ReserveMemorySpace 功能
TEST_F(HybmMemSegmentTest, HybmVmmBasedSegment_ReserveMemorySpace)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::GB;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::HybmVmmBasedSegment segment(options, 100);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    // 测试内存预留
    void *address;
    auto reserveRet = segment.ReserveMemorySpace(&address);
    EXPECT_EQ(reserveRet, BM_OK);

    // AllocLocalMemory
    options.segType = ock::mf::HYBM_MST_HBM;
    ock::mf::MemSlicePtr slice;
    auto ret = segment.AllocLocalMemory(1, slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    ret = segment.AllocLocalMemory(0, slice);
    EXPECT_EQ(ret, BM_OK);

    ret = segment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);

    // 无效type
    options.segType = ock::mf::HYBM_MST_ASYMMETRIC;
    int eid = 100;
    ock::mf::HybmVmmBasedSegment invalidSegTypeSegment(options, eid);
    ret = invalidSegTypeSegment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice);
    EXPECT_EQ(ret, BM_NOT_INITIALIZED);

    // 测试内存释放
    auto unreserveRet = segment.UnReserveMemorySpace();
    EXPECT_EQ(unreserveRet, BM_OK);
}

// 测试 MemSegment SDMA 可达性检查
TEST_F(HybmMemSegmentTest, HybmVmmBasedSegment_CheckSdmaReaches)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 2;
    options.rankId = 0;

    ock::mf::HybmVmmBasedSegment segment(options, 100);
    EXPECT_TRUE(segment.CheckSdmaReaches(0));
    EXPECT_FALSE(segment.CheckSdmaReaches(1));
}

// 测试 MemSegment SDMA 可达性检查
TEST_F(HybmMemSegmentTest, AsymmetricMemSegment_CheckSdmaReaches)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_DRAM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    ock::mf::HbmExportDeviceInfo exportInfo;
    exportInfo.superPodId = 0;
    segment.importedDeviceInfo_[0] = exportInfo;
    segment.superPodId_ = 0;
    // 测试 SDMA 可达性检查
    bool sdmaReaches = segment.CheckSdmaReaches(0);
    EXPECT_EQ(sdmaReaches, true);
}

// 测试 HybmVmmBasedSegment ReserveMemorySpace 功能
TEST_F(HybmMemSegmentTest, AsymmetricMemSegment_ReserveMemorySpace)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    // 测试内存预留
    void *address;
    auto reserveRet = segment.ReserveMemorySpace(&address);
    EXPECT_EQ(reserveRet, BM_INVALID_PARAM);

    // AllocLocalMemory
    options.segType = ock::mf::HYBM_MST_DRAM;
    ock::mf::MemSlicePtr slice;

    auto ret = segment.RegisterMemory(nullptr, 0, slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    uint64_t addr;
    ret = segment.RegisterMemory(&addr, 0, slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);

    ret = segment.AllocLocalMemory(0, slice);
    EXPECT_EQ(ret, BM_NOT_SUPPORTED);
}

// =========================
// 7. MemSegment 静态成员 / 设备信息与工厂分支补充
// =========================

/**
* ResetDeviceInfoInChild_ResetsStatics
*  - 覆盖全部静态成员复位逻辑。
*/
TEST_F(HybmMemSegmentTest, ResetDeviceInfoInChild_ResetsStatics)
{
    ock::mf::MemSegment::deviceInfoReady_ = true;
    ock::mf::MemSegment::deviceId_ = 3;      // 3
    ock::mf::MemSegment::logicDeviceId_ = 4; // 4
    ock::mf::MemSegment::devicePhyId_ = 5;   // 5
    ock::mf::MemSegment::pid_ = 6U;          // 6
    ock::mf::MemSegment::sdid_ = 7U;         // 7
    ock::mf::MemSegment::serverId_ = 8U;     // 8
    ock::mf::MemSegment::superPodId_ = 9U;   // 9
    ock::mf::MemSegment::sysBoolId_ = "test-boot";
    ock::mf::MemSegment::bootIdHead_ = 10U; // 10
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910C;

    ock::mf::MemSegment::ResetDeviceInfoInChild();

    EXPECT_FALSE(ock::mf::MemSegment::deviceInfoReady_);
    EXPECT_EQ(ock::mf::MemSegment::deviceId_, -1);
    EXPECT_EQ(ock::mf::MemSegment::logicDeviceId_, -1);
    EXPECT_EQ(ock::mf::MemSegment::devicePhyId_, -1);
    EXPECT_EQ(ock::mf::MemSegment::pid_, 0U);
    EXPECT_EQ(ock::mf::MemSegment::sdid_, 0U);
    EXPECT_EQ(ock::mf::MemSegment::serverId_, 0U);
    EXPECT_EQ(ock::mf::MemSegment::superPodId_, 0U);
    EXPECT_TRUE(ock::mf::MemSegment::sysBoolId_.empty());
    EXPECT_EQ(ock::mf::MemSegment::bootIdHead_, 0U);
    EXPECT_EQ(ock::mf::MemSegment::socType_, ock::mf::AscendSocType::ASCEND_UNKNOWN);
}

/**
* InitDeviceInfo_AlreadyReady_SameAndDifferentDev
*  - deviceInfoReady_ 后：相同 devId 返回 OK，不同 devId 返回 INVALID_PARAM。
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_AlreadyReady_SameAndDifferentDev)
{
    ock::mf::MemSegment::deviceInfoReady_ = true;
    ock::mf::MemSegment::deviceId_ = 5; // 5

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(5), BM_OK);            // 5
    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(6), BM_INVALID_PARAM); // 6
}

/**
* EnableRemotePeerAccess_InvalidAndSelf
*  - 负 phyId → INVALID_PARAM；与本地 phyId 相同 → OK。
*/
TEST_F(HybmMemSegmentTest, EnableRemotePeerAccess_InvalidAndSelf)
{
    EXPECT_EQ(ock::mf::MemSegment::EnableRemotePeerAccess(-1), BM_INVALID_PARAM);

    ock::mf::MemSegment::devicePhyId_ = 7;                            // 7
    EXPECT_EQ(ock::mf::MemSegment::EnableRemotePeerAccess(7), BM_OK); // 7
}

/**
* CanSdmaReaches_InvalidSuperPodBranches
*  - 本端 superPodId 未知 → false；本端有效时按 superPodId 相等判断。
*/
TEST_F(HybmMemSegmentTest, CanSdmaReaches_InvalidSuperPodBranches)
{
    ock::mf::MemSegment::serverId_ = 0x56;
    ock::mf::MemSegment::superPodId_ = ock::mf::invalidSuperPodId;

    EXPECT_FALSE(ock::mf::MemSegment::CanSdmaReaches(0x99, 0x78, 1));

    ock::mf::MemSegment::superPodId_ = 0xAA;
    EXPECT_TRUE(ock::mf::MemSegment::CanSdmaReaches(0xAA, 0x78, 1));
    EXPECT_FALSE(ock::mf::MemSegment::CanSdmaReaches(0xBB, 0x78, 1));
}

/**
* Create_Dram_HostShmFlag_UsesHostShmSegment
*  - DRAM 段且 dataOpType 含 HOST_SHM → HybmHostShmSegment。
*/
TEST_F(HybmMemSegmentTest, Create_Dram_HostShmFlag_UsesHostShmSegment)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2; // 2
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_DRAM;
    opt.dataOpType = HYBM_DOP_TYPE_HOST_SHM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));
    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910B;

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    ASSERT_NE(seg, nullptr);
    auto hostSeg = std::dynamic_pointer_cast<ock::mf::HybmHostShmSegment>(seg);
    EXPECT_NE(hostSeg, nullptr);
}

/**
* Create_InvalidSegType_ReturnsNull
*  - 非法 segType 走 default 分支，返回 nullptr。
*/
TEST_F(HybmMemSegmentTest, Create_InvalidSegType_ReturnsNull)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2; // 2
    opt.rankId = 0;
    opt.segType = static_cast<ock::mf::MemSegType>(0x7FFF);
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));
    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910B;

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    EXPECT_EQ(seg, nullptr);
}

/**
* Create_Fails_When_VaManagerInitFailed
*  - HybmVaManager::Initialize 失败 → Create 返回 nullptr。
*/
TEST_F(HybmMemSegmentTest, Create_Fails_When_VaManagerInitFailed)
{
    ock::mf::MemSegmentOptions opt{};
    opt.rankCnt = 2;
    opt.rankId = 0;
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER(ock::mf::HybmGetGvaVersion).stubs().will(returnValue(ock::mf::HYBM_GVA_V3));
    MOCKER(ock::mf::MemSegment::InitDeviceInfo).stubs().will(returnValue(0));
    ock::mf::MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_910B;
    MOCKER_CPP(&ock::mf::HybmVaManager::Initialize,
               ock::mf::Result(*)(ock::mf::HybmVaManager *, ock::mf::AscendSocType))
        .stubs()
        .will(returnValue(BM_ERROR));

    auto seg = ock::mf::MemSegment::Create(opt, 0);
    EXPECT_EQ(seg, nullptr);
}

/**
* DevLegacySegment_RegisterMemory_ZeroVa_Fails
*  - RegisterMemCommon：va=0 → GetLocalMemoryType 失败 → 直接返回。
*/
TEST_F(HybmMemSegmentTest, DevLegacySegment_RegisterMemory_ZeroVa_Fails)
{
    ock::mf::MemSegmentOptions opt{};
    opt.segType = ock::mf::HYBM_MST_HBM;
    opt.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    opt.rankCnt = 1;
    opt.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(opt, 0);
    ock::mf::MemSlicePtr slice;
    EXPECT_EQ(segment.RegisterMemory(nullptr, 0, slice), BM_INVALID_PARAM);
}

/**
* InitDeviceInfo_AclrtSetDeviceFails
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_AclrtSetDeviceFails)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    ock::mf::DlAclApi::pAclrtSetDevice = [](int32_t) -> int32_t { return -1; };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

/**
* InitDeviceInfo_RtGetLogicDevIdFails
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_RtGetLogicDevIdFails)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    ock::mf::DlAclApi::pRtGetLogicDevIdByUserDevId = [](const int32_t, int32_t *const) -> int32_t { return -1; };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

/**
* InitDeviceInfo_RtDeviceGetBareTgidFails
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_RtDeviceGetBareTgidFails)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    ock::mf::DlAclApi::pRtDeviceGetBareTgid = [](uint32_t *) -> int32_t { return -1; };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

/**
* InitDeviceInfo_RtGetDeviceInfoFails_Sdid / ServerId / SuperPodId
*  - 分别在第一次/第二次/第三次 RtGetDeviceInfo 调用处失败。
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_RtGetDeviceInfoFails_Sdid)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    g_rtGetDeviceInfoCallCount = 0;
    ock::mf::DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t, int64_t *value) -> int32_t {
        g_rtGetDeviceInfoCallCount++;
        if (g_rtGetDeviceInfoCallCount == 1) {
            return -1;
        }
        *value = 0x1000;
        return 0;
    };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

TEST_F(HybmMemSegmentTest, InitDeviceInfo_RtGetDeviceInfoFails_ServerId)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    g_rtGetDeviceInfoCallCount = 0;
    ock::mf::DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t, int64_t *value) -> int32_t {
        g_rtGetDeviceInfoCallCount++;
        if (g_rtGetDeviceInfoCallCount == 2) { // 2
            return -1;
        }
        *value = 0x1000;
        return 0;
    };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

TEST_F(HybmMemSegmentTest, InitDeviceInfo_RtGetDeviceInfoFails_SuperPodId)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    g_rtGetDeviceInfoCallCount = 0;
    ock::mf::DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t, int64_t *value) -> int32_t {
        g_rtGetDeviceInfoCallCount++;
        if (g_rtGetDeviceInfoCallCount == 3) { // 3
            return -1;
        }
        *value = 0x1000;
        return 0;
    };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_DL_FUNCTION_FAILED);
}

/**
* InitDeviceInfo_SuccessWithInvalidPodServer_FallsBackToBootId
*  - 设备信息全成功但 superPodId/serverId 均为无效值 → serverId_ 回退为 bootIdHead_。
*/
TEST_F(HybmMemSegmentTest, InitDeviceInfo_SuccessWithInvalidPodServer_FallsBackToBootId)
{
    ock::mf::MemSegment::deviceInfoReady_ = false;
    MemSegmentAclFnGuard guard;
    ock::mf::DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t infoType, int64_t *value) -> int32_t {
        if (infoType == ock::mf::INFO_TYPE_SDID) {
            *value = 0x1234;
            return 0;
        }
        if (infoType == ock::mf::INFO_TYPE_SERVER_ID) {
            *value = ock::mf::invalidServerId;
            return 0;
        }
        if (infoType == ock::mf::INFO_TYPE_SUPER_POD_ID) {
            *value = ock::mf::invalidSuperPodId;
            return 0;
        }
        return -1;
    };

    EXPECT_EQ(ock::mf::MemSegment::InitDeviceInfo(0), BM_OK);
    EXPECT_EQ(ock::mf::MemSegment::sdid_, 0x1234U);
    if (ock::mf::MemSegment::bootIdHead_ != 0) {
        EXPECT_EQ(ock::mf::MemSegment::serverId_, ock::mf::MemSegment::bootIdHead_);
    }
}
