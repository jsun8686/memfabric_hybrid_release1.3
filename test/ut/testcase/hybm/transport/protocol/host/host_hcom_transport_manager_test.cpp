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
#include <limits>
#include <string>

#define private   public
#define protected public
#include "host_hcom_transport_manager.h"
#include "host_hcom_common.h"
#include "host_hcom_helper.h"
#include "dl_hcom_api.h"
#undef private
#undef protected

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::host;

namespace {
struct DlHcomApiFnGuard {
    serviceCreateFunc oldServiceCreate{DlHcomApi::gServiceCreate};
    serviceBindFunc oldServiceBind{DlHcomApi::gServiceBind};
    serviceStartFunc oldServiceStart{DlHcomApi::gServiceStart};
    serviceDestroyFunc oldServiceDestroy{DlHcomApi::gServiceDestroy};
    serviceSetTlsOptionsFunc oldSetTls{DlHcomApi::gServiceSetTlsOptions};
    serviceRegisterChannelBrokerHandlerFunc oldRegBroker{DlHcomApi::gServiceRegisterChannelBrokerHandler};
    serviceRegisterHandlerFunc oldRegHandler{DlHcomApi::gServiceRegisterHandler};
    serviceSetDeviceIpMaskFunc oldSetIpMask{DlHcomApi::gServiceSetDeviceIpMask};
    setExternalLoggerFunc oldSetLogger{DlHcomApi::gSetExternalLogger};
    serviceRegisterAssignMemoryRegionFunc oldRegAssignMr{DlHcomApi::gServiceRegisterAssignMemoryRegion};
    serviceGetMemoryRegionInfoFunc oldGetMrInfo{DlHcomApi::gServiceGetMemoryRegionInfo};
    serviceDestroyMemoryRegionFunc oldDestroyMr{DlHcomApi::gServiceDestroyMemoryRegion};
    serviceConnectFunc oldServiceConnect{DlHcomApi::gServiceConnect};
    ImportUrmaSeg oldImportUrmaSeg{DlHcomApi::gImportUrmaSeg};
    contextGetResultFunc oldContextGetResult{DlHcomApi::gContextGetResult};
    serviceDisConnectFunc oldServiceDisConnect{DlHcomApi::gServiceDisConnectFunc};
    channelGetFunc oldChannelGet{DlHcomApi::gChannelGet};
    channelPutFunc oldChannelPut{DlHcomApi::gChannelPut};
    channelBatchPutFunc oldChannelBatchPut{DlHcomApi::gChannelBatchPut};
    channelBatchGetFunc oldChannelBatchGet{DlHcomApi::gChannelBatchGet};

    ~DlHcomApiFnGuard()
    {
        DlHcomApi::gServiceCreate = oldServiceCreate;
        DlHcomApi::gServiceBind = oldServiceBind;
        DlHcomApi::gServiceStart = oldServiceStart;
        DlHcomApi::gServiceDestroy = oldServiceDestroy;
        DlHcomApi::gServiceSetTlsOptions = oldSetTls;
        DlHcomApi::gServiceRegisterChannelBrokerHandler = oldRegBroker;
        DlHcomApi::gServiceRegisterHandler = oldRegHandler;
        DlHcomApi::gServiceSetDeviceIpMask = oldSetIpMask;
        DlHcomApi::gSetExternalLogger = oldSetLogger;
        DlHcomApi::gServiceRegisterAssignMemoryRegion = oldRegAssignMr;
        DlHcomApi::gServiceGetMemoryRegionInfo = oldGetMrInfo;
        DlHcomApi::gServiceDestroyMemoryRegion = oldDestroyMr;
        DlHcomApi::gServiceConnect = oldServiceConnect;
        DlHcomApi::gImportUrmaSeg = oldImportUrmaSeg;
        DlHcomApi::gContextGetResult = oldContextGetResult;
        DlHcomApi::gServiceDisConnectFunc = oldServiceDisConnect;
        DlHcomApi::gChannelGet = oldChannelGet;
        DlHcomApi::gChannelPut = oldChannelPut;
        DlHcomApi::gChannelBatchPut = oldChannelBatchPut;
        DlHcomApi::gChannelBatchGet = oldChannelBatchGet;
    }
};

static Service_LogHandler gCapturedLogger = nullptr;

int FakeServiceCreate(Service_Type, const char *, Service_Options, Hcom_Service *service)
{
    *service = 1;
    return 0;
}
int FakeServiceBind(Hcom_Service, const char *, Service_ChannelHandler)
{
    return 0;
}

// programmable number of bind failures (used to test the port conflict retry)
int g_serviceBindFailTimes = 0;
int FakeServiceBindFailFirst(Hcom_Service, const char *, Service_ChannelHandler)
{
    if (g_serviceBindFailTimes > 0) {
        g_serviceBindFailTimes--;
        return -1;
    }
    return 0;
}
int FakeServiceStart(Hcom_Service)
{
    return 0;
}
int FakeServiceDestroy(Hcom_Service, const char *)
{
    return 0;
}
void FakeSetTlsOptions(Hcom_Service, bool, Service_TlsVersion, Service_CipherSuite, Hcom_TlsGetCertCb,
                       Hcom_TlsGetPrivateKeyCb, Hcom_TlsGetCACb)
{}
void FakeRegisterChannelBrokerHandler(Hcom_Service, Service_ChannelHandler, Service_ChannelPolicy, uint64_t) {}
void FakeRegisterHandler(Hcom_Service, Service_HandlerType, Service_RequestHandler, uint64_t) {}
void FakeSetDeviceIpMask(Hcom_Service, const char *) {}
void FakeSetExternalLogger(Service_LogHandler h)
{
    gCapturedLogger = h;
}

int FakeRegisterAssignMr(Hcom_Service, uintptr_t, uint64_t, Service_MemoryRegion *mr)
{
    *mr = static_cast<Service_MemoryRegion>(0x1234UL);
    return 0;
}
int FakeGetMrInfo(Service_MemoryRegion, Service_MemoryRegionInfo *info)
{
    // Fill a recognizable OneSideKey.
    for (size_t i = 0; i < std::size(info->lKey.keys); i++) {
        info->lKey.keys[i] = 0x1111111100000000ULL + i;
        info->lKey.tokens[i] = 0x2222222200000000ULL + i;
    }
    return 0;
}

int FakeServiceDestroyMemoryRegion(Hcom_Service, Service_MemoryRegion)
{
    return 0;
}

int FakeServiceConnectOk(Hcom_Service, const char *, Hcom_Channel *channel, Service_ConnectOptions)
{
    *channel = static_cast<Hcom_Channel>(0x99UL);
    return 0;
}

int FakeServiceConnectFail(Hcom_Service, const char *, Hcom_Channel *, Service_ConnectOptions)
{
    return -1;
}

int FakeImportUrmaSegOk(Service_Context, uintptr_t, uint64_t, OneSideKey *)
{
    return 0;
}

int FakeContextGetResultOk(Service_Context, int *res)
{
    if (res != nullptr) {
        *res = 0;
    }
    return 0;
}

int FakeContextGetResultFail(Service_Context, int *res)
{
    if (res != nullptr) {
        *res = -1;
    }
    return 0;
}

int FakeContextGetResultApiError(Service_Context, int *)
{
    return BM_ERROR;
}

int FakeChannelGetFail(Hcom_Channel, Channel_OneSideRequest, Channel_Callback *cb)
{
    // 覆盖 ReadRemoteAsync 异常分支：ret != BM_OK
    if (cb == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return BM_ERROR;
}

int FakeChannelPutFail(Hcom_Channel, Channel_OneSideRequest, Channel_Callback *cb)
{
    // 覆盖 WriteRemoteAsync 异常分支：ret != BM_OK
    if (cb == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return BM_ERROR;
}

int FakeChannelPutVFail(Hcom_Channel, Channel_OneSideRequestSgl, Channel_Callback *cb)
{
    if (cb == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return BM_ERROR;
}

int FakeChannelGetVFail(Hcom_Channel, Channel_OneSideRequestSgl, Channel_Callback *cb)
{
    if (cb == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return BM_ERROR;
}

// Initialize the per-rank containers and the default service state of the manager under test in one place,
// so that each case does not have to assemble them by hand.
void SetupManager(const std::shared_ptr<HcomTransportManager> &mgr, uint32_t rankCount, uint32_t rankId = 0)
{
    mgr->rpcService_ = 1;
    mgr->rankId_ = rankId;
    mgr->rankCount_ = rankCount;
    mgr->mrMutex_ = std::vector<std::mutex>(rankCount);
    mgr->mrs_ = std::vector<std::set<HcomMemoryRegion>>(rankCount);
    mgr->mrRefCnts_ = std::vector<std::map<uint64_t, uint32_t>>(rankCount);
    mgr->channelMutex_ = std::vector<std::mutex>(rankCount);
    mgr->channels_ = std::vector<Hcom_Channel>(rankCount, 0);
    mgr->nics_ = std::vector<std::string>(rankCount, "");
}
} // namespace

// 本文件通过 #define private public 直接改写进程级单例 HcomTransportManager::GetInstance() 的私有状态
// （rpcService_/rankCount_/mrs_/channels_ 等），且用例结束不复位。最后一个用例
// ConnectSkipSelfAndEmptyNic 会把 rpcService_ 留成 1，同进程后续 suite 调 OpenDevice 时会命中
// host_hcom_transport_manager.cpp:117 的 BM_ASSERT_LOG_AND_RETURN(rpcService_ == 0, ..., BM_OK)：
// 该宏第三参是返回值，于是 OpenDevice 假装成功并跳过 ServiceCreate/ServiceBind/ServiceStart，
// 之后再拿这个本进程并不存在的句柄去调真实 libhcom，直接抛 std::system_error 打死进程。
// 这里在每个用例结束时复位，保证本 suite 不污染同进程其它 suite（如 SmemTransTest 的 4 个 e2e）。
// 注意：复位后本文件内的用例必须自带前置状态，不可再依赖前一个用例泄漏的 rpcService_（同文件内已逐条核对）。
namespace {
class HcomTransportManagerStateCleaner : public ::testing::EmptyTestEventListener {
public:
    void OnTestEnd(const ::testing::TestInfo &info) override
    {
        if (std::string(info.test_suite_name()) != "HcomTransportManagerTest") {
            return;
        }
        HcomTransportManager::GetInstance()->rpcService_ = 0;
    }
};

struct HcomTransportManagerStateCleanerRegistrar {
    HcomTransportManagerStateCleanerRegistrar()
    {
        ::testing::UnitTest::GetInstance()->listeners().Append(new HcomTransportManagerStateCleaner());
    }
};

HcomTransportManagerStateCleanerRegistrar gHcomTransportManagerStateCleanerRegistrar;
} // namespace

TEST(HcomTransportManagerTest, IndirectlyCoversRuntimeConfigAndLoggerAdapterViaOpenDevice)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceCreate = &FakeServiceCreate;
    DlHcomApi::gServiceBind = &FakeServiceBind;
    DlHcomApi::gServiceStart = &FakeServiceStart;
    DlHcomApi::gServiceDestroy = &FakeServiceDestroy;
    DlHcomApi::gServiceSetTlsOptions = &FakeSetTlsOptions;
    DlHcomApi::gServiceRegisterChannelBrokerHandler = &FakeRegisterChannelBrokerHandler;
    DlHcomApi::gServiceRegisterHandler = &FakeRegisterHandler;
    DlHcomApi::gServiceSetDeviceIpMask = &FakeSetDeviceIpMask;
    DlHcomApi::gSetExternalLogger = &FakeSetExternalLogger;

    auto mgr = HcomTransportManager::GetInstance();
    mgr->CloseDevice();

    TransportOptions opts{};
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.nic = "tcp://127.0.0.1:2048";

    EXPECT_EQ(mgr->OpenDevice(opts), BM_OK);

    ASSERT_NE(gCapturedLogger, nullptr);
    gCapturedLogger(ock::mf::DEBUG_LEVEL, nullptr);
    gCapturedLogger(ock::mf::INFO_LEVEL, "info");
    gCapturedLogger(ock::mf::WARN_LEVEL, "warn");
    gCapturedLogger(ock::mf::ERROR_LEVEL, "err");
    gCapturedLogger(99, "other");

    mgr->CloseDevice();
}

TEST(HcomTransportManagerTest, IndirectlyCoversCopyHcomOneSideKeyViaRegisterMemoryRegion)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceRegisterAssignMemoryRegion = &FakeRegisterAssignMr;
    DlHcomApi::gServiceGetMemoryRegionInfo = &FakeGetMrInfo;

    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    TransportMemoryRegion mr{};
    mr.addr = 0x1000;
    mr.size = 0x100;
    mr.flags = transport::REG_MR_FLAG_DRAM;

    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);
    ASSERT_EQ(mgr->mrs_[0].size(), 1U);

    const auto &stored = mgr->mrs_[0].begin()->lKey;
    for (size_t i = 0; i < 4; i++) {
        EXPECT_EQ(stored.keys[i], 0x1111111100000000ULL + i);
    }
}

// Several entities replaying the same MR: the duplicate registration is reused with the reference count
// incremented, and the MR is destroyed when the last referrer deregisters.
TEST(HcomTransportManagerTest, RegisterMemoryRegion_DuplicateSameRange_ReusesAndRefCounts)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceRegisterAssignMemoryRegion = &FakeRegisterAssignMr;
    DlHcomApi::gServiceGetMemoryRegionInfo = &FakeGetMrInfo;
    DlHcomApi::gServiceDestroyMemoryRegion = &FakeServiceDestroyMemoryRegion;

    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    TransportMemoryRegion mr{};
    mr.addr = 0x1000;
    mr.size = 0x100;
    mr.flags = transport::REG_MR_FLAG_DRAM;

    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);
    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);
    EXPECT_EQ(mgr->mrs_[0].size(), 1U);
    EXPECT_EQ(mgr->mrRefCnts_[0][0x1000], 2U);

    EXPECT_EQ(mgr->UnregisterMemoryRegion(0x1000), BM_OK);
    EXPECT_EQ(mgr->mrs_[0].size(), 1U);
    EXPECT_EQ(mgr->mrRefCnts_[0][0x1000], 1U);

    EXPECT_EQ(mgr->UnregisterMemoryRegion(0x1000), BM_OK);
    EXPECT_EQ(mgr->mrs_[0].size(), 0U);
    EXPECT_TRUE(mgr->mrRefCnts_[0].empty());
}

// A registration at the same address but with a different size, or with a partially overlapping range, is
// still treated as a conflict and rejected.
TEST(HcomTransportManagerTest, RegisterMemoryRegion_PartialOverlap_Rejected)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceRegisterAssignMemoryRegion = &FakeRegisterAssignMr;
    DlHcomApi::gServiceGetMemoryRegionInfo = &FakeGetMrInfo;

    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    TransportMemoryRegion mr{};
    mr.addr = 0x1000;
    mr.size = 0x100;
    mr.flags = transport::REG_MR_FLAG_DRAM;
    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);

    TransportMemoryRegion smaller{};
    smaller.addr = 0x1000;
    smaller.size = 0x80;
    smaller.flags = transport::REG_MR_FLAG_DRAM;
    EXPECT_EQ(mgr->RegisterMemoryRegion(smaller), BM_ERROR);

    TransportMemoryRegion contained{};
    contained.addr = 0x1080;
    contained.size = 0x40;
    contained.flags = transport::REG_MR_FLAG_DRAM;
    EXPECT_EQ(mgr->RegisterMemoryRegion(contained), BM_ERROR);

    EXPECT_EQ(mgr->mrs_[0].size(), 1U);
    EXPECT_EQ(mgr->mrRefCnts_[0][0x1000], 1U);
}

TEST(HcomTransportManagerTest, IndirectlyCoversCopyHcomOneSideKeyToOneSideViaQueryMemoryKey)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    // Prepare a local MR with a known TransportMemoryKey layout.
    HcomMemoryRegion info{};
    info.addr = 0x1000;
    info.lva = 0x1000;
    info.size = 0x100;
    info.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 4; i++) {
        info.lKey.keys[i] = 0xABC0000000000000ULL + i;
        info.lKey.keys[4 + i] = 0xDEF0000000000000ULL + i;
    }
    mgr->mrs_[0].insert(info);

    TransportMemoryKey out{};
    EXPECT_EQ(mgr->QueryMemoryKey(0x1000, out), BM_OK);

    RegMemoryKey key{};
    ASSERT_TRUE(ReadStructFromKey(out, key));
    const auto &lKey = key.hcomInfo.lKey;

    for (size_t i = 0; i < 4; i++) {
        EXPECT_EQ(lKey.keys[i], 0xABC0000000000000ULL + i);
        EXPECT_EQ(lKey.tokens[i], 0xDEF0000000000000ULL + i);
    }
}

TEST(HcomTransportManagerTest, CloseDeviceWithoutServiceReturnsOk)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    EXPECT_EQ(mgr->CloseDevice(), BM_OK);
}

TEST(HcomTransportManagerTest, RemoveRanksReturnsOk)
{
    auto mgr = HcomTransportManager::GetInstance();
    EXPECT_EQ(mgr->RemoveRanks({1, 2, 3}), BM_OK);
}

TEST(HcomTransportManagerTest, CheckTransportOptionsValidNicBuildsLocalNic)
{
    auto mgr = HcomTransportManager::GetInstance();
    TransportOptions opts{};
    opts.rankId = 1;
    opts.rankCount = 2;
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.nic = "tcp://127.0.0.1:2048";

    EXPECT_EQ(mgr->CheckTransportOptions(opts), BM_OK);
    EXPECT_EQ(mgr->localIp_, "127.0.0.1");
    EXPECT_EQ(mgr->localNic_, "tcp://127.0.0.1:2049");
}

TEST(HcomTransportManagerTest, EndPointBrokenValidPayloadDisconnectsChannel)
{
    auto mgr = std::make_shared<HcomTransportManager>();
    mgr->rankId_ = 0;
    mgr->rankCount_ = 2;
    mgr->channelMutex_ = std::vector<std::mutex>(mgr->rankCount_);
    mgr->channels_ = std::vector<Hcom_Channel>(2, 0);
    mgr->channels_[1] = static_cast<Hcom_Channel>(0x123UL);
    mgr->nics_ = std::vector<std::string>(mgr->rankCount_);

    // payload carries rank id.
    EXPECT_EQ(mgr->OnEndPointBroken(static_cast<Hcom_Channel>(0x123UL), "1"), BM_OK);
    EXPECT_EQ(mgr->channels_[1], static_cast<Hcom_Channel>(0));
}

TEST(HcomTransportManagerTest, InnerReadWriteRemoteNotConnectedOrMissingKeysReturnError)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    // Not connected.
    EXPECT_NE(BM_OK, mgr->InnerReadRemote(0, 0x1000, 0x2000, 16));
    EXPECT_NE(BM_OK, mgr->InnerWriteRemote(0, 0x1000, 0x2000, 16));

    // Connected but missing local key.
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x10UL);
    EXPECT_NE(BM_OK, mgr->InnerReadRemote(0, 0x1000, 0x2000, 16));
    EXPECT_NE(BM_OK, mgr->InnerWriteRemote(0, 0x1000, 0x2000, 16));
}

TEST(HcomTransportManagerTest, ChannelAsyncCallbackFinishesOrFailsBasedOnContextResult)
{
    DlHcomApiFnGuard guard;
    HostHcomCounterStream stream(0);
    Service_Context ctx = static_cast<Service_Context>(0x1UL);

    // Finish path.
    DlHcomApi::gContextGetResult = &FakeContextGetResultOk;
    stream.Reset();
    stream.SubmitTasks(1);
    HcomTransportManager::ChannelAsyncCallback(&stream, ctx);
    EXPECT_EQ(stream.Synchronize(0), 0);

    // Failed path (res != 0).
    DlHcomApi::gContextGetResult = &FakeContextGetResultFail;
    stream.Reset();
    stream.SubmitTasks(1);
    HcomTransportManager::ChannelAsyncCallback(&stream, ctx);
    EXPECT_EQ(stream.Synchronize(0), BM_ERROR);

    // API error path (ContextGetResult returns non-zero => res treated as error).
    DlHcomApi::gContextGetResult = &FakeContextGetResultApiError;
    stream.Reset();
    stream.SubmitTasks(1);
    HcomTransportManager::ChannelAsyncCallback(&stream, ctx);
    EXPECT_EQ(stream.Synchronize(0), BM_ERROR);
}

TEST(HcomTransportManagerTest, SynchronizeReturnsErrorWhenStreamFailed)
{
    auto mgr = HcomTransportManager::GetInstance();
    auto stream = std::make_shared<HostHcomCounterStream>(0);
    HcomTransportManager::stream_ = stream;

    stream->SubmitTasks(1);
    stream->FailedOne();

    EXPECT_EQ(mgr->Synchronize(0), BM_ERROR);
    HcomTransportManager::stream_.reset();
}

TEST(HcomTransportManagerTest, UpdateRankMrInfosSkipsSelfAndZeroSizeAndStoresMr)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 0);
    mgr->bmOptype_ = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_URMA);

    DlHcomApiFnGuard guard;
    DlHcomApi::gImportUrmaSeg = &FakeImportUrmaSegOk; // NO_XPU 下会被调用，否则也不影响

    // rank 0 (self) should be skipped.
    TransportRankPrepareInfo selfInfo{};
    selfInfo.nic = "tcp://127.0.0.1:2048";

    // rank 1: add one zero-size key (skipped) and one valid key (stored).
    TransportRankPrepareInfo r1{};
    r1.nic = "tcp://127.0.0.1:2048";

    RegMemoryKey k0{};
    k0.type = TT_HCOM;
    k0.gva = 0x2000;
    k0.hcomInfo.lAddress = static_cast<uintptr_t>(0x1000);
    k0.hcomInfo.size = 0; // should be skipped
    TransportMemoryKey k0Key{};
    WriteStructToKey(k0, k0Key);
    r1.memKeys.push_back(k0Key);

    RegMemoryKey k1{};
    k1.type = TT_HCOM;
    k1.gva = 0x3000;
    k1.hcomInfo.lAddress = static_cast<uintptr_t>(0x1111);
    k1.hcomInfo.size = 0x100;
    for (size_t i = 0; i < 4; i++) {
        k1.hcomInfo.lKey.keys[i] = 0x1111111100000000ULL + i;
        k1.hcomInfo.lKey.tokens[i] = 0x2222222200000000ULL + i;
    }
    TransportMemoryKey k1Key{};
    WriteStructToKey(k1, k1Key);
    r1.memKeys.push_back(k1Key);

    std::unordered_map<uint32_t, TransportRankPrepareInfo> opt;
    opt.emplace(0U, selfInfo);
    opt.emplace(1U, r1);

    EXPECT_EQ(mgr->UpdateRankMrInfos(opt), BM_OK);
    ASSERT_EQ(mgr->mrs_[1].size(), 1U);
    EXPECT_EQ(mgr->mrs_[1].begin()->addr, 0x3000U);
    EXPECT_EQ(mgr->mrs_[1].begin()->size, 0x100U);
    EXPECT_EQ(mgr->mrs_[1].begin()->lva, reinterpret_cast<uint64_t>(k1.hcomInfo.lAddress));
}

TEST(HcomTransportManagerTest, UpdateRankConnectInfosConnectsWhenChannelEmptyAndOptionProvided)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 1);

    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceConnect = &FakeServiceConnectOk;

    TransportRankPrepareInfo r0{};
    r0.nic = "tcp://127.0.0.1:2048";

    std::unordered_map<uint32_t, TransportRankPrepareInfo> opt;
    opt.emplace(0U, r0);

    EXPECT_EQ(mgr->UpdateRankConnectInfos(opt), BM_OK);
    EXPECT_EQ(mgr->nics_[0], r0.nic);
}

TEST(HcomTransportManagerTest, UpdateRankConnectInfosStoresNic)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 1);

    TransportRankPrepareInfo r0{};
    r0.nic = "tcp://127.0.0.1:2048";
    std::unordered_map<uint32_t, TransportRankPrepareInfo> opt;
    opt.emplace(0U, r0);

    EXPECT_EQ(mgr->UpdateRankConnectInfos(opt), BM_OK);
    EXPECT_EQ(mgr->nics_[0], r0.nic);
}

TEST(HcomTransportManagerTest, UpdateRankOptionsInvalidRankReturnsInvalidParam)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rankCount_ = 1;

    HybmTransPrepareOptions param{};
    TransportRankPrepareInfo info{};
    param.options.emplace(5U, info);

    EXPECT_EQ(mgr->UpdateRankOptions(param), BM_INVALID_PARAM);
}

TEST(HcomTransportManagerTest, UpdateRankOptionsSucceeds)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 1);
    mgr->bmOptype_ = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_URMA);

    HybmTransPrepareOptions param{};
    TransportRankPrepareInfo r0{};
    r0.nic = "tcp://127.0.0.1:2048";
    param.options.emplace(0U, r0);

    EXPECT_EQ(mgr->UpdateRankOptions(param), BM_OK);
    EXPECT_EQ(mgr->nics_[0], r0.nic);
}

TEST(HcomTransportManagerTest, ConnectHcomChannelAlreadyConnectedReturnsOkWithoutServiceConnect)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 0);

    // Mark rank 1 already connected.
    mgr->channels_[1] = static_cast<Hcom_Channel>(0x123UL);

    DlHcomApiFnGuard guard;
    // If ServiceConnect is called, return failure to catch unexpected calls.
    DlHcomApi::gServiceConnect = &FakeServiceConnectFail;

    EXPECT_EQ(mgr->ConnectHcomChannel(1, "tcp://127.0.0.1:2048"), BM_OK);
    EXPECT_EQ(mgr->channels_[1], static_cast<Hcom_Channel>(0x123UL));
}

TEST(HcomTransportManagerTest, ConnectHcomChannelSetsLinkCountAndPayload)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 3, 2);

    DlHcomApiFnGuard guard;
    static uint64_t capturedLinkCount = 0;
    static std::string capturedPayload;
    DlHcomApi::gServiceConnect = +[](Hcom_Service, const char *, Hcom_Channel *ch, Service_ConnectOptions opt) -> int {
        capturedLinkCount = opt.linkCount;
        capturedPayload = std::string(opt.payLoad);
        *ch = static_cast<Hcom_Channel>(0x55UL);
        return 0;
    };

    // TCP url should use HCOM_TRANS_EP_SIZE.
    HcomPayload payload{};
    payload.client = mgr->rankId_;
    payload.server = 1;
    EXPECT_EQ(mgr->ConnectHcomChannel(1, "tcp://127.0.0.1:2048"), BM_OK);
    EXPECT_EQ(mgr->channels_[1], static_cast<Hcom_Channel>(0x55UL));
    EXPECT_EQ(capturedPayload, std::to_string(payload.payload));
    EXPECT_GE(capturedLinkCount, 1U);

    // UBC url should set linkCount = 1.
    mgr->channels_[1] = 0;
    EXPECT_EQ(mgr->ConnectHcomChannel(1, std::string(UBC_PROTOCOL_PREFIX) + "xxxx"), BM_OK);
    EXPECT_EQ(capturedPayload, std::to_string(payload.payload));
    EXPECT_EQ(capturedLinkCount, 1U);
}

TEST(HcomTransportManagerTest, ConnectHcomChannelPropagatesServiceConnectFailure)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 2, 0);

    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceConnect = &FakeServiceConnectFail;

    EXPECT_EQ(mgr->ConnectHcomChannel(1, "tcp://127.0.0.1:2048"), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(mgr->channels_[1], static_cast<Hcom_Channel>(0));
}

TEST(HcomTransportManagerTest, HcomChannelDisconnectedClearsMatchingChannelOnly)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rankId_ = 1;
    mgr->rankCount_ = 2;
    mgr->channels_ = std::vector<Hcom_Channel>(2, 0);
    mgr->channels_[0] = static_cast<Hcom_Channel>(0xAAUL);

    // Matching should clear.
    mgr->HcomChannelDisconnected(1, static_cast<Hcom_Channel>(0xAAUL));
    EXPECT_EQ(mgr->channels_[1], static_cast<Hcom_Channel>(0));
}

TEST(HcomTransportManagerTest, GetNicDefaultEmpty)
{
    auto mgr = HcomTransportManager::GetInstance();
    // Singleton might be mutated by other tests; normalize to default.
    mgr->localNic_.clear();
    const std::string &nic = mgr->GetNic();
    EXPECT_TRUE(nic.empty());
}

// NewEndPoint callback: invalid payload rejected; valid payload registers channel; out-of-range rank rejected.
TEST(HcomTransportManagerTest, NewEndPointCallbackHandlesNullAndNonNull)
{
    auto mgr = std::make_shared<HcomTransportManager>();
    mgr->rankCount_ = 1;
    mgr->channelMutex_ = std::vector<std::mutex>(1);
    mgr->channels_ = std::vector<Hcom_Channel>(1, 0);

    Hcom_Channel dummyCh = static_cast<Hcom_Channel>(0x1UL);
    EXPECT_NE(mgr->OnNewEndPoint(dummyCh, nullptr), BM_OK);
    EXPECT_NE(mgr->OnNewEndPoint(dummyCh, "test_payload"), BM_OK);

    // payload "0": client=0, server=0; local rank(UINT32_MAX) != server -> peer index = server = 0
    EXPECT_EQ(mgr->OnNewEndPoint(dummyCh, "0"), BM_OK);
    EXPECT_EQ(mgr->channels_[0], dummyCh);

    // payload "4294967296": client=0, server=1 -> peer index = server = 1, exceeds rankCount_=1 -> rejected
    EXPECT_NE(mgr->OnNewEndPoint(dummyCh, "4294967296"), BM_OK);

    // local rank equals payload.server -> peer index = payload.client = 0, still valid
    mgr->rankId_ = 1;
    EXPECT_EQ(mgr->OnNewEndPoint(dummyCh, "4294967296"), BM_OK);
    EXPECT_EQ(mgr->channels_[0], dummyCh);
}

// EndPointBroken: invalid payload (null / non-number) returns BM_ERROR.
TEST(HcomTransportManagerTest, EndPointBrokenCallbackInvalidPayload)
{
    auto mgr = std::make_shared<HcomTransportManager>();
    mgr->rankId_ = 0;
    mgr->rankCount_ = 1;
    mgr->channelMutex_ = std::vector<std::mutex>(1);
    mgr->channels_ = std::vector<Hcom_Channel>(1, 0);
    mgr->nics_ = std::vector<std::string>(1);

    Hcom_Channel dummyCh = static_cast<Hcom_Channel>(0x2UL);
    EXPECT_EQ(mgr->OnEndPointBroken(dummyCh, nullptr), BM_ERROR);

    const char *badPayload = "not_a_number";
    EXPECT_EQ(mgr->OnEndPointBroken(dummyCh, badPayload), BM_ERROR);
}

// Simple lifecycle callbacks just log and return BM_OK.
TEST(HcomTransportManagerTest, SimpleCallbacksReturnOk)
{
    Service_Context ctx = static_cast<Service_Context>(0x3UL);
    uint64_t usrCtx = 123;

    EXPECT_EQ(HcomTransportManager::TransportRpcHcomRequestReceived(ctx, usrCtx), BM_OK);
    EXPECT_EQ(HcomTransportManager::TransportRpcHcomRequestPosted(ctx, usrCtx), BM_OK);
    EXPECT_EQ(HcomTransportManager::TransportRpcHcomOneSideDone(ctx, usrCtx), BM_OK);
}

// Several instances can Open/Close at the same time, their service names and slots do not clash, and a slot
// can be reused after close.
TEST(HcomTransportManagerTest, MultiInstanceOpenClose_IndependentServiceAndSlot)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceCreate = &FakeServiceCreate;
    DlHcomApi::gServiceBind = &FakeServiceBind;
    DlHcomApi::gServiceStart = &FakeServiceStart;
    DlHcomApi::gServiceDestroy = &FakeServiceDestroy;
    DlHcomApi::gServiceSetTlsOptions = &FakeSetTlsOptions;
    DlHcomApi::gServiceRegisterChannelBrokerHandler = &FakeRegisterChannelBrokerHandler;
    DlHcomApi::gServiceRegisterHandler = &FakeRegisterHandler;
    DlHcomApi::gServiceSetDeviceIpMask = &FakeSetDeviceIpMask;
    DlHcomApi::gSetExternalLogger = &FakeSetExternalLogger;

    auto a = std::make_shared<HcomTransportManager>();
    auto b = std::make_shared<HcomTransportManager>();

    TransportOptions optsA{};
    optsA.rankId = 1;
    optsA.rankCount = 2;
    optsA.protocol = HYBM_DOP_TYPE_HOST_TCP;
    optsA.nic = "tcp://127.0.0.1:2048";
    TransportOptions optsB = optsA;
    optsB.rankId = 2;

    EXPECT_EQ(a->OpenDevice(optsA), BM_OK);
    EXPECT_EQ(b->OpenDevice(optsB), BM_OK);
    EXPECT_NE(a->slot_, b->slot_);
    EXPECT_NE(a->serviceName_, b->serviceName_);
    EXPECT_NE(a->localNic_, b->localNic_); // basePort + rankId gives disjoint ports

    a->CloseDevice();
    b->CloseDevice();
    EXPECT_EQ(a->slot_, HcomTransportManager::kInvalidSlot);
    EXPECT_EQ(b->slot_, HcomTransportManager::kInvalidSlot);

    // the slot has been released: a new instance can run the full Open/Close again
    auto c = std::make_shared<HcomTransportManager>();
    EXPECT_EQ(c->OpenDevice(optsA), BM_OK);
    c->CloseDevice();
}

// port conflict avoidance: when the starting port fails to bind, retry with an increment and publish the
// address actually bound in localNic_
TEST(HcomTransportManagerTest, OpenDevice_BindPortConflict_RetriesWithOffset)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gServiceCreate = &FakeServiceCreate;
    DlHcomApi::gServiceBind = &FakeServiceBindFailFirst;
    DlHcomApi::gServiceStart = &FakeServiceStart;
    DlHcomApi::gServiceDestroy = &FakeServiceDestroy;
    DlHcomApi::gServiceSetTlsOptions = &FakeSetTlsOptions;
    DlHcomApi::gServiceRegisterChannelBrokerHandler = &FakeRegisterChannelBrokerHandler;
    DlHcomApi::gServiceRegisterHandler = &FakeRegisterHandler;
    DlHcomApi::gServiceSetDeviceIpMask = &FakeSetDeviceIpMask;
    DlHcomApi::gSetExternalLogger = &FakeSetExternalLogger;

    auto mgr = std::make_shared<HcomTransportManager>();
    TransportOptions opts{};
    opts.rankId = 1;
    opts.rankCount = 2;
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.nic = "tcp://127.0.0.1:2048";

    // preferred port 2048+1=2049 is occupied (one bind failure) -> the retry on 2050 succeeds
    g_serviceBindFailTimes = 1;
    EXPECT_EQ(mgr->OpenDevice(opts), BM_OK);
    EXPECT_EQ(mgr->localNic_, "tcp://127.0.0.1:2050");
    mgr->CloseDevice();
    g_serviceBindFailTimes = 0;
}

// PrepareThreadLocalStream should create stream once and reuse.
TEST(HcomTransportManagerTest, PrepareThreadLocalStreamCreatesOnce)
{
    auto mgr = HcomTransportManager::GetInstance();

    HcomTransportManager::stream_ = nullptr;

    int ret1 = mgr->PrepareThreadLocalStream();
    EXPECT_EQ(ret1, BM_OK);
    EXPECT_NE(HcomTransportManager::stream_, nullptr);

    auto *oldPtr = HcomTransportManager::stream_.get();
    int ret2 = mgr->PrepareThreadLocalStream();
    EXPECT_EQ(ret2, BM_OK);
    EXPECT_EQ(HcomTransportManager::stream_.get(), oldPtr);
}

// InnerReadRemote: size > uint32_t max should be rejected early.
TEST(HcomTransportManagerTest, InnerReadRemoteRejectsTooLargeSize)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x10UL);

    uint64_t bigSize = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ULL;
    Result ret = mgr->InnerReadRemote(0, 0x1000, 0x2000, bigSize);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// InnerWriteRemote: size > uint32_t max should be rejected early.
TEST(HcomTransportManagerTest, InnerWriteRemoteRejectsTooLargeSize)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x20UL);

    uint64_t bigSize = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ULL;
    Result ret = mgr->InnerWriteRemote(0, 0x1000, 0x2000, bigSize);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// OpenDevice：rpcService_ != 0 时命中 host_hcom_transport_manager.cpp:117 的幂等短路，先于 options 校验返回 BM_OK。
// 本用例原先依赖前一个用例泄漏的 rpcService_ = 1，逐用例复位后改为显式构造该前置状态。
TEST(HcomTransportManagerTest, OpenDeviceInvalidOptions)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 1;

    TransportOptions opts{};
    opts.rankId = 1;
    opts.rankCount = 0;
    opts.protocol = 0;
    opts.nic = "";

    Result ret = mgr->OpenDevice(opts);
    EXPECT_EQ(ret, BM_OK);
}

// UpdateRankOptions: rankId 超过 rankCount_ 时返回 BM_INVALID_PARAM。
TEST(HcomTransportManagerTest, UpdateRankOptionsInvalidRank)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rankCount_ = 1;

    HybmTransPrepareOptions opts{};
    TransportRankPrepareInfo info{};
    opts.options.emplace(5U, info); // 超出 rankCount_

    Result ret = mgr->UpdateRankOptions(opts);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// RegisterMemoryRegion: addr 或 size 为 0 返回 BM_INVALID_PARAM。
TEST(HcomTransportManagerTest, RegisterMemoryRegionInvalidAddrOrSize)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 1; // 让 rpcService_ 判定通过

    TransportMemoryRegion mr{};

    mr.addr = 0;
    mr.size = 1024;
    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_INVALID_PARAM);

    mr.addr = 0x1000;
    mr.size = 0;
    EXPECT_EQ(mgr->RegisterMemoryRegion(mr), BM_INVALID_PARAM);
}

// ReadRemote / WriteRemote: 在 rpcService_ 为 0 时应直接返回 BM_ERROR（早退分支）。
TEST(HcomTransportManagerTest, ReadWriteRemoteWithoutRpcService)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    mgr->rankCount_ = 0; // 不进入任何循环

    Result r1 = mgr->ReadRemote(0, 0x1000, 0x2000, 128);
    Result r2 = mgr->WriteRemote(0, 0x1000, 0x2000, 128);
    EXPECT_EQ(r1, BM_ERROR);
    EXPECT_EQ(r2, BM_ERROR);
}

// ReadRemoteBatchAsync: counts 为空时走 BM_INVALID_PARAM 分支。
TEST(HcomTransportManagerTest, ReadRemoteBatchAsyncEmptyDescriptor)
{
    auto mgr = HcomTransportManager::GetInstance();
    CopyDescriptor desc; // counts 默认空
    Result ret = mgr->ReadRemoteBatchAsync(0, desc);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

TEST(HcomTransportManagerTest, ReadRemoteBatchAsyncChannelGetVFailAfterKeyLookup)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gChannelBatchGet = &FakeChannelGetVFail;

    auto mgr = HcomTransportManager::GetInstance();
    HcomTransportManager::stream_.reset();

    SetupManager(mgr, 1, 0);
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x1UL);

    // For ReadRemoteBatchAsync:
    // - req.lAddress = descriptor.globalAddrs[i]
    // - rAddr        = descriptor.localAddrs[i]
    HcomMemoryRegion localMr{};
    localMr.addr = 0x2000; // matches globalAddrs
    localMr.lva = 0x2000;
    localMr.size = 0x1000;
    localMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        localMr.lKey.keys[i] = 0x2000ULL + i;
    }

    HcomMemoryRegion remoteMr{};
    remoteMr.addr = 0x1000; // matches localAddrs (remote source)
    remoteMr.lva = 0x1000;
    remoteMr.size = 0x1000;
    remoteMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        remoteMr.lKey.keys[i] = 0x1000ULL + i;
    }

    mgr->mrs_[0].insert(localMr);
    mgr->mrs_[0].insert(remoteMr);

    CopyDescriptor desc{};
    desc.counts.push_back(16);
    desc.globalAddrs.push_back(reinterpret_cast<void *>(0x2000)); // local destination
    desc.localAddrs.push_back(reinterpret_cast<void *>(0x1000));  // remote source gva

    EXPECT_EQ(mgr->ReadRemoteBatchAsync(0, desc), BM_ERROR);
}

// ReadRemoteAsync / WriteRemoteAsync: rpcService_ 未初始化时返回 BM_ERROR。
TEST(HcomTransportManagerTest, ReadWriteRemoteAsyncWithoutRpcService)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    mgr->rankCount_ = 1;
    mgr->channels_.clear();
    mgr->channels_.push_back(static_cast<Hcom_Channel>(0x1UL)); // 只填充简单通道

    Result r1 = mgr->ReadRemoteAsync(0, 0x1000, 0x2000, 128);
    Result r2 = mgr->WriteRemoteAsync(0, 0x1000, 0x2000, 128);
    EXPECT_EQ(r1, BM_ERROR);
    EXPECT_EQ(r2, BM_ERROR);
}

TEST(HcomTransportManagerTest, ReadWriteRemoteAsyncChannelGetPutFailAfterKeyLookup)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gChannelGet = &FakeChannelGetFail;
    DlHcomApi::gChannelPut = &FakeChannelPutFail;

    auto mgr = HcomTransportManager::GetInstance();

    // Keep thread_local stream isolated and deterministic.
    HcomTransportManager::stream_.reset();

    SetupManager(mgr, 1, 0);
    mgr->runtimeConfig_.maxSliceSize = 1024;
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x1UL);

    HcomMemoryRegion localMr{};
    localMr.addr = 0x1000;
    localMr.lva = 0x1000;
    localMr.size = 0x1000;
    localMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        localMr.lKey.keys[i] = 0x1000ULL + i;
    }

    HcomMemoryRegion remoteMr{};
    remoteMr.addr = 0x2000;
    remoteMr.lva = 0x2000;
    remoteMr.size = 0x1000;
    remoteMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        remoteMr.lKey.keys[i] = 0x2000ULL + i;
    }

    // For this UT, both lAddr and rAddr are found within the same rank's MR list.
    mgr->mrs_[0].insert(localMr);
    mgr->mrs_[0].insert(remoteMr);

    const uint64_t lAddr = 0x1000;
    const uint64_t rAddr = 0x2000;
    const uint64_t size = 128;

    EXPECT_EQ(mgr->ReadRemoteAsync(0, lAddr, rAddr, size), BM_ERROR);
    EXPECT_EQ(mgr->WriteRemoteAsync(0, lAddr, rAddr, size), BM_ERROR);
}

// WriteRemoteBatchAsync: 未初始化 rpcService_ 时直接返回 BM_ERROR。
TEST(HcomTransportManagerTest, WriteRemoteBatchAsyncCheckTransportOptions)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    mgr->rankCount_ = 1;
    mgr->channels_.clear();
    mgr->channels_.push_back(static_cast<Hcom_Channel>(0x1UL));

    CopyDescriptor desc{};
    desc.counts.push_back(16);
    desc.localAddrs.push_back(reinterpret_cast<void *>(0x1000));
    desc.globalAddrs.push_back(reinterpret_cast<void *>(0x2000));

    Result ret = mgr->WriteRemoteBatchAsync(0, desc);
    EXPECT_EQ(ret, BM_ERROR);
}

// WriteRemoteBatchAsync: counts 为空时，直接走循环外返回 BM_OK（覆盖空 batch 分支）。
TEST(HcomTransportManagerTest, WriteRemoteBatchAsyncEmptyCountsReturnsOk)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 1;
    mgr->rankCount_ = 1;
    mgr->channels_.clear();
    mgr->channels_.push_back(static_cast<Hcom_Channel>(0x1UL));

    CopyDescriptor desc{};
    EXPECT_TRUE(desc.counts.empty());

    Result ret = mgr->WriteRemoteBatchAsync(0, desc);
    EXPECT_EQ(ret, BM_OK);
}

TEST(HcomTransportManagerTest, WriteRemoteBatchAsyncChannelPutVFailAfterKeyLookup)
{
    DlHcomApiFnGuard guard;
    DlHcomApi::gChannelBatchPut = &FakeChannelPutVFail;

    auto mgr = HcomTransportManager::GetInstance();
    HcomTransportManager::stream_.reset();

    SetupManager(mgr, 1, 0);
    mgr->channels_[0] = static_cast<Hcom_Channel>(0x1UL);

    HcomMemoryRegion localMr{};
    localMr.addr = 0x1000;
    localMr.lva = 0x1000;
    localMr.size = 0x1000;
    localMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        localMr.lKey.keys[i] = 0x1000ULL + i;
    }

    HcomMemoryRegion remoteMr{};
    remoteMr.addr = 0x2000;
    remoteMr.lva = 0x2000;
    remoteMr.size = 0x1000;
    remoteMr.lKey.keys.assign(8, 0);
    for (size_t i = 0; i < 8; i++) {
        remoteMr.lKey.keys[i] = 0x2000ULL + i;
    }

    mgr->mrs_[0].insert(localMr);
    mgr->mrs_[0].insert(remoteMr);

    CopyDescriptor desc{};
    desc.counts.push_back(16);
    desc.localAddrs.push_back(reinterpret_cast<void *>(0x1000));  // local addr (lAddress)
    desc.globalAddrs.push_back(reinterpret_cast<void *>(0x2000)); // remote/global addr (rAddr)

    EXPECT_EQ(mgr->WriteRemoteBatchAsync(0, desc), BM_ERROR);
}

// （注意）ReadRemoteAsync / WriteRemoteAsync / WriteRemoteBatchAsync 的“正常路径”依赖 DlHcomApi::ChannelGet/Put/PutV
// 的真实 stub，此处不再强行构造 happy path，仅保持 rpcService_ 为 0 的早退覆盖，避免触发 gChannelXXX 断言。

// GetCACallBack: 传入空指针参数返回非 0（错误分支）。
TEST(HcomTransportManagerTest, GetCACallBackInvalidArgs)
{
    char *caPath = nullptr;
    char *crlPath = nullptr;
    Hcom_PeerCertVerifyType verifyType{};
    Hcom_TlsCertVerify verify{};

    int ret1 = HcomTransportManager::GetCACallBack(nullptr, nullptr, &crlPath, &verifyType, &verify);
    int ret2 = HcomTransportManager::GetCACallBack(nullptr, &caPath, nullptr, &verifyType, &verify);
    EXPECT_NE(ret1, 0);
    EXPECT_NE(ret2, 0);
}

// GetCACallBack: 基本成功路径，校验 caPath / crlPath 被正确回填。
TEST(HcomTransportManagerTest, GetCACallBackSuccessBasic)
{
    auto mgr = HcomTransportManager::GetInstance();
    std::memset(mgr->tlsConfig_.caPath, 0, sizeof(mgr->tlsConfig_.caPath));
    std::memset(mgr->tlsConfig_.crlPath, 0, sizeof(mgr->tlsConfig_.crlPath));
    std::strncpy(mgr->tlsConfig_.caPath, "/tmp/ca.pem", sizeof(mgr->tlsConfig_.caPath) - 1);
    std::strncpy(mgr->tlsConfig_.crlPath, "/tmp/crl.pem", sizeof(mgr->tlsConfig_.crlPath) - 1);

    char *caPath = nullptr;
    char *crlPath = nullptr;
    Hcom_PeerCertVerifyType verifyType{};
    Hcom_TlsCertVerify verify{};

    int ret = HcomTransportManager::GetCACallBack("test", &caPath, &crlPath, &verifyType, &verify);
    EXPECT_EQ(ret, 0);
    EXPECT_STREQ(caPath, mgr->tlsConfig_.caPath);
    EXPECT_STREQ(crlPath, mgr->tlsConfig_.crlPath);
}

// GetPrivateKeyCallBack: 空指针参数直接返回非 0（错误分支）。
TEST(HcomTransportManagerTest, GetPrivateKeyCallBackInvalidArgs)
{
    int ret = HcomTransportManager::GetPrivateKeyCallBack(nullptr, nullptr, nullptr, nullptr);
    EXPECT_NE(ret, 0);
}

// KeyPassEraseCallBack: 将缓冲区内容置零。
TEST(HcomTransportManagerTest, KeyPassEraseCallBackZeroizesBuffer)
{
    char buf[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    HcomTransportManager::KeyPassEraseCallBack(buf, 8);
    for (char c : buf) {
        EXPECT_EQ(c, 0);
    }
}

// ChannelAsyncCallback: 基本调用路径，不崩溃即可。
TEST(HcomTransportManagerTest, ChannelAsyncCallbackBasic)
{
    HostHcomCounterStream stream(0);
    Service_Context ctx = static_cast<Service_Context>(0);
    HcomTransportManager::ChannelAsyncCallback(&stream, ctx);
}

// Synchronize: 当 stream_ 为空时应直接返回 BM_OK，不阻塞。
TEST(HcomTransportManagerTest, SynchronizeWithoutStreamReturnsOk)
{
    auto mgr = HcomTransportManager::GetInstance();
    HcomTransportManager::stream_.reset();

    Result ret = mgr->Synchronize(0);
    EXPECT_EQ(ret, BM_OK);
}

// Synchronize: 通过 stream_ 的 SubmitTasks / FinishOne 路径，确保不会死锁且返回 BM_OK。
TEST(HcomTransportManagerTest, SubmitTasksAndFinishThenSynchronizeOkViaManager)
{
    auto mgr = HcomTransportManager::GetInstance();

    // 准备一个计数为 0 的流，并挂到 manager 的 thread_local stream_ 上。
    auto stream = std::make_shared<HostHcomCounterStream>(0);
    HcomTransportManager::stream_ = stream;

    // 模拟一次提交 + 完成，随后通过 Synchronize 等待。
    stream->SubmitTasks(1);
    stream->FinishOne();

    Result ret = mgr->Synchronize(0);
    EXPECT_EQ(ret, BM_OK);
}

// GetCertCallBack: certPath 为空指针时返回非 0。
TEST(HcomTransportManagerTest, GetCertCallBackInvalidArgs)
{
    int ret = HcomTransportManager::GetCertCallBack(nullptr, nullptr);
    EXPECT_NE(ret, 0);
}

// GetCertCallBack: 正常路径，将 certPath 指向 tlsConfig_.certPath。
TEST(HcomTransportManagerTest, GetCertCallBackSuccessBasic)
{
    auto mgr = HcomTransportManager::GetInstance();
    std::memset(mgr->tlsConfig_.certPath, 0, sizeof(mgr->tlsConfig_.certPath));
    std::strncpy(mgr->tlsConfig_.certPath, "/tmp/cert.pem", sizeof(mgr->tlsConfig_.certPath) - 1);

    char *certPath = nullptr;
    int ret = HcomTransportManager::GetCertCallBack("test", &certPath);
    EXPECT_EQ(ret, 0);
    EXPECT_STREQ(certPath, mgr->tlsConfig_.certPath);
}

// GetMemoryRegionByAddr: 没有注册 MR 时应返回 BM_ERROR。
TEST(HcomTransportManagerTest, GetMemoryRegionByAddrNotFound)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    HcomMemoryRegion mr{};
    Result ret = mgr->GetMemoryRegionByAddr(0, 0x1000, mr);
    EXPECT_EQ(ret, BM_ERROR);
}

// GetMemoryRegionByAddr: 命中的 MR 区间应返回 BM_OK。
TEST(HcomTransportManagerTest, GetMemoryRegionByAddrHit)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    HcomMemoryRegion info{};
    info.addr = 0x1000;
    info.size = 0x100;
    mgr->mrs_[0].insert(info);

    HcomMemoryRegion out{};
    Result ret = mgr->GetMemoryRegionByAddr(0, 0x1080, out);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(out.addr, info.addr);
    EXPECT_EQ(out.size, info.size);
}

// QueryHasRegistered: 根据 MR 区间返回 true/false。
TEST(HcomTransportManagerTest, QueryHasRegisteredBasic)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    HcomMemoryRegion info{};
    info.addr = 0x2000;
    info.size = 0x200;
    mgr->mrs_[0].insert(info);

    EXPECT_TRUE(mgr->QueryHasRegistered(0x2000, 0x100));
    EXPECT_FALSE(mgr->QueryHasRegistered(0x3000, 0x10));
}

// UnregisterMemoryRegion: addr 为 0 直接返回 BM_INVALID_PARAM。
TEST(HcomTransportManagerTest, UnregisterMemoryRegionZeroAddr)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 1;
    Result ret = mgr->UnregisterMemoryRegion(0);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// UnregisterMemoryRegion: 未找到 addr 时返回 BM_OK，不崩溃。
TEST(HcomTransportManagerTest, UnregisterMemoryRegionAddrNotFound)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    Result ret = mgr->UnregisterMemoryRegion(0x1234);
    EXPECT_EQ(ret, BM_OK);
}

TEST(HcomTransportManagerTest, UnregisterMemoryRegionWithoutServiceReturnsError)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    // addr != 0, but service not started -> BM_ERROR
    Result ret = mgr->UnregisterMemoryRegion(0x1234);
    EXPECT_EQ(ret, BM_ERROR);
}

// QueryMemoryKey: 未找到 MR 时应返回 BM_ERROR。
TEST(HcomTransportManagerTest, QueryMemoryKeyNoRegion)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    TransportMemoryKey key{};
    Result ret = mgr->QueryMemoryKey(0x4000, key);
    EXPECT_EQ(ret, BM_ERROR);
}

// QueryMemoryKey: when the HVA->GVA reverse lookup misses it must fall back to the HVA, so that 0 is not
// taken downstream as the peer MR address.
TEST(HcomTransportManagerTest, QueryMemoryKeyFallsBackToHvaWhenTransformMisses)
{
    auto mgr = HcomTransportManager::GetInstance();
    SetupManager(mgr, 1, 0);

    HcomMemoryRegion info{};
    info.addr = 0x3000;
    info.size = 0x100;
    mgr->mrs_[0].insert(info);

    TransportMemoryKey key{};
    Result ret = mgr->QueryMemoryKey(0x3000, key);
    EXPECT_EQ(ret, BM_OK);

    RegMemoryKey parsed{};
    ASSERT_TRUE(ReadStructFromKey(key, parsed));
    // the VA table has no paired record for this HVA => TransformVa returns 0 => the HVA must be kept
    EXPECT_EQ(parsed.gva, 0x3000UL);
}

// UpdateMemoryKey: in the trans scene the gva of the host key should be overwritten with the local rebuilt
// GVA given by the caller, leaving the other fields untouched.
TEST(HcomTransportManagerTest, UpdateMemoryKeyRewritesGvaForHostKey)
{
    auto mgr = HcomTransportManager::GetInstance();

    RegMemoryKey hostKey{};
    hostKey.type = TT_HCOM;
    hostKey.gva = 0x1000UL;
    hostKey.hcomInfo.lAddress = 0x2800UL;
    TransportMemoryKey key{};
    WriteStructToKey(hostKey, key);

    mgr->UpdateMemoryKey(key, reinterpret_cast<void *>(0x20000000UL));

    RegMemoryKey updated{};
    ASSERT_TRUE(ReadStructFromKey(key, updated));
    EXPECT_EQ(updated.type, TT_HCOM);
    EXPECT_EQ(updated.gva, 0x20000000UL);
    EXPECT_EQ(updated.hcomInfo.lAddress, 0x2800UL);
}

// UpdateMemoryKey: a non-host key must not be rewritten, to avoid polluting the keys of other transports.
TEST(HcomTransportManagerTest, UpdateMemoryKeySkipsNonHostKey)
{
    auto mgr = HcomTransportManager::GetInstance();

    TransportMemoryKey key{};
    key.keys.assign(1, 0x1234UL);

    mgr->UpdateMemoryKey(key, reinterpret_cast<void *>(0x20000000UL));

    EXPECT_EQ(key.keys[0], 0x1234UL);
}

// UpdateMemoryKey: keeps the original value when addr is empty.
TEST(HcomTransportManagerTest, UpdateMemoryKeyNullAddrKeepsGva)
{
    auto mgr = HcomTransportManager::GetInstance();

    RegMemoryKey hostKey{};
    hostKey.type = TT_HCOM;
    hostKey.gva = 0x1000UL;
    TransportMemoryKey key{};
    WriteStructToKey(hostKey, key);

    mgr->UpdateMemoryKey(key, nullptr);

    RegMemoryKey updated{};
    ASSERT_TRUE(ReadStructFromKey(key, updated));
    EXPECT_EQ(updated.gva, 0x1000UL);
}

// CloseDevice: 在 rpcService_ 为 0 时直接返回 BM_OK。
TEST(HcomTransportManagerTest, CloseDeviceWithoutService)
{
    auto mgr = HcomTransportManager::GetInstance();
    mgr->rpcService_ = 0;
    Result ret = mgr->CloseDevice();
    EXPECT_EQ(ret, BM_OK);
}

TEST(HcomTransportManagerTest, GetPrivateKeyCallBackFileOpenFail)
{
    auto mgr = HcomTransportManager::GetInstance();
    std::memset(mgr->tlsConfig_.keyPath, 0, sizeof(mgr->tlsConfig_.keyPath));
    std::memset(mgr->tlsConfig_.keyPassPath, 0, sizeof(mgr->tlsConfig_.keyPassPath));
    std::memset(mgr->tlsConfig_.decrypterLibPath, 0, sizeof(mgr->tlsConfig_.decrypterLibPath));

    std::strncpy(mgr->tlsConfig_.keyPath, "/tmp/key.pem", sizeof(mgr->tlsConfig_.keyPath) - 1);
    std::strncpy(mgr->tlsConfig_.keyPassPath, "/path/not/exist", sizeof(mgr->tlsConfig_.keyPassPath) - 1);

    char *priKeyPath = nullptr;
    char *keyPass = nullptr;
    Hcom_TlsKeyPassErase erase{};
    int ret = HcomTransportManager::GetPrivateKeyCallBack("test", &priKeyPath, &keyPass, &erase);
    EXPECT_NE(ret, 0);
}

TEST(HcomTransportManagerTest, CertVerifyCallBackReturnsZero)
{
    EXPECT_EQ(HcomTransportManager::CertVerifyCallBack(nullptr, nullptr), 0);
}

// CheckTransportOptions: 非法 nic 时应直接返回错误码（来自 AnalysisNic）。
TEST(HcomTransportManagerTest, CheckTransportOptionsInvalidNic)
{
    auto mgr = HcomTransportManager::GetInstance();

    TransportOptions opts{};
    opts.nic = "not_an_ip";
    opts.rankId = 0;
    opts.rankCount = 1;

    Result ret = mgr->CheckTransportOptions(opts);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// Prepare: options 中存在 rankId >= rankCount_ 时返回 BM_INVALID_PARAM。
TEST(HcomTransportManagerTest, PrepareInvalidRankInOptions)
{
    auto mgr = HcomTransportManager::GetInstance();

    mgr->rankCount_ = 1;

    HybmTransPrepareOptions param{};
    TransportRankPrepareInfo info{};
    param.options.emplace(5U, info); // 超出 rankCount_

    Result ret = mgr->Prepare(param);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// HcomChannelDisconnected: 给定的 channel 在数组中不存在时，应安全返回（不崩溃）。
TEST(HcomTransportManagerTest, HcomChannelDisconnectedNoMatchChannel)
{
    auto mgr = HcomTransportManager::GetInstance();

    mgr->rankCount_ = 2;
    mgr->channels_.clear();
    mgr->channels_.push_back(static_cast<Hcom_Channel>(0x1UL));
    mgr->channels_.push_back(static_cast<Hcom_Channel>(0x2UL));

    // 不存在 0x9UL 这个 channel，只验证不会崩溃。
    mgr->HcomChannelDisconnected(0, static_cast<Hcom_Channel>(0x9UL));
}
