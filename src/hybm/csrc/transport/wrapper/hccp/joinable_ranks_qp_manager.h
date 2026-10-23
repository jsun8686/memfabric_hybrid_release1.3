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

#ifndef MF_HYBRID_JOINABLE_RANKS_QP_MANAGER_H
#define MF_HYBRID_JOINABLE_RANKS_QP_MANAGER_H

#include <set>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <vector>
#include "device_qp_manager.h"
#include "mf_rwlock.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
class QpCreator {
public:
    QpCreator() noexcept;
    ~QpCreator() noexcept = default;
    int Create(void *rdmaHandle, int flag, int qpMode, void *&qpHandle) noexcept;

private:
    bool useAttr_;
    HccpQpExtAttrs qpExtAttrs_;
};

class JoinableRanksQpManager : public DeviceQpManager {
public:
    JoinableRanksQpManager(uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId, uint32_t rankCount,
                           sockaddr_in devNet, hybm_role_type role) noexcept;
    ~JoinableRanksQpManager() noexcept override;

    int SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept override;
    int RemoveRanks(const std::unordered_set<uint32_t> &ranks) noexcept override;
    int Startup(void *rdma) noexcept override;
    void Shutdown() noexcept override;
    UserQpInfo *GetQpHandleWithRankId(uint32_t rankId) noexcept override;
    void PutQpHandle(UserQpInfo *qp) const noexcept override;
    bool CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept override;
    std::string GetErrorConnectionsStatus(const std::vector<uint32_t> &rankIds) const noexcept override;

private:
    void CloseServices() noexcept;
    int StartServerSide() noexcept;
    int StartClientSide() noexcept;
    void ServerSideRunLoop() noexcept;
    void ClientSideRunLoop() noexcept;
    void ServerSideHandleNewClients(const std::set<uint32_t> &newRanks) noexcept;
    int WaitSocketConnections(const std::set<uint32_t> &newRanks) noexcept;
    bool IsConnectBatchCurrent(const std::set<uint32_t> &ranks) noexcept;
    bool IsConnectRankCurrent(uint32_t rank) noexcept;
    bool IsRankPending(uint32_t rank) const noexcept; // Requires mutex_.
    void MakeQpConnections(const std::set<uint32_t> &newRanks) noexcept;
    void WaitQpConnections(const std::set<uint32_t> &newRanks) noexcept;
    int GenerateWhiteList(const std::set<uint32_t> &newClients) noexcept;
    int CreateConnectionToServers(const std::set<uint32_t> &newServers) noexcept;
    void RemoveRanksProcess(const std::set<uint32_t> &ranks) noexcept;
    std::map<uint32_t, ConnectionChannel> DetachConnections(const std::set<uint32_t> &ranks) noexcept;
    void FinishRemovingRanks(const std::set<uint32_t> &ranks) noexcept;
    void DeleteWhiteList(const std::map<uint32_t, ConnectionChannel> &removedConnections) noexcept;
    void FillHccpTag(char *output, uint32_t peerRank);
    void TrySetQos();

protected:
    virtual int CreateQpHandle(uint32_t rankId, void *&qpHandle) noexcept;
    void *GetRdmaHandle() const noexcept
    {
        return rdmaHandle_;
    }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{true};
    std::shared_ptr<std::thread> clientConnectThread_;
    std::shared_ptr<std::thread> serverConnectThread_;
    void *rdmaHandle_{nullptr};
    MemoryRegionMap currentLocalMrs_;
    std::vector<UserQpInfo *> qpArray_;
    ReadWriteLock qpLock_;
    std::vector<ConnectionChannel> connections_; // connections_操作由mutex_保证不并发
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::set<uint32_t> newClients_;
    std::set<uint32_t> newServers_;
    std::set<uint32_t> removedClientRanks_;
    std::set<uint32_t> removedServerRanks_;
    std::unordered_map<uint32_t, sockaddr_in> replacementNetworks_; // Guarded by mutex_.
    struct QosAttr qosAttr_ {};
    uint32_t userDeviceId_{0};
    QpCreator qpCreator_;
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
#endif // MF_HYBRID_JOINABLE_RANKS_QP_MANAGER_H
