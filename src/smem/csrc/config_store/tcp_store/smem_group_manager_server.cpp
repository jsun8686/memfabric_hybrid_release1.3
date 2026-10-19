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

#include "smem_group_manager_server.h"

#include <pthread.h>
#include <unordered_map>

#include "hybm_ptracer.h"
#include "smem_config_store_logger.h"
#include "smem_message_packer.h"

namespace ock::smem {

// Each alive rank needs two ADD tasks (src->newRank and newRank->src) to build bidirectional links.
constexpr size_t K_ADD_DIRECTIONS = 2;

// 重发超时以秒配置，而链路状态持续时长（LinkScanContext::elapsedMs）以毫秒统计，比较时统一换算。
constexpr int64_t K_MS_PER_SECOND = 1000;

// SendTaskKey packs srcRank into the high bits, op into bits 32..39 and linkIdx
// into the low 32 bits, so distinct ops on the same link (e.g. REMOVE then CLOSE
// during teardown) are queued independently instead of being deduped together.
constexpr uint32_t K_SEND_TASK_LINK_IDX_MASK = 0xFFFFFFFFU;
constexpr uint32_t K_SEND_TASK_OP_SHIFT = 32U;
constexpr uint32_t K_SEND_TASK_SRC_RANK_SHIFT = 40U;

const char *AckTagName(ControlOp op) noexcept
{
    switch (op) {
        case CONTROL_ADD_TO_WHITELIST_ACK:
            return "ADDWACK";
        case CONTROL_REMOVE_FROM_WHITELIST_ACK:
            return "REMWACK";
        case CONTROL_ESTABLISH_CONNECTION_ACK:
            return "ESTCACK";
        default:
            return "UNKNOWN";
    }
}

SmemGroupManagerServer::SmemGroupManagerServer(SmemGroupCommandSender sender, const uint32_t maxRanks,
                                               const uint32_t linkRetryTimeoutS) noexcept
    : sender_(std::move(sender)), maxRanks_(maxRanks > SMEM_RANK_MAX ? SMEM_RANK_MAX : maxRanks),
      linkRetryTimeoutS_(linkRetryTimeoutS == 0U ? kDefaultLinkRetryTimeoutS : linkRetryTimeoutS)
{
    /*
     * Vectors are sized once on construction and never resized.
     * Using vector instead of raw array or std::array so that
     * when maxRanks is small (e.g. 4 on a single-node test),
     * we don't pay for 1024 entries.
     *
     * links_ is a flat adjacency matrix: index = src * maxRanks_ + dst.
     * For 1024 ranks this is ~1 MiB (1024 * 1024 * 1 byte).
     * transitions_ parallels links_, each cell tracks the timestamp
     * and retry count for the corresponding link state.
     */
    states_.assign(maxRanks_, RANK_IDLE);
    links_.assign(maxRanks_ * maxRanks_, LINK_IDLE);
    transitions_.assign(maxRanks_ * maxRanks_, LinkTransition{});
    rankBase_.resize(maxRanks_);
    rankExternal_.resize(maxRanks_);
    rankRole_.assign(maxRanks_, SMEM_GROUP_RANK_ROLE_UNKNOWN);
    isActiveLink_.assign(maxRanks_ * maxRanks_, false);
}

/*
 * Stop driver and send worker threads.
 */
SmemGroupManagerServer::~SmemGroupManagerServer() noexcept
{
    Stop();
}

/*
 * Returns ranks that are ACTIVE or CHECKED_IN.
 * LEAVING ranks are excluded because they are in teardown.
 * IDLE ranks have never connected.
 */
std::vector<uint32_t> SmemGroupManagerServer::GetAliveRanks() const noexcept
{
    std::vector<uint32_t> alive;
    std::unique_lock<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < maxRanks_; ++i) {
        if (states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) {
            alive.push_back(i);
        }
    }
    return alive;
}

/*
 * Register a rank into the group. Transitions IDLE → CHECKED_IN.
 * Stores rank metadata in-memory and enqueues AddToWhitelist tasks
 * for all existing active ranks.
 * Returns 0 on success, -1 on invalid rankId or duplicate CheckIn.
 */
int SmemGroupManagerServer::CheckIn(const RankFullInfo &info, uint64_t reqId) noexcept
{
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << info.rankId << " type=JOINREQ reqId=" << reqId
                                                  << ", role=" << RankRoleName(info.role));
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_CHECKIN);
    if (info.rankId >= maxRanks_) {
        STORE_LOG_ERROR("CheckIn failed, invalid rankId=" << info.rankId << ", maxRanks=" << maxRanks_
                                                          << ", reqId=" << reqId);
        TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 1);
        return -1;
    }

    std::vector<RankBaseInfo> aliveBases;
    auto now = std::chrono::steady_clock::now();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[info.rankId] != RANK_IDLE) {
            // A Trans entry always joins with a fresh JOINREQ after the store
            // reconnects. On a freshly-started server the earlier reconnect
            // (rec:1) may have marked this rank ACTIVE and enqueued a QUERYREQ,
            // which is stale relative to the new entry. Reset and re-join so the
            // new entity can come up; BM (protocol == DEFAULT) keeps rejecting a
            // non-IDLE duplicate join.
            if (info.protocol != SMEM_RANK_PROTOCOL_TRANS) {
                STORE_LOG_INFO("[GM][Server][Recv] JOINREQ state=" << static_cast<int>(states_[info.rankId])
                                                                   << " protocol=" << static_cast<int>(info.protocol));
                TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 1);
                return -1;
            }
            STORE_LOG_INFO("[GM][Server][Recv] TRANS JOINREQ overrides state=" << static_cast<int>(states_[info.rankId])
                                                                               << " rank=" << info.rankId);
            std::vector<uint32_t> ghostPeers;
            ResetRankState(info.rankId, ghostPeers);
            aliveRanks_.erase(info.rankId);
            RemoveActiveLinksForRank(info.rankId);
        }

        rankBase_[info.rankId] = info.baseInfo;
        rankExternal_[info.rankId] = info.externalInfo;
        // 角色必须先于建链循环写入：IsLinkPruned 会同时读本 rank 与对端 rank 的角色。
        rankRole_[info.rankId] = info.role;

        states_[info.rankId] = RANK_CHECKED_IN;
        aliveRanks_.insert(info.rankId);

        for (uint32_t i = 0; i < maxRanks_; ++i) {
            if ((states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) && i != info.rankId) {
                // 分离部署剪枝：两个 PROXY 之间不建链，双向保持 LINK_IDLE。
                if (IsLinkPruned(info.rankId, i)) {
                    continue;
                }
                uint32_t fwdIdx = LinkIndex(info.rankId, i);
                uint32_t revIdx = LinkIndex(i, info.rankId);
                links_[fwdIdx] = LINK_EXCHANGING;
                links_[revIdx] = LINK_EXCHANGING;
                transitions_[fwdIdx] = {now, 0};
                transitions_[revIdx] = {now, 0};
                AddActiveLink(fwdIdx);
                AddActiveLink(revIdx);

                RankBaseInfo base;
                base.rankId = i;
                base.baseInfo = rankBase_[i];
                aliveBases.push_back(base);
            }
        }
    }

    if (sender_ && !aliveBases.empty()) {
        std::vector<SendTask> addTasks;
        addTasks.reserve(aliveBases.size() * K_ADD_DIRECTIONS);
        for (auto &[rankId, baseInfo] : aliveBases) {
            addTasks.push_back({SendOp::ADD, rankId, info.rankId, LinkIndex(rankId, info.rankId)});
            addTasks.push_back({SendOp::ADD, info.rankId, rankId, LinkIndex(info.rankId, rankId)});
        }
        EnqueueBatchSend(std::move(addTasks));
    }

    TP_TRACE_END(TP_SMEM_GROUP_SERVER_CHECKIN, 0);
    return 0;
}

int SmemGroupManagerServer::ProcessExtendMemory(uint32_t rankId, const MultiBytes &additionalSlices,
                                                uint64_t reqId) noexcept
{
    (void)reqId;
    std::unique_lock<std::mutex> lock(mutex_);
    if (rankId >= maxRanks_) {
        STORE_LOG_ERROR("ProcessExtendMemory: invalid rankId=" << rankId);
        return -1;
    }
    if (states_[rankId] != RANK_ACTIVE) {
        STORE_LOG_ERROR("ProcessExtendMemory: rankId=" << rankId << " not ACTIVE, state=" << states_[rankId]);
        return -1;
    }
    rankExternal_[rankId].insert(rankExternal_[rankId].end(), additionalSlices.begin(), additionalSlices.end());
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=EXTMEM slices=" << additionalSlices.size());

    // Broadcast ADD_SLICES to all active peers via sender callback
    if (sender_) {
        SmemMessage addMsg = SmemMessage::PackAddSlices(rankId, additionalSlices);
        auto packed = SmemMessagePacker::Pack(addMsg);
        for (uint32_t peerId : aliveRanks_) {
            if (peerId != rankId) {
                sender_(peerId, packed);
            }
        }
        std::vector<uint32_t> aliveVec(aliveRanks_.begin(), aliveRanks_.end());
        STORE_LOG_INFO("[GM][Server][Send] type=ADD_SLICES extendingRank=" << rankId << " peers=" << aliveVec.size());
    }

    return 0;
}

void SmemGroupManagerServer::MarkRankReconnected(uint32_t rankId) noexcept
{
    if (rankId >= maxRanks_) {
        STORE_LOG_ERROR("mark rank reconnected failed, rankId out of range: " << rankId);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (states_[rankId] != RANK_IDLE) {
        lock.unlock();
        STORE_LOG_WARN("mark rank:" << rankId << " reconnected, but state not idle: " << states_[rankId]);
        return;
    }

    states_[rankId] = RANK_ACTIVE;
    aliveRanks_.insert(rankId);

    if (sender_) {
        lock.unlock();
        STORE_LOG_INFO("[GM][Server][Send] rank=" << rankId << " type=QUERYREQ");
        EnqueueSend({SendOp::QUERY_LINK_STATE, rankId, 0, 0, 0, 0});
    }
}

/*
 * Deregister a rank from the group.
 * Immediately transitions all links to IDLE, broadcasts RemoveFromWhitelist to peers,
 * and clears in-memory rank metadata.
 */
void SmemGroupManagerServer::ResetRankState(uint32_t rankId, std::vector<uint32_t> &connectedPeers) noexcept
{
    states_[rankId] = RANK_IDLE;
    rankBase_[rankId].clear();
    rankExternal_[rankId].clear();
    rankRole_[rankId] = SMEM_GROUP_RANK_ROLE_UNKNOWN;

    for (uint32_t i = 0; i < maxRanks_; ++i) {
        if (i == rankId) {
            continue;
        }
        if (states_[i] != RANK_ACTIVE && states_[i] != RANK_CHECKED_IN) {
            continue;
        }

        uint32_t fwdIdx = LinkIndex(rankId, i);
        uint32_t revIdx = LinkIndex(i, rankId);
        if (links_[fwdIdx] != LINK_IDLE || links_[revIdx] != LINK_IDLE) {
            connectedPeers.push_back(i);
        }
        links_[fwdIdx] = LINK_IDLE;
        transitions_[fwdIdx] = {};
        links_[revIdx] = LINK_IDLE;
        transitions_[revIdx] = {};
    }
}

int SmemGroupManagerServer::Checkout(uint32_t rankId, uint64_t reqId, const std::string &reason) noexcept
{
    STORE_LOG_INFO("[GM][Server] rank=" << rankId << " Checkout reason=" << reason << " reqId=" << reqId);
    if (rankId >= maxRanks_) {
        return -1;
    }

    std::vector<uint32_t> connectedPeers;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[rankId] != RANK_CHECKED_IN && states_[rankId] != RANK_ACTIVE) {
            return -1;
        }
        const Bytes removedBaseInfo = rankBase_[rankId];
        ResetRankState(rankId, connectedPeers);
        aliveRanks_.erase(rankId);

        if (sender_ && !connectedPeers.empty()) {
            for (uint32_t peerId : connectedPeers) {
                EnqueueSend({SendOp::REMOVE, peerId, rankId, LinkIndex(peerId, rankId), 0, 0, removedBaseInfo});
            }
        }
    }

    STORE_LOG_INFO("rank=" << rankId << " LEAVE done, notify peers=" << connectedPeers.size());
    return 0;
}

/*
 * Compute the row-major index for the directional link src→dst.
 */
uint32_t SmemGroupManagerServer::LinkIndex(uint32_t src, uint32_t dst) const noexcept
{
    return src * maxRanks_ + dst;
}

bool SmemGroupManagerServer::IsLinkPruned(uint32_t a, uint32_t b) const noexcept
{
    if (a >= maxRanks_ || b >= maxRanks_) {
        return false;
    }
    return rankRole_[a] == SMEM_GROUP_RANK_ROLE_PROXY && rankRole_[b] == SMEM_GROUP_RANK_ROLE_PROXY;
}

void SmemGroupManagerServer::QueryLinkStates() noexcept
{
    std::vector<uint32_t> alive;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < maxRanks_; ++i) {
            if (states_[i] == RANK_ACTIVE || states_[i] == RANK_CHECKED_IN) {
                alive.push_back(i);
            }
        }
    }

    if (sender_) {
        for (auto rankId : alive) {
            auto msg = SmemMessage::PackQueryLinkState(rankId);
            auto packed = SmemMessagePacker::Pack(msg);
            int qRet = -1;
            if (!packed.empty()) {
                qRet = sender_(rankId, packed);
            }
            if (qRet != 0) {
                STORE_LOG_WARN("[GM][Server][Send] QueryLinkStates: failed for rank " << rankId << ": " << qRet);
            }
        }
    }
}

void SmemGroupManagerServer::OnLinkStateResponse(const RankFullInfo &rankInfo,
                                                 const std::vector<LinkStateEntry> &entries, uint64_t reqId) noexcept
{
    const uint32_t rankId = rankInfo.rankId;
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=LNKSRSP reqId=" << reqId
                                                  << ", role=" << RankRoleName(rankInfo.role));
    if (rankId >= maxRanks_) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    rankBase_[rankId] = rankInfo.baseInfo;
    rankExternal_[rankId] = rankInfo.externalInfo;
    // 重连路径（rec:1）不发 JOINREQ，非 etcd 模式又没有角色持久化载体：这里与
    // base/external 并列，靠对端上报把 rankRole_ 回填，让剪枝判定随重连自愈；
    // etcd 模式下则作为持久化结果的校验/兜底。必须早于下面的剪枝判定。
    const uint8_t previousRole = rankRole_[rankId];
    rankRole_[rankId] = rankInfo.role;
    // 角色被覆盖是排查剪枝失效的关键节点：PROXY 被改写成非 PROXY 会让该 rank 的剪枝静默
    // 失效，必须 WARN 留痕；其余角色的覆盖只记 INFO。
    if (previousRole != rankInfo.role) {
        if (previousRole == SMEM_GROUP_RANK_ROLE_PROXY && rankInfo.role != SMEM_GROUP_RANK_ROLE_PROXY) {
            STORE_LOG_WARN("[GM][Server] rank role downgraded from proxy to "
                           << RankRoleName(rankInfo.role) << " by LNKSRSP. rank=" << rankId
                           << ", pruning for this rank is now disabled");
        } else {
            STORE_LOG_INFO("[GM][Server] rank role overwritten by LNKSRSP. rank=" << rankId << ", role="
                                                                                  << RankRoleName(rankInfo.role));
        }
    }
    aliveRanks_.insert(rankId);
    for (const auto &entry : entries) {
        if (entry.dstRankId >= maxRanks_ || aliveRanks_.find(entry.dstRankId) == aliveRanks_.end()) {
            continue;
        }
        // 剪枝对：忽略对端上报的链路状态，强制保持 IDLE，避免它进入重试/降级扫描。
        if (IsLinkPruned(rankId, entry.dstRankId)) {
            links_[LinkIndex(rankId, entry.dstRankId)] = LINK_IDLE;
            continue;
        }
        auto idx = LinkIndex(rankId, entry.dstRankId);
        auto newSt = static_cast<LinkState>(entry.state);
        links_[idx] = newSt;
        if (!IsStableLinkState(newSt)) {
            AddActiveLink(idx);
        }
    }
}

bool SmemGroupManagerServer::ShouldDegradeLink(LinkTransition &tr, size_t idx) noexcept
{
    if (tr.retryCount >= LINK_QUERY_MAX_RETRIES) {
        const uint32_t src = static_cast<uint32_t>(idx / maxRanks_);
        const uint32_t dst = static_cast<uint32_t>(idx % maxRanks_);
        STORE_LOG_WARN("[GM][Server][Event] link degrade to IDLE after " << tr.retryCount << " retries, src=" << src
                                                                         << " tgt=" << dst);
        links_[idx] = LINK_IDLE;
        tr = {};
        return true;
    }
    return false;
}

bool SmemGroupManagerServer::IsTimeout(int64_t elapsed) const noexcept
{
    return elapsed >= static_cast<int64_t>(linkRetryTimeoutS_) * K_MS_PER_SECOND;
}

void SmemGroupManagerServer::HandleLinkExchanging(LinkScanContext &ctx, std::vector<LinkScanJob> &addJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        STORE_LOG_WARN("[GM][Server][Event] link EXCHANGING retry " << ctx.tr.retryCount << "/"
                                                                    << LINK_QUERY_MAX_RETRIES << " src=" << ctx.srcRank
                                                                    << " tgt=" << ctx.dstRank << ", will retry later");
        addJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkExchanged(LinkScanContext &ctx, std::vector<LinkScanJob> &establishJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (ctx.tr.retryCount == 0 || IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        STORE_LOG_WARN("[GM][Server][Event] link EXCHANGED retry " << ctx.tr.retryCount << "/" << LINK_QUERY_MAX_RETRIES
                                                                   << " src=" << ctx.srcRank << " tgt=" << ctx.dstRank
                                                                   << ", will retry later");
        establishJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::HandleLinkConnecting(LinkScanContext &ctx,
                                                  std::vector<LinkScanJob> &establishJobs) noexcept
{
    if (ShouldDegradeLink(ctx.tr, ctx.linkIdx)) {
        return;
    }
    if (IsTimeout(ctx.elapsedMs)) {
        ctx.tr.retryCount++;
        STORE_LOG_WARN("[GM][Server][Event] link CONNECTING retry " << ctx.tr.retryCount << "/"
                                                                    << LINK_QUERY_MAX_RETRIES << " src=" << ctx.srcRank
                                                                    << " tgt=" << ctx.dstRank << ", will retry later");
        establishJobs.push_back({ctx.srcRank, ctx.dstRank, ctx.linkIdx});
    }
}

void SmemGroupManagerServer::ScanLinkStates(LinkScanContext &ctx, LinkState st, LinkScanJobs &jobs) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
            HandleLinkExchanging(ctx, jobs.addJobs);
            break;
        case LINK_EXCHANGED:
            HandleLinkExchanged(ctx, jobs.establishJobs);
            break;
        case LINK_CONNECTING:
            HandleLinkConnecting(ctx, jobs.establishJobs);
            break;
        default:
            break;
    }
}

/*
 * Promote CHECKED_IN ranks to ACTIVE once all their links reach stable states
 * and at least half of the peer links are CONNECTED.
 * Populates promotedToActive for caller to persist state changes.
 */
void SmemGroupManagerServer::PromoteCheckedInRanks(std::vector<uint32_t> &promotedToActive) noexcept
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_PROMOTE);
    for (uint32_t r = 0; r < maxRanks_; ++r) {
        if (states_[r] != RANK_CHECKED_IN) {
            continue;
        }
        bool allStable = true;
        for (uint32_t j = 0; j < maxRanks_; ++j) {
            if (j == r) {
                continue;
            }
            // 剪枝对（PROXY<->PROXY）不参与稳定性判定：它永远不会被建链。
            if (IsLinkPruned(r, j)) {
                continue;
            }
            LinkState ls = links_[LinkIndex(r, j)];
            if (ls != LINK_IDLE && ls != LINK_CONNECTED) {
                allStable = false;
                break;
            }
        }
        if (allStable && HasEnoughConnectedLinks(r)) {
            states_[r] = RANK_ACTIVE;
            promotedToActive.push_back(r);
            STORE_LOG_INFO("rank=" << r << " state=ACTIVE (all links stable, enough connected)");
        }
    }
    TP_TRACE_END(TP_SMEM_GROUP_SERVER_PROMOTE, 0);
}

bool SmemGroupManagerServer::HasEnoughConnectedLinks(uint32_t r) noexcept
{
    uint32_t connected = 0;
    uint32_t peers = 0;
    for (uint32_t j = 0; j < maxRanks_; ++j) {
        if (j == r) {
            continue;
        }
        if (states_[j] != RANK_ACTIVE && states_[j] != RANK_CHECKED_IN) {
            continue;
        }
        // 剪枝对不计入期望链路数，否则 PROXY rank 会因为没有 PROXY<->PROXY 链路而永远无法 promote。
        if (IsLinkPruned(r, j)) {
            continue;
        }
        peers++;
        if (links_[LinkIndex(r, j)] == LINK_CONNECTED) {
            connected++;
        }
    }
    if (peers == 0) {
        return true;
    }
    return connected == peers;
}

void SmemGroupManagerServer::RetryIdleLinks() noexcept
{
    constexpr int64_t kIdleLinkRetryIntervalMs = 30000;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastIdleRetryTime_).count();
    if (elapsed < kIdleLinkRetryIntervalMs) {
        return;
    }
    lastIdleRetryTime_ = now;

    for (uint32_t r = 0; r < maxRanks_; ++r) {
        if (states_[r] != RANK_ACTIVE && states_[r] != RANK_CHECKED_IN) {
            continue;
        }
        for (uint32_t j = 0; j < maxRanks_; ++j) {
            if (j == r) {
                continue;
            }
            if (states_[j] != RANK_ACTIVE && states_[j] != RANK_CHECKED_IN) {
                continue;
            }
            // 剪枝对（PROXY<->PROXY）必须保持 IDLE：否则 30s 一次的断链重试会把剪掉的链路重新拉起。
            if (IsLinkPruned(r, j)) {
                continue;
            }
            uint32_t idx = LinkIndex(r, j);
            if (links_[idx] == LINK_IDLE) {
                links_[idx] = LINK_EXCHANGING;
                transitions_[idx] = {now, 0};
                AddActiveLink(idx);
            }
        }
    }
}

/*
 * Convert ScanLinkStates results into EnqueueSend calls for the send worker.
 */
void SmemGroupManagerServer::EnqueueLinkJobs(const LinkScanJobs &jobs) noexcept
{
    auto count = jobs.addJobs.size() + jobs.establishJobs.size();
    if (count == 0) {
        return;
    }

    std::vector<SendTask> batch;
    batch.reserve(count);

    auto append = [&batch](const auto &jobs, SendOp op) {
        for (auto &job : jobs) {
            batch.push_back({op, job.srcRank, job.dstRank, job.linkIdx});
        }
    };
    append(jobs.addJobs, SendOp::ADD);
    append(jobs.establishJobs, SendOp::ESTABLISH);

    EnqueueBatchSend(std::move(batch));
}

bool SmemGroupManagerServer::IsStableLinkState(LinkState st) noexcept
{
    return st == LINK_IDLE || st == LINK_CONNECTED;
}

void SmemGroupManagerServer::RemoveActiveLinksForRank(uint32_t rankId) noexcept
{
    for (uint32_t i = 0; i < maxRanks_; ++i) {
        if (i == rankId) {
            continue;
        }
        for (size_t idx : {LinkIndex(rankId, i), LinkIndex(i, rankId)}) {
            if (!isActiveLink_[idx]) {
                continue;
            }
            isActiveLink_[idx] = false;
            for (size_t j = 0; j < activeLinks_.size(); ++j) {
                if (activeLinks_[j] == idx) {
                    activeLinks_[j] = activeLinks_.back();
                    activeLinks_.pop_back();
                    break;
                }
            }
        }
    }
}

void SmemGroupManagerServer::AddActiveLink(size_t idx) noexcept
{
    if (isActiveLink_[idx]) {
        return;
    }
    if (IsStableLinkState(links_[idx])) {
        return;
    }
    isActiveLink_[idx] = true;
    activeLinks_.push_back(idx);
}

/*
 * Main driver: scan all non-stable links and enqueue actions for expired
 * states, promote CHECKED_IN ranks.
 * Called periodically by TransitionDriverTask.
 */
void SmemGroupManagerServer::DrivePendingTransitions() noexcept
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_SERVER_DRIVE_TRANSITIONS);
    LinkScanJobs jobs;
    auto now = std::chrono::steady_clock::now();

    std::vector<uint32_t> promotedToActive;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (size_t i = 0; i < activeLinks_.size();) {
            size_t idx = activeLinks_[i];
            LinkState st = links_[idx];
            if (IsStableLinkState(st)) {
                isActiveLink_[idx] = false;
                activeLinks_[i] = activeLinks_.back();
                activeLinks_.pop_back();
                continue;
            }
            auto src = static_cast<uint32_t>(idx / maxRanks_);
            auto dst = static_cast<uint32_t>(idx % maxRanks_);
            LinkTransition &tr = transitions_[idx];
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - tr.timestamp).count();
            LinkScanContext ctx{tr, src, dst, idx, elapsed, now};
            ScanLinkStates(ctx, st, jobs);
            ++i;
        }
        PromoteCheckedInRanks(promotedToActive);
        RetryIdleLinks();
    }

    // Notify newly promoted ranks (outside lock — sender_ may block)
    for (auto r : promotedToActive) {
        auto msg = SmemMessage::PackPromoteToActive(r);
        auto packed = SmemMessagePacker::Pack(msg);
        if (!packed.empty()) {
            sender_(r, packed);
            STORE_LOG_INFO("[GM][Server][Send] rank=" << r << " type=PROMOTE_TO_ACTIVE");
        }
    }

    if (!promotedToActive.empty() && onStateChange_) {
        onStateChange_();
    }

    EnqueueLinkJobs(jobs);
    TP_TRACE_END(TP_SMEM_GROUP_SERVER_DRIVE_TRANSITIONS, 0);
}

/*
 * Advance the link state machine on receipt of a client ACK.
 * Only advances if the link is in the expected state for that ACK type.
 * Stale / duplicate ACKs are safely ignored.
 */
void SmemGroupManagerServer::OnControlAck(ControlOp ackOp, uint32_t senderRankId,
                                          const std::map<uint32_t, int32_t> &targetRankRes, uint64_t requestId) noexcept
{
    auto tag = AckTagName(ackOp);
    std::string ok;
    std::string nok;
    for (auto &kv : targetRankRes) {
        std::string &list = (kv.second == 0) ? ok : nok;
        if (!list.empty()) {
            list += ",";
        }
        list += std::to_string(kv.first);
    }
    std::string extra;
    if (!ok.empty()) {
        extra += " ok=(" + ok + ")";
    }
    if (!nok.empty()) {
        extra += " nok=(" + nok + ")";
    }
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << senderRankId << " type=" << tag << " rid=" << requestId << extra);

    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    bool linksChanged = false;
    for (auto &kv : targetRankRes) {
        uint32_t targetRankId = kv.first;
        if (kv.second != 0) {
            STORE_LOG_INFO("[GM][Server][Event] ackOp=" << tag << " src=" << senderRankId << " tgt=" << targetRankId);
            continue;
        }
        if (ApplyAckToLink(ackOp, senderRankId, targetRankId, now)) {
            linksChanged = true;
        }
    }
    lock.unlock();
    if (linksChanged && onStateChange_) {
        onStateChange_();
    }
}

bool SmemGroupManagerServer::ApplyAckToLink(ControlOp ackOp, uint32_t senderRankId, uint32_t targetRankId,
                                            const std::chrono::steady_clock::time_point &now) noexcept
{
    if (senderRankId >= maxRanks_ || targetRankId >= maxRanks_) {
        return false;
    }
    size_t idx = LinkIndex(senderRankId, targetRankId);
    LinkState &st = links_[idx];
    LinkTransition &tr = transitions_[idx];

    switch (ackOp) {
        case CONTROL_ADD_TO_WHITELIST_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_ADD);
            bool advanced = (st == LINK_EXCHANGING);
            if (advanced) {
                st = LINK_EXCHANGED;
                tr = {now, 0};
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_ADD, 0);
            return advanced;
        }
        case CONTROL_ESTABLISH_CONNECTION_ACK: {
            TP_TRACE_BEGIN(TP_SMEM_GROUP_ON_ACK_ESTABLISH);
            bool advanced = (st == LINK_CONNECTING);
            if (advanced) {
                st = LINK_CONNECTED;
                tr = {};
                STORE_LOG_INFO("link CONNECTED sender=" << senderRankId << " target=" << targetRankId);
            }
            TP_TRACE_END(TP_SMEM_GROUP_ON_ACK_ESTABLISH, 0);
            return advanced;
        }
        default:
            break;
    }
    return false;
}

void SmemGroupManagerServer::OnLinkBroken(uint32_t rankId) noexcept
{
    STORE_LOG_INFO("[GM][Server][Recv] RCV rank=" << rankId << " type=LEAVREQ reason=LinkBroken");
    if (rankId >= maxRanks_) {
        return;
    }

    std::vector<uint32_t> connectedPeers;

    Bytes removedBaseInfo;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (states_[rankId] != RANK_CHECKED_IN && states_[rankId] != RANK_ACTIVE) {
            return;
        }
        removedBaseInfo = rankBase_[rankId];
        ResetRankState(rankId, connectedPeers);
        aliveRanks_.erase(rankId);
    }

    STORE_LOG_INFO("rank=" << rankId << " linkBroken connectedPeers=" << connectedPeers.size());
    if (sender_ && !connectedPeers.empty()) {
        for (uint32_t peerId : connectedPeers) {
            EnqueueSend({SendOp::REMOVE, peerId, rankId, LinkIndex(peerId, rankId), 0, 0, removedBaseInfo});
        }
    }
}

/*
 * Execute a batch of send tasks grouped by (srcRank, op).
 * Groups tasks and delegates to SendBatchGroup + UpdateLinksAfterBatch.
 */
void SmemGroupManagerServer::ProcessBatch(const std::vector<SendTask> &tasks) noexcept
{
    if (!sender_) {
        return;
    }

    std::unordered_map<uint64_t, std::vector<const SendTask *>> groups;
    groups.reserve(tasks.size());
    for (auto &t : tasks) {
        uint64_t key = (static_cast<uint64_t>(t.srcRank) << reqIdShift) | static_cast<uint32_t>(t.op);
        groups[key].push_back(&t);
    }

    for (auto &kv : groups) {
        auto &g = kv.second;
        int ret = SendBatchGroup(g[0]->srcRank, g[0]->op, g);
        UpdateLinksAfterBatch(g, ret);
    }
}

int SmemGroupManagerServer::SendBatchAddRemove(uint32_t srcRank, SendOp op, const std::vector<const SendTask *> &group,
                                               uint64_t reqId) noexcept
{
    SmemMessage msg{MessageType::CONTROL};
    if (op == SendOp::ADD) {
        std::vector<RankFullInfo> bases;
        bases.reserve(group.size());
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto *t : group) {
                RankFullInfo base;
                base.rankId = t->dstRank;
                base.baseInfo = rankBase_[t->dstRank];
                base.externalInfo = rankExternal_[t->dstRank];
                bases.push_back(base);
            }
        }
        msg = SmemMessage::PackAddToWhitelist(srcRank, bases);
    } else {
        std::vector<RankBaseInfo> bases;
        bases.reserve(group.size());
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto *t : group) {
                RankBaseInfo base;
                base.rankId = t->dstRank;
                base.baseInfo = t->removedBaseInfo;
                bases.push_back(base);
            }
        }
        msg = SmemMessage::PackRemoveFromWhitelist(srcRank, bases);
    }
    msg.requestId = reqId;
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchEstablish(uint32_t srcRank, const std::vector<const SendTask *> &group,
                                               uint64_t reqId) noexcept
{
    std::vector<RankFullInfo> fulls;
    fulls.reserve(group.size());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto *t : group) {
            RankFullInfo full;
            full.rankId = t->dstRank;
            full.baseInfo = rankBase_[t->dstRank];
            full.externalInfo = rankExternal_[t->dstRank];
            fulls.push_back(full);
        }
    }
    auto msg = SmemMessage::PackEstablishConnection(srcRank, fulls);
    msg.requestId = reqId;
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchQueryLinkState(uint32_t srcRank) noexcept
{
    auto msg = SmemMessage::PackQueryLinkState(srcRank);
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

int SmemGroupManagerServer::SendBatchGroup(uint32_t srcRank, SendOp op,
                                           const std::vector<const SendTask *> &group) noexcept
{
    static const char *opTag[] = {"ADDWLST", "REMWLST", "ESTCONN", "QRY_LST"};
    const char *tag = (static_cast<size_t>(op) < std::size(opTag)) ? opTag[static_cast<size_t>(op)] : "??????";
    uint64_t reqId = group.empty() ? 0 : group.front()->requestId;
    std::string tgt;
    for (size_t i = 0; i < group.size(); ++i) {
        if (i > 0) {
            tgt += ",";
        }
        tgt += std::to_string(group[i]->dstRank);
    }
    STORE_LOG_INFO("[GM][Server][Send] rank=" << srcRank << " type=" << tag << " tgt=(" << tgt << ")");
    switch (op) {
        case SendOp::ADD:
        case SendOp::REMOVE:
            return SendBatchAddRemove(srcRank, op, group, reqId);
        case SendOp::ESTABLISH:
            return SendBatchEstablish(srcRank, group, reqId);
        case SendOp::QUERY_LINK_STATE:
            return SendBatchQueryLinkState(srcRank);
    }
    return -1;
}

void SmemGroupManagerServer::UpdateLinkTransitionOnSuccess(LinkState st, size_t linkIdx, LinkTransition &tr,
                                                           const std::chrono::steady_clock::time_point &now) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
        case LINK_CONNECTING:
            tr.timestamp = now;
            break;
        case LINK_EXCHANGED:
            links_[linkIdx] = LINK_CONNECTING;
            tr = {now, 0};
            break;
        default:
            break;
    }
}

void SmemGroupManagerServer::UpdateLinkTransitionOnFailure(LinkState st, size_t linkIdx, LinkTransition &tr,
                                                           const std::chrono::steady_clock::time_point &now) noexcept
{
    switch (st) {
        case LINK_EXCHANGING:
        case LINK_EXCHANGED:
        case LINK_CONNECTING:
            links_[linkIdx] = LINK_IDLE;
            tr = {};
            break;
        default:
            break;
    }
}

void SmemGroupManagerServer::UpdateLinksAfterBatch(const std::vector<const SendTask *> &group, int overallRet) noexcept
{
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto t : group) {
        if (t->op == SendOp::QUERY_LINK_STATE) {
            continue;
        }
        LinkState st = links_[t->linkIdx];
        LinkTransition &tr = transitions_[t->linkIdx];
        if (st == LINK_IDLE) {
            continue;
        }

        if (overallRet == 0) {
            UpdateLinkTransitionOnSuccess(st, t->linkIdx, tr, now);
        } else {
            UpdateLinkTransitionOnFailure(st, t->linkIdx, tr, now);
        }
    }
}

/*
 * Push a send task to the queue and wake up the send worker thread.
 */
void SmemGroupManagerServer::EnqueueSend(const SendTask &task) noexcept
{
    SendTask t = task;
    t.requestId = (static_cast<uint64_t>(maxRanks_ + 1) << reqIdShift) | g_ctrlReqSeq.fetch_add(1);
    std::lock_guard<std::mutex> lk(sendMutex_);
    uint64_t key = SendTaskKey(t);
    if (!pendingSendKeys_.insert(key).second && t.op != SendOp::REMOVE) {
        return;
    }
    sendQueue_.push_back(t);
    sendCond_.notify_one();
}

void SmemGroupManagerServer::EnqueueBatchSend(std::vector<SendTask> &&tasks) noexcept
{
    if (tasks.empty()) {
        return;
    }
    auto base = static_cast<uint64_t>(maxRanks_ + 1) << reqIdShift;
    std::lock_guard<std::mutex> lk(sendMutex_);
    bool enqueued = false;
    for (auto &t : tasks) {
        t.requestId = base | g_ctrlReqSeq.fetch_add(1);
        uint64_t key = SendTaskKey(t);
        if (!pendingSendKeys_.insert(key).second && t.op != SendOp::REMOVE) {
            continue;
        }
        sendQueue_.push_back(t);
        enqueued = true;
    }
    if (enqueued) {
        sendCond_.notify_one();
    }
}

uint64_t SmemGroupManagerServer::SendTaskKey(const SendTask &t) const noexcept
{
    const uint64_t srcRankFlag = static_cast<uint64_t>(t.srcRank) << K_SEND_TASK_SRC_RANK_SHIFT;
    const uint64_t opFlag = static_cast<uint64_t>(t.op) << K_SEND_TASK_OP_SHIFT;
    const uint64_t linkIdxLow = static_cast<uint64_t>(t.linkIdx) & K_SEND_TASK_LINK_IDX_MASK;
    return srcRankFlag | opFlag | linkIdxLow;
}

int SmemGroupManagerServer::SendPackedMessage(uint32_t srcRank, const SmemMessage &msg) const noexcept
{
    auto packed = SmemMessagePacker::Pack(msg);
    if (packed.empty()) {
        return -1;
    }
    return sender_(srcRank, packed);
}

/*
 * Background thread: dequeue ALL pending send tasks, batch them by
 * (srcRank, op), and send each group as a single packed message.
 * Wakes on new tasks or every 100 ms (poll timeout).
 */
void SmemGroupManagerServer::SendWorkerTask() noexcept
{
    pthread_setname_np(pthread_self(), "group_send");
    while (running_) {
        std::vector<SendTask> batch;
        {
            std::unique_lock<std::mutex> lk(sendMutex_);
            sendCond_.wait_for(lk, std::chrono::milliseconds(sendWorkerPollMs),
                               [this] { return !sendQueue_.empty() || !running_; });
            if (!running_ && sendQueue_.empty()) {
                break;
            }
            if (sendQueue_.empty()) {
                continue;
            }
            batch.swap(sendQueue_);
            pendingSendKeys_.clear();
        }
        ProcessBatch(batch);
    }
}

/*
 * Start the driver and send worker background threads.
 * Safe to call multiple times (no-op if already running).
 */
void SmemGroupManagerServer::Start() noexcept
{
    if (running_) {
        return;
    }
    running_ = true;
    driverThread_ = std::thread(&SmemGroupManagerServer::TransitionDriverTask, this);
    sendThread_ = std::thread(&SmemGroupManagerServer::SendWorkerTask, this);
}

/*
 * Signal threads to stop and wait for them to join.
 * Safe to call multiple times (no-op if not running).
 */
void SmemGroupManagerServer::Stop() noexcept
{
    if (!running_) {
        return;
    }
    running_ = false;
    sendCond_.notify_all();
    driverCond_.notify_all();
    if (sendThread_.joinable()) {
        sendThread_.join();
    }
    if (driverThread_.joinable()) {
        driverThread_.join();
    }
}

/*
 * Background thread: periodically call DrivePendingTransitions to
 * scan and advance the link state machine.
 */
void SmemGroupManagerServer::TransitionDriverTask() noexcept
{
    pthread_setname_np(pthread_self(), "group_mgr_drv");
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        lock.unlock();
        DrivePendingTransitions();
        lock.lock();
        if (!running_) {
            break;
        }
        driverCond_.wait_for(lock, std::chrono::milliseconds(linkTransitionDriverIntervalMs),
                             [this] { return !running_; });
    }
}

// ── 状态恢复 ──

void SmemGroupManagerServer::RestoreState(const std::vector<RankState> &states, const std::vector<LinkState> &links,
                                          const std::unordered_set<uint32_t> &alive) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint32_t n = std::min(static_cast<uint32_t>(states.size()), maxRanks_);
    for (uint32_t i = 0; i < n; ++i) {
        states_[i] = states[i];
    }
    uint32_t linkCount = std::min(static_cast<uint32_t>(links.size()), maxRanks_ * maxRanks_);
    for (uint32_t i = 0; i < linkCount; ++i) {
        links_[i] = links[i];
    }
    aliveRanks_ = alive;
    STORE_LOG_INFO("RestoreState: " << n << " ranks, " << linkCount << " links, " << alive.size() << " alive");
}

void SmemGroupManagerServer::RestoreState(const std::vector<RankState> &states,
                                          const std::unordered_set<uint32_t> &alive) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint32_t n = std::min(static_cast<uint32_t>(states.size()), maxRanks_);
    for (uint32_t i = 0; i < n; ++i) {
        states_[i] = states[i];
    }
    aliveRanks_ = alive;
    STORE_LOG_INFO("RestoreState (states only): " << n << " ranks, " << alive.size() << " alive");
}

void SmemGroupManagerServer::SetRankBase(uint32_t rankId, const Bytes &data) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    rankBase_[rankId] = data;
}

void SmemGroupManagerServer::SetRankExternal(uint32_t rankId, const std::vector<Bytes> &data) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    rankExternal_[rankId] = data;
}

void SmemGroupManagerServer::SetRankRole(uint32_t rankId, uint8_t role) noexcept
{
    if (rankId >= maxRanks_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    rankRole_[rankId] = role;
}

uint8_t SmemGroupManagerServer::GetRankRole(uint32_t rankId) const noexcept
{
    if (rankId >= maxRanks_) {
        return SMEM_GROUP_RANK_ROLE_UNKNOWN;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return rankRole_[rankId];
}

void SmemGroupManagerServer::SetRankRoles(const std::vector<uint8_t> &roles) noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    uint32_t n = std::min(static_cast<uint32_t>(roles.size()), maxRanks_);
    for (uint32_t i = 0; i < n; ++i) {
        rankRole_[i] = roles[i];
    }
    // 不在此处打日志：恢复路径由 EtcdStateStore 汇总输出（含 Proxy 列表），避免重复且不利于定位的日志。
}

std::vector<uint8_t> SmemGroupManagerServer::GetRoles() const noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    return rankRole_;
}

void SmemGroupManagerServer::RebuildActiveLinks() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    isActiveLink_.assign(maxRanks_ * maxRanks_, false);
    activeLinks_.clear();
    for (uint32_t i = 0; i < maxRanks_ * maxRanks_; ++i) {
        // 剪枝对保持 IDLE（稳定态），不进活跃扫描表。
        if (IsLinkPruned(i / maxRanks_, i % maxRanks_)) {
            links_[i] = LINK_IDLE;
            transitions_[i] = {};
            continue;
        }
        if (!IsStableLinkState(links_[i])) {
            isActiveLink_[i] = true;
            activeLinks_.push_back(i);
        }
    }
    STORE_LOG_INFO("RebuildActiveLinks: " << activeLinks_.size() << " non-stable links");
}

} // namespace ock::smem
