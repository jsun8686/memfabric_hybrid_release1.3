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
#include "hybm_def.h"
#include "hybm_define.h"
#include "devmm_svm_gva.h"
#include "dl_acl_api.h"
#include "hybm_va_manager.h"
#include "hybm_ex_info_transfer.h"
#undef private
#undef protected

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;

namespace {
constexpr int TEST_ENTITY_ID = 200; // G.CNS.02: 段构造的 entityId 参数具名化
int g_rollbackCloseCount = 0;
// stub 签名必须与被 mock 的 API 函数指针 typedef 完全一致（G.FUN.05 的 void* 参数无法更改）
ock::mf::Result RtIpcCloseMemoryCountStub(const void *ptr)
{
    (void)ptr;
    g_rollbackCloseCount++;
    return 0;
}
} // namespace

namespace {
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
    AscendSocType socType{ock::mf::MemSegment::socType_};

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

class HybmDevSegmentTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}

    void SetUp() override
    {
        GlobalMockObject::reset();
        staticsGuard = std::make_unique<MemSegmentStaticsGuard>();
    }

    void TearDown() override
    {
        staticsGuard.reset();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    std::unique_ptr<MemSegmentStaticsGuard> staticsGuard;
};

// 测试 HybmDevLegacySegment 功能
TEST_F(HybmDevSegmentTest, HybmDevLegacySegment)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::HybmDevLegacySegment segment(options, 100);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);
}

// 测试 AsymmetricMemSegment 功能
TEST_F(HybmDevSegmentTest, AsymmetricMemSegment)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试构造和参数验证
    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);
}

// 测试设备段功能修改拦截
TEST_F(HybmDevSegmentTest, DevSegment_FunctionModification_Intercept)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    // 测试创建一致性
    ock::mf::HybmDevLegacySegment segment1(options, 300);
    ock::mf::HybmDevLegacySegment segment2(options, 400);

    // 验证参数验证的一致性
    auto ret1 = segment1.ValidateOptions();
    auto ret2 = segment2.ValidateOptions();
    EXPECT_EQ(ret1, ret2);
}

// 测试设备段边界情况
TEST_F(HybmDevSegmentTest, DevSegment_BoundaryCases)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;

    // 测试无效大小
    options.maxSize = 0;
    ock::mf::HybmDevLegacySegment segment(options, 500);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_INVALID_PARAM);

    // 测试非大页对齐大小
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE / 2;
    ock::mf::HybmDevLegacySegment segment2(options, 600);
    validateRet = segment2.ValidateOptions();
    EXPECT_EQ(validateRet, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ReleaseSliceMemory_NotExist1)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    auto unregisteredSlice = std::make_shared<ock::mf::MemSlice>(99999, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM,
                                                                 0x10000000ULL, 0x20000000ULL, 4096ULL);

    auto ret = segment.ReleaseSliceMemory(unregisteredSlice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ReleaseSliceMemory_NotExist2)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    auto slice = std::make_shared<ock::mf::MemSlice>(0xFFFF, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM,
                                                     0x10000000ULL, 0x20000000ULL, 4096ULL);

    auto ret = segment.ReleaseSliceMemory(slice);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_RollbackImportedSlices)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.shared = true; // RollbackImportedSlices 仅在 shared 段执行

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    auto validateRet = segment.ValidateOptions();
    EXPECT_EQ(validateRet, BM_OK);

    // 回滚按 remoteSlices_ 登记名分流：仅 IPC 且 vAddress_ 非零的条目关闭 RtIpcCloseMemory；
    // 空名（HOST）与零 vAddress_ 条目为 no-op
    auto hostSlice = std::make_shared<ock::mf::MemSlice>(0xFF01, HYBM_MEM_TYPE_HOST, ock::mf::MEM_PT_TYPE_SVM,
                                                         0x10000000ULL, 0x20000000ULL, 4096ULL);
    auto devSliceNoIpc = std::make_shared<ock::mf::MemSlice>(0xFF02, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM,
                                                             0x10001000ULL, 0, 4096ULL);
    auto devSliceIpc = std::make_shared<ock::mf::MemSlice>(0xFF03, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM,
                                                           0x10002000ULL, 0x20002000ULL, 4096ULL);
    std::string ipcName(ock::mf::USER_HBM_NAME_MAX_LEN, '\0');
    ipcName[0] = static_cast<char>(ock::mf::USER_HBM_NAME_TYPE_IPC);
    segment.remoteSlices_[hostSlice->index_] = ock::mf::RegisterSlice{hostSlice, ""};
    segment.remoteSlices_[devSliceNoIpc->index_] = ock::mf::RegisterSlice{devSliceNoIpc, ipcName};
    segment.remoteSlices_[devSliceIpc->index_] = ock::mf::RegisterSlice{devSliceIpc, ipcName};

    MOCKER(&ock::mf::DlAclApi::RtIpcCloseMemory).stubs().will(invoke(RtIpcCloseMemoryCountStub));

    g_rollbackCloseCount = 0;
    std::vector<ock::mf::MemSlicePtr> slices = {hostSlice, devSliceNoIpc, devSliceIpc};
    segment.RollbackImportedSlices(slices);
    EXPECT_EQ(g_rollbackCloseCount, 1);

    g_rollbackCloseCount = 0;
    segment.RollbackImportedSlices({});
    EXPECT_EQ(g_rollbackCloseCount, 0);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Import_Empty)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::vector<std::string> emptyInfos;
    void *addrs[1] = {nullptr};

    auto ret = segment.Import(emptyInfos, addrs, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Import_ValidSliceMagic)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    ock::mf::UserSliceExportInfo exportInfo{};
    exportInfo.magic = ock::mf::USER_MEM_SLICE_EXPORT_INFO_MAGIC;
    exportInfo.segmentType = ock::mf::SEGMENT_TYPE_USER_DEV;
    exportInfo.gvaOffset = 0x10000000ULL;
    exportInfo.address = 0x20000000ULL;
    exportInfo.size = 4096;
    strncpy(exportInfo.name, "test", sizeof(exportInfo.name) - 1);
    exportInfo.name[sizeof(exportInfo.name) - 1] = '\0';

    std::string infoStr(reinterpret_cast<const char *>(&exportInfo), sizeof(exportInfo));
    std::vector<std::string> allExInfo = {infoStr};
    void *addresses[1] = {nullptr};

    auto ret = segment.Import(allExInfo, addresses, HYBM_IMPORT_RANK_INVALID);

    EXPECT_NE(ret, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Import_InvalidMagic)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::string badInfo(16, 'X');
    *reinterpret_cast<uint64_t *>(badInfo.data()) = 0xBADBADBADBADULL;

    std::vector<std::string> infos = {badInfo};
    void *addrs[1] = {nullptr};

    auto ret = segment.Import(infos, addrs, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Import_AddressesNull)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::string badInfo(16, 'X');
    *reinterpret_cast<uint64_t *>(badInfo.data()) = 0xBADBADBADBADULL;

    std::vector<std::string> infos = {badInfo};

    auto ret = segment.Import(infos, nullptr, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_RemoveImported_NoCrash)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    EXPECT_EQ(segment.RemoveImported({}), BM_OK);

    EXPECT_EQ(segment.RemoveImported({999}), BM_OK);

    EXPECT_EQ(segment.RemoveImported({1, 2, 3}), BM_OK);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_RemoveSliceInfo_RankNotExist)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    segment.RemoveSliceInfo(999);
    SUCCEED();
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_RemoveSliceInfo_SingleSliceNoSdma)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.dataOpType = HYBM_DOP_TYPE_DEFAULT;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    const uint32_t rankId = 5;
    const uint32_t sliceIndex = 100;
    const uint64_t gva = 0x10000000ULL;
    const uint64_t vAddress = 0x20000000ULL;
    const std::string sliceName = "slice_5_100";

    auto remoteSlice = std::make_shared<ock::mf::MemSlice>(sliceIndex, HYBM_MEM_TYPE_DEVICE, ock::mf::MEM_PT_TYPE_SVM,
                                                           gva, vAddress, 4096ULL);

    segment.rankToRemoteSlices_[rankId].assign({remoteSlice});

    segment.remoteSlices_[static_cast<uint16_t>(sliceIndex)] = ock::mf::RegisterSlice(remoteSlice, sliceName);

    ock::mf::UserSliceExportInfo exportInfo{};
    exportInfo.magic = ock::mf::USER_MEM_SLICE_EXPORT_INFO_MAGIC;
    exportInfo.segmentType = ock::mf::SEGMENT_TYPE_USER_DEV;
    exportInfo.gvaOffset = gva;
    exportInfo.address = vAddress;
    exportInfo.size = 4096;
    exportInfo.rankId = rankId;
    exportInfo.devicePhyId = 0;
    exportInfo.superPodId = 0;
    exportInfo.serverId = 0;
    strncpy(exportInfo.name, sliceName.c_str(), sizeof(exportInfo.name) - 1);
    exportInfo.name[sizeof(exportInfo.name) - 1] = '\0';

    segment.importedSliceInfo_[sliceName] = exportInfo;

    EXPECT_EQ(segment.rankToRemoteSlices_.count(rankId), 1U);
    EXPECT_EQ(segment.remoteSlices_.count(static_cast<uint16_t>(sliceIndex)), 1U);
    EXPECT_EQ(segment.importedSliceInfo_.count(sliceName), 1U);

    segment.RemoveSliceInfo(rankId);

    EXPECT_EQ(segment.rankToRemoteSlices_.count(rankId), 0U);
    EXPECT_EQ(segment.remoteSlices_.count(static_cast<uint16_t>(sliceIndex)), 0U);
    EXPECT_EQ(segment.importedSliceInfo_.count(sliceName), 0U);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Mmap_NotSupported)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    auto ret = segment.Mmap();

    EXPECT_EQ(ret, BM_NOT_SUPPORTED);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_Unmap_NotSupported)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    auto ret = segment.Unmap();

    EXPECT_EQ(ret, BM_NOT_SUPPORTED);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportDeviceInfo_DeserializeFailed)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::string badInfo = "invalid_serialized_data";
    auto ret = segment.ImportDeviceInfo(badInfo, HYBM_IMPORT_RANK_INVALID);
    EXPECT_NE(ret, BM_OK);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportDeviceInfo_InvalidLogicDeviceId)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    ock::mf::HbmExportDeviceInfo deviceInfo{};
    deviceInfo.magic = ock::mf::ENTITY_EXPORT_INFO_MAGIC;
    deviceInfo.devicePhyId = 16;
    deviceInfo.rankId = 0;
    deviceInfo.sdid = 123;
    deviceInfo.pid = 456;

    std::string info(reinterpret_cast<const char *>(&deviceInfo), sizeof(deviceInfo));
    auto ret = segment.ImportDeviceInfo(info, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportDeviceInfo_SuccessNoP2PNoSlices)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    const uint32_t localDeviceId = 5;
    segment.logicDeviceId_ = localDeviceId;
    segment.devicePhyId_ = localDeviceId;
    segment.deviceId_ = 0;

    ASSERT_TRUE(segment.registerSlices_.empty());

    ock::mf::HbmExportDeviceInfo deviceInfo{};
    deviceInfo.magic = ock::mf::ENTITY_EXPORT_INFO_MAGIC;
    deviceInfo.devicePhyId = localDeviceId;
    deviceInfo.rankId = 10;
    deviceInfo.sdid = 123;
    deviceInfo.pid = 456;

    std::string info(reinterpret_cast<const char *>(&deviceInfo), sizeof(deviceInfo));
    auto ret = segment.ImportDeviceInfo(info, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_OK);

    EXPECT_TRUE(segment.importedDeviceInfo_.count(10) > 0);
    const auto &stored = segment.importedDeviceInfo_.at(10);
    EXPECT_EQ(stored.devicePhyId, localDeviceId);
    EXPECT_EQ(stored.rankId, 10U);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportSliceInfo_DeserializeFailed)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.dataOpType = 0; // avoid hardware paths

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::string badInfo = "invalid_data";
    ock::mf::MemSlicePtr remoteSlice;
    auto ret = segment.ImportSliceInfo(badInfo, remoteSlice, HYBM_IMPORT_RANK_INVALID);
    EXPECT_NE(ret, BM_OK);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportSliceInfo_InvalidLogicDeviceId)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.dataOpType = 0;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    ock::mf::UserSliceExportInfo sliceInfo{};
    sliceInfo.devicePhyId = 16; // >= MAX_DEVICE_COUNT (16) → invalid
    sliceInfo.rankId = 5;
    sliceInfo.gvaOffset = 0x10000000ULL;
    sliceInfo.size = 4096;
    strncpy(sliceInfo.name, "test_slice", sizeof(sliceInfo.name) - 1);

    std::string info(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    ock::mf::MemSlicePtr remoteSlice;
    auto ret = segment.ImportSliceInfo(info, remoteSlice, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_ERROR);
}

TEST_F(HybmDevSegmentTest, AsymmetricMemSegment_ImportSliceInfo_SuccessNoHardware)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.dataOpType = 0; // ← 关键：禁用 SDMA/RDMA
    options.shared = false;

    ock::mf::AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    // Prepare valid slice info
    ock::mf::UserSliceExportInfo sliceInfo{};
    sliceInfo.segmentType = ock::mf::SEGMENT_TYPE_USER_DEV;
    sliceInfo.devicePhyId = 5; // < 16, valid
    sliceInfo.rankId = 10;
    sliceInfo.gvaOffset = 0x10000000ULL;
    sliceInfo.size = 4096;
    strncpy(sliceInfo.name, "slice_10_5", sizeof(sliceInfo.name) - 1);

    std::string info(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    ock::mf::MemSlicePtr remoteSlice;
    auto ret = segment.ImportSliceInfo(info, remoteSlice, HYBM_IMPORT_RANK_INVALID);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(remoteSlice, nullptr);

    // Verify outputs
    EXPECT_EQ(remoteSlice->gva_, sliceInfo.gvaOffset);
    EXPECT_EQ(remoteSlice->size_, sliceInfo.size);
    EXPECT_EQ(remoteSlice->memType_, HYBM_MEM_TYPE_DEVICE);
    EXPECT_EQ(remoteSlice->index_, 0U); // first slice, sliceCount_=0 → index=0

    // Verify containers (via friend)
    EXPECT_EQ(segment.rankToRemoteSlices_.count(10), 1U);
    EXPECT_EQ(segment.rankToRemoteSlices_.at(10).size(), 1U);
    EXPECT_EQ(segment.rankToRemoteSlices_.at(10)[0], remoteSlice);

    // shared=false 亦按 129B 定长名登记（防 VMM 二进制名 strlen 截断）
    const std::string storedName(sliceInfo.name, USER_HBM_NAME_MAX_LEN);
    EXPECT_EQ(segment.remoteSlices_.count(0), 1U);
    EXPECT_EQ(segment.remoteSlices_.at(0).name, storedName);
    EXPECT_EQ(segment.remoteSlices_.at(0).slice, remoteSlice);

    EXPECT_EQ(segment.importedSliceInfo_.count(storedName), 1U);
    const auto &stored = segment.importedSliceInfo_.at(storedName);
    EXPECT_EQ(stored.gvaOffset, sliceInfo.gvaOffset);
    EXPECT_EQ(stored.rankId, 10U);
}

// =========================
// HybmDevLegacySegment 错误路径补充
// =========================

TEST_F(HybmDevSegmentTest, DevLegacy_ReleaseSliceMemory_ErrorPaths)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::HybmDevLegacySegment segment(options, 0);

    EXPECT_EQ(segment.ReleaseSliceMemory(nullptr), BM_INVALID_PARAM);

    auto unknownSlice =
        std::make_shared<ock::mf::MemSlice>(99, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0x1000, 0x2000, 4096ULL);
    EXPECT_EQ(segment.ReleaseSliceMemory(unknownSlice), BM_INVALID_PARAM);

    // 同 index 不同对象 → magic 不匹配
    auto realSlice =
        std::make_shared<ock::mf::MemSlice>(7, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0x1000, 0x2000, 4096ULL);
    segment.slices_.emplace(realSlice->index_, ock::mf::MemSliceStatus(realSlice));
    auto fakeSlice =
        std::make_shared<ock::mf::MemSlice>(7, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0x1000, 0x2000, 4096ULL);
    EXPECT_EQ(segment.ReleaseSliceMemory(fakeSlice), BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, DevLegacy_ExportNoArg_And_GetExportSliceSize)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    std::string exInfo;
    EXPECT_EQ(segment.Export(exInfo), BM_OK);

    size_t size = 0U;
    EXPECT_EQ(segment.GetExportSliceSize(size), BM_OK);
    EXPECT_EQ(size, sizeof(ock::mf::HbmExportInfo));
}

TEST_F(HybmDevSegmentTest, DevLegacy_RemoveImported_InvalidRank)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 2; // 2

    ock::mf::HybmDevLegacySegment segment(options, 0);
    EXPECT_EQ(segment.RemoveImported({2}), BM_INVALID_PARAM);
}

TEST_F(HybmDevSegmentTest, DevLegacy_GetMemSlice_ValidAndInvalid)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    ock::mf::MemSlicePtr slice;
    uint64_t addr = 0x10000000ULL;
    EXPECT_EQ(segment.RegisterMemory(&addr, 4096ULL, slice), BM_OK);
    ASSERT_NE(slice, nullptr);

    EXPECT_EQ(segment.GetMemSlice(slice->ConvertToId(), false), slice);
    EXPECT_EQ(segment.GetMemSlice(reinterpret_cast<hybm_mem_slice_t>(0xDEADBEEF), true), nullptr);
}

TEST_F(HybmDevSegmentTest, DevLegacy_MemoryInRange_BeforeBase)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    segment.globalVirtualAddress_ = reinterpret_cast<uint8_t *>(0x20000000ULL);
    segment.totalVirtualSize_ = options.maxSize;
    EXPECT_FALSE(segment.MemoryInRange(reinterpret_cast<void *>(0x10000000ULL), 4096ULL));
}

TEST_F(HybmDevSegmentTest, DevLegacy_CheckSdmaReaches_AllBranches)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;

    ock::mf::HybmDevLegacySegment segment(options, 0);

    // 无 import 记录 → false
    EXPECT_FALSE(segment.CheckSdmaReaches(0));

    ock::mf::HbmExportInfo info{};
    segment.importMap_[0] = info;
    segment.serverId_ = 0x1234;
    // serverId 相同 → true
    segment.importMap_[0].serverId = 0x1234;
    EXPECT_TRUE(segment.CheckSdmaReaches(0));

    // serverId 不同且本端 superPodId 未知 → false
    segment.importMap_[0].serverId = 0x9999;
    segment.superPodId_ = ock::mf::invalidSuperPodId;
    EXPECT_FALSE(segment.CheckSdmaReaches(0));

    // 两端 superPodId 有效且相同 → true
    segment.importMap_[0].serverId = 0x9999;
    segment.superPodId_ = 0xAA;
    segment.importMap_[0].superPodId = 0xAA;
    EXPECT_TRUE(segment.CheckSdmaReaches(0));

    // 两端 superPodId 不同 → false
    segment.importMap_[0].superPodId = 0xBB;
    EXPECT_FALSE(segment.CheckSdmaReaches(0));
}

TEST_F(HybmDevSegmentTest, DevLegacy_Import_DeserializeFailAndMagicInvalid)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 2; // 2
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    std::vector<std::string> bad{"bad_format"};
    EXPECT_EQ(segment.Import(bad, nullptr, HYBM_IMPORT_RANK_INVALID), BM_INVALID_PARAM);

    // 本 rank 自己的信息 → 跳过
    ock::mf::HbmExportInfo self{};
    self.magic = ock::mf::HBM_SLICE_EXPORT_INFO_MAGIC;
    self.rankId = options.rankId;
    std::string encoded;
    ASSERT_EQ(ock::mf::LiteralExInfoTranslater<ock::mf::HbmExportInfo>{}.Serialize(self, encoded), BM_OK);
    EXPECT_EQ(segment.Import({encoded}, nullptr, HYBM_IMPORT_RANK_INVALID), BM_OK);
}

TEST_F(HybmDevSegmentTest, DevLegacy_Mmap_SizeZeroAndAlreadyMapped)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 2; // 2
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);

    // size == 0 → skip
    ock::mf::HbmExportInfo zeroSize{};
    zeroSize.rankId = 1;
    zeroSize.size = 0;
    segment.imports_.push_back(zeroSize);

    // 已映射 gva → skip
    ock::mf::HbmExportInfo mapped{};
    mapped.rankId = 1;
    mapped.size = 4096ULL;
    mapped.gva = 0x5000ULL;
    segment.imports_.push_back(mapped);
    segment.mappedGvaMem_.insert(0x5000ULL);

    EXPECT_EQ(segment.Mmap(), BM_OK);
    EXPECT_TRUE(segment.imports_.empty());
}

namespace {
int g_halGvaReserveCalls = 0;
int32_t MockHalGvaReserveFirstOkThenFail(uint64_t *address, size_t, int32_t, uint64_t)
{
    if (g_halGvaReserveCalls++ == 0) {
        *address = 0x10000000000ULL;
        return 0;
    }
    return -1;
}
} // namespace

TEST_F(HybmDevSegmentTest, DevLegacy_ReserveMemorySpace_HalGvaReserveFails)
{
    // totalSize = rankCnt * maxSize > 128G → 分两块预留，第二块失败
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = 70ULL * ock::mf::GB;
    options.rankCnt = 2; // 2
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);

    g_halGvaReserveCalls = 0;
    MOCKER_CPP(&ock::mf::drv::HalGvaReserveMemory, int32_t(*)(uint64_t *, size_t, int32_t, uint64_t))
        .stubs()
        .will(invoke(MockHalGvaReserveFirstOkThenFail));
    MOCKER_CPP(&ock::mf::drv::HalGvaUnreserveMemory, int32_t(*)(uint64_t)).stubs().will(returnValue(BM_OK));

    void *address = nullptr;
    EXPECT_EQ(segment.ReserveMemorySpace(&address), BM_MALLOC_FAILED);
}

TEST_F(HybmDevSegmentTest, DevLegacy_AllocLocalMemory_HalGvaAllocFails)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    segment.lvaBase_ = reinterpret_cast<uint8_t *>(0x20000000ULL);
    segment.globalVirtualAddress_ = reinterpret_cast<uint8_t *>(0x10000000ULL);

    MOCKER_CPP(&ock::mf::drv::HalGvaAlloc, int32_t(*)(uint64_t, size_t, uint64_t)).stubs().will(returnValue(BM_ERROR));

    ock::mf::MemSlicePtr slice;
    EXPECT_EQ(segment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice), BM_DL_FUNCTION_FAILED);
}

TEST_F(HybmDevSegmentTest, DevLegacy_AllocLocalMemory_AddVaInfoFails)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    segment.lvaBase_ = reinterpret_cast<uint8_t *>(0x20000000ULL);
    segment.globalVirtualAddress_ = reinterpret_cast<uint8_t *>(0x10000000ULL);

    MOCKER_CPP(&ock::mf::drv::HalGvaAlloc, int32_t(*)(uint64_t, size_t, uint64_t)).stubs().will(returnValue(BM_OK));
    MOCKER_CPP(&ock::mf::drv::HalGvaFree, int32_t(*)(uint64_t, size_t)).stubs().will(returnValue(BM_OK));
    using AddVaInfoMemberFn =
        ock::mf::Result (ock::mf::HybmVaManager::*)(const BaseAllocatedGvaInfo &, bool, ock::mf::AddressOwnership);
    auto addVaInfoPtr = static_cast<AddVaInfoMemberFn>(&ock::mf::HybmVaManager::AddVaInfo);
    MOCKER_CPP(addVaInfoPtr, ock::mf::Result(*)(ock::mf::HybmVaManager *, const BaseAllocatedGvaInfo &, bool,
                                                ock::mf::AddressOwnership))
        .stubs()
        .will(returnValue(BM_ERROR));

    ock::mf::MemSlicePtr slice;
    EXPECT_NE(segment.AllocLocalMemory(ock::mf::HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    EXPECT_TRUE(segment.slices_.empty());
}

TEST_F(HybmDevSegmentTest, DevLegacy_Export_RtIpcSetMemoryNameFails)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    auto slice = std::make_shared<ock::mf::MemSlice>(1, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0x3000, 0x3000, 4096ULL);
    segment.slices_.emplace(slice->index_, ock::mf::MemSliceStatus(slice));

    auto oldRtIpcSetMemoryName = ock::mf::DlAclApi::pRtIpcSetMemoryName;
    ock::mf::DlAclApi::pRtIpcSetMemoryName = [](const void *, size_t, char *, uint32_t) -> int32_t { return BM_ERROR; };

    std::string exInfo;
    EXPECT_NE(segment.Export(slice, exInfo), BM_OK);

    ock::mf::DlAclApi::pRtIpcSetMemoryName = oldRtIpcSetMemoryName;
}

TEST_F(HybmDevSegmentTest, DevLegacy_Export_Success_SetsExportMap)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;
    options.devId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    auto slice = std::make_shared<ock::mf::MemSlice>(3, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0x4000, 0x4000, 4096ULL);
    segment.slices_.emplace(slice->index_, ock::mf::MemSliceStatus(slice));

    auto oldRtIpcSetMemoryName = ock::mf::DlAclApi::pRtIpcSetMemoryName;
    ock::mf::DlAclApi::pRtIpcSetMemoryName = [](const void *, size_t, char *name, uint32_t nameSize) -> int32_t {
        (void)nameSize;
        const char *fakeName = "fake_shm_name";
        std::memcpy(name, fakeName, std::strlen(fakeName) + 1);
        return BM_OK;
    };
    auto oldRtIpcDestroyMemoryName = ock::mf::DlAclApi::pRtIpcDestroyMemoryName;
    ock::mf::DlAclApi::pRtIpcDestroyMemoryName = [](const char *) -> int32_t { return BM_OK; };

    std::string exInfo;
    EXPECT_EQ(segment.Export(slice, exInfo), BM_OK);
    EXPECT_EQ(segment.exportMap_.count(slice->index_), 1U);

    // 缓存命中
    std::string cached;
    EXPECT_EQ(segment.Export(slice, cached), BM_OK);
    EXPECT_EQ(cached, exInfo);

    ock::mf::DlAclApi::pRtIpcSetMemoryName = oldRtIpcSetMemoryName;
    ock::mf::DlAclApi::pRtIpcDestroyMemoryName = oldRtIpcDestroyMemoryName;
}

TEST_F(HybmDevSegmentTest, DevLegacy_FreeMemory_HalGvaUnreserveFails)
{
    ock::mf::MemSegmentOptions options{};
    options.segType = ock::mf::HYBM_MST_HBM;
    options.maxSize = ock::mf::HYBM_LARGE_PAGE_SIZE;
    options.rankCnt = 1;
    options.rankId = 0;

    ock::mf::HybmDevLegacySegment segment(options, 0);
    segment.globalVirtualAddress_ = reinterpret_cast<uint8_t *>(0x10000000ULL);
    segment.totalVirtualSize_ = ock::mf::HYBM_LARGE_PAGE_SIZE;

    MOCKER_CPP(&ock::mf::drv::HalGvaUnreserveMemory, int32_t(*)(uint64_t)).stubs().will(returnValue(BM_ERROR));

    segment.FreeMemory();
    EXPECT_EQ(segment.globalVirtualAddress_, nullptr);
}
