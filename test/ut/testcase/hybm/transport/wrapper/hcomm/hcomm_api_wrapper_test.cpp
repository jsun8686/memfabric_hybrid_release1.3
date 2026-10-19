/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
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
#include <cstdint>
#include <limits>

#define private public
#include "hcomm_api_wrapper.h"
#include "dl_hcomm_api.h"
#include "device_kernel_helper.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport::device;

static constexpr uint64_t MOCK_ADDR_1 = 0x1000UL;
static constexpr uint64_t MOCK_ADDR_2 = 0x2000UL;
static constexpr uint64_t MOCK_SIZE = 0x1000UL;

namespace {

// ── HcommMemEntry helpers ──
HcommMemEntry MakeEntry(uint64_t addr = MOCK_ADDR_1, uint64_t size = MOCK_SIZE, int32_t memType = COMM_MEM_TYPE_HOST,
                        HcommMemHandleType handle = nullptr)
{
    HcommMemEntry e{};
    e.handle = handle;
    e.addr = addr;
    e.size = size;
    e.memType = memType;
    e.refCount = 1;
    return e;
}

HcommCommMem MakeCommMem(uint64_t addr = MOCK_ADDR_1, uint64_t size = MOCK_SIZE, int32_t memType = COMM_MEM_TYPE_HOST)
{
    return HcommCommMem{memType, reinterpret_cast<void *>(addr), size};
}

// ── GetRangeEnd ──

TEST(HcommApiWrapperTest, GetRangeEnd_Normal)
{
    uint64_t end = 0;
    EXPECT_TRUE(HcommApiWrapper::GetRangeEnd(0x1000, 0x100, end));
    EXPECT_EQ(end, 0x1100);
}

TEST(HcommApiWrapperTest, GetRangeEnd_ZeroAddr)
{
    uint64_t end = 0;
    EXPECT_FALSE(HcommApiWrapper::GetRangeEnd(0, 0x100, end));
}

TEST(HcommApiWrapperTest, GetRangeEnd_ZeroSize)
{
    uint64_t end = 0;
    EXPECT_FALSE(HcommApiWrapper::GetRangeEnd(0x1000, 0, end));
}

TEST(HcommApiWrapperTest, GetRangeEnd_Overflow)
{
    uint64_t end = 0;
    EXPECT_FALSE(HcommApiWrapper::GetRangeEnd(std::numeric_limits<uint64_t>::max() - 10, 20, end));
}

// ── IsValidMem ──

TEST(HcommApiWrapperTest, IsValidMem_Host)
{
    auto mem = MakeCommMem(0x1000, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_TRUE(HcommApiWrapper::IsValidMem(mem));
}

TEST(HcommApiWrapperTest, IsValidMem_Device)
{
    auto mem = MakeCommMem(0x1000, 0x100, COMM_MEM_TYPE_DEVICE);
    EXPECT_TRUE(HcommApiWrapper::IsValidMem(mem));
}

TEST(HcommApiWrapperTest, IsValidMem_InvalidType)
{
    auto mem = MakeCommMem(0x1000, 0x100, COMM_MEM_TYPE_INVALID);
    EXPECT_FALSE(HcommApiWrapper::IsValidMem(mem));
}

TEST(HcommApiWrapperTest, IsValidMem_ZeroAddr)
{
    auto mem = MakeCommMem(0, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(HcommApiWrapper::IsValidMem(mem));
}

// ── SameMem ──

TEST(HcommApiWrapperTest, SameMem_Matching)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    EXPECT_TRUE(HcommApiWrapper::SameMem(entry, mem));
}

TEST(HcommApiWrapperTest, SameMem_DifferentAddr)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_2, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(HcommApiWrapper::SameMem(entry, mem));
}

TEST(HcommApiWrapperTest, SameMem_DifferentType)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_DEVICE);
    EXPECT_FALSE(HcommApiWrapper::SameMem(entry, mem));
}

// ── Overlaps ──

TEST(HcommApiWrapperTest, Overlaps_ExactOverlap)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    EXPECT_TRUE(HcommApiWrapper::Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, Overlaps_PartialOverlap)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_1 + 0x800, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    EXPECT_TRUE(HcommApiWrapper::Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, Overlaps_NoOverlap)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_2 + MOCK_SIZE, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(HcommApiWrapper::Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, Overlaps_DifferentType)
{
    auto entry = MakeEntry(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_DEVICE);
    EXPECT_FALSE(HcommApiWrapper::Overlaps(entry, mem));
}

// ── RegisterMemory (cached) ──

struct DlHcommApiGuard {
    hcommMemRegFunc oldMemReg{DlHcommApi::gHcommMemReg};
    hcommMemUnregFunc oldMemUnreg{DlHcommApi::gHcommMemUnreg};
    hcommMemExportFunc oldMemExport{DlHcommApi::gHcommMemExport};
    hcommMemImportFunc oldMemImport{DlHcommApi::gHcommMemImport};
    hcommMemUnimportFunc oldMemUnimport{DlHcommApi::gHcommMemUnimport};
    hcommEndpointCreateFunc oldEndpointCreate{DlHcommApi::gHcommEndpointCreate};
    hcommEndpointDestroyFunc oldEndpointDestroy{DlHcommApi::gHcommEndpointDestroy};
    hcommThreadAllocFunc oldThreadAlloc{DlHcommApi::gHcommThreadAlloc};
    hcommThreadFreeFunc oldThreadFree{DlHcommApi::gHcommThreadFree};
    hcommChannelCreateFunc oldChannelCreate{DlHcommApi::gHcommChannelCreate};
    hcommChannelDestroyFunc oldChannelDestroy{DlHcommApi::gHcommChannelDestroy};
    hcommChannelGetStatusFunc oldChannelGetStatus{DlHcommApi::gHcommChannelGetStatus};

    ~DlHcommApiGuard()
    {
        DlHcommApi::gHcommMemReg = oldMemReg;
        DlHcommApi::gHcommMemUnreg = oldMemUnreg;
        DlHcommApi::gHcommMemExport = oldMemExport;
        DlHcommApi::gHcommMemImport = oldMemImport;
        DlHcommApi::gHcommMemUnimport = oldMemUnimport;
        DlHcommApi::gHcommEndpointCreate = oldEndpointCreate;
        DlHcommApi::gHcommEndpointDestroy = oldEndpointDestroy;
        DlHcommApi::gHcommThreadAlloc = oldThreadAlloc;
        DlHcommApi::gHcommThreadFree = oldThreadFree;
        DlHcommApi::gHcommChannelCreate = oldChannelCreate;
        DlHcommApi::gHcommChannelDestroy = oldChannelDestroy;
        DlHcommApi::gHcommChannelGetStatus = oldChannelGetStatus;
    }
};

TEST(HcommApiWrapperTest, RegisterMemory_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto ret = wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_NE(handle, nullptr);
}

TEST(HcommApiWrapperTest, RegisterMemory_DuplicateTag)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle1 = nullptr;
    HcommMemHandleType handle2 = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);

    // First registration
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle1), BM_OK);

    // Same tag, same mem → should refCount++ and succeed
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle2), BM_OK);
    ASSERT_EQ(handle1, handle2); // same handle (refCount increased)
}

TEST(HcommApiWrapperTest, RegisterMemory_OverlapConflict)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    HcommMemHandleType h1 = nullptr;
    HcommMemHandleType h2 = nullptr;
    auto mem1 = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto mem2 = MakeCommMem(MOCK_ADDR_1 + 0x800, MOCK_SIZE, COMM_MEM_TYPE_HOST);

    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem1, h1), BM_OK);

    // Overlapping mem with different tag → should fail
    ASSERT_NE(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xCAFE, mem2, h2), BM_OK);
}

TEST(HcommApiWrapperTest, RegisterMemory_MultiEndpoint_SameTagSameMem)
{
    DlHcommApiGuard guard;
    static int regCount = 0;
    static int unregCount = 0;
    regCount = 0;
    unregCount = 0;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL + regCount);
        regCount++;
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t {
        unregCount++;
        return 0;
    };

    HcommApiWrapper wrapper;
    auto ep0 = reinterpret_cast<HcommEndpointHandle>(0xA001UL);
    auto ep1 = reinterpret_cast<HcommEndpointHandle>(0xA002UL);
    HcommMemHandleType h0 = nullptr;
    HcommMemHandleType h1 = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);

    // Same tag / same mem on different endpoints must register independently
    ASSERT_EQ(wrapper.RegisterMemory(ep0, 0xBEEF, mem, h0), BM_OK);
    ASSERT_EQ(wrapper.RegisterMemory(ep1, 0xBEEF, mem, h1), BM_OK);
    EXPECT_EQ(regCount, 2);
    EXPECT_NE(h0, h1);

    // Same endpoint / tag / mem → cache hit, no new registration
    HcommMemHandleType h0Again = nullptr;
    ASSERT_EQ(wrapper.RegisterMemory(ep0, 0xBEEF, mem, h0Again), BM_OK);
    EXPECT_EQ(regCount, 2);
    EXPECT_EQ(h0, h0Again);

    // Unregister is scoped per endpoint: ep0 refCount is 2, ep1 is 1
    ASSERT_EQ(wrapper.UnregisterMemory(ep0, h0), BM_OK);
    EXPECT_EQ(unregCount, 0);
    ASSERT_EQ(wrapper.UnregisterMemory(ep0, h0), BM_OK);
    EXPECT_EQ(unregCount, 1);
    ASSERT_EQ(wrapper.UnregisterMemory(ep1, h1), BM_OK);
    EXPECT_EQ(unregCount, 2);
}

TEST(HcommApiWrapperTest, RegisterMemory_MultiEndpoint_OverlapAllowedAcrossEndpoints)
{
    DlHcommApiGuard guard;
    static int regCount = 0;
    regCount = 0;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL + regCount);
        regCount++;
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    auto ep0 = reinterpret_cast<HcommEndpointHandle>(0xA001UL);
    auto ep1 = reinterpret_cast<HcommEndpointHandle>(0xA002UL);
    HcommMemHandleType h0 = nullptr;
    HcommMemHandleType h1 = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);

    // Same range with different tags on different endpoints → both allowed
    ASSERT_EQ(wrapper.RegisterMemory(ep0, 0xBEEF, mem, h0), BM_OK);
    ASSERT_EQ(wrapper.RegisterMemory(ep1, 0xCAFE, mem, h1), BM_OK);
    EXPECT_EQ(regCount, 2);

    // Same range / different tag on the SAME endpoint → still a conflict
    HcommMemHandleType h2 = nullptr;
    EXPECT_NE(wrapper.RegisterMemory(ep0, 0xCAFE, mem, h2), BM_OK);
}

TEST(HcommApiWrapperTest, UnregisterMemory_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle), BM_OK);

    auto ret = wrapper.UnregisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), handle);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, ExportMemory_NoCache)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };
    DlHcommApi::gHcommMemExport = [](EndpointHandle, HcommMemHandle, void **desc, uint32_t *descLen) -> int32_t {
        static uint8_t stubDesc[8] = {0xA5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        *desc = stubDesc;
        *descLen = sizeof(stubDesc);
        return 0;
    };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle), BM_OK);

    const uint8_t *desc = nullptr;
    uint32_t descLen = 0;
    auto ret = wrapper.ExportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), handle, desc, descLen);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(descLen, 8U);
}

TEST(HcommApiWrapperTest, ImportMemory_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t descLen, HcommCommMem *outMem) -> int32_t {
        outMem->type = COMM_MEM_TYPE_HOST;
        outMem->addr = reinterpret_cast<void *>(0xC001UL);
        outMem->size = 0x1000UL;
        return 0;
    };

    HcommApiWrapper wrapper;
    uint8_t dummyDesc[8] = {};
    HcommCommMem outMem{};
    auto ret =
        wrapper.ImportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), dummyDesc, sizeof(dummyDesc), outMem);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(outMem.type, COMM_MEM_TYPE_HOST);
}

TEST(HcommApiWrapperTest, ImportMemory_InvalidType)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t, HcommCommMem *outMem) -> int32_t {
        outMem->type = COMM_MEM_TYPE_INVALID;
        outMem->addr = nullptr;
        outMem->size = 0;
        return 0;
    };

    HcommApiWrapper wrapper;
    uint8_t dummyDesc[4] = {};
    HcommCommMem outMem{};
    auto ret =
        wrapper.ImportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), dummyDesc, sizeof(dummyDesc), outMem);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, UnimportMemory_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemUnimport = [](EndpointHandle, const void *, uint32_t) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    uint8_t dummyDesc[4] = {};
    auto ret = wrapper.UnimportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), dummyDesc, sizeof(dummyDesc));
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, CreateEndpoint_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = reinterpret_cast<EndpointHandle>(0xA001UL);
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    EndpointDesc desc{};
    HcommEndpointHandle handle = nullptr;
    auto ret = wrapper.CreateEndpoint(desc, handle);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_NE(handle, nullptr);
}

TEST(HcommApiWrapperTest, DestroyEndpoint_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = reinterpret_cast<EndpointHandle>(0xA001UL);
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    EndpointDesc desc{};
    HcommEndpointHandle handle = nullptr;
    ASSERT_EQ(wrapper.CreateEndpoint(desc, handle), BM_OK);
    ASSERT_EQ(wrapper.DestroyEndpoint(handle), BM_OK);
}

TEST(HcommApiWrapperTest, RawMemReg_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };

    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto ret = HcommApiWrapper::RawMemReg(reinterpret_cast<EndpointHandle>(0xA001UL), "test", mem, handle);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, AllocThread_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommThreadAlloc = [](CommEngine, uint32_t, const uint32_t *, ThreadHandle *thread) -> int32_t {
        *thread = 0xA004UL;
        return 0;
    };

    ThreadHandle thread = 0;
    auto ret = HcommApiWrapper::AllocThread(COMM_ENGINE_AICPU, 1, thread);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(thread, 0xA004UL);
}

TEST(HcommApiWrapperTest, FreeThread_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommThreadFree = [](const ThreadHandle *, uint32_t) -> int32_t { return 0; };

    ThreadHandle thread = 0xA004UL;
    auto ret = HcommApiWrapper::FreeThread(thread);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, CreateChannel_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelCreate = [](EndpointHandle, CommEngine, HcommChannelDesc *, uint32_t,
                                         ChannelHandle *ch) -> int32_t {
        *ch = 0xA003UL;
        return 0;
    };
    DlHcommApi::gHcommChannelDestroy = [](ChannelHandle *) -> int32_t { return 0; };

    HcommChannelDesc desc{};
    ChannelHandle channel = 0;
    auto ret = HcommApiWrapper::CreateChannel(reinterpret_cast<HcommEndpointHandle>(0xA001UL), COMM_ENGINE_AICPU, desc,
                                              channel);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(channel, 0xA003UL);
}

TEST(HcommApiWrapperTest, DestroyChannel_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelDestroy = [](ChannelHandle *) -> int32_t { return 0; };

    ChannelHandle channel = 0xA003UL;
    auto ret = HcommApiWrapper::DestroyChannel(channel);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, GetChannelStatus_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelGetStatus = [](ChannelHandle *, uint32_t, int32_t *status) -> int32_t {
        *status = 0;
        return 0;
    };

    ChannelHandle channel = 0xA003UL;
    int32_t status = -1;
    auto ret = HcommApiWrapper::GetChannelStatus(channel, status);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(status, 0);
}

TEST(HcommApiWrapperTest, WaitForChannelsReadyTimesOutWhenChannelStaysInProgress)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelGetStatus = [](ChannelHandle *, uint32_t, int32_t *status) -> int32_t {
        *status = 1; // HCOMM_CHANNEL_IN_PROGRESS
        return 0;
    };

    const ChannelHandle channel = 0xA003UL;
    const std::vector<HcommChannelReadyPeer> peerRanks{{channel, 3U}};
    const auto start = std::chrono::steady_clock::now();
    const auto ret = HcommApiWrapper::WaitForChannelsReady(peerRanks, std::chrono::milliseconds(5));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(ret, BM_TIMEOUT);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST(HcommApiWrapperTest, WaitForChannelsReadyPollsEveryPendingChannel)
{
    static constexpr ChannelHandle firstChannel = 0xA003UL;
    static constexpr ChannelHandle secondChannel = 0xA004UL;
    static constexpr int32_t channelReady = 0;
    static constexpr int32_t channelInProgress = 1;
    static constexpr int32_t channelFailed = 2;
    static bool secondChannelPolled = false;
    static uint32_t firstChannelPollCount = 0U;
    static uint32_t secondChannelPollCount = 0U;
    secondChannelPolled = false;
    firstChannelPollCount = 0U;
    secondChannelPollCount = 0U;

    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelGetStatus = [](const ChannelHandle *channels, uint32_t channelCount,
                                            int32_t *statuses) -> int32_t {
        EXPECT_EQ(channelCount, 1U);
        if (channels[0] == firstChannel) {
            ++firstChannelPollCount;
            statuses[0] = secondChannelPolled ? channelReady : channelInProgress;
        } else if (channels[0] == secondChannel) {
            ++secondChannelPollCount;
            secondChannelPolled = true;
            statuses[0] = channelReady;
        } else {
            ADD_FAILURE() << "unexpected channel: " << channels[0];
            statuses[0] = channelFailed;
        }
        return 0;
    };

    const std::vector<HcommChannelReadyPeer> peerRanks{{firstChannel, 1U}, {secondChannel, 2U}};
    EXPECT_EQ(HcommApiWrapper::WaitForChannelsReady(peerRanks, std::chrono::seconds(1)), BM_OK);
    EXPECT_EQ(firstChannelPollCount, 2U);
    EXPECT_EQ(secondChannelPollCount, 1U);
}

// ── DlAclApi wrapper tests ──

TEST(HcommApiWrapperTest, RegisterMemory_InvalidMem_ZeroAddr)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(0, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto ret = wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, RegisterMemory_InvalidMem_InvalidType)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_INVALID);
    auto ret = wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, ExportMemory_HandleNotFound)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    const uint8_t *desc = nullptr;
    uint32_t descLen = 0;
    auto ret = wrapper.ExportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL),
                                    reinterpret_cast<HcommMemHandleType>(0xDEADUL), desc, descLen);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, UnregisterMemory_NullEndpoint)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    auto ret = wrapper.UnregisterMemory(nullptr, reinterpret_cast<HcommMemHandleType>(0xB001UL));
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, UnregisterMemory_NullHandle)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    auto ret = wrapper.UnregisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), nullptr);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, RawMemExport_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemExport = [](EndpointHandle, HcommMemHandle, void **desc, uint32_t *descLen) -> int32_t {
        static uint8_t stubDesc[4] = {0xA1, 0xB2, 0xC3, 0xD4};
        *desc = stubDesc;
        *descLen = sizeof(stubDesc);
        return 0;
    };

    void *desc = nullptr;
    uint32_t descLen = 0;
    auto ret = HcommApiWrapper::RawMemExport(reinterpret_cast<HcommEndpointHandle>(0xA001UL),
                                             reinterpret_cast<HcommMemHandleType>(0xB001UL), desc, descLen);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(descLen, 4U);
}

TEST(HcommApiWrapperTest, RawMemUnreg_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto ret = HcommApiWrapper::RawMemUnreg(reinterpret_cast<HcommEndpointHandle>(0xA001UL),
                                            reinterpret_cast<HcommMemHandleType>(0xB001UL));
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, RegisterMemory_HcommMemRegFails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *, HcommMemHandle *) -> int32_t {
        return -1;
    };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    auto ret = wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, Overlaps_EntryStartAfterMemEnd)
{
    HcommApiWrapper wrapper;
    auto entry = MakeEntry(0x2000, 0x100, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(0x1000, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(wrapper.Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, Overlaps_EntryBeforeMem_NoOverlap)
{
    HcommApiWrapper wrapper;
    auto entry = MakeEntry(0x1000, 0x100, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(0x2000, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(wrapper.Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, SameMem_NullAddr)
{
    auto entry = MakeEntry(0, 0, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(0, 0, COMM_MEM_TYPE_HOST);
    EXPECT_TRUE(HcommApiWrapper::SameMem(entry, mem));
}

using namespace ock::mf::transport::device;

struct DlAclDeviceCommonGuard {
    aclrtGetDeviceFunc oldGetDevice{DlAclApi::pAclrtGetDevice};
    aclrtGetPhyDevIdByLogicDevIdFunc oldGetPhyDev{DlAclApi::pAclrtGetPhyDevIdByLogicDevId};
    rtGetDeviceInfoFunc oldGetDeviceInfo{DlAclApi::pRtGetDeviceInfo};
    aclrtFreeFunc oldFree{DlAclApi::pAclrtFree};

    ~DlAclDeviceCommonGuard()
    {
        DlAclApi::pAclrtGetDevice = oldGetDevice;
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = oldGetPhyDev;
        DlAclApi::pRtGetDeviceInfo = oldGetDeviceInfo;
        DlAclApi::pAclrtFree = oldFree;
    }
};

TEST(HcommApiWrapperTest, ReleaseDeviceTransferBuffers_NonNullDstList)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };

    DeviceTransferBuffers buffers{};
    void *dummyPtr = reinterpret_cast<void *>(0xD001UL);
    buffers.dstList = dummyPtr;
    buffers.srcList = dummyPtr;
    buffers.lenList = dummyPtr;

    EXPECT_NO_THROW(ReleaseDeviceTransferBuffers(buffers));
    EXPECT_EQ(buffers.dstList, nullptr);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_AclrtGetDeviceFails)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *deviceId) -> int32_t {
        *deviceId = -1;
        return -8;
    };

    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    auto ret = InitLocalDeviceInfo(phyId, sdid, serverId, superPodId);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, ReleaseDeviceTransferBuffers_NullDstListOnly)
{
    DeviceTransferBuffers buffers{};
    EXPECT_NO_THROW(ReleaseDeviceTransferBuffers(buffers));
    EXPECT_EQ(buffers.dstList, nullptr);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_GetPhyDevFails)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *) -> int32_t { return -1; };

    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    auto ret = InitLocalDeviceInfo(phyId, sdid, serverId, superPodId);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_QuerySdidFails)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *phyId) -> int32_t {
        *phyId = 0;
        return 0;
    };
    DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t infoType, int64_t *val) -> int32_t {
        if (infoType == INFO_TYPE_SDID)
            return -1;
        *val = 0;
        return 0;
    };

    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    auto ret = InitLocalDeviceInfo(phyId, sdid, serverId, superPodId);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_AllSuccess)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *phyId) -> int32_t {
        *phyId = 2;
        return 0;
    };
    DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t, int64_t *val) -> int32_t {
        *val = 3;
        return 0;
    };

    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    auto ret = InitLocalDeviceInfo(phyId, sdid, serverId, superPodId);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(phyId, 2U);
    ASSERT_EQ(sdid, 3U);
}

// ── LaunchDeviceKernel / PrepareLaunchBuffer tests ──
// These cover ~110 critical lines in device_kernel_helper.h

struct DlAclKernelGuard {
    aclrtMallocFunc oldMalloc{DlAclApi::pAclrtMalloc};
    aclrtFreeFunc oldFree{DlAclApi::pAclrtFree};
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    aclrtKernelArgsInitFunc oldArgsInit{DlAclApi::pAclrtKernelArgsInit};
    aclrtKernelArgsAppendFunc oldArgsAppend{DlAclApi::pAclrtKernelArgsAppend};
    aclrtKernelArgsFinalizeFunc oldArgsFinalize{DlAclApi::pAclrtKernelArgsFinalize};
    aclrtLaunchKernelWithConfigFunc oldLaunch{DlAclApi::pAclrtLaunchKernelWithConfig};

    ~DlAclKernelGuard()
    {
        DlAclApi::pAclrtMalloc = oldMalloc;
        DlAclApi::pAclrtFree = oldFree;
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlAclApi::pAclrtKernelArgsInit = oldArgsInit;
        DlAclApi::pAclrtKernelArgsAppend = oldArgsAppend;
        DlAclApi::pAclrtKernelArgsFinalize = oldArgsFinalize;
        DlAclApi::pAclrtLaunchKernelWithConfig = oldLaunch;
    }
};

static void *gMockDevPtr = reinterpret_cast<void *>(0xE0000000UL);
static uint32_t gMallocSize = 0;
static int32_t MockAclrtMalloc(void **ptr, size_t size, uint32_t)
{
    *ptr = gMockDevPtr;
    gMallocSize = static_cast<uint32_t>(size);
    return 0;
}

TEST(HcommApiWrapperTest, PrepareLaunchBuffer_Success)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };

    KernelLaunchConfig config{};
    config.batchSize = 2;
    config.isRead = false;
    uint64_t localAddrs[2] = {0x1000, 0x2000};
    uint64_t remoteAddrs[2] = {0xA000, 0xB000};
    uint64_t sizes[2] = {1024, 2048};
    config.localAddrs = localAddrs;
    config.remoteAddrs = remoteAddrs;
    config.sizes = sizes;

    void *dstDev = nullptr;
    auto ret = PrepareLaunchBuffer(config, dstDev);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(dstDev, gMockDevPtr);
    // Should allocate: ptrBytes*2 + batchSize*8 = 16*2 + 16 = 48
    ASSERT_EQ(gMallocSize, 48U);
}

TEST(HcommApiWrapperTest, PrepareLaunchBuffer_MallocFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = [](void **, size_t, uint32_t) -> int32_t { return -1; };

    KernelLaunchConfig config{};
    config.batchSize = 1;
    uint64_t localAddrs[1] = {0x1000};
    uint64_t remoteAddrs[1] = {0xA000};
    uint64_t sizes[1] = {1024};
    config.localAddrs = localAddrs;
    config.remoteAddrs = remoteAddrs;
    config.sizes = sizes;

    void *dstDev = nullptr;
    auto ret = PrepareLaunchBuffer(config, dstDev);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, PrepareLaunchBuffer_MemcpyFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return -1; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };

    KernelLaunchConfig config{};
    config.batchSize = 1;
    uint64_t localAddrs[1] = {0x1000};
    uint64_t remoteAddrs[1] = {0xA000};
    uint64_t sizes[1] = {1024};
    config.localAddrs = localAddrs;
    config.remoteAddrs = remoteAddrs;
    config.sizes = sizes;

    void *dstDev = nullptr;
    auto ret = PrepareLaunchBuffer(config, dstDev);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_BatchSizeZero)
{
    KernelLaunchConfig config{};
    config.batchSize = 0;
    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_NullAddrs)
{
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = nullptr;
    config.remoteAddrs = nullptr;
    config.sizes = nullptr;
    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_NullFuncHandle)
{
    DlAclKernelGuard guard;
    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = nullptr;
    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_NullStream)
{
    DlAclKernelGuard guard;
    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = nullptr;
    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_ArgsInitFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsInit = [](aclrtFuncHandle, aclrtArgsHandle *) -> int32_t { return -1; };

    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = reinterpret_cast<void *>(0xE001UL);
    config.thread = 0xA004UL;
    config.channel = 0xA003UL;

    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_Success)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsInit = [](aclrtFuncHandle, aclrtArgsHandle *h) -> int32_t {
        *h = nullptr;
        return 0;
    };
    DlAclApi::pAclrtKernelArgsAppend = [](aclrtArgsHandle, void *, size_t, aclrtParamHandle *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsFinalize = [](aclrtArgsHandle) -> int32_t { return 0; };
    DlAclApi::pAclrtLaunchKernelWithConfig = [](aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *,
                                                aclrtArgsHandle, void *) -> int32_t { return 0; };

    uint64_t addrs[2] = {0x1000, 0x2000};
    KernelLaunchConfig config{};
    config.batchSize = 2;
    config.isRead = true;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = reinterpret_cast<void *>(0xE001UL);
    config.thread = 0xA004UL;
    config.channel = 0xA003UL;

    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_NE(buffers.dstList, nullptr);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_LaunchFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsInit = [](aclrtFuncHandle, aclrtArgsHandle *h) -> int32_t {
        *h = nullptr;
        return 0;
    };
    DlAclApi::pAclrtKernelArgsAppend = [](aclrtArgsHandle, void *, size_t, aclrtParamHandle *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsFinalize = [](aclrtArgsHandle) -> int32_t { return 0; };
    DlAclApi::pAclrtLaunchKernelWithConfig = [](aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *,
                                                aclrtArgsHandle, void *) -> int32_t { return -1; };

    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = reinterpret_cast<void *>(0xE001UL);

    DeviceTransferBuffers buffers{};
    auto ret = LaunchDeviceKernel(config, buffers);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, ExportMemory_NullEndpoint)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    const uint8_t *desc = nullptr;
    uint32_t descLen = 0;
    auto ret = wrapper.ExportMemory(nullptr, reinterpret_cast<HcommMemHandleType>(0xB001UL), desc, descLen);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, ExportMemory_NullHandle)
{
    DlHcommApiGuard guard;
    HcommApiWrapper wrapper;
    const uint8_t *desc = nullptr;
    uint32_t descLen = 0;
    auto ret = wrapper.ExportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), nullptr, desc, descLen);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, GetRangeEnd_MaxAddrNoOverflow)
{
    uint64_t end = 0;
    EXPECT_TRUE(HcommApiWrapper::GetRangeEnd(std::numeric_limits<uint64_t>::max() - 100, 100, end));
    EXPECT_EQ(end, std::numeric_limits<uint64_t>::max());
}

TEST(HcommApiWrapperTest, IsValidMem_NullAddr)
{
    auto mem = MakeCommMem(0, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(HcommApiWrapper::IsValidMem(mem));
}

TEST(HcommApiWrapperTest, Overlaps_EntryEndEqMemStart_NoOverlap)
{
    HcommApiWrapper wrapper;
    auto entry = MakeEntry(0x1000, 0x200, COMM_MEM_TYPE_HOST);
    auto mem = MakeCommMem(0x1200, 0x100, COMM_MEM_TYPE_HOST);
    EXPECT_FALSE(wrapper.Overlaps(entry, mem));
}

TEST(HcommApiWrapperTest, RawMemImport_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemImport = [](EndpointHandle, const void *, uint32_t, HcommCommMem *outMem) -> int32_t {
        outMem->type = COMM_MEM_TYPE_DEVICE;
        outMem->addr = reinterpret_cast<void *>(0xC000UL);
        outMem->size = 0x800UL;
        return 0;
    };
    HcommCommMem outMem{};
    uint8_t dummy[4] = {};
    auto ret = HcommApiWrapper::RawMemImport(reinterpret_cast<EndpointHandle>(0xA001UL), dummy, 4, outMem);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, RawMemUnimport_Success)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemUnimport = [](EndpointHandle, const void *, uint32_t) -> int32_t { return 0; };
    uint8_t dummy[4] = {};
    auto ret = HcommApiWrapper::RawMemUnimport(reinterpret_cast<EndpointHandle>(0xA001UL), dummy, 4);
    ASSERT_EQ(ret, BM_OK);
}

} // namespace

TEST(HcommApiWrapperTest, UnloadHcomm_NoCrash)
{
    DlHcommApiGuard guard;
    EXPECT_NO_THROW(HcommApiWrapper::UnloadHcomm());
}

TEST(HcommApiWrapperTest, DestroyEndpoint_Fails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return -1; };
    HcommApiWrapper wrapper;
    auto ret = wrapper.DestroyEndpoint(reinterpret_cast<HcommEndpointHandle>(0xA001UL));
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, DestroyEndpoint_NullHandle)
{
    HcommApiWrapper wrapper;
    auto ret = wrapper.DestroyEndpoint(nullptr);
    ASSERT_EQ(ret, BM_OK);
}

TEST(HcommApiWrapperTest, CreateEndpoint_Fails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *) -> int32_t { return -1; };
    HcommApiWrapper wrapper;
    EndpointDesc desc{};
    HcommEndpointHandle handle = nullptr;
    auto ret = wrapper.CreateEndpoint(desc, handle);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, AllocThread_Fails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommThreadAlloc = [](CommEngine, uint32_t, const uint32_t *, ThreadHandle *) -> int32_t {
        return -1;
    };
    ThreadHandle thread = 0;
    auto ret = HcommApiWrapper::AllocThread(COMM_ENGINE_AICPU, 1, thread);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, CreateChannel_Fails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelCreate = [](EndpointHandle, CommEngine, HcommChannelDesc *, uint32_t,
                                         ChannelHandle *) -> int32_t { return -1; };
    HcommChannelDesc desc{};
    ChannelHandle channel = 0;
    auto ret = HcommApiWrapper::CreateChannel(reinterpret_cast<HcommEndpointHandle>(0xA001UL), COMM_ENGINE_AICPU, desc,
                                              channel);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, GetChannelStatus_Fails)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommChannelGetStatus = [](ChannelHandle *, uint32_t, int32_t *) -> int32_t { return -1; };
    ChannelHandle channel = 0xA003UL;
    int32_t status = 0;
    auto ret = HcommApiWrapper::GetChannelStatus(channel, status);
    ASSERT_NE(ret, BM_OK);
}

TEST(HcommApiWrapperTest, RegisterMemory_RollbackOnDupTag)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    // Register two different mems with SAME tag → second one's emplace fails → rollback
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle), BM_OK);
    auto mem2 = MakeCommMem(MOCK_ADDR_1 + 0x1000, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    // Same tag with different addr → SameMem fails → BM_ERROR
    ASSERT_NE(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem2, handle), BM_OK);
}

TEST(HcommApiWrapperTest, ExportMemory_CacheHit)
{
    DlHcommApiGuard guard;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };
    DlHcommApi::gHcommMemExport = [](EndpointHandle, HcommMemHandle, void **desc, uint32_t *descLen) -> int32_t {
        static uint8_t stubDesc[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        *desc = stubDesc;
        *descLen = sizeof(stubDesc);
        return 0;
    };

    HcommApiWrapper wrapper;
    HcommMemHandleType handle = nullptr;
    auto mem = MakeCommMem(MOCK_ADDR_1, MOCK_SIZE, COMM_MEM_TYPE_HOST);
    ASSERT_EQ(wrapper.RegisterMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), 0xBEEF, mem, handle), BM_OK);

    const uint8_t *desc1 = nullptr;
    uint32_t len1 = 0;
    ASSERT_EQ(wrapper.ExportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), handle, desc1, len1), BM_OK);

    // Second export should hit cache
    const uint8_t *desc2 = nullptr;
    uint32_t len2 = 0;
    ASSERT_EQ(wrapper.ExportMemory(reinterpret_cast<HcommEndpointHandle>(0xA001UL), handle, desc2, len2), BM_OK);
    ASSERT_EQ(len1, len2);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_ArgsAppendFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsInit = [](aclrtFuncHandle, aclrtArgsHandle *h) -> int32_t {
        *h = nullptr;
        return 0;
    };
    DlAclApi::pAclrtKernelArgsAppend = [](aclrtArgsHandle, void *, size_t, aclrtParamHandle *) -> int32_t {
        return -1;
    };

    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = reinterpret_cast<void *>(0xE001UL);
    DeviceTransferBuffers buffers{};
    ASSERT_NE(LaunchDeviceKernel(config, buffers), BM_OK);
}

TEST(HcommApiWrapperTest, LaunchDeviceKernel_ArgsFinalizeFails)
{
    DlAclKernelGuard guard;
    DlAclApi::pAclrtMalloc = MockAclrtMalloc;
    DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
    DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsInit = [](aclrtFuncHandle, aclrtArgsHandle *h) -> int32_t {
        *h = nullptr;
        return 0;
    };
    DlAclApi::pAclrtKernelArgsAppend = [](aclrtArgsHandle, void *, size_t, aclrtParamHandle *) -> int32_t { return 0; };
    DlAclApi::pAclrtKernelArgsFinalize = [](aclrtArgsHandle) -> int32_t { return -1; };

    uint64_t addrs[1] = {0x1000};
    KernelLaunchConfig config{};
    config.batchSize = 1;
    config.localAddrs = addrs;
    config.remoteAddrs = addrs;
    config.sizes = addrs;
    config.funcHandle = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    config.stream = reinterpret_cast<void *>(0xE001UL);
    DeviceTransferBuffers buffers{};
    ASSERT_NE(LaunchDeviceKernel(config, buffers), BM_OK);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_ServerIdFails)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *phyId) -> int32_t {
        *phyId = 0;
        return 0;
    };
    DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t infoType, int64_t *val) -> int32_t {
        if (infoType == INFO_TYPE_SERVER_ID)
            return -1;
        *val = 0;
        return 0;
    };
    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    ASSERT_NE(InitLocalDeviceInfo(phyId, sdid, serverId, superPodId), BM_OK);
}

TEST(HcommApiWrapperTest, InitLocalDeviceInfo_SuperPodFails)
{
    DlAclDeviceCommonGuard guard;
    DlAclApi::pAclrtGetDevice = [](int32_t *id) -> int32_t {
        *id = 0;
        return 0;
    };
    DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *phyId) -> int32_t {
        *phyId = 0;
        return 0;
    };
    DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t infoType, int64_t *val) -> int32_t {
        if (infoType == INFO_TYPE_SUPER_POD_ID)
            return -1;
        *val = 0;
        return 0;
    };
    uint32_t phyId = 0, sdid = 0, serverId = 0, superPodId = 0;
    ASSERT_NE(InitLocalDeviceInfo(phyId, sdid, serverId, superPodId), BM_OK);
}
