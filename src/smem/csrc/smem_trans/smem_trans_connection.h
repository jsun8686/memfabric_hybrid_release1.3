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
#ifndef MF_SMEM_TRANS_CONNECTION_H
#define MF_SMEM_TRANS_CONNECTION_H

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "smem_common_includes.h"
#include "smem_config_store.h"
#include "smem_group_manager.h"
#include "smem_tcp_config_store.h"
#include "mf_net.h"
#include "hybm_def.h"
#include "mf_rwlock.h"
#include "smem_trans.h"
#include "smem_net_common.h"

namespace ock {
namespace smem {

struct WorkerUniqueId {
    ock::mf::net_addr_t address{};
    uint32_t pid{0};
    uint16_t port{0};
    uint16_t reserved{0}; // used in join
};

using WorkerId = std::array<uint8_t, sizeof(WorkerUniqueId)>;

struct WorkerIdHash {
    size_t operator()(const WorkerId &id) const
    {
        return std::hash<std::string>()(std::string(id.begin(), id.end()));
    }
};

union WorkerIdUnion {
    WorkerUniqueId session;
    WorkerId workerId;

    explicit WorkerIdUnion(WorkerUniqueId ws) : session(ws) {}
    explicit WorkerIdUnion(WorkerId id) : workerId{id} {}
    explicit WorkerIdUnion() {}
};

struct LocalMapAddress {
    void *address;
    uint64_t size;
    LocalMapAddress() : address{nullptr}, size{0} {}
    LocalMapAddress(void *p, uint64_t s) : address{p}, size{s} {}
};

struct SmemTransExchangeInfo {
    hybm_exchange_info hybmInfo;
    union U {
        WorkerUniqueId session;
        LocalMapAddress address;
        U() noexcept {}
    } u;
};

using Local2GlobalMap = std::map<const void *, LocalMapAddress, std::greater<const void *>>;

// Wire format of a SmemTransExchangeInfo: [u (session or address)][descLen][desc]
inline void SerializeTransExchangeInfo(std::vector<uint8_t> &out, const SmemTransExchangeInfo &info) noexcept
{
    const auto *uPtr = reinterpret_cast<const uint8_t *>(&info.u);
    out.insert(out.end(), uPtr, uPtr + sizeof(info.u));
    const auto *lenPtr = reinterpret_cast<const uint8_t *>(&info.hybmInfo.descLen);
    out.insert(out.end(), lenPtr, lenPtr + sizeof(info.hybmInfo.descLen));
    if (info.hybmInfo.descLen > 0 && info.hybmInfo.desc != nullptr) {
        out.insert(out.end(), info.hybmInfo.desc, info.hybmInfo.desc + info.hybmInfo.descLen);
    }
}

class SmemTransConnection;
using SmemTransConnectionPtr = SmRef<SmemTransConnection>;

// Override-rank window allocator injected by the entry (backed by
// SmemTransEntry::AllocOverrideRank). The uniqueId lets the entry reuse a
// previously allocated rank window when the same peer reconnects.
using RankAllocCb = std::function<Result(const std::string &, uint32_t &)>;

// Override-rank reference release callback injected by the entry (backed by
// SmemTransEntry::ReleaseOverrideRank, one unified uniqueId -> rank map for both
// entities). Only the self connection gets one: a reference is released when the
// peer leaves the local group (RemoveOnePeer); actively created connections are
// released by the entry when their uniqueId record is removed from connections_
// (store link broken / uninit).
using RankReleaseCb = std::function<void(const std::string &)>;

/*
 * One transfer connection to a remote store (or to the local store server via the
 * self connection). Borrows the entry-owned hybm entity (send entity for actively
 * created connections, recv entity for the self connection), the auto-ranked
 * client store and the group manager callbacks of that store. Every remote peer
 * (group member) gets its own locally-allocated override-rank window so that
 * peers' slices never collide inside the entity; imports are mapped via
 * HYBM_IMPORT_RANK_ENCODE flags and recycled per member on leave.
 */
class SmemTransConnection : public SmReferable {
public:
    explicit SmemTransConnection(const smem_trans_config_t &config, const std::string &name,
                                 const std::string &remoteName, uint32_t rank, hybm_entity_t entity, StorePtr &store,
                                 const WorkerUniqueId &localWorker, RankAllocCb rankAlloc,
                                 RankReleaseCb rankRelease = nullptr)
        : config_(config), name_(name), store_(store), rankId_(rank), entity_(entity), workerUniqueId_(localWorker),
          rankAlloc_(std::move(rankAlloc)), rankRelease_(std::move(rankRelease))
    {
        // 一个 connection 只对应一个对端 uniqueId（entry 按 uniqueId 建链，传输时传入的 name 恒为该值），
        // 故建链时即完成 name -> WorkerId 解析并缓存，此后只读：数据面无需再解析，也无需加锁。
        // 解析失败仅表示不缓存（如 UT 裸构造），后续按需解析回退，不影响连接可用性。
        WorkerId id{};
        if (TryParseNameToWorkerId(remoteName, id)) {
            remoteName_ = remoteName;
            remoteWorkerId_ = id;
        }
    }

    ~SmemTransConnection() override;

    Result Initialize();
    void UnInitialize();
    Result Join(uint32_t flags, const SmemTransExchangeInfo &entityInfo, const MultiBytes &externalSlices,
                uint8_t rankRole);

    Result BroadcastSlices(const MultiBytes &slices, const void *sliceVa, uint64_t sliceSize, uint64_t gva);
    Result BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId, void *remoteAddrs[],
                             const size_t dataSizes[], uint32_t batchSize, smem_bm_copy_type opcode, void *stream,
                             uint32_t flags);

    static bool ParseTransName(const std::string &name, ock::mf::net_addr_t &ip, uint16_t &port);

private:
    int SetupGroupManagerCallbacks() noexcept;
    int OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) noexcept;
    // Incremental slice delivery: ADDWLST only carries that moment's snapshot, later slices arrive here
    int OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept;
    std::vector<ock::smem::LinkStateEntry> OnQueryLinkState() noexcept;
    // Per-peer leave handling: override-window recycle + recv-side reference release + bookkeeping cleanup
    void RemoveOnePeer(uint32_t leavingRankId) noexcept;
    // Per-peer override-rank window management (allocation goes through the entry-injected allocator)
    uint32_t GetOrCreatePeerRank(uint32_t groupRankId, const std::string &uniqueId) noexcept;
    // Extract the peer uniqueId from its baseInfo and allocate (or reuse) the override rank window
    uint32_t AllocPeerOverrideRank(const RankFullInfo &peer) noexcept;
    // Lightweight parse of the peer session from baseInfo (no desc alloc) -> uniqueId string
    bool ExtractPeerUniqueId(const RankFullInfo &peer, std::string &uniqueId) const noexcept;
    uint32_t FindPeerRank(uint32_t groupRankId) noexcept;
    void ReleaseAllPeerRanks() noexcept;
    void RemoveImportedWindow(uint32_t overrideRank) noexcept;

    // Record the local registered slice (sliceVa -> gva) under the own workerId in remoteSlices_,
    // so BatchSyncTransfer with the self uniqueId can transform registered addresses to gva
    void RecordSelfSlice(const void *sliceVa, uint64_t sliceSize, uint64_t gva) noexcept;

    // Convert a WorkerId to the peer's unique-id string (ip:port)
    std::string WorkerIdToUniqueId(const WorkerId &id) const noexcept;

    // Group-manager callback plumbing (extracted to keep SetupGroupManagerCallbacks small)
    int RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;
    int RegisterExecutorCallbacks(SmemGroupManagerClient *executor, SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;

    // Parse a peer's baseInfo/externalInfo into exchange info (shared by whitelist/establish paths)
    bool ParsePeerEntity(const RankFullInfo &peer, SmemTransExchangeInfo &entityInfo, smem_trans_role_t &role,
                         WorkerId &id) noexcept;
    int ParsePeerSlices(const RankFullInfo &peer, std::vector<SmemTransExchangeInfo> &slices,
                        std::vector<LocalMapAddress> &local) noexcept;
    // Per-peer import/dedup/record for the establish path (extracted from OnEstablishConnection)
    int ImportPeerInfo(const RankFullInfo &peer) noexcept;
    int ParseAddSlicesPayload(const MultiBytes &newSlices, uint32_t extendingRankId,
                              std::vector<hybm_exchange_info> &hybmInfos, std::vector<LocalMapAddress> &local) noexcept;
    int ImportIncrementalSlices(uint32_t extendingRankId, uint32_t overrideRank, const WorkerId &peerId,
                                smem_trans_role_t peerRole, std::vector<hybm_exchange_info> &hybmInfos,
                                const std::vector<LocalMapAddress> &local) noexcept;

    // 解析 name(ip:port) -> WorkerId；纯函数，不触碰任何共享状态，成功返回 true
    static bool TryParseNameToWorkerId(const std::string &name, WorkerId &uniqueId);
    // 取 name 对应的 WorkerId：正常走构造期缓存（只读，无锁）；仅当传入 name 与建链 name 不一致时
    // 才回退到按需解析，保持"按请求的 name 处理"的旧语义
    Result ParseNameToUniqueId(const std::string &name, WorkerId &uniqueId) const;
    Result TransformAddr(Local2GlobalMap &maps, std::vector<void *> &addr, void *remoteAddrs[],
                         const size_t dataSizes[], uint32_t size);
    // Clear the store dispatcher callbacks and executor so the cached store never invokes
    // this destroyed connection again (the store itself lives in the factory map until uninit)
    void ClearStoreCallbacks() noexcept;

private:
    hybm_entity_t entity_ = nullptr; /* hybm entity owned by the entry (borrowed, never destroyed here) */

    uint32_t rankId_ = 0; /* store group rank assigned by ApplyAutoRanking */

    // 对端窗口：rank 为 entry 分配的本地唯一 override rank 窗口，uniqueId 为分配时使用的
    // 对端标识（含 baseInfo 异常时的降级 key），释放时原样归还 entry 做引用计数减一
    struct PeerWindow {
        uint32_t rank = HYBM_IMPORT_RANK_INVALID;
        std::string uniqueId;
    };

    std::mutex rankMutex_;
    std::unordered_map<uint32_t, PeerWindow> peerOverrideRanks_; /* group rank -> 对端窗口 */
    RankAllocCb rankAlloc_;                                      /* entry 级 override rank 分配回调 */
    RankReleaseCb rankRelease_; /* 仅 self connection 注入：对端离开本组时归还引用 */

    const std::string name_; /* local unique id, format: ip:port */
    smem_trans_config_t config_{};
    WorkerUniqueId workerUniqueId_; /* local worker session parsed from the entry url */

    ock::mf::ReadWriteLock remoteSliceRwMutex_;
    std::unordered_map<WorkerId, Local2GlobalMap, WorkerIdHash> remoteSlices_;
    std::unordered_map<uint32_t, WorkerId> rankToWorkerId_;
    std::unordered_map<uint32_t, smem_trans_role_t> ranksRole_;
    // 对端 uniqueId（即建链时的 name）及其 WorkerId：构造期解析一次，此后只读。
    // remoteName_ 为空表示构造期解析失败，由 ParseNameToUniqueId 按需回退解析
    std::string remoteName_;
    WorkerId remoteWorkerId_{};

    StorePtr store_;
    std::atomic<bool> joined_{false}; /* 组回调线程写、Join/UnInitialize 线程读，需原子访问 */
    std::shared_ptr<std::promise<void>> joinComplete_;
};
} // namespace smem
} // namespace ock

#endif // MF_SMEM_TRANS_CONNECTION_H
