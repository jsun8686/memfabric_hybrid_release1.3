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
#include <chrono>
#include <cstdio>
#include <sstream>
#include "hybm_logger.h"
#include "hybm_ptracer.h"
#include "dl_hccp_api.h"
#include "dl_acl_api.h"
#include "joinable_ranks_qp_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
constexpr int MR_INFO_ACCESS = 7;
constexpr int WAIT_TIME_MS = 300;
constexpr int QP_MODE = 4;                // HCCP_RDMA_OP_MODE_EXT, same as original RaQpCreate(rdmaHandle, 0, 4)
constexpr uint32_t QP_VERSION = 1;        // QP_CREATE_WITH_ATTR_VERSION required by RaQpCreateWithAttrs
constexpr uint32_t SEND_CQ_DEPTH = 16384; // RS_DRV_CQ_DEPTH, driver default for RaQpCreate
constexpr uint32_t RECV_CQ_DEPTH = 16384; // RS_DRV_CQ_DEPTH, driver default for RaQpCreate
constexpr uint32_t MAX_SEND_WR = 32767;   // RS_QP_32K_DEPTH, driver default for RaQpCreate
constexpr uint32_t MAX_RECV_WR = 32767;   // RS_QP_32K_DEPTH, driver default for RaQpCreate
constexpr uint32_t MAX_SEND_SGE = 1;
constexpr uint32_t MAX_RECV_SGE = 1;
constexpr uint32_t CQ_DEPTH_MIN = 64;    // CQ_DEFAULT_MIN_SEND_DEPTH / CQ_DEFAULT_MIN_RECV_DEPTH
constexpr uint32_t CQ_DEPTH_MAX = 32768; // RS_DRV_CQ_32K_DEPTH
constexpr uint32_t WR_DEPTH_MIN = 64;    // QP_DEFAULT_MIN_CAP_SEND_WR / QP_DEFAULT_MIN_CAP_RECV_WR
constexpr uint32_t WR_DEPTH_MAX = 32767; // RS_QP_32K_DEPTH
static uint32_t GetValidatedDepth(const char *envName, uint32_t envValue, uint32_t defaultValue, uint32_t minValue,
                                  uint32_t maxValue)
{
    if (envValue < minValue || envValue > maxValue) {
        BM_LOG_WARN(envName << " is invalid:" << envValue << ", use default:" << defaultValue);
        return defaultValue;
    }
    return envValue;
}

QpCreator::QpCreator() noexcept
{
    uint32_t sendCqDepth = 0;
    uint32_t recvCqDepth = 0;
    uint32_t maxSendWr = 0;
    uint32_t maxRecvWr = 0;
    bool hasSendCqDepth = MfEnvUtil::GetOptionalUint(env::MF_NPU_RDMA_SEND_CQ_DEPTH, sendCqDepth);
    bool hasRecvCqDepth = MfEnvUtil::GetOptionalUint(env::MF_NPU_RDMA_RECV_CQ_DEPTH, recvCqDepth);
    bool hasMaxSendWr = MfEnvUtil::GetOptionalUint(env::MF_NPU_RDMA_MAX_SEND_WR, maxSendWr);
    bool hasMaxRecvWr = MfEnvUtil::GetOptionalUint(env::MF_NPU_RDMA_MAX_RECV_WR, maxRecvWr);
    if (!hasSendCqDepth && !hasRecvCqDepth && !hasMaxSendWr && !hasMaxRecvWr) {
        useAttr_ = false;
        return;
    }
    useAttr_ = true;
    qpExtAttrs_ = HccpQpExtAttrs{};
    qpExtAttrs_.qpMode = QP_MODE;
    qpExtAttrs_.version = QP_VERSION;
    qpExtAttrs_.cqAttr.sendCqDepth = hasSendCqDepth ? GetValidatedDepth("MF_NPU_RDMA_SEND_CQ_DEPTH", sendCqDepth,
                                                                        SEND_CQ_DEPTH, CQ_DEPTH_MIN, CQ_DEPTH_MAX)
                                                    : SEND_CQ_DEPTH;
    qpExtAttrs_.cqAttr.recvCqDepth = hasRecvCqDepth ? GetValidatedDepth("MF_NPU_RDMA_RECV_CQ_DEPTH", recvCqDepth,
                                                                        RECV_CQ_DEPTH, CQ_DEPTH_MIN, CQ_DEPTH_MAX)
                                                    : RECV_CQ_DEPTH;
    qpExtAttrs_.qp_attr.cap.max_send_wr =
        hasMaxSendWr ? GetValidatedDepth("MF_NPU_RDMA_MAX_SEND_WR", maxSendWr, MAX_SEND_WR, WR_DEPTH_MIN, WR_DEPTH_MAX)
                     : MAX_SEND_WR;
    qpExtAttrs_.qp_attr.cap.max_recv_wr =
        hasMaxRecvWr ? GetValidatedDepth("MF_NPU_RDMA_MAX_RECV_WR", maxRecvWr, MAX_RECV_WR, WR_DEPTH_MIN, WR_DEPTH_MAX)
                     : MAX_RECV_WR;
    qpExtAttrs_.qp_attr.cap.max_send_sge = MAX_SEND_SGE;
    qpExtAttrs_.qp_attr.cap.max_recv_sge = MAX_RECV_SGE;
    qpExtAttrs_.qp_attr.qp_type = IBV_QPT_RC;
    BM_LOG_TRACE("QpCreator use attrs, sendCqDepth:" << qpExtAttrs_.cqAttr.sendCqDepth
                                                     << " recvCqDepth:" << qpExtAttrs_.cqAttr.recvCqDepth
                                                     << " maxSendWr:" << qpExtAttrs_.qp_attr.cap.max_send_wr
                                                     << " maxRecvWr:" << qpExtAttrs_.qp_attr.cap.max_recv_wr);
}

int QpCreator::Create(void *rdmaHandle, int flag, int qpMode, void *&qpHandle) noexcept
{
    if (!useAttr_) {
        return DlHccpApi::RaQpCreate(rdmaHandle, flag, qpMode, qpHandle);
    }
    return DlHccpApi::RaQpCreateWithAttrs(rdmaHandle, &qpExtAttrs_, qpHandle);
}

JoinableRanksQpManager::JoinableRanksQpManager(uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId,
                                               uint32_t rankCount, sockaddr_in devNet, hybm_role_type role) noexcept
    : DeviceQpManager(deviceId, rankId, rankCount, devNet, role)
{
    connections_.resize(rankCount);
    qpArray_.resize(rankCount, nullptr);
    userDeviceId_ = userDeviceId;
}

JoinableRanksQpManager::~JoinableRanksQpManager() noexcept
{
    CloseServices();
}

int JoinableRanksQpManager::SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto it = ranks.begin(); it != ranks.end(); ++it) {
        if (it->first >= rankCount_) {
            continue;
        }
        if (removedServerRanks_.count(it->first) != 0U || removedClientRanks_.count(it->first) != 0U) {
            replacementNetworks_[it->first] = it->second.network;
        } else {
            connections_[it->first].remoteNet = it->second.network;
        }
        if (it->first < rankId_) {
            newServers_.emplace(it->first);
        }
        if (it->first > rankId_) {
            newClients_.emplace(it->first);
        }
    }
    uniqueLock.unlock();

    if (started_.load()) {
        cond_.notify_all();
    }

    return BM_OK;
}

// 上层保证 SetRemoteRankInfo和RemoveRanks操作不并发
int JoinableRanksQpManager::RemoveRanks(const std::unordered_set<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rank : ranks) {
        replacementNetworks_.erase(rank);
        if (rank < rankId_) {
            removedServerRanks_.emplace(rank);
            newServers_.erase(rank);
        } else {
            removedClientRanks_.emplace(rank);
            newClients_.erase(rank);
        }
    }
    uniqueLock.unlock();

    if (started_.load()) {
        cond_.notify_all();
    }

    return BM_OK;
}

int JoinableRanksQpManager::Startup(void *rdma) noexcept
{
    if (rdma == nullptr) {
        BM_LOG_ERROR("input rdma is null");
        return BM_INVALID_PARAM;
    }
    std::unique_lock<std::mutex> unique_lock(mutex_);
    if (started_.load()) {
        BM_LOG_DEBUG("already started.");
        return BM_OK;
    }

    rdmaHandle_ = rdma;
    TrySetQos();
    auto ret = StartServerSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start server side failed: " << ret);
        return ret;
    }

    ret = StartClientSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start client side failed: " << ret);
        return ret;
    }

    started_.store(true);
    return BM_OK;
}

void JoinableRanksQpManager::Shutdown() noexcept
{
    // Join workers before detaching resources; their cancellation checks also acquire mutex_.
    CloseServices();
    std::set<uint32_t> list;
    for (uint32_t i = 0; i < connections_.size(); ++i) {
        list.insert(i);
    }
    RemoveRanksProcess(list);
    std::unique_lock<std::mutex> unique_lock(mutex_);
    started_.store(false);
    running_.store(false);
    rdmaHandle_ = nullptr;
    qpArray_.clear();
    connections_.clear();
    newClients_.clear();
    newServers_.clear();
    removedClientRanks_.clear();
    removedServerRanks_.clear();
    replacementNetworks_.clear();
    unique_lock.unlock();
}

UserQpInfo *JoinableRanksQpManager::GetQpHandleWithRankId(uint32_t rankId) noexcept
{
    if (rankId >= rankCount_) {
        BM_LOG_ERROR("invalid rank id: " << rankId << ", rank count: " << rankCount_);
        return nullptr;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_.load() || removedServerRanks_.count(rankId) != 0U || removedClientRanks_.count(rankId) != 0U) {
        return nullptr;
    }
    ReadGuard lockGuard(qpLock_);
    if (qpArray_[rankId] != nullptr) {
        qpArray_[rankId]->ref.fetch_add(1U);
        return qpArray_[rankId];
    } else {
        return nullptr;
    }
}

void JoinableRanksQpManager::PutQpHandle(UserQpInfo *qp) const noexcept
{
    uint32_t val = qp->ref.fetch_sub(1U);
    if (val == 1U) { // 返回减之前的值
        TP_TRACE_BEGIN(TP_HYBM_RA_QP_DESTROY);
        auto ret = DlHccpApi::RaQpDestroy(qp->qpHandle);
        TP_TRACE_END(TP_HYBM_RA_QP_DESTROY, ret);
        if (ret != 0) {
            BM_LOG_WARN("unable to close qp from " << rankId_ << " , ret: " << ret);
        }
        delete qp;
    }
}

bool JoinableRanksQpManager::CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_.load()) {
        return false;
    }
    for (auto rankId : rankIds) {
        if (rankId == rankId_) {
            continue;
        }
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("invalid rank id: " << rankId << ", rank count: " << rankCount_);
            return false;
        }
        if (removedServerRanks_.count(rankId) != 0U || removedClientRanks_.count(rankId) != 0U ||
            connections_[rankId].qpStatus != 1) {
            return false;
        }
    }
    return true;
}

std::string JoinableRanksQpManager::GetErrorConnectionsStatus(const std::vector<uint32_t> &rankIds) const noexcept
{
    std::ostringstream oss;
    uint32_t okCnt = 0;
    for (auto rankId : rankIds) {
        if (rankId == rankId_) {
            continue;
        }
        if (rankId >= rankCount_) {
            oss << "[rank=" << rankId << ": is out of range]\r\n";
            continue;
        }
        if (connections_[rankId].qpStatus == 1) {
            okCnt++;
            continue;
        }
        oss << "[rank=" << rankId << " : remoteNet = " << DescribeIPv4(connections_[rankId].remoteNet.sin_addr) << ":"
            << connections_[rankId].remoteNet.sin_port << " socketHandle=" << connections_[rankId].socketHandle
            << " socketFd=" << connections_[rankId].socketFd
            << " qpConnectCalled=" << connections_[rankId].qpConnectCalled
            << " qpStatus=" << connections_[rankId].qpStatus << "]\r\n";
    }
    oss << "[success connection ranks count=" << okCnt << "]\r\n";
    return oss.str();
}

void JoinableRanksQpManager::CloseServices() noexcept
{
    running_.store(false);
    cond_.notify_all();
    if (serverConnectThread_ != nullptr) {
        serverConnectThread_->join();
        serverConnectThread_ = nullptr;
    }
    if (clientConnectThread_ != nullptr) {
        clientConnectThread_->join();
        clientConnectThread_ = nullptr;
    }
}

int JoinableRanksQpManager::StartServerSide() noexcept
{
    if (rankId_ + 1U == rankCount_) {
        return BM_OK;
    }

    auto ret = CreateServerSocket();
    if (ret != BM_OK) {
        BM_LOG_ERROR("create server socket failed: " << ret);
        return ret;
    }

    serverConnectThread_ = std::make_shared<std::thread>([this]() { ServerSideRunLoop(); });
    return BM_OK;
}

int JoinableRanksQpManager::StartClientSide() noexcept
{
    if (rankId_ > 0U) {
        clientConnectThread_ = std::make_shared<std::thread>([this]() { ClientSideRunLoop(); });
    }
    return BM_OK;
}

void JoinableRanksQpManager::ServerSideHandleNewClients(const std::set<uint32_t> &newRanks) noexcept
{
    auto ret = GenerateWhiteList(newRanks);
    if (ret != 0) {
        BM_LOG_ERROR("generate white list failed: " << ret);
        return;
    }

    ret = WaitSocketConnections(newRanks);
    if (ret != 0) {
        BM_LOG_ERROR("make socket connections for server side failed: " << ret);
        return;
    }

    MakeQpConnections(newRanks);
    WaitQpConnections(newRanks);
}

void JoinableRanksQpManager::ServerSideRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(userDeviceId_);

    while (running_) {
        std::unique_lock<std::mutex> uniqueLock{mutex_};
        cond_.wait_for(uniqueLock, std::chrono::milliseconds(WAIT_TIME_MS));
        if (newClients_.empty() && removedClientRanks_.empty() && running_) {
            cond_.wait_for(uniqueLock, std::chrono::minutes(1));
        }
        if (!running_) {
            break;
        }
        auto newClients = newClients_;
        auto removedRanks = removedClientRanks_;
        uniqueLock.unlock();

        if (!removedRanks.empty()) {
            RemoveRanksProcess(removedRanks);
        }
        if (!newClients.empty() && IsConnectBatchCurrent(newClients)) {
            ServerSideHandleNewClients(newClients);
        }
    }
}

void JoinableRanksQpManager::ClientSideRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(userDeviceId_);
    while (running_) {
        std::unique_lock<std::mutex> uniqueLock{mutex_};
        cond_.wait_for(uniqueLock, std::chrono::milliseconds(WAIT_TIME_MS));
        if (newServers_.empty() && removedServerRanks_.empty() && running_) {
            cond_.wait_for(uniqueLock, std::chrono::minutes(1));
        }
        if (!running_) {
            break;
        }
        auto newServers = newServers_;
        auto removedRanks = removedServerRanks_;
        uniqueLock.unlock();

        if (!removedRanks.empty()) {
            RemoveRanksProcess(removedRanks);
        }

        if (!newServers.empty() && IsConnectBatchCurrent(newServers)) {
            auto ret = CreateConnectionToServers(newServers);
            if (ret != 0) {
                BM_LOG_ERROR("create connection to server failed: " << ret);
                continue;
            }
            ret = WaitSocketConnections(newServers);
            if (ret != 0) {
                BM_LOG_ERROR("make socket connections for client side failed: " << ret);
                continue;
            }
            MakeQpConnections(newServers);
            WaitQpConnections(newServers);
        }
    }
}

int JoinableRanksQpManager::WaitSocketConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty()) {
        return BM_OK;
    }
    bool socketRole = *newRanks.begin() < rankId_ ? 1 : 0;
    uint32_t cnt = 0;
    std::vector<HccpSocketInfo> socketInfos;
    std::map<std::pair<in_addr_t, std::string>, uint32_t> socketKeyToRank;
    for (auto rankId : newRanks) {
        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            continue;
        }

        if (connections_[rankId].socketFd != nullptr) {
            continue;
        }

        HccpSocketInfo info{};
        info.handle = connections_[rankId].socketHandle;
        info.fd = nullptr;
        info.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        info.status = 0;
        bzero(info.tag, sizeof(info.tag));
        FillHccpTag(info.tag, rankId);
        socketInfos.push_back(info);
        socketKeyToRank.emplace(std::make_pair(info.remoteIp.addr.s_addr, std::string(info.tag)), rankId);
    }

    if (socketInfos.empty()) {
        return BM_OK;
    }

    std::unordered_set<uint32_t> connectedRanks;
    auto startTime = std::chrono::steady_clock::now();
    constexpr auto MAX_WAIT_DURATION = std::chrono::hours(2);
    uint32_t batchCnt = 16U;
    do {
        if (!IsConnectBatchCurrent(newRanks)) {
            return BM_ERROR;
        }
        uint32_t getSize = socketInfos.size() < batchCnt ? socketInfos.size() : batchCnt;
        TP_TRACE_BEGIN(TP_HYBM_RA_GET_SOCKETS);
        auto ret = DlHccpApi::RaGetSockets(socketRole, socketInfos.data(), getSize, cnt);
        TP_TRACE_END(TP_HYBM_RA_GET_SOCKETS, ret);
        if (ret != 0) {
            BM_LOG_ERROR("socketRole(" << socketRole << ") side get sockets failed: " << ret);
            return 1;
        }
        if (!IsConnectBatchCurrent(newRanks)) {
            return BM_ERROR;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100L));
        if (!IsConnectBatchCurrent(newRanks)) {
            return BM_ERROR;
        }
        if (std::chrono::steady_clock::now() - startTime > MAX_WAIT_DURATION) {
            BM_LOG_ERROR("GetSocketFdByAddrs timeout after 2 hours, remaining sockets: " << socketInfos.size());
            return 1;
        }
        if (cnt == 0) {
            continue;
        }
        for (auto i = 0U; i < getSize; i++) {
            if (socketInfos[i].status != 1) {
                continue;
            }
            auto socketInfoPos = socketKeyToRank.find(
                std::make_pair(socketInfos[i].remoteIp.addr.s_addr, std::string(socketInfos[i].tag)));
            if (socketInfoPos == socketKeyToRank.end()) {
                BM_LOG_ERROR("socket ip/tag (" << DescribeIPv4(socketInfos[i].remoteIp.addr) << "/"
                                               << socketInfos[i].tag << ") should not exist.");
                return BM_ERROR;
            }
            auto rankId = socketInfoPos->second;
            if (rankId >= rankCount_) {
                BM_LOG_ERROR("socket ip(" << DescribeIPv4(socketInfos[i].remoteIp.addr) << ") should not exist.");
                continue;
            }
            if (connections_[rankId].socketFd != nullptr) {
                BM_LOG_ERROR("get ip(" << DescribeIPv4(socketInfos[i].remoteIp.addr) << ") already get socket fd.");
                continue;
            }
            connections_[rankId].socketFd = socketInfos[i].fd;
        }
        std::vector<HccpSocketInfo>::iterator it = socketInfos.begin();
        for (; it != socketInfos.end();) {
            if (it->status == 1) {
                it = socketInfos.erase(it);
            } else {
                it++;
            }
        }
    } while (socketInfos.size() > 0);
    return BM_OK;
}

bool JoinableRanksQpManager::IsConnectBatchCurrent(const std::set<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_.load()) {
        return false;
    }
    for (auto rank : ranks) {
        if (rank == rankId_) {
            continue;
        }
        if (rank >= rankCount_) {
            return false;
        }
        if (!IsRankPending(rank)) {
            return false;
        }
    }
    return true;
}

bool JoinableRanksQpManager::IsRankPending(uint32_t rank) const noexcept
{
    const auto &pending = rank < rankId_ ? newServers_ : newClients_;
    const auto &removed = rank < rankId_ ? removedServerRanks_ : removedClientRanks_;
    return pending.count(rank) != 0U && removed.count(rank) == 0U;
}

bool JoinableRanksQpManager::IsConnectRankCurrent(uint32_t rank) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    return running_.load() && rank < rankCount_ && (rank == rankId_ || IsRankPending(rank));
}

void JoinableRanksQpManager::TrySetQos()
{
    uint32_t tc = MfEnvUtil::GetOptionalUintOrDefault(env::MF_DEVICE_RDMA_TC, HCCL_RDMA_TC_DEFAULT);
    uint32_t sl = MfEnvUtil::GetOptionalUintOrDefault(env::MF_DEVICE_RDMA_SL, HCCL_RDMA_SL_DEFAULT);
    if (tc > HCCL_RDMA_TC_MAX || (tc % HCCL_RDMA_TC_BASE != 0)) {
        BM_LOG_WARN(env::MF_DEVICE_RDMA_TC << " is invalid:" << tc);
        tc = HCCL_RDMA_TC_DEFAULT;
    }
    if (sl > HCCL_RDMA_SL_MAX) {
        BM_LOG_WARN(env::MF_DEVICE_RDMA_SL << " is invalid:" << sl);
        sl = HCCL_RDMA_SL_DEFAULT;
    }

    qosAttr_.tc = tc;
    qosAttr_.sl = sl;
}

int JoinableRanksQpManager::CreateQpHandle(uint32_t rankId, void *&qpHandle) noexcept
{
    BM_LOG_DEBUG("create normal rc qp, rank: " << rankId_ << " remoteRank: " << rankId);
    return qpCreator_.Create(rdmaHandle_, 0, QP_MODE, qpHandle);
}

void JoinableRanksQpManager::MakeQpConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty()) {
        return;
    }

    for (auto rankId : newRanks) {
        if (!IsConnectRankCurrent(rankId)) {
            return;
        }
        if (connections_[rankId].socketFd == nullptr) {
            continue;
        }
        if (connections_[rankId].qpHandle == nullptr) {
            void *qpHandle = nullptr;
            auto info = new (std::nothrow) UserQpInfo;
            BM_ASSERT_RET_VOID(info != nullptr, "info is nullptr");
            TP_TRACE_BEGIN(TP_HYBM_RA_QP_CREATE);
            auto ret = CreateQpHandle(rankId, qpHandle);
            TP_TRACE_END(TP_HYBM_RA_QP_CREATE, ret);
            if (ret != 0) {
                BM_LOG_ERROR("create QP to " << rankId << " failed: " << ret);
                delete info;
                continue;
            }
            TP_TRACE_BEGIN(TP_HYBM_RA_SET_QP_ATTR_QOS);
            ret = DlHccpApi::RaSetQpAttrQos(qpHandle, &qosAttr_);
            TP_TRACE_END(TP_HYBM_RA_SET_QP_ATTR_QOS, ret);
            if (ret != 0) {
                BM_LOG_WARN("set qos failed, tc:" << static_cast<uint32_t>(qosAttr_.tc)
                                                  << " sl:" << static_cast<uint32_t>(qosAttr_.sl) << " ret:" << ret);
            }

            connections_[rankId].qpHandle = qpHandle;
            connections_[rankId].qpConnectCalled = false;

            info->ref.store(1U);
            info->qpHandle = qpHandle;
            WriteGuard lockGuard(qpLock_);
            qpArray_[rankId] = info;
        }

        if (!connections_[rankId].qpConnectCalled) {
            if (!IsConnectRankCurrent(rankId)) {
                return;
            }
            TP_TRACE_BEGIN(TP_HYBM_RA_QP_CONNECT_ASYNC);
            auto ret = DlHccpApi::RaQpConnectAsync(connections_[rankId].qpHandle, connections_[rankId].socketFd);
            TP_TRACE_END(TP_HYBM_RA_QP_CONNECT_ASYNC, ret);
            if (ret != 0) {
                BM_LOG_ERROR("create QP from " << rankId_ << " to " << rankId << " failed: " << ret);
                continue;
            }
            BM_LOG_DEBUG("create QP success from " << rankId_ << " to " << rankId);
            connections_[rankId].qpConnectCalled = true;
        }
    }
}

void JoinableRanksQpManager::WaitQpConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty() || !IsConnectBatchCurrent(newRanks)) {
        return;
    }

    std::set<uint32_t> finishedRanks;
    for (auto rankId : newRanks) {
        if (connections_[rankId].qpHandle == nullptr || !connections_[rankId].qpConnectCalled) {
            continue;
        }

        if (connections_[rankId].qpStatus == 1) {
            finishedRanks.emplace(rankId);
            continue;
        }

        TP_TRACE_BEGIN(TP_HYBM_RA_GET_QP_STATUS);
        int qpStatus = -1;
        auto ret = DlHccpApi::RaGetQpStatus(connections_[rankId].qpHandle, qpStatus);
        TP_TRACE_END(TP_HYBM_RA_GET_QP_STATUS, ret);
        if (ret != 0) {
            BM_LOG_ERROR("get QP status to " << rankId << " failed: " << ret);
            continue;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (!IsRankPending(rankId)) {
            continue;
        }
        connections_[rankId].qpStatus = qpStatus;
        if (qpStatus == 1) {
            BM_LOG_DEBUG("from " << rankId_ << " to " << rankId << " query qp ready.");
            finishedRanks.emplace(rankId);
        }
    }

    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rankId : finishedRanks) {
        if (!IsRankPending(rankId)) {
            continue;
        }
        if (rankId < rankId_) {
            newServers_.erase(rankId);
        } else {
            newClients_.erase(rankId);
        }
    }
}

int JoinableRanksQpManager::GenerateWhiteList(const std::set<uint32_t> &newClients) noexcept
{
    std::vector<HccpSocketWhiteListInfo> whitelist;
    std::vector<uint32_t> whitelistRanks;
    for (auto rankId : newClients) {
        if (!IsConnectRankCurrent(rankId)) {
            return BM_ERROR;
        }
        if (rankId <= rankId_ || rankId >= rankCount_) {
            BM_LOG_ERROR("new client rankId: " << rankId << "invalid. self: " << rankId_ << ", total: " << rankCount_);
            return BM_ERROR;
        }

        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            return BM_ERROR;
        }

        if (connections_[rankId].socketHandle != nullptr) {
            continue;
        }

        HccpSocketWhiteListInfo info{};
        info.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        info.connLimit = rankCount_;
        bzero(info.tag, sizeof(info.tag));
        FillHccpTag(info.tag, rankId);
        whitelist.emplace_back(info);
        whitelistRanks.emplace_back(rankId);
    }

    if (whitelist.empty()) {
        return BM_OK;
    }

    uint32_t batchSize = 16;
    for (size_t i = 0; i < whitelist.size(); i += batchSize) {
        size_t currentBatchSize = (whitelist.size() - i) >= batchSize ? batchSize : (whitelist.size() - i);
        std::set<uint32_t> batchRanks(whitelistRanks.begin() + i, whitelistRanks.begin() + i + currentBatchSize);
        if (!IsConnectBatchCurrent(batchRanks)) {
            return BM_ERROR;
        }
        auto batchStart = whitelist.begin() + i;
        auto batchEnd = batchStart + currentBatchSize;
        std::vector<HccpSocketWhiteListInfo> currentBatch(batchStart, batchEnd);
        TP_TRACE_BEGIN(TP_HYBM_RA_SOCKET_WHITE_LIST_ADD);
        auto ret = DlHccpApi::RaSocketWhiteListAdd(serverSocketHandle_, currentBatch.data(), currentBatch.size());
        TP_TRACE_END(TP_HYBM_RA_SOCKET_WHITE_LIST_ADD, ret);
        if (ret != 0) {
            BM_LOG_ERROR("RaSocketWhiteListAdd() with size=" << currentBatch.size() << " failed: " << ret);
            return BM_ERROR;
        }
        for (size_t j = 0; j < currentBatchSize; ++j) {
            connections_[whitelistRanks[i + j]].socketHandle = serverSocketHandle_;
        }
    }

    return BM_OK;
}

int JoinableRanksQpManager::CreateConnectionToServers(const std::set<uint32_t> &newServers) noexcept
{
    std::vector<HccpSocketConnectInfo> connectInfos;
    std::vector<uint32_t> connectRanks;
    std::set<uint32_t> rollbacks;
    auto rollback = [&]() {
        for (auto rank : rollbacks) {
            if (connections_[rank].socketHandle != nullptr) {
                (void)DlHccpApi::RaSocketDeinit(connections_[rank].socketHandle);
                connections_[rank].socketHandle = nullptr;
            }
        }
        rollbacks.clear();
    };
    for (auto rankId : newServers) {
        if (!IsConnectRankCurrent(rankId)) {
            rollback();
            return BM_ERROR;
        }
        if (rankId >= rankId_) {
            BM_LOG_ERROR("new server rankId: " << rankId << "invalid. self: " << rankId_);
            rollback();
            return BM_ERROR;
        }

        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            rollback();
            return BM_ERROR;
        }

        if (connections_[rankId].socketHandle != nullptr) {
            continue;
        }

        auto socketHandle = CreateLocalSocket();
        if (socketHandle == nullptr) {
            BM_LOG_ERROR("create local socket to connect to server: " << rankId << " failed");
            rollback();
            return BM_ERROR;
        }
        connections_[rankId].socketHandle = socketHandle;
        rollbacks.emplace(rankId);

        HccpSocketConnectInfo connectInfo{};
        connectInfo.handle = socketHandle;
        connectInfo.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        connectInfo.port = connections_[rankId].remoteNet.sin_port;
        bzero(connectInfo.tag, sizeof(connectInfo.tag));
        FillHccpTag(connectInfo.tag, rankId);
        BM_LOG_DEBUG("add connecting server " << connectInfo);
        connectInfos.emplace_back(connectInfo);
        connectRanks.emplace_back(rankId);
    }

    if (connectInfos.empty()) {
        return BM_OK;
    }
    uint32_t batchSize = 16;
    for (size_t i = 0; i < connectInfos.size(); i += batchSize) {
        size_t currentBatchSize = (connectInfos.size() - i) >= batchSize ? batchSize : (connectInfos.size() - i);
        std::set<uint32_t> batchRanks(connectRanks.begin() + i, connectRanks.begin() + i + currentBatchSize);
        if (!IsConnectBatchCurrent(batchRanks)) {
            rollback();
            return BM_ERROR;
        }
        auto batchStart = connectInfos.begin() + i;
        auto batchEnd = batchStart + currentBatchSize;
        std::vector<HccpSocketConnectInfo> currentBatch(batchStart, batchEnd);

        TP_TRACE_BEGIN(TP_HYBM_RA_SOCKET_BATCH_CONNECT);
        auto ret = DlHccpApi::RaSocketBatchConnect(currentBatch.data(), currentBatch.size());
        TP_TRACE_END(TP_HYBM_RA_SOCKET_BATCH_CONNECT, ret);
        if (ret != 0) {
            BM_LOG_ERROR("connect to all servers failed: " << ret << ", servers count = " << connectInfos.size());
            rollback();
            return BM_ERROR;
        }
    }
    return BM_OK;
}

std::map<uint32_t, ConnectionChannel>
JoinableRanksQpManager::DetachConnections(const std::set<uint32_t> &ranks) noexcept
{
    std::map<uint32_t, ConnectionChannel> removedConnections;
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto rank : ranks) {
        if (rank >= connections_.size() || rank == rankId_) {
            continue;
        }
        removedConnections.emplace(rank, connections_[rank]);
        connections_[rank] = ConnectionChannel{};
    }
    return removedConnections;
}

void JoinableRanksQpManager::FinishRemovingRanks(const std::set<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto rank : ranks) {
        auto replacement = replacementNetworks_.find(rank);
        if (rank < connections_.size() && replacement != replacementNetworks_.end()) {
            connections_[rank].remoteNet = replacement->second;
            replacementNetworks_.erase(replacement);
        }
        removedServerRanks_.erase(rank);
        removedClientRanks_.erase(rank);
    }
}

void JoinableRanksQpManager::RemoveRanksProcess(const std::set<uint32_t> &ranks) noexcept
{
    auto removedConnections = DetachConnections(ranks);
    for (auto it = removedConnections.begin(); it != removedConnections.end(); ++it) {
        BM_LOG_DEBUG("close connection from " << rankId_ << " to " << it->first);
        if (it->second.qpHandle != nullptr) {
            WriteGuard guard(qpLock_);
            auto info = qpArray_[it->first];
            qpArray_[it->first] = nullptr;
            PutQpHandle(info);
        }

        HccpSocketCloseInfo closeInfo{};
        closeInfo.handle = it->second.socketHandle;
        closeInfo.fd = it->second.socketFd;
        closeInfo.linger = 0;
        if (closeInfo.handle == nullptr) {
            continue;
        }
        TP_TRACE_BEGIN(TP_HYBM_RA_SOCKET_BATCH_CLOSE);
        auto ret = DlHccpApi::RaSocketBatchClose(&closeInfo, 1U);
        TP_TRACE_END(TP_HYBM_RA_SOCKET_BATCH_CLOSE, ret);
        if (ret != 0) {
            BM_LOG_WARN("unable to close socket from " << rankId_ << " to " << it->first << " : " << ret);
        } else {
            BM_LOG_DEBUG("close socket from " << rankId_ << " to " << it->first << " successful: " << ret);
        }
    }

    DeleteWhiteList(removedConnections);
    FinishRemovingRanks(ranks);
}

void JoinableRanksQpManager::DeleteWhiteList(const std::map<uint32_t, ConnectionChannel> &removedConnections) noexcept
{
    if (serverSocketHandle_ == nullptr) {
        return;
    }

    std::vector<HccpSocketWhiteListInfo> whitelist;
    for (auto it = removedConnections.begin(); it != removedConnections.end(); ++it) {
        // 只有 server 侧（rank > self，曾作为 newClients_ 加入白名单）需要清理白名单
        if (it->first <= rankId_ || it->second.remoteNet.sin_addr.s_addr == 0) {
            continue;
        }
        HccpSocketWhiteListInfo info{};
        info.remoteIp.addr = it->second.remoteNet.sin_addr;
        info.connLimit = 0;
        bzero(info.tag, sizeof(info.tag));
        FillHccpTag(info.tag, it->first);
        whitelist.emplace_back(info);
    }

    if (whitelist.empty()) {
        return;
    }

    uint32_t batchSize = 16;
    for (size_t i = 0; i < whitelist.size(); i += batchSize) {
        size_t currentBatchSize = (whitelist.size() - i) >= batchSize ? batchSize : (whitelist.size() - i);
        auto batchStart = whitelist.begin() + i;
        auto batchEnd = batchStart + currentBatchSize;
        std::vector<HccpSocketWhiteListInfo> currentBatch(batchStart, batchEnd);
        auto ret = DlHccpApi::RaSocketWhiteListDel(serverSocketHandle_, currentBatch.data(), currentBatch.size());
        if (ret != 0) {
            BM_LOG_WARN("RaSocketWhiteListDel() with size=" << currentBatch.size() << " failed: " << ret
                                                            << " self=" << rankId_);
        }
    }
}

void JoinableRanksQpManager::FillHccpTag(char *output, uint32_t peerRank)
{
    static constexpr const char *bmTag = "BM_TAG";
    static constexpr const char *undefineTag = "UNDEFINE_TAG";

    if (rankRole_ == HYBM_ROLE_PEER) {
        std::snprintf(output, HCCP_SOCK_CONN_TAG_SIZE, "%s", bmTag);
    } else if (rankRole_ == HYBM_ROLE_SENDER || rankRole_ == HYBM_ROLE_RECEIVER) {
        uint32_t firstRank = std::min(rankId_, peerRank);
        uint32_t secondRank = std::max(rankId_, peerRank);
        std::snprintf(output, HCCP_SOCK_CONN_TAG_SIZE, "TRANS_TAG_%u_%u", firstRank, secondRank);
    } else {
        std::snprintf(output, HCCP_SOCK_CONN_TAG_SIZE, "%s", undefineTag);
    }
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
