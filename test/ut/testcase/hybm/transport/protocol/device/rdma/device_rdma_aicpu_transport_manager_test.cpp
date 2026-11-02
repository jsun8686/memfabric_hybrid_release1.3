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

#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "hybm_types.h"

#define private   public
#define protected public
#include "device_rdma_aicpu_transport_manager.h"
#include "aicpu_joinable_ranks_qp_manager.h"
#include "dl_hccp_api.h"
#include "dl_acl_api.h"
#include "dl_rt_api.h"
#include "device_chip_info.h"
#include "hybm_acl_notify.h"
#include "hybm_stream.h"
#include "hybm_stream_manager.h"
#include "hybm_kernel_hepler.h"
#undef private
#undef protected

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;

namespace {
constexpr uint64_t kTestLocalAddr = 0x1000ULL;
constexpr uint32_t kTestLocalLkey = 7U;
constexpr uint64_t kTestRemoteAddr = 0x2000ULL;
constexpr uint32_t kTestRemoteRkey = 9U;
constexpr uint64_t kTestBatchBytes = 4096ULL;
constexpr uint64_t kTestDeviceQpAddr = 0xE00000A01000ULL;
constexpr uint32_t kTestInvalidRankId = 9U;

struct DlAclApiFnGuard {
    aclrtMallocFunc oldAclrtMalloc{DlAclApi::pAclrtMalloc};
    aclrtFreeFunc oldAclrtFree{DlAclApi::pAclrtFree};
    aclrtMemcpyFunc oldAclrtMemcpy{DlAclApi::pAclrtMemcpy};
    aclrtMallocHostFunc oldAclrtMallocHost{DlAclApi::pAclrtMallocHost};
    aclrtFreeHostFunc oldAclrtFreeHost{DlAclApi::pAclrtFreeHost};
    aclrtGetDeviceFunc oldAclrtGetDevice{DlAclApi::pAclrtGetDevice};
    rtGetLogicDevIdByUserDevIdFunc oldRtGetLogicDevIdByUserDevId{DlAclApi::pRtGetLogicDevIdByUserDevId};
    aclrtGetPhyDevIdByLogicDevIdFunc oldAclrtGetPhyDevIdByLogicDevId{DlAclApi::pAclrtGetPhyDevIdByLogicDevId};
    aclrtCreateStreamWithConfigFunc oldAclrtCreateStreamWithConfig{DlAclApi::pAclrtCreateStreamWithConfig};
    aclrtStreamGetIdFunc oldAclrtStreamGetId{DlAclApi::pAclrtStreamGetId};
    aclrtDestroyStreamFunc oldAclrtDestroyStream{DlAclApi::pAclrtDestroyStream};
    aclrtSynchronizeStreamFunc oldAclrtSynchronizeStream{DlAclApi::pAclrtSynchronizeStream};
    aclrtWaitAndResetNotifyFunc oldAclrtWaitAndResetNotify{DlAclApi::pAclrtWaitAndResetNotify};
    aclrtDestroyNotifyFunc oldAclrtDestroyNotify{DlAclApi::pAclrtDestroyNotify};

    ~DlAclApiFnGuard()
    {
        DlAclApi::pAclrtMalloc = oldAclrtMalloc;
        DlAclApi::pAclrtFree = oldAclrtFree;
        DlAclApi::pAclrtMemcpy = oldAclrtMemcpy;
        DlAclApi::pAclrtMallocHost = oldAclrtMallocHost;
        DlAclApi::pAclrtFreeHost = oldAclrtFreeHost;
        DlAclApi::pAclrtGetDevice = oldAclrtGetDevice;
        DlAclApi::pRtGetLogicDevIdByUserDevId = oldRtGetLogicDevIdByUserDevId;
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = oldAclrtGetPhyDevIdByLogicDevId;
        DlAclApi::pAclrtCreateStreamWithConfig = oldAclrtCreateStreamWithConfig;
        DlAclApi::pAclrtStreamGetId = oldAclrtStreamGetId;
        DlAclApi::pAclrtDestroyStream = oldAclrtDestroyStream;
        DlAclApi::pAclrtSynchronizeStream = oldAclrtSynchronizeStream;
        DlAclApi::pAclrtWaitAndResetNotify = oldAclrtWaitAndResetNotify;
        DlAclApi::pAclrtDestroyNotify = oldAclrtDestroyNotify;
    }
};

struct DlRtApiFnGuard {
    rtStreamGetSqidFunc oldRtStreamGetSqid{DlRtApi::pRtStreamGetSqid};

    ~DlRtApiFnGuard()
    {
        DlRtApi::pRtStreamGetSqid = oldRtStreamGetSqid;
    }
};

int32_t g_expectedPosted = 0U;

int32_t FakeAclrtGetDeviceOk(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return 0;
}

int32_t FakeAclrtGetDeviceFail(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = -1;
    }
    return -1;
}

int32_t FakeRtGetLogicOk(int32_t, int32_t *logicId)
{
    if (logicId != nullptr) {
        *logicId = 0;
    }
    return 0;
}

int32_t FakeRtGetLogicFail(int32_t, int32_t *logicId)
{
    if (logicId != nullptr) {
        *logicId = -1;
    }
    return -1;
}

int32_t FakeAclrtGetPhyOk(int32_t, int32_t *phyId)
{
    if (phyId != nullptr) {
        *phyId = 0;
    }
    return 0;
}

int32_t FakeAclrtGetPhyFail(int32_t, int32_t *phyId)
{
    if (phyId != nullptr) {
        *phyId = -1;
    }
    return -1;
}

int32_t FakeAclrtCreateStreamOk(void **stream, uint32_t, uint32_t)
{
    if (stream != nullptr) {
        *stream = reinterpret_cast<void *>(0x6000UL);
    }
    return 0;
}

int32_t FakeAclrtCreateStreamFail(void **stream, uint32_t, uint32_t)
{
    if (stream != nullptr) {
        *stream = nullptr;
    }
    return -1;
}

int32_t FakeRtStreamGetSqidOk(const void *, uint32_t *sqId)
{
    if (sqId != nullptr) {
        *sqId = 3U;
    }
    return 0;
}

int32_t FakeRtStreamGetSqidFail(const void *, uint32_t *sqId)
{
    if (sqId != nullptr) {
        *sqId = 0U;
    }
    return -1;
}

int32_t FakeAclrtStreamGetIdOk(void *, int32_t *streamId)
{
    if (streamId != nullptr) {
        *streamId = 5;
    }
    return 0;
}

int32_t FakeAclrtDestroyStreamOk(void *)
{
    return 0;
}

int32_t FakeAclrtSynchronizeStreamOk(void *)
{
    return 0;
}

int32_t FakeAclrtSynchronizeStreamFail(void *)
{
    return -1;
}

int32_t FakeAclrtWaitAndResetNotifyOk(void *, void *, uint32_t)
{
    return 0;
}

int32_t FakeAclrtWaitAndResetNotifyFail(void *, void *, uint32_t)
{
    return -1;
}

int32_t FakeAclrtDestroyNotifyOk(void *)
{
    return 0;
}

int32_t FakeAclrtMemcpyFail(void *, size_t, const void *, size_t, uint32_t)
{
    return -1;
}

// 模拟算子读回 out 缓冲：H2D staging 直接成功，D2H 读回写入期望的完成信息。
int32_t FakeAclrtMemcpyAicpuOut(void *dst, size_t, const void *, size_t srcSize, uint32_t kind)
{
    if (kind == ACL_MEMCPY_DEVICE_TO_HOST && dst != nullptr && srcSize == sizeof(ock::mf::aicpu::RdmaAicpuOut)) {
        auto *out = static_cast<ock::mf::aicpu::RdmaAicpuOut *>(dst);
        out->doorbellRet = 0;
        out->posted = static_cast<uint32_t>(g_expectedPosted);
    }
    return 0;
}

int32_t FakeAclrtMemcpyAicpuOutFail(void *dst, size_t, const void *, size_t srcSize, uint32_t kind)
{
    if (kind == ACL_MEMCPY_DEVICE_TO_HOST && dst != nullptr && srcSize == sizeof(ock::mf::aicpu::RdmaAicpuOut)) {
        auto *out = static_cast<ock::mf::aicpu::RdmaAicpuOut *>(dst);
        out->doorbellRet = -1;
        out->posted = 0U;
    }
    return 0;
}

int32_t FakeAclrtMallocOk(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    if (ptr != nullptr) {
        *ptr = reinterpret_cast<void *>(0x12340000UL);
    }
    return 0;
}

int32_t FakeAclrtMallocFail(void **ptr, size_t size, uint32_t flags)
{
    (void)size;
    (void)flags;
    if (ptr != nullptr) {
        *ptr = nullptr;
    }
    return -1;
}

int32_t FakeAclrtMallocHostOk(void **ptr, size_t size)
{
    if (ptr == nullptr) {
        return -1;
    }
    // host staging 会被真实写入（填充段描述），必须返回可写内存，不能用假地址
    *ptr = std::malloc(size);
    return (*ptr != nullptr) ? 0 : -1;
}

int FakeAclrtFreeOk(void *ptr)
{
    (void)ptr;
    return 0;
}

int FakeAclrtFreeHostOk(void *ptr)
{
    std::free(ptr);
    return 0;
}

int32_t FakeAclrtMemcpyOk(void *dst, size_t dstSize, const void *src, size_t srcSize, uint32_t kind)
{
    (void)dst;
    (void)dstSize;
    (void)src;
    (void)srcSize;
    (void)kind;
    return 0;
}

class FakeQpManager final : public DeviceQpManager {
public:
    FakeQpManager() : DeviceQpManager(0, 0, 1, sockaddr_in{}, HYBM_SCENE_DEFAULT) {}

    int SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &) noexcept override
    {
        return BM_OK;
    }
    int Startup(void *) noexcept override
    {
        return BM_OK;
    }
    void Shutdown() noexcept override {}
    const void *GetQpInfoAddress() const noexcept override
    {
        return nullptr;
    }
    UserQpInfo *GetQpHandleWithRankId(uint32_t) noexcept override
    {
        return nullptr;
    }
    void PutQpHandle(UserQpInfo *) const noexcept override {}
    uint64_t GetDeviceQpAddr(uint32_t) const noexcept override
    {
        return deviceQpAddr;
    }
    bool CheckQpReady(const std::vector<uint32_t> &) const noexcept override
    {
        return false;
    }

    uint64_t deviceQpAddr{0U};
};

std::vector<RdmaAicpuTransportManager::AicpuSegment> MakeOneSegmentBatch()
{
    std::vector<RdmaAicpuTransportManager::AicpuSegment> segments;
    RdmaAicpuTransportManager::AicpuSegment seg{};
    seg.localAddr = kTestLocalAddr;
    seg.localLkey = kTestLocalLkey;
    seg.remoteAddr = kTestRemoteAddr;
    seg.remoteRkey = kTestRemoteRkey;
    seg.len = kTestBatchBytes;
    segments.push_back(seg);
    return segments;
}

void ResetAicpuThreadState()
{
    RdmaAicpuTransportManager::aicpuNotify_ = nullptr;
    RdmaAicpuTransportManager::aicpuPending_ = RdmaAicpuTransportManager::AicpuPendingBatch{};
    RdmaAicpuTransportManager::aicpuStream_ = AicpuStream{};
}

// notify 就绪：后续 Synchronize 会在该 ACL 流上 wait&reset notify 并同步、回读 out
void SetAicpuNotifyReady()
{
    DlAclApi::pAclrtDestroyNotify = &FakeAclrtDestroyNotifyOk;
    auto notify = std::make_shared<ock::mf::HybmAclNotify>(reinterpret_cast<void *>(0x6000UL));
    notify->notify_ = reinterpret_cast<void *>(0x9000UL);
    notify->notifyId_ = 7U;
    notify->offset_ = 0x1000ULL;
    notify->regAddr_ = 0xA000ULL;
    notify->regLen_ = 8U;
    notify->inited_ = true;
    RdmaAicpuTransportManager::aicpuNotify_ = notify;
}

void SetAicpuPendingBatch(uint64_t outAddr, uint32_t expectedPosted)
{
    RdmaAicpuTransportManager::aicpuPending_.outAddr = outAddr;
    RdmaAicpuTransportManager::aicpuPending_.expectedPosted = expectedPosted;
    RdmaAicpuTransportManager::aicpuPending_.pending = true;
}

std::shared_ptr<AicpuJoinableRanksQpManager> MakeAiQpManager(uint32_t rankCount, uint32_t rankId = 0U)
{
    sockaddr_in net{};
    net.sin_family = AF_INET;
    net.sin_port = htons(8000);
    inet_pton(AF_INET, "127.0.0.1", &net.sin_addr);
    return std::make_shared<AicpuJoinableRanksQpManager>(0, 0, rankId, rankCount, net, HYBM_SCENE_DEFAULT);
}

void AddAiQpInfo(const std::shared_ptr<AicpuJoinableRanksQpManager> &mgr, uint32_t rankId, uint32_t dbIndex)
{
    HccpAiQpInfo info{};
    info.aiQpAddr = kTestDeviceQpAddr;
    info.sqIndex = 3U;
    info.dbIndex = dbIndex;
    info.data_plane_info.sq.wqn = 7U;
    info.data_plane_info.sq.depth = 128U;
    mgr->aiQpInfos_[rankId] = info;
}

} // namespace

class RdmaAicpuMockTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }
};

TEST(RdmaAicpuTransportManagerTest, RemoteIORejectsMissingQpManagerAndOutOfRangeRank)
{
    RdmaAicpuTransportManager mgr;

    // qpManager_ 为空：单段入口同样先判连接管理器
    EXPECT_EQ(mgr.RemoteIO(0, kTestLocalAddr, kTestRemoteAddr, kTestBatchBytes, false, false), BM_ERROR);

    // rankId 越界：在地址解析/下发前拦截
    mgr.qpManager_ = std::make_shared<FakeQpManager>();
    mgr.rankMutex_ = std::vector<std::mutex>(1);
    EXPECT_EQ(mgr.RemoteIO(kTestInvalidRankId, kTestLocalAddr, kTestRemoteAddr, kTestBatchBytes, false, false),
              BM_ERROR);
}

TEST(RdmaAicpuTransportManagerTest, BatchAsyncRejectsMissingQpManagerAndEmptyBatch)
{
    RdmaAicpuTransportManager mgr;
    CopyDescriptor descriptor{};
    descriptor.counts.push_back(16U);

    // qpManager_ 为空：批量入口先判连接管理器
    EXPECT_EQ(mgr.ReadRemoteBatchAsync(0, descriptor), BM_ERROR);
    EXPECT_EQ(mgr.WriteRemoteBatchAsync(0, descriptor), BM_ERROR);

    // qpManager_ 存在但批量为空：直接成功
    mgr.qpManager_ = std::make_shared<FakeQpManager>();
    CopyDescriptor empty{};
    EXPECT_EQ(mgr.ReadRemoteBatchAsync(0, empty), BM_OK);
    EXPECT_EQ(mgr.WriteRemoteBatchAsync(0, empty), BM_OK);

    // rankId 越界：在地址解析前拦截（两条路径都会按下标访问 rankMutex_）
    mgr.rankMutex_ = std::vector<std::mutex>(1);
    EXPECT_EQ(mgr.ReadRemoteBatchAsync(kTestInvalidRankId, descriptor), BM_INVALID_PARAM);
    EXPECT_EQ(mgr.WriteRemoteBatchAsync(kTestInvalidRankId, descriptor), BM_INVALID_PARAM);
}

TEST(RdmaAicpuTransportManagerTest, RemoteIoBatchAicpuStagingAndResourceGuards)
{
    DlAclApiFnGuard aclGuard;

    RdmaAicpuTransportManager mgr;
    auto qp = std::make_shared<FakeQpManager>();
    mgr.qpManager_ = qp;
    mgr.rankMutex_ = std::vector<std::mutex>(1);

    const auto batch = MakeOneSegmentBatch();

    // 1) 空段：无需下发，直接成功
    std::vector<RdmaAicpuTransportManager::AicpuSegment> emptyBatch;
    EXPECT_EQ(mgr.RemoteIoBatchAicpu(0, emptyBatch, false), BM_OK);

    // 2) AI QP 的 device 地址为 0：无法下发
    EXPECT_EQ(mgr.RemoteIoBatchAicpu(0, batch, false), BM_ERROR);

    // 3) device 地址非 0，但段描述 staging 申请失败：BM_MALLOC_FAILED
    qp->deviceQpAddr = kTestDeviceQpAddr;
    DlAclApi::pAclrtMalloc = &FakeAclrtMallocFail;
    EXPECT_EQ(mgr.RemoteIoBatchAicpu(0, batch, false), BM_MALLOC_FAILED);

    // 4) staging 成功后再遇到不可下发条件（UT 环境无 ACL stream / 无 kernel 句柄）：必须失败并向上传播
    DlAclApi::pAclrtMalloc = &FakeAclrtMallocOk;
    DlAclApi::pAclrtMallocHost = &FakeAclrtMallocHostOk;
    DlAclApi::pAclrtMemcpy = &FakeAclrtMemcpyOk;
    DlAclApi::pAclrtFree = &FakeAclrtFreeOk;
    DlAclApi::pAclrtFreeHost = &FakeAclrtFreeHostOk;
    EXPECT_NE(mgr.RemoteIoBatchAicpu(0, batch, false), BM_OK);
}

TEST_F(RdmaAicpuMockTest, InitDeviceContextBranches)
{
    DlAclApiFnGuard aclGuard;
    TransportOptions options{};
    options.rankId = 1U;
    options.rankCount = 2U;
    options.role = HYBM_ROLE_PEER;
    options.nic = "tcp://127.0.0.1:8000";

    RdmaAicpuTransportManager mgr;
    int32_t userId = -1;

    DlAclApi::pAclrtGetDevice = &FakeAclrtGetDeviceFail;
    EXPECT_NE(mgr.InitDeviceContext(options, userId), BM_OK);

    DlAclApi::pAclrtGetDevice = &FakeAclrtGetDeviceOk;
    DlAclApi::pRtGetLogicDevIdByUserDevId = &FakeRtGetLogicFail;
    EXPECT_NE(mgr.InitDeviceContext(options, userId), BM_OK);

    DlAclApi::pRtGetLogicDevIdByUserDevId = &FakeRtGetLogicOk;
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = &FakeAclrtGetPhyFail; // warn fallback to logicId
    EXPECT_EQ(mgr.InitDeviceContext(options, userId), BM_OK);
    EXPECT_EQ(mgr.rankId_, 1U);
    EXPECT_EQ(mgr.rankCount_, 2U);

    options.nic = "not-a-nic";
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = &FakeAclrtGetPhyOk;
    EXPECT_EQ(mgr.InitDeviceContext(options, userId), BM_INVALID_PARAM);
}

TEST_F(RdmaAicpuMockTest, OpenDeviceFailurePaths)
{
    DlAclApiFnGuard aclGuard;
    TransportOptions options{};
    options.rankId = 0U;
    options.rankCount = 2U;
    options.nic = "tcp://127.0.0.1:8000";

    RdmaAicpuTransportManager mgr;
    DlAclApi::pAclrtGetDevice = &FakeAclrtGetDeviceFail;
    EXPECT_NE(mgr.OpenDevice(options), BM_OK);

    DlAclApi::pAclrtGetDevice = &FakeAclrtGetDeviceOk;
    DlAclApi::pRtGetLogicDevIdByUserDevId = &FakeRtGetLogicOk;
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = &FakeAclrtGetPhyOk;
    MOCKER(&ock::mf::transport::device::RdmaTransportManager::PrepareOpenDevice).stubs().will(returnValue(false));
    EXPECT_EQ(mgr.OpenDevice(options), BM_ERROR);
}

TEST_F(RdmaAicpuMockTest, ComputeDoorbellAddrBranches)
{
    RdmaAicpuTransportManager mgr;
    mgr.rankCount_ = 2U;
    uint64_t dbAddr = 0U;

    EXPECT_EQ(mgr.ComputeDoorbellAddr(5U, dbAddr), BM_ERROR); // rank out of range

    EXPECT_EQ(mgr.ComputeDoorbellAddr(0U, dbAddr), BM_ERROR); // chip info missing

    mgr.deviceChipInfo_ = std::make_shared<DeviceChipInfo>(0U);
    mgr.qpManager_ = std::make_shared<FakeQpManager>();
    EXPECT_EQ(mgr.ComputeDoorbellAddr(0U, dbAddr), BM_ERROR); // not an AI qp manager

    auto aiMgr = MakeAiQpManager(2U);
    AddAiQpInfo(aiMgr, 1U, 0x00030000U); // dieId = 3
    mgr.qpManager_ = aiMgr;
    EXPECT_EQ(mgr.ComputeDoorbellAddr(1U, dbAddr), BM_OK);
    EXPECT_NE(dbAddr, 0ULL);
}

TEST_F(RdmaAicpuMockTest, FillDoorbellParamBranches)
{
    DlAclApiFnGuard aclGuard;
    DlRtApiFnGuard rtGuard;
    ResetAicpuThreadState();

    RdmaAicpuTransportManager mgr;
    mgr.rankCount_ = 2U;
    mgr.deviceChipInfo_ = std::make_shared<DeviceChipInfo>(0U);
    ock::mf::aicpu::RdmaAicpuParam param{};

    DlAclApi::pAclrtCreateStreamWithConfig = &FakeAclrtCreateStreamFail;
    EXPECT_NE(mgr.FillDoorbellParam(0U, param), BM_OK);

    DlAclApi::pAclrtCreateStreamWithConfig = &FakeAclrtCreateStreamOk;
    DlRtApi::pRtStreamGetSqid = &FakeRtStreamGetSqidFail;
    EXPECT_NE(mgr.FillDoorbellParam(0U, param), BM_OK); // sqid failure

    ResetAicpuThreadState();
    DlRtApi::pRtStreamGetSqid = &FakeRtStreamGetSqidOk;
    DlAclApi::pAclrtStreamGetId = &FakeAclrtStreamGetIdOk;
    mgr.qpManager_ = std::make_shared<FakeQpManager>();
    EXPECT_EQ(mgr.FillDoorbellParam(0U, param), BM_ERROR); // compute doorbell forbids non-AI manager

    auto aiMgr = MakeAiQpManager(2U);
    AddAiQpInfo(aiMgr, 0U, 0x00030000U);
    mgr.qpManager_ = aiMgr;
    EXPECT_EQ(mgr.FillDoorbellParam(0U, param), BM_OK);
    EXPECT_EQ(param.sqId, 3U);
    EXPECT_EQ(param.sqStreamId, 5U);
    EXPECT_NE(param.doorbellAddr, 0ULL);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, StageBatchSegmentsBranches)
{
    DlAclApiFnGuard aclGuard;
    RdmaAicpuTransportManager mgr;
    const auto batch = MakeOneSegmentBatch();
    void *dev = nullptr;

    // 注：StageBatchSegments 内部 thread_local DeviceBuffer 会跨用例缓存容量，
    // malloc 失败分支由 RemoteIoBatchAicpuStagingAndResourceGuards 覆盖，这里验证 memcpy 分支。
    DlAclApi::pAclrtMalloc = &FakeAclrtMallocOk;
    DlAclApi::pAclrtMemcpy = &FakeAclrtMemcpyFail;
    EXPECT_NE(mgr.StageBatchSegments(batch, dev), BM_OK);

    DlAclApi::pAclrtMemcpy = &FakeAclrtMemcpyOk;
    EXPECT_EQ(mgr.StageBatchSegments(batch, dev), BM_OK);
    EXPECT_NE(dev, nullptr);
}

TEST_F(RdmaAicpuMockTest, ResolveAicpuBatchAddressesBranches)
{
    RdmaAicpuTransportManager mgr;
    mgr.rankCount_ = 2U;
    mgr.rankMutex_ = std::vector<std::mutex>(2);
    mgr.ranksMRs_.resize(2U);

    CopyDescriptor descriptor{};
    descriptor.localAddrs.push_back(reinterpret_cast<void *>(kTestLocalAddr));
    descriptor.globalAddrs.push_back(reinterpret_cast<void *>(kTestRemoteAddr));
    descriptor.counts.push_back(kTestBatchBytes);

    std::vector<RdmaAicpuTransportManager::AicpuSegment> segments;

    EXPECT_EQ(mgr.ResolveAicpuBatchAddresses(0U, descriptor, segments), BM_INVALID_PARAM); // local not registered

    mgr.registerMRS_[kTestLocalAddr] =
        RegMemResult(kTestLocalAddr, kTestLocalAddr, kTestBatchBytes, nullptr, kTestLocalLkey, kTestRemoteRkey);
    EXPECT_EQ(mgr.ResolveAicpuBatchAddresses(0U, descriptor, segments), BM_INVALID_PARAM); // remote rank not prepared

    mgr.ranksMRs_[0][kTestRemoteAddr] =
        RegMemResult(kTestRemoteAddr, kTestRemoteAddr, kTestBatchBytes, nullptr, kTestLocalLkey, kTestRemoteRkey);
    EXPECT_EQ(mgr.ResolveAicpuBatchAddresses(0U, descriptor, segments), BM_OK);
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_EQ(segments[0].len, kTestBatchBytes);
    EXPECT_EQ(segments[0].localLkey, kTestLocalLkey);
    EXPECT_EQ(segments[0].remoteRkey, kTestRemoteRkey);

    CopyDescriptor zeroDesc{};
    zeroDesc.localAddrs.push_back(reinterpret_cast<void *>(kTestLocalAddr));
    zeroDesc.globalAddrs.push_back(reinterpret_cast<void *>(kTestRemoteAddr));
    zeroDesc.counts.push_back(0U);
    std::vector<RdmaAicpuTransportManager::AicpuSegment> zeroSegments;
    EXPECT_EQ(mgr.ResolveAicpuBatchAddresses(0U, zeroDesc, zeroSegments), BM_OK);
    EXPECT_EQ(zeroSegments.size(), 0U);
}

TEST_F(RdmaAicpuMockTest, SynchronizeFailureAndSuccess)
{
    DlAclApiFnGuard aclGuard;
    ResetAicpuThreadState();
    RdmaAicpuTransportManager mgr;

    EXPECT_EQ(mgr.Synchronize(0U), BM_OK); // nothing pending

    SetAicpuPendingBatch(0x7000ULL, 2U);
    EXPECT_EQ(mgr.Synchronize(0U), BM_ERROR); // notify missing

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, SynchronizeWaitsNotifySuccess)
{
    DlAclApiFnGuard aclGuard;
    ResetAicpuThreadState();
    RdmaAicpuTransportManager mgr;
    MOCKER(&ock::mf::HybmStreamManager::GetThreadAclStream)
        .stubs()
        .will(returnValue(reinterpret_cast<void *>(0x6000UL)));

    SetAicpuNotifyReady();
    SetAicpuPendingBatch(0x7000ULL, 2U);
    DlAclApi::pAclrtWaitAndResetNotify = &FakeAclrtWaitAndResetNotifyOk;
    DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamOk;
    DlAclApi::pAclrtMemcpy = &FakeAclrtMemcpyAicpuOut;
    g_expectedPosted = 2;

    EXPECT_EQ(mgr.Synchronize(0U), BM_OK);
    EXPECT_FALSE(RdmaAicpuTransportManager::aicpuPending_.pending);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, SynchronizeNotifyWaitAndReadBackFailures)
{
    DlAclApiFnGuard aclGuard;
    ResetAicpuThreadState();
    RdmaAicpuTransportManager mgr;
    MOCKER(&ock::mf::HybmStreamManager::GetThreadAclStream)
        .stubs()
        .will(returnValue(reinterpret_cast<void *>(0x6000UL)));

    SetAicpuNotifyReady();
    SetAicpuPendingBatch(0x7000ULL, 2U);

    // 1) 入队/等待 notify 失败
    DlAclApi::pAclrtWaitAndResetNotify = &FakeAclrtWaitAndResetNotifyFail;
    EXPECT_EQ(mgr.Synchronize(0U), BM_ERROR);
    EXPECT_TRUE(RdmaAicpuTransportManager::aicpuPending_.pending);

    // 2) notify 入队成功但 ACL 流同步失败
    DlAclApi::pAclrtWaitAndResetNotify = &FakeAclrtWaitAndResetNotifyOk;
    DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamFail;
    EXPECT_EQ(mgr.Synchronize(0U), BM_ERROR);
    EXPECT_TRUE(RdmaAicpuTransportManager::aicpuPending_.pending);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, RunAicpuIoLaunchFailed)
{
    ResetAicpuThreadState();
    RdmaAicpuTransportManager mgr;
    ock::mf::aicpu::RdmaAicpuParam param{};
    param.outAddr = 0x7000ULL;

    MOCKER(&ock::mf::transport::device::LaunchAicpuKernel).stubs().will(returnValue(BM_ERROR));
    EXPECT_NE(mgr.RunAicpuIo(0U, reinterpret_cast<void *>(0x6000UL), param, 1U), BM_OK);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, RunAicpuIoPostLaunchBranches)
{
    DlAclApiFnGuard aclGuard;
    ResetAicpuThreadState();
    RdmaAicpuTransportManager mgr;
    ock::mf::aicpu::RdmaAicpuParam param{};
    param.outAddr = 0x7000ULL;
    param.sqId = 3U;
    param.sqStreamId = 5U;
    param.devId = 7U;

    MOCKER(&ock::mf::transport::device::LaunchAicpuKernel).stubs().will(returnValue(BM_OK));

    // 只负责下发并登记 pending：notify 等待与 out 回读都放到 Synchronize()
    EXPECT_EQ(mgr.RunAicpuIo(0U, reinterpret_cast<void *>(0x6000UL), param, 2U), BM_OK);
    EXPECT_TRUE(RdmaAicpuTransportManager::aicpuPending_.pending);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuPending_.outAddr, param.outAddr);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuPending_.expectedPosted, 2U);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuPending_.sqId, param.sqId);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuPending_.sqStreamId, param.sqStreamId);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuPending_.devId, param.devId);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, RemoteIoBatchAicpuAndBatchAsyncSuccessPath)
{
    DlAclApiFnGuard aclGuard;
    DlRtApiFnGuard rtGuard;
    ResetAicpuThreadState();

    RdmaAicpuTransportManager mgr;
    mgr.rankCount_ = 2U;
    mgr.rankMutex_ = std::vector<std::mutex>(2);
    mgr.ranksMRs_.resize(2U);
    mgr.deviceChipInfo_ = std::make_shared<DeviceChipInfo>(0U);

    auto aiMgr = MakeAiQpManager(2U);
    AddAiQpInfo(aiMgr, 0U, 0x00030000U);
    mgr.qpManager_ = aiMgr;

    mgr.notifyInfo_.srcAddr = 0x3000ULL;
    mgr.notifyInfo_.notifyAddr = 0x4000ULL;
    mgr.notifyInfo_.len = 8U;
    mgr.notifyInfo_.srcRkey = 0x11U;
    mgr.notifyInfo_.notifyLkey = 0x22U;
    mgr.notifyRemoteInfo_ = {{0x5000ULL, 0x33U}};

    mgr.registerMRS_[kTestLocalAddr] =
        RegMemResult(kTestLocalAddr, kTestLocalAddr, kTestBatchBytes, nullptr, kTestLocalLkey, kTestRemoteRkey);
    mgr.ranksMRs_[0][kTestRemoteAddr] =
        RegMemResult(kTestRemoteAddr, kTestRemoteAddr, kTestBatchBytes, nullptr, kTestLocalLkey, kTestRemoteRkey);

    SetAicpuNotifyReady();
    RdmaAicpuTransportManager::aicpuStream_ = AicpuStream{reinterpret_cast<void *>(0x6000UL), 3U, 5U};

    DlAclApi::pAclrtMalloc = &FakeAclrtMallocOk;
    DlAclApi::pAclrtFree = &FakeAclrtFreeOk;
    DlAclApi::pAclrtMemcpy = &FakeAclrtMemcpyAicpuOut;
    DlAclApi::pAclrtSynchronizeStream = &FakeAclrtSynchronizeStreamOk;
    DlAclApi::pAclrtWaitAndResetNotify = &FakeAclrtWaitAndResetNotifyOk;
    g_expectedPosted = 2; // 1 data segment + 1 notify

    MOCKER(&ock::mf::transport::device::LaunchAicpuKernel).stubs().will(returnValue(BM_OK));
    MOCKER(&ock::mf::HybmStreamManager::GetThreadAclStream)
        .stubs()
        .will(returnValue(reinterpret_cast<void *>(0x6000UL)));

    const auto batch = MakeOneSegmentBatch();
    EXPECT_EQ(mgr.RemoteIoBatchAicpu(0U, batch, false), BM_OK);
    EXPECT_FALSE(RdmaAicpuTransportManager::aicpuPending_.pending);

    CopyDescriptor descriptor{};
    descriptor.localAddrs.push_back(reinterpret_cast<void *>(kTestLocalAddr));
    descriptor.globalAddrs.push_back(reinterpret_cast<void *>(kTestRemoteAddr));
    descriptor.counts.push_back(kTestBatchBytes);
    EXPECT_EQ(mgr.ReadRemoteBatchAsync(0U, descriptor), BM_OK);
    EXPECT_EQ(mgr.WriteRemoteBatchAsync(0U, descriptor), BM_OK);
    EXPECT_EQ(mgr.RemoteIO(0U, kTestLocalAddr, kTestRemoteAddr, kTestBatchBytes, true, true), BM_OK);

    ResetAicpuThreadState();
}

TEST_F(RdmaAicpuMockTest, CloseDeviceDestroysAicpuStream)
{
    DlAclApiFnGuard aclGuard;
    ResetAicpuThreadState();
    DlAclApi::pAclrtDestroyStream = &FakeAclrtDestroyStreamOk;

    RdmaAicpuTransportManager mgr;
    RdmaAicpuTransportManager::aicpuStream_ = AicpuStream{reinterpret_cast<void *>(0x6000UL), 3U, 5U};
    EXPECT_EQ(mgr.CloseDevice(), BM_OK);
    EXPECT_EQ(RdmaAicpuTransportManager::aicpuStream_.stream, nullptr);

    ResetAicpuThreadState();
}
