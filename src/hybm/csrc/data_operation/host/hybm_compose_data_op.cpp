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
#include <new>

#include "hybm_logger.h"
#include "hybm_data_op_factory.h"
#include "dl_acl_api.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "hybm_compose_data_op.h"

namespace ock {
namespace mf {
HostComposeDataOp::HostComposeDataOp(hybm_options options, transport::TransManagerPtr tm,
                                     HybmEntityTagInfoPtr tag) noexcept
    : options_{std::move(options)}, transport_{std::move(tm)}, entityTagInfo_{std::move(tag)}
{}

HostComposeDataOp::~HostComposeDataOp() noexcept {}

Result HostComposeDataOp::Initialize() noexcept
{
    // AI_CORE驱动不走这里的dataOperator
    if (options_.bmType == HYBM_TYPE_AI_CORE_INITIATE) {
        return BM_OK;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) {
        sdmaDataOperator_ = DataOperatorFactory::CreateSdmaDataOperator();
        auto ret = sdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("SDMA data operator init failed, ret: " << ret << " rankId: " << options_.rankId
                                                                 << " bmDataOpType: " << options_.bmDataOpType);
            sdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    // Device data operator: DEVICE_RDMA 用 DataOpDeviceRDMA（老 native RDMA 与新 HCOMM 共用，
    // 传输无关，经 TransportManager 基类虚接口）；URMA/UBOE 走 develop 原生 DataOpDeviceURMA。
    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
        devRdmaDataOperator_ = DataOperatorFactory::CreateDevRdmaDataOperator(options_.rankId, transport_);
        auto ret = devRdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Device data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & (HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) {
        devUrmaDataOperator_ = DataOperatorFactory::CreateDevUrmaDataOperator(options_.rankId, transport_);
        auto ret = devUrmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Device URMA data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            devUrmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & (HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP)) {
        hostRdmaDataOperator_ = DataOperatorFactory::CreateHostRdmaDataOperator(options_.rankId, transport_);
        auto ret = hostRdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Host RDMA data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            hostRdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) {
        hostRdmaDataOperator_ = DataOperatorFactory::CreateHostShmDataOperator(options_.rankId);
        auto ret = hostRdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Host shm data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            hostRdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    return BM_OK;
}

void HostComposeDataOp::UnInitialize() noexcept
{
    if (hostRdmaDataOperator_ != nullptr) {
        hostRdmaDataOperator_->UnInitialize();
        hostRdmaDataOperator_ = nullptr;
    }
    if (devRdmaDataOperator_ != nullptr) {
        devRdmaDataOperator_->UnInitialize();
        devRdmaDataOperator_ = nullptr;
    }
    if (devUrmaDataOperator_ != nullptr) {
        devUrmaDataOperator_->UnInitialize();
        devUrmaDataOperator_ = nullptr;
    }
    if (sdmaDataOperator_ != nullptr) {
        sdmaDataOperator_->UnInitialize();
        sdmaDataOperator_ = nullptr;
    }
}

Result HostComposeDataOp::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                   const ExtOptions &options) noexcept
{
    auto availableOps = GetPrioritedDataOperators(options);
    if (availableOps.empty()) {
        BM_LOG_ERROR("data copy from rank " << options.srcRankId << " to rank " << options.destRankId
                                            << " no data operator available");
        return BM_INVALID_PARAM;
    }

    Result result = BM_ERROR;
    for (auto &ops : availableOps) {
        BM_LOG_DEBUG("try data copy from rank " << options.srcRankId << " to rank " << options.destRankId
                                                << " with data op " << ops.first << " direction:" << direction);
        hybm_copy_params param2 = params;
        result = ops.second->DataCopy(param2, direction, options);
        if (result == BM_OK) {
            break;
        }

        BM_LOG_WARN("data copy from rank " << options.srcRankId << " to rank " << options.destRankId << " with data op "
                                           << ops.first << " failed " << result);
    }

    if (result != BM_OK) {
        BM_LOG_ERROR("data copy from rank " << options.srcRankId << " to rank " << options.destRankId << " failed "
                                            << result);
    }
    return result;
}

Result HostComposeDataOp::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                        const ExtOptions &options) noexcept
{
    if (options.groupMap.empty()) {
        BM_LOG_ERROR("multi-rank batch copy has empty groupMap, batchSize: " << params.batchSize);
        return BM_INVALID_PARAM;
    }

    if (AllSupportSdma(options)) {
        return sdmaDataOperator_->BatchDataCopy(params, direction, options);
    }
    if (AllPreferDeviceUrma(options)) {
        return devUrmaDataOperator_->BatchDataCopy(params, direction, options);
    }

    ExtOptions urmaOptions{options.srcRankId, options.destRankId, options.stream, options.flags, {}};
    ExtOptions otherOptions{options.srcRankId, options.destRankId, options.stream, options.flags, {}};
    try {
        for (const auto &[p2pInfo, indices] : options.groupMap) {
            ExtOptions copyOptions{p2pInfo.first, p2pInfo.second, options.stream, options.flags, {}};
            auto availableOps = GetPrioritedDataOperators(copyOptions);
            if (availableOps.empty()) {
                BM_LOG_ERROR("batch data copy from rank " << p2pInfo.first << " to rank " << p2pInfo.second
                                                          << " no data operator available");
                return BM_INVALID_PARAM;
            }
            auto &targetGroupMap =
                availableOps.front().second == devUrmaDataOperator_ ? urmaOptions.groupMap : otherOptions.groupMap;
            targetGroupMap.emplace(p2pInfo, indices);
        }
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("batch data copy split groupMap allocation failed, batchSize: "
                     << params.batchSize << " groupNum: " << options.groupMap.size());
        return BM_MALLOC_FAILED;
    }

    if (!urmaOptions.groupMap.empty()) {
        auto ret = devUrmaDataOperator_->BatchDataCopy(params, direction, urmaOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("multi-rank batch copy failed, ret: " << ret << " rankNum: " << urmaOptions.groupMap.size());
            return ret;
        }
    }
    return otherOptions.groupMap.empty() ? BM_OK : BatchDataCopyByGroup(params, direction, otherOptions);
}

//原始的单rank BatchDataCopy
Result HostComposeDataOp::BatchDataCopyByGroup(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                               const ExtOptions &options) noexcept
{
    for (const auto &[p2pInfo, indices] : options.groupMap) {
        ExtOptions copyOptions{};
        copyOptions.srcRankId = p2pInfo.first;
        copyOptions.destRankId = p2pInfo.second;
        copyOptions.stream = options.stream;
        copyOptions.flags = options.flags;
        auto availableOps = GetPrioritedDataOperators(copyOptions);
        if (availableOps.empty()) {
            BM_LOG_ERROR("batch data copy from rank " << p2pInfo.first << " to rank " << p2pInfo.second
                                                      << " no data operator available");
            return BM_INVALID_PARAM;
        }

        const uint32_t groupSize = static_cast<uint32_t>(indices.size());
        std::vector<void *> sources(groupSize);
        std::vector<void *> destinations(groupSize);
        std::vector<size_t> dataSizes(groupSize);
        for (uint32_t i = 0; i < groupSize; ++i) {
            const uint32_t index = indices[i];
            sources[i] = params.sources[index];
            destinations[i] = params.destinations[index];
            dataSizes[i] = params.dataSizes[index];
        }
        hybm_batch_copy_params copyParams{sources.data(), destinations.data(), dataSizes.data(), groupSize};
        auto ret = availableOps.front().second->BatchDataCopy(copyParams, direction, copyOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("data batch copy failed, ret: " << ret << " srcRank: " << p2pInfo.first
                                                         << " destRank: " << p2pInfo.second);
            return ret;
        }
    }
    return BM_OK;
}

Result HostComposeDataOp::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                        const ExtOptions &options) noexcept
{
    auto availableOps = GetPrioritedDataOperators(options);
    if (availableOps.empty()) {
        BM_LOG_ERROR("data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                  << " no data operator available");
        return BM_INVALID_PARAM;
    }

    Result result = BM_ERROR;
    for (auto &ops : availableOps) {
        BM_LOG_DEBUG("try data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                      << " with data op " << ops.first << " direction:" << direction);
        result = ops.second->DataCopyAsync(params, direction, options);
        if (result == BM_OK) {
            break;
        }

        BM_LOG_ERROR("data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                  << " with data op " << ops.first << " failed " << result);
    }
    return result;
}

Result HostComposeDataOp::Wait(int32_t waitId) noexcept
{
    /*
     * Note: Currently, only SDMA supports asynchronous operations; we only perform the wait for the SDMA Data Operator.
     * Subsequent consideration involves using the 3 bits in the wait ID to indicate which data operator is being used.
     */
    if (sdmaDataOperator_ != nullptr) {
        return sdmaDataOperator_->Wait(waitId);
    }
    // For non-SDMA paths (e.g. device_rdma via HCOMM), all operations are
    // synchronous (WriteRemote includes Synchronize → AclrtSynchronizeStream).
    // Nothing to wait for, return success.
    return BM_OK;
}

bool HostComposeDataOp::AllSupportSdma(const ExtOptions &options) noexcept
{
    if (sdmaDataOperator_ == nullptr) {
        return false;
    }

    for (auto &[p2pInfo, indices] : options.groupMap) {
        auto opTypes = entityTagInfo_->GetRank2RankOpType(p2pInfo.first, p2pInfo.second);
        if (!(opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA))) {
            return false;
        }
    }
    return true;
}

bool HostComposeDataOp::AllPreferDeviceUrma(const ExtOptions &options) noexcept
{
    if (devUrmaDataOperator_ == nullptr) {
        return false;
    }

    for (const auto &[p2pInfo, indices] : options.groupMap) {
        (void)indices;
        ExtOptions copyOptions{p2pInfo.first, p2pInfo.second, options.stream, options.flags, {}};
        auto availableOps = GetPrioritedDataOperators(copyOptions);
        if (availableOps.empty() || availableOps.front().second != devUrmaDataOperator_) {
            return false;
        }
    }
    return true;
}

HostComposeDataOp::DataOperators HostComposeDataOp::GetPrioritedDataOperators(const ExtOptions &options) noexcept
{
    HostComposeDataOp::DataOperators dataOperators;
    auto opTypes = entityTagInfo_->GetRank2RankOpType(options.srcRankId, options.destRankId);
    if (sdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_SDMA, sdmaDataOperator_);
    }

    if (devRdmaDataOperator_ != nullptr) {
        if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA)) != 0U) {
            dataOperators.emplace_back(HYBM_DOP_TYPE_DEVICE_RDMA, devRdmaDataOperator_);
        }
    }
    if (devUrmaDataOperator_ != nullptr) {
        if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_URMA)) != 0U) {
            dataOperators.emplace_back(HYBM_DOP_TYPE_DEVICE_URMA, devUrmaDataOperator_);
        }
        if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_UBOE)) != 0U) {
            dataOperators.emplace_back(HYBM_DOP_TYPE_DEVICE_UBOE, devUrmaDataOperator_);
        }
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_RDMA, hostRdmaDataOperator_);
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_URMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_URMA, hostRdmaDataOperator_);
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_TCP)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_TCP, hostRdmaDataOperator_);
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_SHM)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_SHM, hostRdmaDataOperator_);
    }

    return dataOperators;
}
} // namespace mf
} // namespace ock
