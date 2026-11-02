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
#include <type_traits>
#include <iomanip>
#include "hybm_logger.h"
#include "hybm_ptracer.h"
#include "mf_num_util.h"
#include "hybm_entity_factory.h"
#include "hybm_data_op.h"
#include "hybm_va_manager.h"

using namespace ock::mf;

HYBM_API int32_t hybm_data_copy(hybm_entity_t e, hybm_copy_params *params, hybm_data_copy_direction direction,
                                void *stream, uint32_t flags)
{
    BM_ASSERT_LOG_AND_RETURN(e != nullptr, "e is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params != nullptr, "params is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->src != nullptr, "params->src is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dest != nullptr, "params->dest is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dataSize != 0, "params->dataSize = " << params->dataSize, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(direction < HYBM_DATA_COPY_DIRECTION_BUTT, "direction = " << direction, BM_INVALID_PARAM);
    BM_LOG_DEBUG("Src: " << VaToInfo(params->src) << ", dest: " << VaToInfo(params->dest) << " flag:" << VaToStr(flags)
                         << " direction:" << direction);

    auto &vaMgr = ock::mf::HybmVaManager::GetInstance();
    uint8_t srcMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->src));
    uint8_t dstMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->dest));
    uint8_t except = srcMask | (dstMask << 4);

    if (direction == HYBM_DATA_COPY_DIRECTION_AUTO) {
        direction = static_cast<hybm_data_copy_direction>(HybmVaManager::directionLut[except]);
        if (direction >= HYBM_DATA_COPY_DIRECTION_AUTO) {
            BM_LOG_ERROR("Failed to auto infer copy direction, src:" << std::hex << params->src
                                                                     << ", dest:" << params->dest
                                                                     << ", except:" << static_cast<int>(except));
            return BM_INVALID_PARAM;
        }
    } else if ((HybmVaManager::dirMask[direction] & except) != HybmVaManager::dirMask[direction]) {
        BM_LOG_ERROR("Direction mismatch: specified=" << static_cast<int>(direction)
                                                      << " except:" << static_cast<int>(except)
                                                      << " src=" << params->src << " dest=" << params->dest);
        return BM_INVALID_PARAM;
    }

    auto entity = MemEntityFactory::Instance().FindEngineByPtr(e);
    BM_ASSERT_LOG_AND_RETURN(entity != nullptr, "entity is nullptr", BM_INVALID_PARAM);

    return entity->CopyData(*params, direction, stream, flags);
}

HYBM_API int32_t hybm_wait(hybm_entity_t e)
{
    if (e == nullptr) {
        BM_LOG_ERROR("input parameter invalid, e: 0x" << std::hex << e);
        return BM_INVALID_PARAM;
    }
    auto entity = MemEntityFactory::Instance().FindEngineByPtr(e);
    BM_ASSERT_LOG_AND_RETURN(entity != nullptr, "entity is nullptr", BM_INVALID_PARAM);
    return entity->Wait();
}

static int32_t BatchCopyByAutoGroup(MemEntity *entity, const hybm_batch_copy_params *params, void *stream,
                                    uint32_t flags)
{
    auto &vaMgr = ock::mf::HybmVaManager::GetInstance();
    std::map<hybm_data_copy_direction, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < params->batchSize; i++) {
        if (params->sources[i] == nullptr || params->destinations[i] == nullptr) {
            BM_LOG_ERROR("input copy address is invalid, source or dest is nullptr, index:" << i);
            return BM_INVALID_PARAM;
        }
        uint8_t srcMask = 0U;
        uint8_t dstMask = 0U;
        vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->sources[i]),
                                  reinterpret_cast<uint64_t>(params->destinations[i]), srcMask, dstMask);
        uint8_t except = srcMask | (dstMask << 4);
        auto dir = static_cast<hybm_data_copy_direction>(HybmVaManager::directionLut[except]);
        if (dir >= HYBM_DATA_COPY_DIRECTION_AUTO) {
            BM_LOG_ERROR("failed to auto infer copy direction, index: "
                         << i << ", src: " << std::hex << params->sources[i] << ", dest: " << params->destinations[i]
                         << ", except:" << static_cast<int>(except));
            return BM_INVALID_PARAM;
        }
        groups[dir].push_back(i);
    }
    for (auto &[dir, indices] : groups) {
        std::vector<void *> subSrc;
        std::vector<void *> subDst;
        std::vector<uint64_t> subSizes;
        subSrc.reserve(indices.size());
        subDst.reserve(indices.size());
        subSizes.reserve(indices.size());
        for (auto idx : indices) {
            subSrc.push_back(params->sources[idx]);
            subDst.push_back(params->destinations[idx]);
            subSizes.push_back(params->dataSizes[idx]);
        }
        hybm_batch_copy_params subParams = {subSrc.data(), subDst.data(), subSizes.data(),
                                            static_cast<uint32_t>(indices.size())};
        auto ret = entity->BatchCopyData(subParams, dir, stream, flags);
        if (ret != BM_OK) {
            BM_LOG_ERROR("batch copy data failed, direction: " << dir << ", batchSize: " << indices.size()
                                                               << ", ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

HYBM_API int32_t hybm_data_batch_copy(hybm_entity_t e, hybm_batch_copy_params *params,
                                      hybm_data_copy_direction direction, void *stream, uint32_t flags)
{
    BM_ASSERT_LOG_AND_RETURN(e != nullptr, "e is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params != nullptr, "params is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->sources != nullptr, "params->sources is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->destinations != nullptr, "params->destinations is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dataSizes != nullptr, "params->dataSizes is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->batchSize != 0, "params->batchSize = " << params->batchSize, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(direction < HYBM_DATA_COPY_DIRECTION_BUTT, "direction = " << direction, BM_INVALID_PARAM);
    BM_LOG_DEBUG("Src[0]: " << VaToInfo(params->sources[0]) << ", dest[0]: " << VaToInfo(params->destinations[0])
                            << " flag:" << VaToStr(flags) << " direction:" << direction);

    for (uint32_t i = 0; i < params->batchSize; ++i) {
        if (params->dataSizes[i] == 0) {
            BM_LOG_ERROR("input copy data size is zero, index:" << i << " batchSize:" << params->batchSize);
            return BM_INVALID_PARAM;
        }
    }

    auto entity = (MemEntity *)e;

    if (direction == HYBM_DATA_COPY_DIRECTION_AUTO) {
        return BatchCopyByAutoGroup(entity, params, stream, flags);
    }

    TP_TRACE_BEGIN(TP_HYBM_BATCH_VALIDATE_ADDRESSES);
    for (uint32_t i = 0; i < params->batchSize; i++) {
        if (params->sources[i] == nullptr || params->destinations[i] == nullptr) {
            TP_TRACE_END(TP_HYBM_BATCH_VALIDATE_ADDRESSES, BM_INVALID_PARAM);
            BM_LOG_ERROR("input copy address is invalid, source or dest is nullptr, index:" << i);
            return BM_INVALID_PARAM;
        }

        auto &vaMgr = HybmVaManager::GetInstance();
        uint8_t srcMask = 0U;
        uint8_t dstMask = 0U;
        vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->sources[i]),
                                  reinterpret_cast<uint64_t>(params->destinations[i]), srcMask, dstMask);
        uint8_t except = srcMask | (dstMask << 4);

        if ((HybmVaManager::dirMask[direction] & except) != HybmVaManager::dirMask[direction]) {
            TP_TRACE_END(TP_HYBM_BATCH_VALIDATE_ADDRESSES, BM_INVALID_PARAM);
            BM_LOG_ERROR("Direction mismatch at index "
                         << i << ": dir=" << static_cast<int>(direction) << " except:" << static_cast<int>(except)
                         << " src=" << params->sources[i] << " dest=" << params->destinations[i]);
            return BM_INVALID_PARAM;
        }
    }
    TP_TRACE_END(TP_HYBM_BATCH_VALIDATE_ADDRESSES, BM_OK);
    return entity->BatchCopyData(*params, direction, stream, flags);
}
