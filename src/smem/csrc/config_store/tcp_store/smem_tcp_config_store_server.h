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
    /* 断链原因：决定是否允许同步持久化存活名单。心跳超时仅表示本窗口内未观测到心跳，可能由
       本进程线程饥饿或被冻结导致观测失效，也可能对端确实停止上报，不等价于对端真的退出。 */
    enum class BrokenReason : uint32_t {
        TCP_BROKEN,        /* acc 层检测到真实链路断开 */
        HEARTBEAT_TIMEOUT, /* 心跳巡检判定超时 */
    };

    /* 心跳记录：除心跳时刻外，记录该时刻链路所属 worker 的累计不可观测时长快照；
       判定时以两次快照之差，从静默时长中扣除本进程不可观测的窗口。stampMs 为单调时钟毫秒。 */
    struct HeartbeatRecord {
        int64_t stampMs{0};    /* 最近一次心跳时刻（单调时钟 ms） */
        uint64_t unobsMs{0};   /* 该时刻所属 worker 的累计不可观测时长(ms) */
        uint32_t workerIdx{0}; /* 链路所属 worker 下标 */
    };

    // 持有 storeMutex_ 拷出集群状态快照：仅做数据拷贝（短持锁），GM 三维度经 GetSnapshot
    // 单次持锁拷贝，保证与 server 级集合同一状态机瞬间
    ClusterStatusSnapshot CaptureClusterStatus() noexcept;

    Result ReceiveMessageHandler(const ock::acc::AccTcpRequestContext &context) noexcept;
    Result LinkConnectedHandler(const ock::acc::AccConnReq &req, const ock::acc::AccTcpLinkComplexPtr &link) noexcept;
    Result LinkBrokenHandler(const ock::acc::AccTcpLinkComplexPtr &link) noexcept;
    Result LinkBrokenHandler(uint32_t linkId, BrokenReason reason) noexcept;

    /* 当前单调时钟(ms)，心跳时间戳与巡检判定的统一时基 */
    static int64_t NowMonotonicMs() noexcept;

    /* 本链路自上次心跳以来其所属 worker 处于不可观测状态的时长(ms)，即应从静默时长中扣除的
       部分。accServer 为调用方在 storeMutex_ 锁内取出的 acc 服务句柄快照；本函数自身不获取
       任何 store 锁。 */
    uint64_t UnobservableDeltaMs(const HeartbeatRecord &record, int64_t rawElapsedMs,
                                 const ock::acc::AccTcpServerPtr &accServer) noexcept;

    /* 由前后两次不可观测时长快照与静默时长计算应扣除量(ms)：快照未增长时返回 0，否则取差值
       并按静默时长封顶（快照差可能大于静默时长）。 */
    static uint64_t ClampUnobservableDeltaMs(uint64_t prevUnobsMs, uint64_t curUnobsMs, int64_t rawElapsedMs) noexcept;

    /* 判定单条链路本次是否心跳超时：静默时长扣除本进程不可观测窗口后与阈值比较。
       timeoutMs <= 0 表示关闭巡检。 */
    static bool IsHeartbeatExpired(int64_t rawElapsedMs, uint64_t unobservableMs, int64_t timeoutMs) noexcept;
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
    /* linkRankMap_ 的独立叶子锁：上层注册的断链回调（memcache 侧 ClearResource → RemoveBlobs）
       经 GetRankIdByLinkId 读本表时并不持有 storeMutex_（回调必须在 storeMutex_ 之外执行），
       故表内读写统一由本锁串行，避免无锁 find 撞上加锁写引起的 rehash UB。
       锁序固定为 storeMutex_ → linkRankMutex_，禁止反向获取。 */
    mutable std::mutex linkRankMutex_;
    /* 心跳巡检判定超时、即将经 ReleaseLink 断链的 linkId 集合（storeMutex_ 保护）。
       ReleaseLink 复用 acc 标准断链路径，回调不携带原因，故用该标记让回调还原为
       HEARTBEAT_TIMEOUT。 */
    std::unordered_set<uint32_t> heartbeatTimeoutLinkIds_;
    /* 后端 KEY_ALIVE_RANK_LIST 的本地镜像：记录"上次成功持久化所代表的集合"（storeMutex_ 保护）。
       心跳超时摘除的 rank 不会写回该名单（见 LinkBrokenHandler），因此镜像可能比 aliveRankSet_ 多出
       这些被保留的 rank；集群全断时据此判断删除会不会连带丢掉它们的恢复窗口。 */
    std::unordered_set<uint32_t> persistedAliveRanks_;
    ConfigStoreServerBrokenHandler externalBrokenHandler_{nullptr};
    /* 上层断链回调（memcache 侧 ClearResource → RemoveBlobs，百万级元数据下为全表扫描）的串行化锁。
       断链回调由 acc 各自的 worker 线程回调（多个 rank 同时断链即多线程并发进入 LinkBrokenHandler），
       修复前该回调在 storeMutex_ 临界区内调用、被全局串行化；为缩短持锁把它移到锁外后，必须用这把
       独立锁（而非 storeMutex_，否则又回到"断链清理全程持 store 锁"）保证同一时刻只有一个回调在执行，
       避免不可重入的元数据回收被并发执行。锁序固定为 externalBrokenCallbackMutex_ → linkRankMutex_
       （回调经 GetRankIdByLinkId 读 linkRankMap_），禁止反向获取。 */
    std::mutex externalBrokenCallbackMutex_;
    /* heartBeatMap_ 使用独立锁保护：心跳写入处于热路径，不能与断链清理（可能秒级持锁）
       共用 storeMutex_，否则清理会同时堵住心跳的"收"与"记"。
       锁序固定为 storeMutex_ → heartBeatMutex_。 */
    std::mutex heartBeatMutex_;
    std::unordered_map<uint32_t, HeartbeatRecord> heartBeatMap_;
    uint32_t heartBeatTimeoutS_;
    std::thread checkerThread_;
    /* 心跳巡检线程标识（storeMutex_ 保护）。LinkBrokenHandler(link) 需区分本次断链是"巡检线程
       经 ReleaseLink 主动发起"（HEARTBEAT_TIMEOUT）还是"acc worker 线程上报的真实 TCP 断链"
       （TCP_BROKEN）：两条路径复用同一回调、均不携带原因，只能用调用线程身份区分。仅在回调确实
       发生在巡检线程上时才信任 heartbeatTimeoutLinkIds_ 标记，避免预置标记把真实断链误判为
       心跳超时（从而漏删 KEY_ALIVE_RANK_LIST，该删不删）。 */
    std::thread::id checkerThreadId_{};
    SmemGroupManagerServerPtr groupManager_;
    SmemGroupCommandSender sender_;
    std::unique_ptr<EtcdStateStore> etcdStore_;
    std::unordered_map<uint32_t, ock::acc::AccTcpLinkComplexPtr> rankLinks_;
    std::unordered_map<uint32_t, uint32_t> linkToRankMap_;

    uint32_t LookupRankIdByLink(uint32_t linkId) noexcept;
    /* persistedAliveRanks_ 中是否存在 aliveRanks 之外、且不等于 excludeRank 的 rank，即"被心跳
       超时保留、尚未确认退出"的项。excludeRank 用于排除本次刚确认退出的 rank：它本就在镜像里，
       不排除会把"该删"误判成"有保留项"而拒绝删除（storeMutex_ 保护，调用方须已持锁）。 */
    bool HasPersistedRanksOutside(const std::unordered_set<uint32_t> &aliveRanks, uint32_t excludeRank) const noexcept;
};
using AccStoreServerPtr = SmRef<AccStoreServer>;
} // namespace ock::smem

#endif // SMEM_SMEM_TCP_CONFIG_STORE_SERVER_H
