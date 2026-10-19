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
#ifndef SMEM_SMEM_GROUP_MANAGER_SERVER_H
#define SMEM_SMEM_GROUP_MANAGER_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "smem_config_store_backend.h"
#include "smem_group_manager_def.h"
#include "smem_message_packer.h"
#include "smem_ref.h"

namespace ock {

// Upper bound for managed ranks; must stay in sync with SMEM_GROUP_MAX in smem_group_manager.h.
inline constexpr uint32_t SMEM_RANK_MAX = 1024U;

/* Middle-state (EXCHANGING/EXCHANGED/CONNECTING) link retry constants.
 * A non-stable link is re-sent once it has waited linkRetryTimeoutS without
 * receiving an ACK, up to LINK_QUERY_MAX_RETRIES times; after that the link
 * degrades to IDLE and is retried later by the periodic idle-link scan.
 * The wait is configurable via MF_CONFIG_STORE_LINK_RETRY_TIMEOUT_S (seconds).
 */
inline constexpr uint32_t kDefaultLinkRetryTimeoutS = 2U;
inline constexpr uint32_t LINK_QUERY_MAX_RETRIES = 10U;

namespace smem {

using StoreBackendPtr = SmRef<ConfigStoreBackend>;

// Control message sequence number shared by server-side senders (defined in
// smem_tcp_config_store_server.cpp). Declared here so .cpp files include the
// header instead of a local extern declaration (G.EXP.05).
extern std::atomic<uint32_t> g_ctrlReqSeq;

/**
 * Rank lifecycle:
 *
 *   IDLE ──(CheckIn)──► CHECKED_IN ──(Connect)──► ACTIVE
 *     ▲                                              │
 *     └──────────────────(ResetRankState)────────────┘
 *
 * - IDLE:       rank not in the group, no info stored
 * - CHECKED_IN: info stored via CheckIn, not yet accepting connections
 * - ACTIVE:     online and accepting connections
 */
enum RankState : int8_t {
    RANK_IDLE = 0,
    RANK_CHECKED_IN = 1,
    RANK_ACTIVE = 2,
};

/**
 * Directional link state between two ranks (from srcRank to dstRank):
 *
 *   IDLE ──(whitelist add)──► EXCHANGING ──(add response)──► EXCHANGED
 *     ▲                                                          │
 *     │                                          connect request │
 *     │                                                          ▼
 *     │                                                    CONNECTING
 *     │                                                          │
 *     │                                          connect response│
 *     │                                                          ▼
 *     │                                                    CONNECTED
 *     └───────────────(ResetRankState / link broken)─────────────┘
 *
 * - IDLE:           no connection exists
 * - EXCHANGING:     whitelist add request sent, waiting for response
 * - EXCHANGED:      whitelist add response received, info exchanged
 * - CONNECTING:     connect request sent, waiting for response
 * - CONNECTED:      link established and usable
 *
 * Teardown does not use an intermediate state: links go back to IDLE
 * directly via ResetRankState when the rank checks out or the link breaks.
 *
 * Stability:
 *   IDLE and CONNECTED are stable states — no action is triggered proactively
 *   and the link remains in that state indefinitely.
 *   EXCHANGED actively triggers the next event.
 *   EXCHANGING and CONNECTING wait for a response
 *   and retry the event on timeout.
 */
enum LinkState : int8_t {
    LINK_IDLE = 0,
    LINK_EXCHANGING = 1,
    LINK_EXCHANGED = 2,
    LINK_CONNECTING = 3,
    LINK_CONNECTED = 4,
};

/**
 * @brief Manages per-rank state and inter-rank link state.
 *
 * Rank state uses a flat vector of size maxRanks.
 * Link state uses a flat adjacency matrix of size maxRanks x maxRanks
 * stored as a single vector (row-major: index = src * maxRanks + dst).
 *
 * Both vectors grow with maxRanks rather than always allocating
 * SMEM_RANK_MAX entries, so small deployments waste less memory.
 *
 * All public methods are thread-safe (guarded by mutex_).
 */
class SmemGroupManagerServer : public SmReferable {
public:
    explicit SmemGroupManagerServer(SmemGroupCommandSender sender, uint32_t maxRanks = SMEM_RANK_MAX,
                                    uint32_t linkRetryTimeoutS = kDefaultLinkRetryTimeoutS) noexcept;
    ~SmemGroupManagerServer() noexcept override;
    SmemGroupManagerServer(const SmemGroupManagerServer &) = delete;
    SmemGroupManagerServer &operator=(const SmemGroupManagerServer &) = delete;

    void SetSendFunc(SmemGroupCommandSender sender) noexcept
    {
        sender_ = std::move(sender);
    }

    int CheckIn(const RankFullInfo &info, uint64_t reqId = 0) noexcept;
    int ProcessExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices, uint64_t reqId = 0) noexcept;
    int Checkout(uint32_t rankId, uint64_t reqId = 0, const std::string &reason = "client-leavreq") noexcept;

    std::vector<uint32_t> GetAliveRanks() const noexcept;

    /**
     * @brief Query all active ranks for their link state and update the in-memory matrix.
     *
     * Called during Restore(). Sends a QUERY_LINK_STATE request to each active rank;
     * each rank replies with its own link-state entries via OnLinkStateResponse.
     * Links for which no response is received are assumed IDLE.
     */
    void QueryLinkStates() noexcept;

    /**
     * @brief Apply a link-state response from a rank.
     *
     * Called when a LINK_STATE_RESPONSE message is received from a rank.
     * Updates the in-memory link-state matrix for the responding rank, and backfills the
     * role carried by the response.
     */
    void OnLinkStateResponse(const RankFullInfo &rankInfo, const std::vector<LinkStateEntry> &entries,
                             uint64_t reqId = 0) noexcept;

    /**
     * @brief Process an ACK from a client for a previous control command.
     *
     * Advances the link state machine based on the confirmed operation:
     *   AddToWhitelist ACK → EXCHANGING → EXCHANGED
     *   EstablishConnection ACK → CONNECTING → CONNECTED
     * RemoveFromWhitelist ACK does not advance any link: teardown is driven
     * directly by ResetRankState.
     */
    void OnControlAck(ControlOp ackOp, uint32_t senderRankId, const std::map<uint32_t, int32_t> &targetRankRes,
                      uint64_t requestId = 0) noexcept;

    /**
     * @brief Notify the state machine that a rank's TCP connection has broken.
     *
     * Transitions all non-IDLE links involving this rank to IDLE immediately
     * and resets the rank state (if not IDLE) back to IDLE, clearing its
     * stored info, since there is no longer a channel to send control
     * messages to that rank.
     */
    void OnLinkBroken(uint32_t rankId) noexcept;
    void MarkRankReconnected(uint32_t rankId) noexcept;

    // ── 状态访问（供 etcd 持久化使用） ──

    /**
     * @brief 单条链路的转换观测信息。
     *
     * 链路进入中间态（EXCHANGING/EXCHANGED/CONNECTING）时记录状态进入时刻与重试计数；
     * 回到稳定态（CONNECTED/IDLE）时清零。供集群状态快照输出 unstableLinks 明细。
     */
    struct LinkTransition {
        std::chrono::steady_clock::time_point timestamp{};
        uint32_t retryCount{0};
    };

    uint32_t GetMaxRanks() const noexcept
    {
        return maxRanks_;
    }
    std::vector<RankState> GetStates() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return states_;
    }
    std::vector<LinkState> GetLinks() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return links_;
    }
    /**
     * @brief 单次持锁同时拷贝 rank 状态数组与链路矩阵。
     *
     * 供观测/持久化使用：分开调用 GetStates()/GetLinks() 会得到两个不同瞬间的快照，
     * 可能出现 ranks 与 links 互不一致的输出；本接口保证两者来自同一状态机瞬间。
     */
    void GetSnapshot(std::vector<RankState> &states, std::vector<LinkState> &links) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states = states_;
        links = links_;
    }
    /**
     * @brief 单次持锁同时拷贝 rank 状态数组、链路矩阵与链路转换信息。
     *
     * 三参数重载：集群状态快照的 unstableLinks 明细需要每条链路的转换重试计数与
     * 状态进入时刻，且必须与 states/links 来自同一状态机瞬间；锁语义与两参数版本一致。
     */
    void GetSnapshot(std::vector<RankState> &states, std::vector<LinkState> &links,
                     std::vector<LinkTransition> &transitions) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states = states_;
        links = links_;
        transitions = transitions_;
    }

    /**
     * @brief 四参数重载：额外拷贝每 rank 的角色，供集群状态统计剔除剪枝对
     * （PROXY<->PROXY 不建链，不应被报成异常/未完成）。锁语义与三参数版本一致。
     */
    void GetSnapshot(std::vector<RankState> &states, std::vector<LinkState> &links,
                     std::vector<LinkTransition> &transitions, std::vector<uint8_t> &roles) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states = states_;
        links = links_;
        transitions = transitions_;
        roles = rankRole_;
    }

    // ── 状态恢复（供 EtcdStateStore 使用） ──

    void RestoreState(const std::vector<RankState> &states, const std::vector<LinkState> &links,
                      const std::unordered_set<uint32_t> &alive) noexcept;
    void RestoreState(const std::vector<RankState> &states, const std::unordered_set<uint32_t> &alive) noexcept;
    void SetRankBase(uint32_t rankId, const Bytes &data) noexcept;
    void SetRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept;
    // 恢复路径使用：把持久化的 rank 角色写回状态机，保证重启后 PROXY<->PROXY 剪枝仍然生效。
    void SetRankRole(uint32_t rankId, uint8_t role) noexcept;
    uint8_t GetRankRole(uint32_t rankId) const noexcept;
    // 批量版本：恢复时一次持锁写入全部角色（避免 N 次加锁），持久化时一次持锁拷出全部角色。
    void SetRankRoles(const std::vector<uint8_t> &roles) noexcept;
    std::vector<uint8_t> GetRoles() const noexcept;
    void RebuildActiveLinks() noexcept;

    // ── 状态变更回调（供 AccStoreServer 监听内部状态变化） ──

    using StateChangeCallback = std::function<void()>;
    void SetOnStateChangeCallback(StateChangeCallback cb) noexcept
    {
        onStateChange_ = std::move(cb);
    }

    void Start() noexcept;
    void Stop() noexcept;

private:
    uint32_t LinkIndex(uint32_t src, uint32_t dst) const noexcept;

    static constexpr uint32_t linkTransitionDriverIntervalMs = 10U;
    static constexpr uint32_t sendWorkerPollMs = 100U;

    struct LinkScanJob {
        uint32_t srcRank;
        uint32_t dstRank;
        size_t linkIdx;
    };

    // Shared context for one link scan step; aggregates the per-link parameters
    // passed to HandleLink* handlers so each handler keeps a small signature.
    struct LinkScanContext {
        LinkTransition &tr;
        uint32_t srcRank;
        uint32_t dstRank;
        size_t linkIdx;
        int64_t elapsedMs;
        std::chrono::steady_clock::time_point now;
    };

    static bool IsStableLinkState(LinkState st) noexcept;
    // PROXY<->PROXY 之间不建链（分离部署剪枝）；其余角色组合保持全互联。
    [[nodiscard]] bool IsLinkPruned(uint32_t a, uint32_t b) const noexcept;
    void AddActiveLink(size_t idx) noexcept;
    void RemoveActiveLinksForRank(uint32_t rankId) noexcept;
    void DrivePendingTransitions() noexcept;
    void ResetRankState(uint32_t rankId, std::vector<uint32_t> &connectedPeers) noexcept;

    bool ShouldDegradeLink(LinkTransition &tr, size_t idx) noexcept;
    bool IsTimeout(int64_t elapsed) const noexcept;
    void HandleLinkExchanging(LinkScanContext &ctx, std::vector<LinkScanJob> &addJobs) noexcept;
    void HandleLinkExchanged(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept;
    void HandleLinkConnecting(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept;
    struct LinkScanJobs {
        std::vector<LinkScanJob> addJobs;
        std::vector<LinkScanJob> establishJobs;
    };
    void ScanLinkStates(LinkScanContext &ctx, LinkState st, LinkScanJobs &jobs) noexcept;
    void PromoteCheckedInRanks(std::vector<uint32_t> &promotedToActive) noexcept;
    bool HasEnoughConnectedLinks(uint32_t r) noexcept;
    void RetryIdleLinks() noexcept;
    void EnqueueLinkJobs(const LinkScanJobs &jobs) noexcept;
    void TransitionDriverTask() noexcept;

    void UpdateLinkTransitionOnSuccess(LinkState st, size_t linkIdx, LinkTransition &tr,
                                       const std::chrono::steady_clock::time_point &now) noexcept;
    void UpdateLinkTransitionOnFailure(LinkState st, size_t linkIdx, LinkTransition &tr,
                                       const std::chrono::steady_clock::time_point &now) noexcept;

    // Apply a per-target ACK to the link state machine (extracted from OnControlAck)
    bool ApplyAckToLink(ControlOp ackOp, uint32_t senderRankId, uint32_t targetRankId,
                        const std::chrono::steady_clock::time_point &now) noexcept;
    // Pack + send one control message
    int SendPackedMessage(uint32_t srcRank, const SmemMessage &msg) const noexcept;

    enum class SendOp { ADD, REMOVE, ESTABLISH, QUERY_LINK_STATE };

    struct SendTask {
        SendOp op{SendOp::ADD};
        uint32_t srcRank{};
        uint32_t dstRank{};
        size_t linkIdx{};
        uint32_t retryCount{0};
        uint64_t requestId{0};
        Bytes removedBaseInfo{}; // Snapshot of the departed instance, retained across rejoin and send delay.
    };

    void ProcessBatch(const std::vector<SendTask> &tasks) noexcept;
    int SendBatchGroup(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group) noexcept;
    int SendBatchAddRemove(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group,
                           uint64_t reqId) noexcept;
    int SendBatchEstablish(uint32_t srcRank, const std::vector<const SendTask *> &group, uint64_t reqId) noexcept;
    int SendBatchQueryLinkState(uint32_t srcRank) noexcept;
    void UpdateLinksAfterBatch(const std::vector<const SendTask *> &group, int overallRet) noexcept;
    void EnqueueSend(const SendTask &task) noexcept;
    void EnqueueBatchSend(std::vector<SendTask> &&tasks) noexcept;
    void SendWorkerTask() noexcept;
    uint64_t SendTaskKey(const SendTask &t) const noexcept;

    // Request-id encoding: high 32 bits = node identifier, low 32 bits = sequence number.
    static constexpr uint32_t reqIdShift = 32U;

    mutable std::mutex mutex_;
    SmemGroupCommandSender sender_;
    StateChangeCallback onStateChange_;            // fired after Promote/OnControlAck
    std::vector<RankState> states_;                // index = rankId, fixed size after construction
    std::vector<LinkState> links_;                 // row-major: [src * maxRanks_ + dst]
    std::vector<LinkTransition> transitions_;      // parallel to links_, tracks timestamp & retry
    std::vector<Bytes> rankBase_;                  // base info per rank (e.g. endpoint)
    std::vector<std::vector<Bytes>> rankExternal_; // external addresses per rank
    std::vector<uint8_t> rankRole_;                // SMEM_GROUP_RANK_ROLE_* per rank (link-mesh pruning)
    std::vector<bool> isActiveLink_;               // parallel to links_, marks presence in activeLinks_
    std::vector<size_t> activeLinks_;              // current non-stable link indices for O(1) scan
    std::unordered_set<uint32_t> aliveRanks_;
    uint32_t maxRanks_;
    uint32_t linkRetryTimeoutS_{kDefaultLinkRetryTimeoutS};
    std::chrono::steady_clock::time_point lastIdleRetryTime_{};
    std::thread driverThread_;
    std::condition_variable driverCond_;
    std::thread sendThread_;
    std::mutex sendMutex_;
    std::condition_variable sendCond_;
    std::vector<SendTask> sendQueue_;
    std::unordered_set<uint64_t> pendingSendKeys_; // dedup keys: (srcRank<<40)|(op<<32)|(linkIdx&0xffffffff)
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> reqSeq_{0};
};

using SmemGroupManagerServerPtr = SmRef<SmemGroupManagerServer>;

} // namespace smem
} // namespace ock

#endif // SMEM_SMEM_GROUP_MANAGER_SERVER_H
