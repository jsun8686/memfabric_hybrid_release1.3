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

#ifndef SMEM_SMEM_TCP_CONFIG_STORE_SERVER_H
#define SMEM_SMEM_TCP_CONFIG_STORE_SERVER_H

#include <atomic>
#include <list>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

#include "acc_tcp_server.h"
#include "smem_bm_def.h"
#include "smem_config_store.h"
#include "smem_config_store_backend.h"
#include "smem_group_manager_server.h"
#include "smem_message_packer.h"
#include "etcd_state_store.h"
#include "smem_ref.h"

namespace ock::smem {

/* 集群状态快照 unstableLinks 明细的输出条目上限。
 * join 窗口内全部 N² 条链路依次路过中间态，1024 rank 规模下单次 join 可能瞬时产出
 * 数十万条，超出该上限时按 (retry 降序, elapsedMs 降序) 截断并输出 unstableTruncated
 * 计数。该阈值是对外行为契约（memcache 侧 memcache_restful_api.md 同步登记），
 * 修改需走文档变更。 */
inline constexpr uint32_t kUnstableLinksMaxEntries = 256U;

class StoreWaitContext {
public:
    StoreWaitContext(int64_t tmMs, std::string key, const ock::acc::AccTcpRequestContext &reqCtx) noexcept
        : id_{idGen_.fetch_add(1UL)}, timeoutMs_{tmMs}, key_{std::move(key)}, reqCtx_{reqCtx}
    {}

    [[nodiscard]] uint64_t Id() const noexcept
    {
        return id_;
    }

    [[nodiscard]] int64_t TimeoutMs() const noexcept
    {
        return timeoutMs_;
    }

    [[nodiscard]] const std::string &Key() const noexcept
    {
        return key_;
    }

    [[nodiscard]] const ock::acc::AccTcpRequestContext &ReqCtx() const noexcept
    {
        return reqCtx_;
    }

    ock::acc::AccTcpRequestContext &ReqCtx() noexcept
    {
        return reqCtx_;
    }

private:
    const uint64_t id_;
    const int64_t timeoutMs_;
    const std::string key_;
    ock::acc::AccTcpRequestContext reqCtx_;
    static std::atomic<uint64_t> idGen_;
};

enum StoreServerState : uint32_t { SS_INITED, SS_RECOVERING, SS_RECOVERED, SS_NORMAL, SS_EXITED };

class AccStoreServer : public SmReferable {
public:
    AccStoreServer(std::string ip, uint16_t port, uint32_t worldSize, StoreBackendPtr backend,
                   bool skipRecover) noexcept;
    AccStoreServer(std::string ip, uint16_t port, SmemGroupManagerServerPtr groupManager, StoreBackendPtr backend,
                   bool skipRecover) noexcept;
    ~AccStoreServer() override = default;

    Result Startup(const smem_tls_config &tlsConfig) noexcept;
    void Shutdown(bool afterFork = false) noexcept;
    void RegisterBrokenLinkCHandler(const ConfigStoreServerBrokenHandler &handler) noexcept;

    Result RestoreFromBackend() noexcept;
    bool GetStatus() noexcept;
    Result UpdateStatus(bool status) noexcept;

    // 通过 linkId 查询 rankId，用于外部心跳超时回调
    uint32_t GetRankIdByLinkId(uint32_t linkId) const noexcept;

    /**
     * @brief Cluster status summary of this server as a JSON object string.
     *
     * Fields: server state (SS_*), world size, listen endpoint, backend alive
     * ranks, ranks reconnected after startup, heartbeat count and the
     * group-manager rank/link state summary. Reads thread-safe getters and
     * copies; safe to call from foreign threads (HTTP workers).
     */
    void GetClusterStatusJson(std::string &out) noexcept;
    std::string GetClusterStatusJson() noexcept
    {
        std::string out;
        GetClusterStatusJson(out);
        return out;
    }

    // 集群状态快照：GetClusterStatusJson 的数据形态。锁内一次拷出全部共享状态（copy-out 模式），
    // 锁外做 O(N²) 遍历与 JSON 拼接，避免长持 storeMutex_ 阻塞心跳/CheckIn 等热路径写者
    struct ClusterStatusSnapshot {
        std::string listen;
        std::unordered_set<uint32_t> backendAliveRanks;
        std::unordered_set<uint32_t> aliveRanks;
        std::unordered_set<uint32_t> reconnectedRanks;
        std::vector<std::pair<uint32_t, int64_t>> heartbeats; // (rankId, sinceLastMs)，按 rank 升序
        bool hasGm{false};
        uint32_t maxRanks{0};
        std::vector<RankState> states;
        std::vector<LinkState> links;
        std::vector<SmemGroupManagerServer::LinkTransition> transitions;
        std::vector<uint8_t> roles; // SMEM_GROUP_RANK_ROLE_* per rank，统计时剔除 PROXY<->PROXY 剪枝对
    };

private:
    // 持有 storeMutex_ 拷出集群状态快照：仅做数据拷贝（短持锁），GM 三维度经 GetSnapshot
    // 单次持锁拷贝，保证与 server 级集合同一状态机瞬间
    ClusterStatusSnapshot CaptureClusterStatus() noexcept;

    Result ReceiveMessageHandler(const ock::acc::AccTcpRequestContext &context) noexcept;
    Result LinkConnectedHandler(const ock::acc::AccConnReq &req, const ock::acc::AccTcpLinkComplexPtr &link) noexcept;
    Result LinkBrokenHandler(const ock::acc::AccTcpLinkComplexPtr &link) noexcept;
    Result LinkBrokenHandler(uint32_t linkId) noexcept;
    void GetWakeupList(const std::string &key, std::list<ock::acc::AccTcpRequestContext> &waiters,
                       std::list<ock::acc::AccTcpRequestContext> &watchers) noexcept;

    /* business handler */
    Result SetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result GetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result PrefixGetHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result WatchHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result AddHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result RemoveHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result AppendHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result CasHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result WatchRankStateHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result UnwatchHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result WriteHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HeartbeatHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result QueryAliveHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result ControlHandler(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HandleControlJoin(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HandleControlExtendMemory(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HandleControlLeave(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HandleControlLinkStateResponse(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;
    Result HandleControlAck(const ock::acc::AccTcpRequestContext &context, SmemMessage &request,
                            ControlOp ackOp) noexcept;

    std::list<ock::acc::AccTcpRequestContext> GetOutWaitersInLock(const std::unordered_set<uint64_t> &ids) noexcept;
    void WakeupWaiters(const std::list<ock::acc::AccTcpRequestContext> &waiters,
                       const std::list<ock::acc::AccTcpRequestContext> &watchers,
                       const std::vector<uint8_t> &value) noexcept;
    void ReplyWithMessage(const ock::acc::AccTcpRequestContext &ctx, int16_t code, const std::string &message) noexcept;
    void ReplyWithMessage(const ock::acc::AccTcpRequestContext &ctx, int16_t code,
                          const std::vector<uint8_t> &message) noexcept;
    void TimerThreadTask() noexcept;
    void RankStateTask() noexcept;
    void CheckerThreadTask() noexcept;
    static uint32_t ParseHeartbeatTimeoutS(const std::string &value) noexcept;
    static uint32_t ParseLinkRetryTimeoutS(const std::string &value) noexcept;
    void RestoreFromEtcdIfNeeded() noexcept;
    Result FindOrInsertRank(const ock::acc::AccTcpRequestContext &context, SmemMessage &request) noexcept;

    Result AllocateAndReplyRank(const ock::acc::AccTcpRequestContext &context, SmemMessage &responseMessage,
                                uint32_t linkId, std::unique_lock<std::mutex> &lockGuard) noexcept;
    // Scan for a free rank index, persisting the allocation (extracted from AllocateAndReplyRank).
    // Returns UINT32_MAX when no free rank is available or persist failed.
    uint32_t FindFreeRankIndex(const ock::acc::AccTcpRequestContext &context, uint32_t linkId) noexcept;
    StoreErrorCode PersistWorldSize(uint32_t size) noexcept;
    StoreErrorCode PersistAliveRankIds(const std::unordered_set<uint32_t> &ranks) noexcept;
    StoreErrorCode RecoverAliveRankIds(std::unordered_set<uint32_t> &outRanks) noexcept;
    Result LaunchCleanupThread();
    void CleanupStaleRanks() noexcept;
    bool CanReceiveNewLink();
    void UpdateRecoverState(uint32_t &srcState);
    int SendControlToRank(uint32_t targetRankId, const std::vector<uint8_t> &data) noexcept;

    static constexpr uint32_t MAX_KEY_LEN_SERVER = 2048U;
    static constexpr uint32_t STORE_WAIT_TIMEOUT_SEC = 5U;
    // prevent access broken global value during global static destructor
    const std::string autoRankingStr_ = AutoRankingStr;

    using MessageHandle = int32_t (AccStoreServer::*)(const ock::acc::AccTcpRequestContext &, SmemMessage &);
    const std::unordered_map<MessageType, MessageHandle> requestHandlers_;

    std::mutex storeMutex_;
    std::condition_variable storeCond_;
    std::condition_variable recoveryCond_;
    std::mutex recoveryMutex_;
    StoreBackendPtr backend_;
    std::unordered_map<uint64_t, StoreWaitContext> waitCtx_;
    std::unordered_map<std::string, std::unordered_set<uint64_t>> keyWaiters_;
    ock::acc::AccTcpServerPtr accTcpServer_;
    std::unordered_map<int64_t, std::unordered_set<uint64_t>> timedWaiters_;
    std::thread timerThread_;
    uint64_t startupTimestamp_{0};
    std::atomic<uint32_t> state_{SS_EXITED};
    bool running_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread cleanupThread_;
    std::unordered_set<uint32_t> aliveRankFromBackend_;
    std::unordered_map<uint32_t, StoreWaitContext> rankStateWaiters_;
    std::unordered_map<std::string, std::unordered_map<uint32_t, StoreWaitContext>> watchWaiters_;
    std::unordered_map<uint32_t, std::vector<std::string>> linkWatchList_;
    std::queue<uint32_t> rankStateTaskQueue_;
    std::thread rankStateThread_;

    const std::string listenIp_;
    const uint16_t listenPort_;
    bool skipRecover_;
    uint32_t worldSize_;
    uint32_t rankIndex_{0};
    std::unordered_set<uint32_t> aliveRankSet_;
    std::unordered_set<uint32_t> reconnectedRankSet_;    // only ranks that actually reconnected after startup
    std::unordered_map<uint32_t, uint32_t> linkRankMap_; // linkId → rankId, in-memory only, no etcd
    ConfigStoreServerBrokenHandler externalBrokenHandler_{nullptr};
    std::unordered_map<uint32_t, int64_t> heartBeatMap_;
    uint32_t heartBeatTimeoutS_;
    std::thread checkerThread_;
    SmemGroupManagerServerPtr groupManager_;
    SmemGroupCommandSender sender_;
    std::unique_ptr<EtcdStateStore> etcdStore_;
    std::unordered_map<uint32_t, ock::acc::AccTcpLinkComplexPtr> rankLinks_;
    std::unordered_map<uint32_t, uint32_t> linkToRankMap_;

    uint32_t LookupRankIdByLink(uint32_t linkId) noexcept;
};
using AccStoreServerPtr = SmRef<AccStoreServer>;
} // namespace ock::smem

#endif // SMEM_SMEM_TCP_CONFIG_STORE_SERVER_H
