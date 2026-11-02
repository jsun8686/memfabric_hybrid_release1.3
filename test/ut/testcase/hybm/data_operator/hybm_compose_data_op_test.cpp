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

#include <functional>

#include "hybm_logger.h"
#include "hybm_data_op_factory.h"
#include "hybm_compose_data_op.h"
#define private public
#include "dl_acl_api.h"
#undef private

class DataOperatorMock : public ock::mf::DataOperator {
public:
    DataOperatorMock(std::string nm) : name{std::move(nm)} {}

    ock::mf::Result Initialize() noexcept override;
    void UnInitialize() noexcept override;
    ock::mf::Result DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                             const ock::mf::ExtOptions &options) noexcept override;
    ock::mf::Result BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                  const ock::mf::ExtOptions &options) noexcept override;
    ock::mf::Result DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                  const ock::mf::ExtOptions &options) noexcept override;
    void TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept override;
    ock::mf::Result Wait(int32_t waitId) noexcept override;
    void Reset() noexcept;

    uint64_t initializeCount{0};
    uint64_t uninitializeCount{0};
    uint64_t dataCopyCount{0};
    uint64_t batchDataCopyCount{0};
    uint64_t dataCopyAsyncCount{0};
    uint64_t waitCount{0};
    ock::mf::Result initializeResult{BM_OK};
    ock::mf::Result dataCopyResult{BM_OK};
    ock::mf::Result batchDataCopyResult{BM_OK};
    ock::mf::Result dataCopyAsyncResult{BM_OK};
    ock::mf::Result waitResult{BM_OK};
    std::function<void(hybm_batch_copy_params &, const ock::mf::ExtOptions &)> batchDataCopyHook;

    const std::string name;
};

ock::mf::Result DataOperatorMock::Initialize() noexcept
{
    initializeCount++;
    return initializeResult;
}

void DataOperatorMock::UnInitialize() noexcept
{
    uninitializeCount++;
}

ock::mf::Result DataOperatorMock::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                           const ock::mf::ExtOptions &options) noexcept
{
    dataCopyCount++;
    return dataCopyResult;
}

ock::mf::Result DataOperatorMock::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                                const ock::mf::ExtOptions &options) noexcept
{
    batchDataCopyCount++;
    if (batchDataCopyHook) {
        batchDataCopyHook(params, options);
    }
    return batchDataCopyResult;
}

ock::mf::Result DataOperatorMock::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                                const ock::mf::ExtOptions &options) noexcept
{
    dataCopyAsyncCount++;
    return dataCopyAsyncResult;
}

ock::mf::Result DataOperatorMock::Wait(int32_t waitId) noexcept
{
    waitCount++;
    return waitResult;
}

void DataOperatorMock::Reset() noexcept
{
    initializeCount = 0;
    uninitializeCount = 0;
    dataCopyCount = 0;
    batchDataCopyCount = 0;
    dataCopyAsyncCount = 0;
    waitCount = 0;
    initializeResult = BM_OK;
    dataCopyResult = BM_OK;
    batchDataCopyResult = BM_OK;
    dataCopyAsyncResult = BM_OK;
    waitResult = BM_OK;
    batchDataCopyHook = nullptr;
}

void DataOperatorMock::TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept {}

class HybmComposeDataOpTest : public testing::Test {
public:
    void SetUp() override;
    void TearDown() override;

public:
    static std::shared_ptr<ock::mf::DataOperator> CreateSdmaDataOperator();
    static std::shared_ptr<ock::mf::DataOperator>
    CreateDevRdmaDataOperator(uint32_t rankId, const std::shared_ptr<ock::mf::transport::TransportManager> &tm);
    static std::shared_ptr<ock::mf::DataOperator>
    CreateDevUrmaDataOperator(uint32_t rankId, const std::shared_ptr<ock::mf::transport::TransportManager> &tm);
    static std::shared_ptr<ock::mf::DataOperator>
    CreateHostRdmaDataOperator(uint32_t rankId, const std::shared_ptr<ock::mf::transport::TransportManager> &tm);
    static std::shared_ptr<ock::mf::DataOperator> CreateHostShmDataOperator(uint32_t rankId);

protected:
    uint32_t OpOr()
    {
        return 0U;
    }

    template<class E, class... Args>
    uint32_t OpOr(E head, Args... rest)
    {
        auto headU32 = static_cast<uint32_t>(head);
        auto left = OpOr(rest...);
        return headU32 | left;
    }

    static std::shared_ptr<DataOperatorMock> sdmaDataOpMock;
    static std::shared_ptr<DataOperatorMock> devRdmaDataOpMock;
    static std::shared_ptr<DataOperatorMock> devUrmaDataOpMock;
    static std::shared_ptr<DataOperatorMock> hostRdmaDataOpMock;
    static std::shared_ptr<DataOperatorMock> hostShmDataOpMock;
};

std::shared_ptr<DataOperatorMock> HybmComposeDataOpTest::sdmaDataOpMock = std::make_shared<DataOperatorMock>("sdma");
std::shared_ptr<DataOperatorMock> HybmComposeDataOpTest::devRdmaDataOpMock =
    std::make_shared<DataOperatorMock>("dev_rdma");
std::shared_ptr<DataOperatorMock> HybmComposeDataOpTest::devUrmaDataOpMock =
    std::make_shared<DataOperatorMock>("dev_urma");
std::shared_ptr<DataOperatorMock> HybmComposeDataOpTest::hostRdmaDataOpMock =
    std::make_shared<DataOperatorMock>("host_rdma");
std::shared_ptr<DataOperatorMock> HybmComposeDataOpTest::hostShmDataOpMock =
    std::make_shared<DataOperatorMock>("host_shm");

void HybmComposeDataOpTest::SetUp()
{
    sdmaDataOpMock->Reset();
    devRdmaDataOpMock->Reset();
    devUrmaDataOpMock->Reset();
    hostRdmaDataOpMock->Reset();
    hostShmDataOpMock->Reset();
}

void HybmComposeDataOpTest::TearDown()
{
    GlobalMockObject::verify();
    sdmaDataOpMock->Reset();
    devRdmaDataOpMock->Reset();
    devUrmaDataOpMock->Reset();
    hostRdmaDataOpMock->Reset();
    hostShmDataOpMock->Reset();
}

std::shared_ptr<ock::mf::DataOperator> HybmComposeDataOpTest::CreateSdmaDataOperator()
{
    return sdmaDataOpMock;
}

std::shared_ptr<ock::mf::DataOperator>
HybmComposeDataOpTest::CreateDevRdmaDataOperator(uint32_t rankId,
                                                 const std::shared_ptr<ock::mf::transport::TransportManager> &tm)
{
    return devRdmaDataOpMock;
}

std::shared_ptr<ock::mf::DataOperator>
HybmComposeDataOpTest::CreateDevUrmaDataOperator(uint32_t rankId,
                                                 const std::shared_ptr<ock::mf::transport::TransportManager> &tm)
{
    return devUrmaDataOpMock;
}

std::shared_ptr<ock::mf::DataOperator>
HybmComposeDataOpTest::CreateHostRdmaDataOperator(uint32_t rankId,
                                                  const std::shared_ptr<ock::mf::transport::TransportManager> &tm)
{
    return hostRdmaDataOpMock;
}

std::shared_ptr<ock::mf::DataOperator> HybmComposeDataOpTest::CreateHostShmDataOperator(uint32_t rankId)
{
    return hostShmDataOpMock;
}

TEST_F(HybmComposeDataOpTest, initialize_with_bm_type_ai_core)
{
    hybm_options options;
    options.bmType = HYBM_TYPE_AI_CORE_INITIATE;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).expects(never()).will(returnValue(nullptr));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).expects(never()).will(returnValue(nullptr));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).expects(never()).will(returnValue(nullptr));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
}

TEST_F(HybmComposeDataOpTest, initialize_with_sdma_only)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_dev_rdma_only)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_DEVICE_RDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, devRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_host_rdma_only)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_HOST_RDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_data_op_all)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    uint32_t type = static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA);
    options.bmDataOpType = static_cast<hybm_data_op_type>(type);
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, hostRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_data_op_all_sdma_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    uint32_t type = static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA);
    options.bmDataOpType = static_cast<hybm_data_op_type>(type);
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    sdmaDataOpMock->initializeResult = BM_ERROR;
    auto ret = dataOp.Initialize();
    ASSERT_NE(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->initializeCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_data_op_all_dev_rdma_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    uint32_t type = static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA);
    options.bmDataOpType = static_cast<hybm_data_op_type>(type);
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    devRdmaDataOpMock->initializeResult = BM_ERROR;
    auto ret = dataOp.Initialize();
    ASSERT_NE(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_data_op_all_host_rdma_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    uint32_t type = static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA);
    type |= static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA);
    options.bmDataOpType = static_cast<hybm_data_op_type>(type);
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    hostRdmaDataOpMock->initializeResult = BM_ERROR;
    auto ret = dataOp.Initialize();
    ASSERT_NE(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    // hostRdmaDataOperator_ is nullptr (Initialize failed → set to nullptr before return)
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
    // device DataOp 在 hostRdma Initialize 失败时已被置 nullptr（仅析构，不调用 UnInitialize）
    ASSERT_EQ(0UL, devRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_sdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_all_sdma_first_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->dataCopyCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->dataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_all_sdma_failed_dev_rdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));
    sdmaDataOpMock->dataCopyResult = BM_DL_FUNCTION_FAILED;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->dataCopyCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->dataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_sdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    hybm_batch_copy_params copyParams{};
    void *sources[2ULL];
    void *dest[2ULL];
    uint64_t size[2ULL];
    copyParams.batchSize = 2ULL;
    copyParams.sources = sources;
    copyParams.destinations = dest;
    copyParams.dataSizes = size;

    std::pair<uint32_t, uint32_t> p2pInfo{0, 1};
    ock::mf::ExtOptions extOptions{};
    extOptions.groupMap[p2pInfo].push_back(0);
    ret = dataOp.BatchDataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->batchDataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_all_sdma_first_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_batch_copy_params copyParams{};
    std::vector<void *> sourcesVec = {nullptr};
    std::vector<void *> destinationsVec = {nullptr};
    std::vector<uint64_t> dataSizesVec = {1ULL};
    copyParams.sources = sourcesVec.data();
    copyParams.destinations = destinationsVec.data();
    copyParams.dataSizes = dataSizesVec.data();

    ock::mf::ExtOptions extOptions{};
    extOptions.groupMap.emplace(std::make_pair<uint32_t, uint32_t>(0, 1), std::vector<uint32_t>{0});
    ret = dataOp.BatchDataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->batchDataCopyCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->batchDataCopyCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->batchDataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_all_sdma_failed_dev_rdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));
    sdmaDataOpMock->batchDataCopyResult = BM_DL_FUNCTION_FAILED;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_batch_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    void *sources[1];
    void *dest[1];
    uint64_t size[1];
    copyParams.batchSize = 1;
    copyParams.sources = sources;
    copyParams.destinations = dest;
    copyParams.dataSizes = size;
    std::pair<uint32_t, uint32_t> p2pInfo{};
    extOptions.groupMap[p2pInfo].push_back(0);
    ret = dataOp.BatchDataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_DL_FUNCTION_FAILED, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->batchDataCopyCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->batchDataCopyCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->batchDataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_non_rank0_rdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_DEVICE_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_DEVICE_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));
    sdmaDataOpMock->batchDataCopyResult = BM_DL_FUNCTION_FAILED;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_batch_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    void *sources[2ULL];
    void *dest[2ULL];
    uint64_t size[2ULL];
    copyParams.batchSize = 2ULL;
    copyParams.sources = sources;
    copyParams.destinations = dest;
    copyParams.dataSizes = size;
    std::pair<uint32_t, uint32_t> p2pInfo{0, 1};
    std::pair<uint32_t, uint32_t> p2pInfo2{2, 3};
    extOptions.groupMap[p2pInfo].push_back(0);
    extOptions.groupMap[p2pInfo2].push_back(1);
    ret = dataOp.BatchDataCopy(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(2UL, devRdmaDataOpMock->batchDataCopyCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, devRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_all_groups_urma_uses_single_submission)
{
    constexpr uint32_t kBatchSize = 2U;
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_DEVICE_URMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    ASSERT_EQ(tag->AddRankTag(0U, "local"), BM_OK);
    ASSERT_EQ(tag->AddRankTag(1U, "urma1"), BM_OK);
    ASSERT_EQ(tag->AddRankTag(2U, "urma2"), BM_OK);
    ASSERT_EQ(tag->AddTagOpInfo("local:DEVICE_URMA:urma1,local:DEVICE_URMA:urma2"), BM_OK);

    MOCKER(ock::mf::DataOperatorFactory::CreateDevUrmaDataOperator).stubs().will(invoke(CreateDevUrmaDataOperator));
    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(dataOp.Initialize(), BM_OK);

    char sourcesData[kBatchSize]{};
    char destinationsData[kBatchSize]{};
    void *sources[kBatchSize] = {&sourcesData[0], &sourcesData[1]};
    void *destinations[kBatchSize] = {&destinationsData[0], &destinationsData[1]};
    uint64_t sizes[kBatchSize] = {1U, 1U};
    hybm_batch_copy_params params{sources, destinations, sizes, kBatchSize};
    ock::mf::ExtOptions extOptions{};
    extOptions.groupMap[{0U, 1U}] = {0U};
    extOptions.groupMap[{0U, 2U}] = {1U};
    devUrmaDataOpMock->batchDataCopyHook = [&](hybm_batch_copy_params &actual,
                                               const ock::mf::ExtOptions &actualOptions) {
        EXPECT_EQ(&actualOptions, &extOptions);
        EXPECT_EQ(actual.sources, sources);
        EXPECT_EQ(actual.destinations, destinations);
        EXPECT_EQ(actual.batchSize, kBatchSize);
        EXPECT_EQ(actualOptions.groupMap, extOptions.groupMap);
    };

    EXPECT_EQ(dataOp.BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions), BM_OK);
    EXPECT_EQ(devUrmaDataOpMock->batchDataCopyCount, 1U);
    EXPECT_EQ(devRdmaDataOpMock->batchDataCopyCount, 0U);
    EXPECT_EQ(hostRdmaDataOpMock->batchDataCopyCount, 0U);
    dataOp.UnInitialize();
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_mixed_urma_and_host_aggregates_urma_groups)
{
    constexpr uint32_t kBatchSize = 3U;
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_TCP));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    ASSERT_EQ(tag->AddRankTag(0U, "local"), BM_OK);
    ASSERT_EQ(tag->AddRankTag(1U, "urma1"), BM_OK);
    ASSERT_EQ(tag->AddRankTag(2U, "host"), BM_OK);
    ASSERT_EQ(tag->AddRankTag(3U, "urma2"), BM_OK);
    ASSERT_EQ(tag->AddTagOpInfo("local:DEVICE_URMA:urma1,local:HOST_TCP:host,local:DEVICE_URMA:urma2"), BM_OK);

    MOCKER(ock::mf::DataOperatorFactory::CreateDevUrmaDataOperator).stubs().will(invoke(CreateDevUrmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));
    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(dataOp.Initialize(), BM_OK);

    char sourceData[kBatchSize]{};
    char destinationData[kBatchSize]{};
    void *sources[kBatchSize] = {&sourceData[0], &sourceData[1], &sourceData[2]};
    void *destinations[kBatchSize] = {&destinationData[0], &destinationData[1], &destinationData[2]};
    uint64_t sizes[kBatchSize] = {1U, 1U, 1U};
    hybm_batch_copy_params params{sources, destinations, sizes, kBatchSize};
    ock::mf::ExtOptions extOptions{};
    extOptions.groupMap[{0U, 1U}] = {0U};
    extOptions.groupMap[{0U, 2U}] = {1U};
    extOptions.groupMap[{0U, 3U}] = {2U};
    ock::mf::ExtOptions expectedUrmaOptions{};
    expectedUrmaOptions.groupMap[{0U, 1U}] = {0U};
    expectedUrmaOptions.groupMap[{0U, 3U}] = {2U};
    devUrmaDataOpMock->batchDataCopyHook = [&](hybm_batch_copy_params &actual,
                                               const ock::mf::ExtOptions &actualOptions) {
        EXPECT_EQ(actual.sources, sources);
        EXPECT_EQ(actual.destinations, destinations);
        EXPECT_EQ(actual.dataSizes, sizes);
        EXPECT_EQ(actual.batchSize, kBatchSize);
        EXPECT_EQ(actualOptions.groupMap, expectedUrmaOptions.groupMap);
    };
    hostRdmaDataOpMock->batchDataCopyHook = [&](hybm_batch_copy_params &actual,
                                                const ock::mf::ExtOptions &actualOptions) {
        EXPECT_EQ(actual.batchSize, 1U);
        EXPECT_EQ(actual.sources[0], sources[1]);
        EXPECT_EQ(actualOptions.destRankId, 2U);
        EXPECT_TRUE(actualOptions.groupMap.empty());
    };

    EXPECT_EQ(dataOp.BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions), BM_OK);
    EXPECT_EQ(devUrmaDataOpMock->batchDataCopyCount, 1U);
    EXPECT_EQ(hostRdmaDataOpMock->batchDataCopyCount, 1U);
    dataOp.UnInitialize();
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_urma_rejects_empty_group_map)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_DEVICE_URMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    MOCKER(ock::mf::DataOperatorFactory::CreateDevUrmaDataOperator).stubs().will(invoke(CreateDevUrmaDataOperator));
    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(dataOp.Initialize(), BM_OK);

    hybm_batch_copy_params params{};
    ock::mf::ExtOptions extOptions{};
    EXPECT_EQ(dataOp.BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions), BM_INVALID_PARAM);
    EXPECT_EQ(devUrmaDataOpMock->batchDataCopyCount, 0U);
    dataOp.UnInitialize();
}

TEST_F(HybmComposeDataOpTest, data_copy_async_sdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopyAsync(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyAsyncCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_async_all_sdma_first_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopyAsync(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyAsyncCount);
    ASSERT_EQ(0UL, devRdmaDataOpMock->dataCopyAsyncCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->dataCopyAsyncCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_async_all_sdma_failed_dev_rdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType =
        static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(
        returnValue(OpOr(HYBM_DOP_TYPE_SDMA, HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));
    sdmaDataOpMock->dataCopyAsyncResult = BM_DL_FUNCTION_FAILED;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);

    hybm_copy_params copyParams{};
    ock::mf::ExtOptions extOptions{};
    ret = dataOp.DataCopyAsync(copyParams, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->dataCopyAsyncCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->dataCopyAsyncCount);
    ASSERT_EQ(0UL, hostRdmaDataOpMock->dataCopyAsyncCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, dwait_sdma_success)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    ret = dataOp.Wait(0);
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->waitCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, dwait_sdma_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_SDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateSdmaDataOperator).stubs().will(invoke(CreateSdmaDataOperator));
    sdmaDataOpMock->waitResult = BM_DL_FUNCTION_FAILED;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->initializeCount);

    ret = dataOp.Wait(0);
    ASSERT_NE(BM_OK, ret);
    ASSERT_EQ(1UL, sdmaDataOpMock->waitCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, sdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, dwait_no_sdma_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = static_cast<hybm_data_op_type>(OpOr(HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_HOST_RDMA));
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    union {
        uint32_t (ock::mf::HybmEntityTagInfo::*getRank2RankOpType)(uint32_t rankId1, uint32_t rankId2);
        uint32_t (*mocker)(ock::mf::HybmEntityTagInfo *, uint32_t, uint32_t);
    } u;
    u.getRank2RankOpType = &ock::mf::HybmEntityTagInfo::GetRank2RankOpType;
    MOCKER(u.mocker).stubs().will(returnValue(OpOr(HYBM_DOP_TYPE_SDMA)));
    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));
    MOCKER(ock::mf::DataOperatorFactory::CreateHostRdmaDataOperator).stubs().will(invoke(CreateHostRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(0UL, sdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->initializeCount);

    ret = dataOp.Wait(0);
    // Non-SDMA 路径（如 device_rdma via HCOMM）所有操作均为同步，
    // WriteRemote 已包含 Synchronize，无需额外等待，返回 BM_OK
    ASSERT_EQ(BM_OK, ret);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, devRdmaDataOpMock->uninitializeCount);
    ASSERT_EQ(1UL, hostRdmaDataOpMock->uninitializeCount);
}

TEST_F(HybmComposeDataOpTest, initialize_with_host_shm_failed)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_HOST_SHM;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateHostShmDataOperator).stubs().will(invoke(CreateHostShmDataOperator));
    hostShmDataOpMock->initializeResult = BM_ERROR;

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_NE(BM_OK, ret);
    ASSERT_EQ(1UL, hostShmDataOpMock->initializeCount);
}

TEST_F(HybmComposeDataOpTest, data_copy_no_operator_available)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = 0; // no operators
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(BM_OK, dataOp.Initialize());

    hybm_copy_params params{};
    ock::mf::ExtOptions extOptions{};
    extOptions.srcRankId = 0;
    extOptions.destRankId = 1;
    auto ret = dataOp.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_NE(BM_OK, ret);
}

TEST_F(HybmComposeDataOpTest, batch_data_copy_no_operator_available)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = 0; // no operators
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(BM_OK, dataOp.Initialize());

    hybm_batch_copy_params params{};
    ock::mf::ExtOptions extOptions{};
    extOptions.srcRankId = 0;
    extOptions.destRankId = 1;
    extOptions.groupMap.emplace(std::make_pair(0, 1), std::vector<uint32_t>{0});
    uint64_t src = 0x1000;
    uint64_t dst = 0x2000;
    uint64_t size = 1024;
    params.sources = reinterpret_cast<void **>(&src);
    params.destinations = reinterpret_cast<void **>(&dst);
    params.dataSizes = &size;
    params.batchSize = 1;
    auto ret = dataOp.BatchDataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_NE(BM_OK, ret);
}

TEST_F(HybmComposeDataOpTest, data_copy_async_no_operator_available)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = 0; // no operators
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    ASSERT_EQ(BM_OK, dataOp.Initialize());

    hybm_copy_params params{};
    ock::mf::ExtOptions extOptions{};
    extOptions.srcRankId = 0;
    extOptions.destRankId = 1;
    auto ret = dataOp.DataCopyAsync(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, extOptions);
    ASSERT_NE(BM_OK, ret);
}

// 覆盖 devHcommDataOperator_ 分支（device DataOp 恒创建，与 CANN 版本/开关无关）
TEST_F(HybmComposeDataOpTest, initialize_hcomm_device_then_uninit)
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.bmDataOpType = HYBM_DOP_TYPE_DEVICE_RDMA;
    auto tag = std::make_shared<ock::mf::HybmEntityTagInfo>();
    tag->TagInfoInit(options);

    MOCKER(ock::mf::DataOperatorFactory::CreateDevRdmaDataOperator).stubs().will(invoke(CreateDevRdmaDataOperator));

    ock::mf::HostComposeDataOp dataOp(options, nullptr, tag);
    auto ret = dataOp.Initialize();
    ASSERT_EQ(BM_OK, ret);
    ASSERT_EQ(1UL, devRdmaDataOpMock->initializeCount);

    dataOp.UnInitialize();
    ASSERT_EQ(1UL, devRdmaDataOpMock->uninitializeCount);
}
