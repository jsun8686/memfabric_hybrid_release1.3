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

#include "aicpu_joinable_ranks_qp_manager.h"

#include <cstdint>
#include <vector>

#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "hybm_logger.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {
constexpr uint32_t AI_QP_VERSION = 1;
constexpr uint32_t AI_SEND_CQ_DEPTH = 8192;
constexpr uint32_t AI_RECV_CQ_DEPTH = 128;
constexpr uint32_t AI_MAX_SEND_WR = 8192;
constexpr uint32_t AI_MAX_RECV_WR = 128;
constexpr uint32_t AI_MAX_SEND_SGE = 1;
constexpr uint32_t AI_MAX_RECV_SGE = 1;
constexpr uint32_t AI_CQ_DEPTH_MIN = 64;
constexpr uint32_t AI_CQ_DEPTH_MAX = 32768;
constexpr uint32_t AI_WR_DEPTH_MIN = 64;
constexpr uint32_t AI_WR_DEPTH_MAX = 32767;
constexpr uint32_t AI_DEFAULT_SL = 4;
constexpr int AI_OPBASE_QP_MODE_EXT = 4;

uint32_t GetValidatedDepth(const std::string &envName, uint32_t envValue, uint32_t defaultValue, uint32_t minValue,
                           uint32_t maxValue)
{
    if (envValue < minValue || envValue > maxValue) {
        BM_LOG_WARN(envName << " is invalid:" << envValue << ", use default:" << defaultValue);
        return defaultValue;
    }
    return envValue;
}

uint32_t GetDepthFromEnv(const std::string &envName, uint32_t defaultValue, uint32_t minValue, uint32_t maxValue)
{
    uint32_t value = 0;
    if (!MfEnvUtil::GetOptionalUint(envName, value)) {
        return defaultValue;
    }
    return GetValidatedDepth(envName, value, defaultValue, minValue, maxValue);
}
} // namespace

AicpuJoinableRanksQpManager::AicpuJoinableRanksQpManager(uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId,
                                                         uint32_t rankCount, sockaddr_in devNet,
                                                         hybm_scene scene) noexcept
    : JoinableRanksQpManager(userDeviceId, deviceId, rankId, rankCount, devNet, scene)
{
    sendWrDepth_ = GetDepthFromEnv(env::MF_NPU_RDMA_MAX_SEND_WR, AI_MAX_SEND_WR, AI_WR_DEPTH_MIN, AI_WR_DEPTH_MAX);
}

AicpuJoinableRanksQpManager::~AicpuJoinableRanksQpManager() noexcept
{
    ReleaseQpInfoSpace();
}

int AicpuJoinableRanksQpManager::SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept
{
    {
        std::lock_guard<std::mutex> lock(aiInfoMutex_);
        for (const auto &kv : ranks) {
            if (kv.first >= rankCount_) {
                continue;
            }
            currentRanksInfo_.insert_or_assign(kv.first, kv.second);
        }
    }
    return JoinableRanksQpManager::SetRemoteRankInfo(ranks);
}

int AicpuJoinableRanksQpManager::Startup(void *rdma) noexcept
{
    if (!ReserveQpInfoSpace()) {
        BM_LOG_ERROR("reserve qp info space failed, rank: " << rankId_ << " rankCount: " << rankCount_);
        return BM_MALLOC_FAILED;
    }

    auto ret = JoinableRanksQpManager::Startup(rdma);
    if (ret != BM_OK) {
        BM_LOG_ERROR("startup joinable qp manager failed, rank: " << rankId_ << " ret: " << ret);
        ReleaseQpInfoSpace();
        return ret;
    }
    return BM_OK;
}

void AicpuJoinableRanksQpManager::Shutdown() noexcept
{
    JoinableRanksQpManager::Shutdown();
    {
        std::lock_guard<std::mutex> lock(aiInfoMutex_);
        aiQpInfos_.clear();
        currentRanksInfo_.clear();
    }
    ReleaseQpInfoSpace();
}

const void *AicpuJoinableRanksQpManager::GetQpInfoAddress() const noexcept
{
    FillQpInfo();
    return qpInfo_;
}

uint64_t AicpuJoinableRanksQpManager::GetDeviceQpAddr(uint32_t rankId) const noexcept
{
    std::lock_guard<std::mutex> lock(aiInfoMutex_);
    auto it = aiQpInfos_.find(rankId);
    if (it == aiQpInfos_.end()) {
        BM_LOG_ERROR("aiQpAddr not found, rank: " << rankId_ << " remoteRank: " << rankId);
        return 0;
    }
    return it->second.aiQpAddr;
}

uint32_t AicpuJoinableRanksQpManager::GetDbIndex(uint32_t rankId) const noexcept
{
    std::lock_guard<std::mutex> lock(aiInfoMutex_);
    auto it = aiQpInfos_.find(rankId);
    if (it == aiQpInfos_.end()) {
        return INVALID_DB_INDEX;
    }
    return it->second.dbIndex;
}

int AicpuJoinableRanksQpManager::CreateQpHandle(uint32_t rankId, void *&qpHandle) noexcept
{
    HccpQpExtAttrs attrs{};
    attrs.qpMode = AI_OPBASE_QP_MODE_EXT;
    attrs.version = AI_QP_VERSION;
    attrs.cqAttr.sendCqDepth =
        GetDepthFromEnv(env::MF_NPU_RDMA_SEND_CQ_DEPTH, AI_SEND_CQ_DEPTH, AI_CQ_DEPTH_MIN, AI_CQ_DEPTH_MAX);
    attrs.cqAttr.recvCqDepth =
        GetDepthFromEnv(env::MF_NPU_RDMA_RECV_CQ_DEPTH, AI_RECV_CQ_DEPTH, AI_CQ_DEPTH_MIN, AI_CQ_DEPTH_MAX);
    attrs.qp_attr.cap.max_send_wr =
        GetDepthFromEnv(env::MF_NPU_RDMA_MAX_SEND_WR, AI_MAX_SEND_WR, AI_WR_DEPTH_MIN, AI_WR_DEPTH_MAX);
    attrs.qp_attr.cap.max_recv_wr =
        GetDepthFromEnv(env::MF_NPU_RDMA_MAX_RECV_WR, AI_MAX_RECV_WR, AI_WR_DEPTH_MIN, AI_WR_DEPTH_MAX);
    attrs.qp_attr.cap.max_send_sge = AI_MAX_SEND_SGE;
    attrs.qp_attr.cap.max_recv_sge = AI_MAX_RECV_SGE;
    attrs.qp_attr.qp_type = IBV_QPT_RC;

    HccpAiQpInfo aiQpInfo{};
    auto ret = DlHccpApi::RaQpAiCreate(GetRdmaHandle(), attrs, aiQpInfo, qpHandle);
    if (ret != 0) {
        BM_LOG_ERROR("RaQpAiCreate failed, rank: " << rankId_ << " remoteRank: " << rankId << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    {
        std::lock_guard<std::mutex> lock(aiInfoMutex_);
        aiQpInfos_.insert_or_assign(rankId, aiQpInfo);
    }
    BM_LOG_INFO("AI QP created, rank: " << rankId_ << " remoteRank: " << rankId << " qpHandle: " << qpHandle
                                        << " sqIndex: " << aiQpInfo.sqIndex << " dbIndex: " << aiQpInfo.dbIndex
                                        << " sqDepth: " << aiQpInfo.data_plane_info.sq.depth);
    return BM_OK;
}

bool AicpuJoinableRanksQpManager::ReserveQpInfoSpace() noexcept
{
    if (qpInfo_ != nullptr) {
        return true;
    }
    // 与 fixed_ranks_qp_manager 一致：每个 rank 2 个 WQ + 2 个 CQ，外加 1 个 MR 表项
    const auto oneRankSize = 2U * (sizeof(AiQpRMAWQ) + sizeof(AiQpRMACQ)) + sizeof(RdmaMemRegionInfo);
    qpInfoSize_ = static_cast<uint32_t>(sizeof(AiQpRMAQueueInfo) + oneRankSize * rankCount_);

    void *ptr = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&ptr, qpInfoSize_, 0);
    if (ret != 0 || ptr == nullptr) {
        BM_LOG_ERROR("AclrtMalloc qp info failed, rank: " << rankId_ << " size: " << qpInfoSize_ << " ret: " << ret);
        return false;
    }
    qpInfo_ = static_cast<AiQpRMAQueueInfo *>(ptr);
    BM_LOG_INFO("reserve qp info space success, rank: " << rankId_ << " rankCount: " << rankCount_
                                                        << " size: " << qpInfoSize_);
    return true;
}

void AicpuJoinableRanksQpManager::ReleaseQpInfoSpace() noexcept
{
    if (qpInfo_ != nullptr) {
        DlAclApi::AclrtFree(qpInfo_);
        qpInfo_ = nullptr;
    }
    qpInfoSize_ = 0;
}

void AicpuJoinableRanksQpManager::FillQpInfo() const noexcept
{
    if (qpInfo_ == nullptr) {
        BM_LOG_WARN("qp info space not reserved, rank: " << rankId_);
        return;
    }

    std::lock_guard<std::mutex> lock(aiInfoMutex_);
    BM_LOG_INFO("fill qp info, rank: " << rankId_ << " aiQpInfos size: " << aiQpInfos_.size()
                                       << " currentRanksInfo size: " << currentRanksInfo_.size());
    std::vector<uint8_t> hostBuffer(qpInfoSize_, 0);
    auto *head = reinterpret_cast<AiQpRMAQueueInfo *>(hostBuffer.data());

    // 先按 host 缓冲区布局填写数组指针，填充数据后再改写成 device 地址（两者布局偏移一致）
    SetupHostLayout(*head, hostBuffer.data());
    FillRankEntries(*head);
    FixupDevicePointers(*head);

    for (uint32_t rank = 0; rank < rankCount_; ++rank) {
        auto aiIt = aiQpInfos_.find(rank);
        if (aiIt == aiQpInfos_.end()) {
            BM_LOG_INFO("fill qp info, rank: " << rankId_ << " remoteRank: " << rank << " NOT in aiQpInfos_");
            continue;
        }
        const auto &plane = aiIt->second.data_plane_info;
        BM_LOG_INFO("fill qp info, rank: " << rankId_ << " remoteRank: " << rank << " sq.wqn: " << plane.sq.wqn
                                           << " sq.buf: 0x" << std::hex << plane.sq.buf_addr << " sq.head: 0x"
                                           << plane.sq.head_addr << " sq.db: 0x" << plane.sq.db_reg << std::dec
                                           << " sq.depth: " << plane.sq.depth);
    }

    auto ret = DlAclApi::AclrtMemcpy(qpInfo_, qpInfoSize_, hostBuffer.data(), qpInfoSize_, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != 0) {
        BM_LOG_ERROR("copy qp info to device failed, rank: " << rankId_ << " size: " << qpInfoSize_ << " ret: " << ret);
        return;
    }
    BM_LOG_INFO("fill qp info to device success, rank: " << rankId_ << " count: " << head->count << " qpInfoAddr: 0x"
                                                         << std::hex << reinterpret_cast<uintptr_t>(qpInfo_)
                                                         << std::dec);
}

void AicpuJoinableRanksQpManager::SetupHostLayout(AiQpRMAQueueInfo &head, uint8_t *base) const noexcept
{
    head.count = 1;
    head.sq = reinterpret_cast<AiQpRMAWQ *>(base + sizeof(AiQpRMAQueueInfo));
    head.rq = head.sq + rankCount_;
    head.scq = reinterpret_cast<AiQpRMACQ *>(head.rq + rankCount_);
    head.rcq = head.scq + rankCount_;
    head.mr = reinterpret_cast<RdmaMemRegionInfo *>(head.rcq + rankCount_);
}

void AicpuJoinableRanksQpManager::FillRankEntries(AiQpRMAQueueInfo &head) const noexcept
{
    for (uint32_t rank = 0; rank < rankCount_; ++rank) {
        auto mrIt = currentRanksInfo_.find(rank);
        if (mrIt != currentRanksInfo_.end() && !mrIt->second.memoryMap.empty()) {
            const auto &mr = mrIt->second.memoryMap.begin()->second;
            head.mr[rank].size = mr.size;
            head.mr[rank].addr = mr.regAddress;
            head.mr[rank].lkey = mr.lkey;
            head.mr[rank].rkey = mr.rkey;
        }

        auto aiIt = aiQpInfos_.find(rank);
        if (aiIt == aiQpInfos_.end()) {
            continue;
        }
        const auto &plane = aiIt->second.data_plane_info;
        CopyAiWQInfo(head.sq[rank], plane.sq, DBMode::SW_DB, AI_DEFAULT_SL);
        CopyAiWQInfo(head.rq[rank], plane.rq, DBMode::SW_DB, AI_DEFAULT_SL);
        CopyAiCQInfo(head.scq[rank], plane.scq, DBMode::SW_DB);
        CopyAiCQInfo(head.rcq[rank], plane.rcq, DBMode::SW_DB);
    }
}

void AicpuJoinableRanksQpManager::FixupDevicePointers(AiQpRMAQueueInfo &head) const noexcept
{
    auto deviceBase = reinterpret_cast<uintptr_t>(qpInfo_) + sizeof(AiQpRMAQueueInfo);
    head.sq = reinterpret_cast<AiQpRMAWQ *>(deviceBase);
    deviceBase += sizeof(AiQpRMAWQ) * rankCount_;
    head.rq = reinterpret_cast<AiQpRMAWQ *>(deviceBase);
    deviceBase += sizeof(AiQpRMAWQ) * rankCount_;
    head.scq = reinterpret_cast<AiQpRMACQ *>(deviceBase);
    deviceBase += sizeof(AiQpRMACQ) * rankCount_;
    head.rcq = reinterpret_cast<AiQpRMACQ *>(deviceBase);
    deviceBase += sizeof(AiQpRMACQ) * rankCount_;
    head.mr = reinterpret_cast<RdmaMemRegionInfo *>(deviceBase);
}

void AicpuJoinableRanksQpManager::CopyAiWQInfo(struct AiQpRMAWQ &dest, const struct ai_data_plane_wq &src,
                                               DBMode dbMode, uint32_t sl) noexcept
{
    dest.wqn = src.wqn;
    dest.bufAddr = src.buf_addr;
    dest.wqeSize = src.wqebb_size;
    dest.depth = src.depth;
    dest.headAddr = src.head_addr;
    dest.tailAddr = src.tail_addr;
    dest.dbMode = dbMode;
    dest.dbAddr = (dbMode == DBMode::SW_DB) ? src.swdb_addr : src.db_reg;
    dest.sl = sl;
}

void AicpuJoinableRanksQpManager::CopyAiCQInfo(struct AiQpRMACQ &dest, const ai_data_plane_cq &src,
                                               DBMode dbMode) noexcept
{
    dest.cqn = src.cqn;
    dest.bufAddr = src.buf_addr;
    dest.cqeSize = src.cqe_size;
    dest.depth = src.depth;
    dest.headAddr = src.head_addr;
    dest.tailAddr = src.tail_addr;
    dest.dbMode = dbMode;
    dest.dbAddr = (dbMode == DBMode::SW_DB) ? src.swdb_addr : src.db_reg;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
