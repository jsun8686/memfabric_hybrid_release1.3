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

#ifndef MF_HYBRID_AICPU_JOINABLE_RANKS_QP_MANAGER_H
#define MF_HYBRID_AICPU_JOINABLE_RANKS_QP_MANAGER_H

#include <mutex>
#include <unordered_map>
#include "joinable_ranks_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

/*
 * AICPU 直发场景的 QP 管理器。
 *
 * 复用 JoinableRanksQpManager 的建链逻辑（socket 连接、client/server 角色划分、QP 连接与就绪轮询），
 * 仅把 QP 创建替换为 AI QP（RaQpAiCreate），并把设备侧队列信息（AiQpRMAQueueInfo）整理后下发到 device，
 * 供 AICPU kernel 直接构造 WQE 并敲 NIC 门铃。
 *
 * 说明：初版仅完成 AI QP 创建与队列信息下发；数据面下发与完成检测由上层 transport 负责。
 */
class AicpuJoinableRanksQpManager : public JoinableRanksQpManager {
public:
    AicpuJoinableRanksQpManager(uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount,
                                sockaddr_in devNet, hybm_role_type role) noexcept;
    ~AicpuJoinableRanksQpManager() noexcept override;

    int SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept override;
    int Startup(void *rdma) noexcept override;
    void Shutdown() noexcept override;
    const void *GetQpInfoAddress() const noexcept override;
    uint64_t GetDeviceQpAddr(uint32_t rankId) const noexcept override;

    static constexpr uint32_t INVALID_DB_INDEX = 0xFFFFFFFFU;
    uint32_t GetDbIndex(uint32_t rankId) const noexcept;
    uint32_t GetSendWrDepth() const noexcept
    {
        return sendWrDepth_;
    }

protected:
    int CreateQpHandle(uint32_t rankId, void *&qpHandle) noexcept override;

private:
    bool ReserveQpInfoSpace() noexcept;
    void ReleaseQpInfoSpace() noexcept;
    void FillQpInfo() const noexcept;
    void SetupHostLayout(AiQpRMAQueueInfo &head, uint8_t *base) const noexcept;
    void FillRankEntries(AiQpRMAQueueInfo &head) const noexcept;
    void FixupDevicePointers(AiQpRMAQueueInfo &head) const noexcept;
    static void CopyAiWQInfo(struct AiQpRMAWQ &dest, const struct ai_data_plane_wq &src, DBMode dbMode,
                             uint32_t sl) noexcept;
    static void CopyAiCQInfo(struct AiQpRMACQ &dest, const ai_data_plane_cq &src, DBMode dbMode) noexcept;

private:
    mutable std::mutex aiInfoMutex_;
    std::unordered_map<uint32_t, ConnectRankInfo> currentRanksInfo_;
    std::unordered_map<uint32_t, HccpAiQpInfo> aiQpInfos_;
    AiQpRMAQueueInfo *qpInfo_{nullptr};
    uint32_t qpInfoSize_{0};
    uint32_t sendWrDepth_{0U};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
#endif // MF_HYBRID_AICPU_JOINABLE_RANKS_QP_MANAGER_H
