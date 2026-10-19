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
#include <cstdint>

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

#define private public
#include "smem_bm_entry.h"
#undef private

#include "hybm_big_mem.h"
#include "hybm_data_op.h"
#include "smem_config_store.h"

using namespace ock::smem;

namespace {
class TestConfigStoreManager : public ConfigStoreManager {
public:
    void RegisterReconnectHandler(ConfigStoreReconnectHandler) noexcept override {}
    ock::smem::Result ReConnectAfterBroken(int) noexcept override
    {
        return ock::smem::SM_OK;
    }
    bool GetConnectStatus() noexcept override
    {
        return true;
    }
    void SetConnectStatus(bool) noexcept override {}
    void RegisterClientBrokenHandler(const ConfigStoreClientBrokenHandler &) noexcept override {}
    void RegisterServerBrokenHandler(const ConfigStoreServerBrokenHandler &) noexcept override {}
    ock::smem::Result Set(const std::string &, const std::vector<uint8_t> &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Add(const std::string &, int64_t, int64_t &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Remove(const std::string &, bool) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result QueryAlive(uint32_t, uint32_t &alive) noexcept override
    {
        alive = true;
        return ock::smem::SM_OK;
    }
    ock::smem::Result PrefixGet(const std::string &, std::unordered_map<std::string, std::string> &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Append(const std::string &, const std::vector<uint8_t> &, uint64_t &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Cas(const std::string &, const std::vector<uint8_t> &, const std::vector<uint8_t> &,
                          std::vector<uint8_t> &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Watch(const std::string &,
                            const std::function<void(int, const std::string &, const std::vector<uint8_t> &)> &,
                            uint32_t &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Watch(WatchRankType, const std::function<void(WatchRankType, uint32_t, ock::smem::Result)> &,
                            uint32_t &) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Unwatch(uint32_t) noexcept override
    {
        return ock::smem::SM_OK;
    }
    ock::smem::Result Write(const std::string &, const std::vector<uint8_t> &, uint32_t) noexcept override
    {
        return ock::smem::SM_OK;
    }
    std::string GetCompleteKey(const std::string &key) noexcept override
    {
        return key;
    }
    std::string GetCommonPrefix() noexcept override
    {
        return "";
    }
    SmRef<ConfigStore> GetCoreStore() noexcept override
    {
        return SmRef<ConfigStore>(this);
    }

protected:
    ock::smem::Result GetReal(const std::string &, std::vector<uint8_t> &, int64_t) noexcept override
    {
        return ock::smem::SM_OK;
    }
};
} // namespace

using namespace ock::smem;

static constexpr uint64_t MB = 0x100000ULL;
static constexpr uint64_t GB = 0x40000000ULL;
static constexpr uint32_t TEST_ENTRY_ID = 1;
static constexpr uint32_t TEST_RANK_ID = 0;
static constexpr uint32_t TEST_RANK_COUNT = 4;
static const hybm_entity_t TEST_ENTITY_PTR = reinterpret_cast<hybm_entity_t>(0x1234);
static const hybm_mem_slice_t TEST_SLICE_PTR = reinterpret_cast<hybm_mem_slice_t>(0x5678);
static constexpr uint64_t TEST_DATA_SIZE = 1024;
static constexpr uint64_t TEST_DRAM_SIZE_PER_RANK = 2 * GB;
static constexpr uint64_t TEST_HBM_SIZE_PER_RANK = 1 * GB;

class SmemBmEntryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        SmemBmEntryOptions opts{};
        opts.id = TEST_ENTRY_ID;
        opts.rank = TEST_RANK_ID;
        opts.rankSize = TEST_RANK_COUNT;
        entry_ = std::make_unique<SmemBmEntry>(opts, StorePtr{});

        // Set up memory ranges for tests
        entry_->hostGva_ = reinterpret_cast<void *>(HOST_GVA_BASE);
        entry_->deviceGva_ = reinterpret_cast<void *>(DEVICE_GVA_BASE);
        entry_->coreOptions_.maxDRAMSize = TEST_DRAM_SIZE_PER_RANK;
        entry_->coreOptions_.maxHBMSize = TEST_HBM_SIZE_PER_RANK;
        entry_->coreOptions_.rankCount = TEST_RANK_COUNT;
    }

    void TearDown() override
    {
        entry_->entity_ = nullptr;
        entry_.reset();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }

    std::unique_ptr<SmemBmEntry> entry_;

    static constexpr uint64_t HOST_GVA_BASE = 0x1000000000ULL;
    static constexpr uint64_t DEVICE_GVA_BASE = 0x2000000000ULL;
};

// ======================== AddrInHostGva Tests ========================

TEST_F(SmemBmEntryTest, AddrInHostGva_NullHostGva_ReturnsFalse)
{
    entry_->hostGva_ = nullptr;
    EXPECT_FALSE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE), 1));
}

TEST_F(SmemBmEntryTest, AddrInHostGva_AddressAtStart_ReturnsTrue)
{
    EXPECT_TRUE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE), 1));
}

TEST_F(SmemBmEntryTest, AddrInHostGva_AddressInMiddle_ReturnsTrue)
{
    EXPECT_TRUE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE + GB), 4096)); // 4096
}

TEST_F(SmemBmEntryTest, AddrInHostGva_AddressAtEnd_ReturnsTrue)
{
    auto addr = reinterpret_cast<void *>(HOST_GVA_BASE + 8 * GB - 1); // 8
    EXPECT_TRUE(entry_->AddrInHostGva(addr, 1));
}

TEST_F(SmemBmEntryTest, AddrInHostGva_AddressBeforeRange_ReturnsFalse)
{
    EXPECT_FALSE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE - 1), 1));
}

TEST_F(SmemBmEntryTest, AddrInHostGva_SizeExceedsRange_ReturnsFalse)
{
    auto addr = reinterpret_cast<void *>(HOST_GVA_BASE);
    // Size extends beyond the end
    EXPECT_FALSE(entry_->AddrInHostGva(addr, 8 * GB + 1)); // 8
}

TEST_F(SmemBmEntryTest, AddrInHostGva_AddressAfterRange_ReturnsFalse)
{
    EXPECT_FALSE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE + 8 * GB), 1)); // 8
}

TEST_F(SmemBmEntryTest, AddrInHostGva_ZeroSize_ReturnsTrue)
{
    EXPECT_TRUE(entry_->AddrInHostGva(reinterpret_cast<void *>(HOST_GVA_BASE), 0));
}

// ======================== AddrInDeviceGva Tests ========================

TEST_F(SmemBmEntryTest, AddrInDeviceGva_NullDeviceGva_ReturnsFalse)
{
    entry_->deviceGva_ = nullptr;
    EXPECT_FALSE(entry_->AddrInDeviceGva(reinterpret_cast<void *>(DEVICE_GVA_BASE), 1));
}

TEST_F(SmemBmEntryTest, AddrInDeviceGva_AddressAtStart_ReturnsTrue)
{
    EXPECT_TRUE(entry_->AddrInDeviceGva(reinterpret_cast<void *>(DEVICE_GVA_BASE), 1));
}

TEST_F(SmemBmEntryTest, AddrInDeviceGva_AddressInMiddle_ReturnsTrue)
{
    EXPECT_TRUE(entry_->AddrInDeviceGva(reinterpret_cast<void *>(DEVICE_GVA_BASE + 512 * MB), 4096)); // 512 4096
}

TEST_F(SmemBmEntryTest, AddrInDeviceGva_AddressBeforeRange_ReturnsFalse)
{
    EXPECT_FALSE(entry_->AddrInDeviceGva(reinterpret_cast<void *>(DEVICE_GVA_BASE - 1), 1));
}

TEST_F(SmemBmEntryTest, AddrInDeviceGva_SizeExceedsRange_ReturnsFalse)
{
    auto addr = reinterpret_cast<void *>(DEVICE_GVA_BASE);
    EXPECT_FALSE(entry_->AddrInDeviceGva(addr, 4 * GB + 1)); // 4
}

TEST_F(SmemBmEntryTest, AddrInDeviceGva_AddressAfterRange_ReturnsFalse)
{
    EXPECT_FALSE(entry_->AddrInDeviceGva(reinterpret_cast<void *>(DEVICE_GVA_BASE + 4 * GB), 1)); // 4
}

// ======================== GetHybmMemTypeFromGva Tests ========================

TEST_F(SmemBmEntryTest, GetHybmMemTypeFromGva_HostAddress_ReturnsHost)
{
    EXPECT_EQ(entry_->GetHybmMemTypeFromGva(reinterpret_cast<void *>(HOST_GVA_BASE), 1), SMEM_MEM_TYPE_HOST);
}

TEST_F(SmemBmEntryTest, GetHybmMemTypeFromGva_DeviceAddress_ReturnsDevice)
{
    EXPECT_EQ(entry_->GetHybmMemTypeFromGva(reinterpret_cast<void *>(DEVICE_GVA_BASE), 1), SMEM_MEM_TYPE_DEVICE);
}

TEST_F(SmemBmEntryTest, GetHybmMemTypeFromGva_UnknownAddress_ReturnsButt)
{
    EXPECT_EQ(entry_->GetHybmMemTypeFromGva(reinterpret_cast<void *>(0xDEAD0000), 1), SMEM_MEM_TYPE_BUTT);
}

TEST_F(SmemBmEntryTest, GetHybmMemTypeFromGva_NullHost_ReturnsButt)
{
    entry_->hostGva_ = nullptr;
    entry_->deviceGva_ = nullptr;
    EXPECT_EQ(entry_->GetHybmMemTypeFromGva(reinterpret_cast<void *>(HOST_GVA_BASE), 1), SMEM_MEM_TYPE_BUTT);
}

// ======================== GetRankIdByGva Tests ========================

TEST_F(SmemBmEntryTest, GetRankIdByGva_HostGva_ReturnsCorrectRank)
{
    // Host GVA is split by maxDRAMSize per rank (2GB per rank)
    auto rank1Addr = reinterpret_cast<void *>(HOST_GVA_BASE + 2 * GB); // 2
    EXPECT_EQ(entry_->GetRankIdByGva(rank1Addr), 1U);

    auto rank3Addr = reinterpret_cast<void *>(HOST_GVA_BASE + 6 * GB); // 6
    EXPECT_EQ(entry_->GetRankIdByGva(rank3Addr), 3U);
}

TEST_F(SmemBmEntryTest, GetRankIdByGva_DeviceGva_ReturnsCorrectRank)
{
    // Device GVA is split by maxHBMSize per rank (1GB per rank)
    auto rank2Addr = reinterpret_cast<void *>(DEVICE_GVA_BASE + 2 * GB); // 2
    EXPECT_EQ(entry_->GetRankIdByGva(rank2Addr), 2U);

    auto rank0Addr = reinterpret_cast<void *>(DEVICE_GVA_BASE);
    EXPECT_EQ(entry_->GetRankIdByGva(rank0Addr), 0U);
}

TEST_F(SmemBmEntryTest, GetRankIdByGva_UnknownGva_ReturnsUint32Max)
{
    EXPECT_EQ(entry_->GetRankIdByGva(reinterpret_cast<void *>(0xDEAD0000)), UINT32_MAX);
}

// ======================== TransToHybmDirection Tests ========================

TEST_F(SmemBmEntryTest, TransToHybmDirection_H2G_DeviceToGlobalHost)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_H2G, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_G2L_HostToDevice)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_G2L, reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(HOST_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_G2G_GlobalToGlobal)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_G2G, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_L2G_DeviceToGlobalDevice)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_L2G, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_G2H_GlobalToLocalHost)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_G2H, reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(HOST_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_L2GH_DeviceToGlobalHost)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_L2GH, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(HOST_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_GH2L_GlobalHostToDevice)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_GH2L, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(HOST_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_GH2H_GlobalHostToLocalHost)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_GH2H, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_H2GH_LocalHostToGlobalHost)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_H2GH, reinterpret_cast<void *>(HOST_GVA_BASE), 1024,
                                               reinterpret_cast<void *>(DEVICE_GVA_BASE), 1024);
    EXPECT_NE(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

TEST_F(SmemBmEntryTest, TransToHybmDirection_UnrecognizedAddress_ReturnsButt)
{
    auto direct = entry_->TransToHybmDirection(SMEMB_COPY_G2G, reinterpret_cast<void *>(0xDEAD0000), 1024,
                                               reinterpret_cast<void *>(0xBEEF0000), 1024);
    EXPECT_EQ(direct, HYBM_DATA_COPY_DIRECTION_BUTT);
}

// ======================== Id/RankId Accessors Tests ========================

TEST_F(SmemBmEntryTest, Id_ReturnsConfiguredId)
{
    EXPECT_EQ(entry_->Id(), 1U);
}

TEST_F(SmemBmEntryTest, GetCoreOptions_ReturnsConfiguredOptions)
{
    EXPECT_EQ(entry_->GetCoreOptions().rankCount, 4U);
    EXPECT_EQ(entry_->GetCoreOptions().maxDRAMSize, 2 * GB); // 2
}

TEST_F(SmemBmEntryTest, GetGvaAddress_ReturnsDeviceGva)
{
    EXPECT_EQ(entry_->GetGvaAddress(), entry_->deviceGva_);
}

TEST_F(SmemBmEntryTest, GetHostGvaAddress_ReturnsHostGva)
{
    EXPECT_EQ(entry_->GetHostGvaAddress(), entry_->hostGva_);
}

TEST_F(SmemBmEntryTest, GetDeviceGvaAddress_ReturnsDeviceGva)
{
    EXPECT_EQ(entry_->GetDeviceGvaAddress(), entry_->deviceGva_);
}

// ======================== DataCopy Validation Tests ========================

TEST_F(SmemBmEntryTest, DataCopy_NullSrc_ReturnsInvalidParam)
{
    auto ret = entry_->DataCopy(nullptr, reinterpret_cast<void *>(0x1000), 1024, SMEMB_COPY_G2G, nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopy_NullDest_ReturnsInvalidParam)
{
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(0x1000), nullptr, 1024, SMEMB_COPY_G2G, nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopy_ZeroSize_ReturnsInvalidParam)
{
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(0x1000), reinterpret_cast<void *>(0x2000), 0, SMEMB_COPY_G2G,
                                nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopy_InvalidType_ReturnsInvalidParam)
{
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(0x1000), reinterpret_cast<void *>(0x2000), 1024,
                                SMEMB_COPY_BUTT, nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopy_NotInited_ReturnsNotInitialized)
{
    entry_->inited_ = false;
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(0x1000), reinterpret_cast<void *>(0x2000), 1024,
                                SMEMB_COPY_G2G, nullptr, 0);
    EXPECT_EQ(ret, SM_NOT_INITIALIZED);
}

TEST_F(SmemBmEntryTest, DataCopy_NotJoined_ReturnsNotStarted)
{
    entry_->inited_ = true;
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(HOST_GVA_BASE),
                                reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000), 1024, SMEMB_COPY_G2G, nullptr, 0);
    EXPECT_EQ(ret, SM_NOT_STARTED);
}

// ======================== DataCopyBatch Validation Tests ========================

TEST_F(SmemBmEntryTest, DataCopyBatch_NullSources_ReturnsInvalidParam)
{
    smem_batch_copy_params params{};
    params.sources = nullptr;
    params.destinations = reinterpret_cast<void **>(0x2000);
    params.batchSize = 1;
    params.dataSizes = reinterpret_cast<uint64_t *>(0x3000);
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_G2G, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_NullDestinations_ReturnsInvalidParam)
{
    smem_batch_copy_params params{};
    params.sources = reinterpret_cast<void **>(0x1000);
    params.destinations = nullptr;
    params.batchSize = 1;
    params.dataSizes = reinterpret_cast<uint64_t *>(0x3000);
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_G2G, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_ZeroBatchSize_ReturnsInvalidParam)
{
    smem_batch_copy_params params{};
    params.sources = reinterpret_cast<void **>(0x1000);
    params.destinations = reinterpret_cast<void **>(0x2000);
    params.batchSize = 0; // 0
    params.dataSizes = reinterpret_cast<uint64_t *>(0x3000);
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_G2G, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_NullDataSizes_ReturnsInvalidParam)
{
    smem_batch_copy_params params{};
    params.sources = reinterpret_cast<void **>(0x1000);
    params.destinations = reinterpret_cast<void **>(0x2000);
    params.batchSize = 1;
    params.dataSizes = nullptr;
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_G2G, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_InvalidType_ReturnsInvalidParam)
{
    smem_batch_copy_params params{};
    params.sources = reinterpret_cast<void **>(0x1000);
    params.destinations = reinterpret_cast<void **>(0x2000);
    params.batchSize = 1;
    params.dataSizes = reinterpret_cast<uint64_t *>(0x3000);
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_BUTT, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_NotInited_ReturnsNotInitialized)
{
    smem_batch_copy_params params{};
    params.sources = reinterpret_cast<void **>(0x1000);
    params.destinations = reinterpret_cast<void **>(0x2000);
    params.batchSize = 1;
    params.dataSizes = reinterpret_cast<uint64_t *>(0x3000);
    entry_->inited_ = false;
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_G2G, 0);
    EXPECT_EQ(ret, SM_NOT_INITIALIZED);
}

// ======================== ExtendLocalMem Validation Tests ========================

TEST_F(SmemBmEntryTest, ExtendLocalMem_InvalidMemType_ReturnsInvalidParam)
{
    entry_->inited_ = true;
    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_BUTT, 4096);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, ExtendLocalMem_ZeroSize_ReturnsInvalidParam)
{
    entry_->inited_ = true;
    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, ExtendLocalMem_NotInited_ReturnsNotInitialized)
{
    entry_->inited_ = false;
    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, 4096);
    EXPECT_NE(ret, SM_OK);
}

// ======================== RegisterMem / UnRegisterMem Tests ========================

TEST_F(SmemBmEntryTest, RegisterMem_DuplicateAddr_ReturnsOk)
{
    entry_->inited_ = true;
    // Pre-populate registedSlice_ to test duplicate detection (no hybm call needed)
    auto dummySlice = reinterpret_cast<hybm_mem_slice_t>(0x1234ULL);
    entry_->registedSlice_[0x1000] = std::make_pair(4096ULL, dummySlice);
    auto ret = entry_->RegisterMem(0x1000, 4096);
    EXPECT_EQ(ret, SM_OK); // Duplicate returns SM_OK with warning
}

TEST_F(SmemBmEntryTest, UnRegisterMem_AddrNotFound_ReturnsOk)
{
    entry_->inited_ = true;
    auto ret = entry_->UnRegisterMem(0xDEAD0000);
    EXPECT_EQ(ret, SM_OK); // Not found returns SM_OK
}

TEST_F(SmemBmEntryTest, RegisterMem_NotInited_ReturnsError)
{
    entry_->inited_ = false;
    auto ret = entry_->RegisterMem(0x1000, 4096);
    EXPECT_NE(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, UnRegisterMem_NotInited_ReturnsError)
{
    entry_->inited_ = false;
    auto ret = entry_->UnRegisterMem(0x1000);
    EXPECT_NE(ret, SM_OK);
}

// ======================== Wait / CheckJoined Tests ========================

TEST_F(SmemBmEntryTest, Wait_NotInited_ReturnsNotInitialized)
{
    entry_->inited_ = false;
    auto ret = entry_->Wait();
    EXPECT_EQ(ret, SM_NOT_INITIALIZED);
}

TEST_F(SmemBmEntryTest, CheckJoined_NullGroup_ReturnsNotStarted)
{
    auto ret = entry_->CheckJoined();
    EXPECT_EQ(ret, SM_NOT_STARTED);
}

// ======================== Mock-based Full Path Tests ========================

TEST_F(SmemBmEntryTest, Wait_Success_ReturnsOk)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_wait).stubs().will(returnValue(0));
    auto ret = entry_->Wait();
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, RegisterMem_Success_ReturnsOk)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_register_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    uint64_t testAddr = 0x10000;
    uint64_t testSize = 4096;
    auto ret = entry_->RegisterMem(testAddr, testSize);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, UnRegisterMem_Success_ReturnsOk)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    entry_->registedSlice_[0x20000] = std::make_pair(8192ULL, TEST_SLICE_PTR);
    MOCKER(hybm_free_local_memory).stubs().will(returnValue(0));
    auto ret = entry_->UnRegisterMem(0x20000);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, UnRegisterMem_FreeFail_ReturnsError)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    entry_->registedSlice_[0x20000] = std::make_pair(8192ULL, TEST_SLICE_PTR);
    MOCKER(hybm_free_local_memory).stubs().will(returnValue(-1));
    auto ret = entry_->UnRegisterMem(0x20000);
    EXPECT_NE(ret, SM_OK);
}

// ======================== DataCopy Full Path Tests (function-level mock) ========================

TEST_F(SmemBmEntryTest, DataCopy_FullPath_Success)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    MOCKER(hybm_data_copy).stubs().will(returnValue(0));
    auto ret =
        entry_->DataCopy(reinterpret_cast<void *>(HOST_GVA_BASE), reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000),
                         TEST_DATA_SIZE, SMEMB_COPY_H2GH, nullptr, 0);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, DataCopy_WithExternalStream_Success)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    MOCKER(hybm_data_copy).stubs().will(returnValue(0));
    void *stream = reinterpret_cast<void *>(0xDEAD);
    auto ret =
        entry_->DataCopy(reinterpret_cast<void *>(HOST_GVA_BASE), reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000),
                         TEST_DATA_SIZE, SMEMB_COPY_H2GH, stream, SMEM_BM_FLAG_USE_EXTERNAL_STREAM);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, DataCopy_NotConnected_ReturnsNotConnected)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    MOCKER(hybm_data_copy).stubs().will(returnValue(-101)); // BM_NOT_CONNECTED -101
    auto ret =
        entry_->DataCopy(reinterpret_cast<void *>(HOST_GVA_BASE), reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000),
                         TEST_DATA_SIZE, SMEMB_COPY_H2GH, nullptr, 0);
    EXPECT_EQ(ret, SMEM_NOT_CONNECTED);
}

TEST_F(SmemBmEntryTest, DataCopy_InvalidDirection_ReturnsInvalidParam)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    auto ret = entry_->DataCopy(reinterpret_cast<void *>(0xBEEF0000), reinterpret_cast<void *>(0xDEAD0000),
                                TEST_DATA_SIZE, SMEMB_COPY_G2G, nullptr, 0);
    EXPECT_EQ(ret, SM_INVALID_PARAM);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_FullPath_Success)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    MOCKER(hybm_data_batch_copy).stubs().will(returnValue(0));
    void *src = reinterpret_cast<void *>(HOST_GVA_BASE);
    void *dst = reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000);
    uint64_t ds = TEST_DATA_SIZE;
    smem_batch_copy_params params{};
    params.sources = &src;
    params.destinations = &dst;
    params.batchSize = 1; // 1
    params.dataSizes = &ds;
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_H2GH, 0);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, DataCopyBatch_UseExternalStream)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER_CPP(&SmemBmEntry::CheckJoined, ock::smem::Result(*)(const SmemBmEntry *)).stubs().will(returnValue(0));
    MOCKER(hybm_data_batch_copy).stubs().will(returnValue(0));
    void *src = reinterpret_cast<void *>(HOST_GVA_BASE);
    void *dst = reinterpret_cast<void *>(HOST_GVA_BASE + 0x1000);
    uint64_t ds = TEST_DATA_SIZE;
    smem_batch_copy_params params{};
    params.sources = &src;
    params.destinations = &dst;
    params.batchSize = 1; // 1
    params.dataSizes = &ds;
    auto ret = entry_->DataCopyBatch(&params, SMEMB_COPY_H2GH, SMEM_BM_FLAG_USE_EXTERNAL_STREAM);
    EXPECT_EQ(ret, SM_OK);
}

// ======================== DataCopyBatchConcurrent Validation Only ========================
// (Full path not tested due to thread pool + mockcpp thread safety issues)

// ======================== AllocDramMemBySlice Tests ========================

static constexpr uint64_t GB32 = 32ULL * GB;

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_Exact32GB_OneSlice)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, GB32, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 1U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_64GB_TwoSlices)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, 2ULL * GB32, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 2U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 2U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_33GB_FullSliceAndRemainder)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, GB32 + GB, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 2U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 2U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_LessThan32GB_OneSlice)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, TEST_DRAM_SIZE_PER_RANK, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 1U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_AllocFail_ReturnsError)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(static_cast<hybm_mem_slice_t>(nullptr)));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, GB32, 0);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_TRUE(entry_->slices_.empty());
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_ExportFail_ReturnsError)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(-1));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, TEST_DRAM_SIZE_PER_RANK, 0);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_TRUE(entry_->sliceInfos_.empty());
}

TEST_F(SmemBmEntryTest, AllocDramMemBySlice_96GB_ThreeSlices)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBySlice(TEST_ENTITY_PTR, 3ULL * GB32, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 3U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 3U);
}

// ======================== GetRealDRAMSize / GetRealHBMSize Tests ========================

TEST_F(SmemBmEntryTest, GetRealDRAMSize_DefaultZero)
{
    EXPECT_EQ(entry_->GetRealDRAMSize(), 0U);
}

TEST_F(SmemBmEntryTest, GetRealHBMSize_DefaultZero)
{
    EXPECT_EQ(entry_->GetRealHBMSize(), 0U);
}

TEST_F(SmemBmEntryTest, GetRealDRAMSize_AfterSet)
{
    entry_->realDRAMSize_ = 4ULL * GB;
    EXPECT_EQ(entry_->GetRealDRAMSize(), 4ULL * GB);
}

TEST_F(SmemBmEntryTest, GetRealHBMSize_AfterSet)
{
    entry_->realHBMSize_ = 2ULL * GB;
    EXPECT_EQ(entry_->GetRealHBMSize(), 2ULL * GB);
}

// ======================== AllocDramMemBestEffort Tests ========================

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_SuccessOneSlice)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, TEST_DRAM_SIZE_PER_RANK, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_GE(entry_->slices_.size(), 1U);
    EXPECT_EQ(entry_->sliceInfos_.size(), entry_->slices_.size());
    EXPECT_EQ(entry_->GetRealDRAMSize(), TEST_DRAM_SIZE_PER_RANK);
}

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_MultipleSlices)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, 2ULL * GB32, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 2U);
    EXPECT_EQ(entry_->sliceInfos_.size(), 2U);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 2ULL * GB32);
}

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_AllocFail_RecoversGracefully)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(static_cast<hybm_mem_slice_t>(nullptr)));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, GB32, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_TRUE(entry_->slices_.empty());
    EXPECT_EQ(entry_->GetRealDRAMSize(), 0U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_ExportFail_ReturnsError)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(-1));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, TEST_DRAM_SIZE_PER_RANK, 0);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_TRUE(entry_->sliceInfos_.empty());
    EXPECT_EQ(entry_->GetRealDRAMSize(), 0U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_MaxSizeZero_ReturnsOk)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(static_cast<hybm_mem_slice_t>(nullptr)));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, 0, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_TRUE(entry_->slices_.empty());
    EXPECT_EQ(entry_->GetRealDRAMSize(), 0U);
}

TEST_F(SmemBmEntryTest, AllocDramMemBestEffort_NonAlignedSize_CorrectSliceCount)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMemBestEffort(TEST_ENTITY_PTR, GB32 + TEST_DRAM_SIZE_PER_RANK, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 2U);
    EXPECT_EQ(entry_->GetRealDRAMSize(), GB32 + TEST_DRAM_SIZE_PER_RANK);
}

// ======================== AllocDramMem Tests ========================

TEST_F(SmemBmEntryTest, AllocDramMem_ZeroMaxDRAMSize_ReturnsOk)
{
    hybm_options opts{};
    opts.maxDRAMSize = 0;
    auto ret = entry_->AllocDramMem(TEST_ENTITY_PTR, opts, 0);
    EXPECT_EQ(ret, SM_OK);
}

TEST_F(SmemBmEntryTest, AllocDramMem_BestEffortFlag_UsesBestEffort)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    hybm_options opts{};
    opts.maxDRAMSize = TEST_DRAM_SIZE_PER_RANK;
    opts.hostVASpace = TEST_DRAM_SIZE_PER_RANK;
    opts.flags = SMEM_BM_FLAG_DRAM_BEST_EFFORT;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMem(TEST_ENTITY_PTR, opts, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_GE(entry_->slices_.size(), 1U);
    EXPECT_EQ(entry_->GetRealDRAMSize(), TEST_DRAM_SIZE_PER_RANK);
}

TEST_F(SmemBmEntryTest, AllocDramMem_DefaultPath_Success)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    hybm_options opts{};
    opts.maxDRAMSize = TEST_DRAM_SIZE_PER_RANK;
    opts.hostVASpace = TEST_DRAM_SIZE_PER_RANK;
    opts.flags = 0;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));
    auto ret = entry_->AllocDramMem(TEST_ENTITY_PTR, opts, 0);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_EQ(entry_->GetRealDRAMSize(), TEST_DRAM_SIZE_PER_RANK);
}

TEST_F(SmemBmEntryTest, AllocDramMem_DefaultAllocFail_ReturnsError)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    hybm_options opts{};
    opts.maxDRAMSize = TEST_DRAM_SIZE_PER_RANK;
    opts.hostVASpace = TEST_DRAM_SIZE_PER_RANK;
    opts.flags = 0;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(static_cast<hybm_mem_slice_t>(nullptr)));
    auto ret = entry_->AllocDramMem(TEST_ENTITY_PTR, opts, 0);
    EXPECT_EQ(ret, SM_ERROR);
}

TEST_F(SmemBmEntryTest, AllocDramMem_DefaultExportFail_ReturnsError)
{
    entry_->entity_ = TEST_ENTITY_PTR;
    hybm_options opts{};
    opts.maxDRAMSize = TEST_DRAM_SIZE_PER_RANK;
    opts.hostVASpace = TEST_DRAM_SIZE_PER_RANK;
    opts.flags = 0;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(-1));
    auto ret = entry_->AllocDramMem(TEST_ENTITY_PTR, opts, 0);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_EQ(entry_->slices_.size(), 1U);
    EXPECT_TRUE(entry_->sliceInfos_.empty());
}

/*
// ======================== ExtendLocalMem Real Size Update Tests ========================

TEST_F(SmemBmEntryTest, ExtendLocalMem_UpdatesRealDRAMSize_OnSuccess)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    auto store = Convert<TestConfigStoreManager, ConfigStoreManager>(SmMakeRef<TestConfigStoreManager>());
    SmemGroupOption opt{};
    opt.rankSize = 1;
    opt.rank = 0;
    entry_->globalGroup_ = SmMakeRef<SmemNetGroupEngine>(store, opt);
    MOCKER_CPP(&SmemNetGroupEngine::GroupUpdate, ock::smem::Result(*)(SmemNetGroupEngine *))
        .stubs()
        .will(returnValue(0));
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));

    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, 3ULL * MB);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 3ULL * MB);
    EXPECT_EQ(entry_->GetRealHBMSize(), 0U);
}

TEST_F(SmemBmEntryTest, ExtendLocalMem_UpdatesRealHBMSize_OnSuccess)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    auto store = Convert<TestConfigStoreManager, ConfigStoreManager>(SmMakeRef<TestConfigStoreManager>());
    SmemGroupOption opt{};
    opt.rankSize = 1;
    opt.rank = 0;
    entry_->globalGroup_ = SmMakeRef<SmemNetGroupEngine>(store, opt);
    MOCKER_CPP(&SmemNetGroupEngine::GroupUpdate, ock::smem::Result(*)(SmemNetGroupEngine *))
        .stubs()
        .will(returnValue(0));
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));

    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_DEVICE, 2ULL * MB);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 0U);
    EXPECT_EQ(entry_->GetRealHBMSize(), 2ULL * MB);
}

TEST_F(SmemBmEntryTest, ExtendLocalMem_AccumulatesRealSize_AfterMultipleExtend)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    auto store = Convert<TestConfigStoreManager, ConfigStoreManager>(SmMakeRef<TestConfigStoreManager>());
    SmemGroupOption opt{};
    opt.rankSize = 1;
    opt.rank = 0;
    entry_->globalGroup_ = SmMakeRef<SmemNetGroupEngine>(store, opt);
    MOCKER_CPP(&SmemNetGroupEngine::GroupUpdate, ock::smem::Result(*)(SmemNetGroupEngine *))
        .stubs()
        .will(returnValue(0));
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(0));

    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, MB);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->GetRealDRAMSize(), MB);

    ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, 2ULL * MB);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 3ULL * MB);

    ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_DEVICE, 4ULL * MB);
    EXPECT_EQ(ret, SM_OK);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 3ULL * MB);
    EXPECT_EQ(entry_->GetRealHBMSize(), 4ULL * MB);
}
*/

TEST_F(SmemBmEntryTest, ExtendLocalMem_AllocFail_NoSizeUpdate)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    entry_->realDRAMSize_ = 5ULL * MB;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(static_cast<hybm_mem_slice_t>(nullptr)));

    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, MB);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 5ULL * MB);
}

TEST_F(SmemBmEntryTest, ExtendLocalMem_ExportFail_NoSizeUpdate)
{
    entry_->inited_ = true;
    entry_->entity_ = TEST_ENTITY_PTR;
    entry_->realDRAMSize_ = 3ULL * MB;
    MOCKER(hybm_alloc_local_memory).stubs().will(returnValue(TEST_SLICE_PTR));
    MOCKER(hybm_export).stubs().will(returnValue(-1));
    MOCKER(hybm_free_local_memory).stubs().will(returnValue(0));

    auto ret = entry_->ExtendLocalMem(SMEM_MEM_TYPE_HOST, MB);
    EXPECT_EQ(ret, SM_ERROR);
    EXPECT_EQ(entry_->GetRealDRAMSize(), 3ULL * MB);
}

// ======================== Leave Validation ========================

// =================== OnEstablishConnection: no redundant import ===================
// Peer slices are imported once by OnAddToWhitelist::ImportPeerSlices, and the group
// manager only dispatches ESTABLISH after the whitelist ADD was acked, so the
// establish stage must not import peers' externalInfo again.
TEST_F(SmemBmEntryTest, OnEstablishConnection_DoesNotImportSlicesAgain)
{
    MOCKER_CPP(&hybm_import, int32_t(*)(hybm_entity_t, const hybm_exchange_info[], uint32_t, void *[], uint32_t))
        .expects(never());
    MOCKER_CPP(&hybm_mmap, int32_t(*)(hybm_entity_t, uint32_t)).expects(once()).will(returnValue(BM_OK));
    MOCKER_CPP(&hybm_transport_connect, int32_t(*)(hybm_entity_t, const uint32_t *, uint32_t, uint32_t))
        .expects(once())
        .will(returnValue(SMEM_OK));

    RankFullInfo peer;
    peer.rankId = TEST_RANK_ID + 1;
    peer.externalInfo.push_back(Bytes(8, 0xAB)); // non-empty slice info

    EXPECT_EQ(entry_->OnEstablishConnection(peer.rankId, {peer}, 0), SMEM_OK);
}

TEST_F(SmemBmEntryTest, DeriveRankRole_FromLocalMemory)
{
    /* 不申请任何本地内存（DRAM/HBM 均为 0）即为 Proxy：两个 Proxy 之间不建链 */
    EXPECT_EQ(SmemBmEntry::DeriveRankRole(0, 0), SMEM_GROUP_RANK_ROLE_PROXY);
    /* 只要申请了任意一种内存（无论 DRAM 还是 HBM）就是 Provider，保持全互联 */
    EXPECT_EQ(SmemBmEntry::DeriveRankRole(0, 1), SMEM_GROUP_RANK_ROLE_PROVIDER);
    EXPECT_EQ(SmemBmEntry::DeriveRankRole(1, 0), SMEM_GROUP_RANK_ROLE_PROVIDER);
    EXPECT_EQ(SmemBmEntry::DeriveRankRole(4096, 8192), SMEM_GROUP_RANK_ROLE_PROVIDER);
}
