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
#ifndef MF_SMEM_TRANS_ENTRY_H
#define MF_SMEM_TRANS_ENTRY_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "smem_common_includes.h"
#include "smem_config_store.h"
#include "smem_thread_pool.h"
#include "smem_trans_connection.h"

namespace ock {
namespace smem {

class SmemTransEntry;
using SmemTransEntryPtr = SmRef<SmemTransEntry>;

/*
 * Outer transfer object created by smem_trans_create:
 *  - owns the config store server built from config.url (port 0 means auto detection)
 *  - owns two hybm entities (both rankCount=SMEM_TRANS_RANK_COUNT_MAX; send entity rankId=MAX-1,
 *    recv entity rankId=0 so the transport deterministically assigns client to the send entity and
 *    server to the recv entity):
 *    the send entity backs actively created connections (initiating copies), the
 *    recv entity backs the self connection (passive group membership so remote
 *    peers can exchange info with this process); every registered memory goes
 *    to both entities, and one unified uniqueId -> override-rank map grants the
 *    same peer window ([1, SMEM_TRANS_RANK_COUNT_MAX-2]) on both entities
 *    (reference counted; the rank is recycled when the last reference goes)
 *  - maintains the uniqueId -> SmemTransConnection map of actively created
 *    connections (lazy creation; a broken store link removes its record) plus
 *    one special self connection to the local store server (kept outside the map)
 *  - runs the ExecutorService executing asynchronous batch copy tasks
 */
class SmemTransEntry : public SmReferable {
public:
    static SmemTransEntryPtr Create(const smem_trans_config_t &config);

public:
    explicit SmemTransEntry(const smem_trans_config_t &config) : config_(config), name_(config.url) {}

    ~SmemTransEntry() override;

    uint16_t RpcPort() const;

    Result Initialize();
    void UnInitialize();

    Result RegisterLocalMemory(const void *address, uint64_t size, uint32_t flags);
    Result RegisterLocalMemories(const std::vector<std::pair<const void *, size_t>> &regMemories, uint32_t flags);
    Result UnregisterLocalMemory(const void *address);

    Result SyncTransfer(void *localAddr, const std::string &remoteUniqueId, void *remoteAddr, size_t dataSize,
                        smem_bm_copy_type opcode, void *stream, uint32_t flags);
    Result BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId, void *remoteAddrs[],
                             const size_t dataSizes[], uint32_t batchSize, smem_bm_copy_type opcode, void *stream,
                             uint32_t flags);

    Result SubmitBatchCopy(const smem_trans_batch_params_t &param, uint64_t &batchId);
    int32_t QueryBatchStatus(uint64_t batchId, int32_t *result, uint32_t flags);

    // Pure helpers (no instance state); exposed public so unit tests can cover align/merge logic
    static void AlignMemory(const void *&address, uint64_t &size);
    static std::vector<std::pair<const void *, size_t>>
    CombineMemories(std::vector<std::pair<const void *, size_t>> &input);

private:
    struct BatchTask {
        std::string destUrl;
        std::vector<void *> srcList;
        std::vector<void *> destList;
        std::vector<size_t> sizeList;
        smem_trans_opcode_t opcode;
        void *stream = nullptr;
        uint32_t flags = 0;
    };
    struct BatchState {
        bool done = false;
        int32_t ret = SM_OK;
    };
    // 注册一片内存后在单个 entity 上生成的交换记录：serialized 为序列化后的记录（Join/Extend 用），
    // slice 为注册句柄（回滚释放用），sliceVa/sliceSize 为 slice 的真实范围，gva 为该 slice 在此 entity 的全局地址
    struct SliceRecord {
        MultiBytes serialized;
        hybm_mem_slice_t slice = nullptr;
        void *sliceVa = nullptr;
        uint64_t sliceSize = 0;
        uint64_t gva = 0;
    };

private:
    // bootstrap helpers of Initialize
    Result CreateStoreServer();
    Result CreateStoreServerWithAutoPort(const std::string &ip);
    Result StartExecutor();
    Result CreateHybmEntities();
    Result CreateOneHybmEntity(uint32_t entityId, SmemTransExchangeInfo &entityInfo, hybm_entity_t &entity,
                               uint32_t rankId);
    Result CreateSelfConnection();

    // shared-entity helpers
    hybm_options GenerateHybmOptions(uint32_t entityId, uint32_t rankId);
    bool ApplyDataOpType(hybm_options &options, uint32_t flag, hybm_data_op_type hybmFlag, const char *opName) noexcept;
    // 在统一映射表中为 uniqueId 分配 override rank 窗口；send/recv 两个 entity 的 rankCount 相同
    // （均为 SMEM_TRANS_RANK_COUNT_MAX），本地窗口分别为 SMEM_TRANS_RANK_COUNT_MAX-1 与 0，同一 uniqueId
    // 无论主动建链（send entity）还是 self connection 导入（recv entity）都复用同一个 override rank。
    // 重复分配返回相同窗口并递增引用计数
    Result AllocOverrideRank(const std::string &uniqueId, uint32_t &rank);
    // 引用计数减一，归零后删除 uniqueId -> rank 映射、rank 可复用：
    // self connection 在 RemoveOnePeer（对端离开本组）时释放引用；
    // 主动建链在从 connections_ 移除该 uniqueId 记录（断链/反初始化）时释放引用
    void ReleaseOverrideRank(const std::string &uniqueId);

    // connection management
    SmemTransConnectionPtr GetOrCreateConnection(const std::string &uniqueId);
    // isSelf=true 创建连接本地 store server 的 self connection（recv entity），否则主动建链（send entity）
    SmemTransConnectionPtr CreateAndJoinConnection(const std::string &uniqueId, bool isSelf);
    Result CreateConnectionStore(const std::string &storeUrl, StorePtr &store);
    static Result ApplyAutoRanking(const StorePtr &store, uint32_t &rankId);
    // 断链处理：store 客户端链路断开时仅移除 connections_ 中该 uniqueId 的记录
    void OnStoreLinkBroken(const std::string &uniqueId);

    // register helpers: each slice is registered on both entities; the recv record is immediately
    // extended to the self connection, the send record is accumulated for later Joins
    Result RegisterOneMemory(const void *address, uint64_t size, uint32_t flags);
    Result RegisterAndExportSlice(hybm_entity_t entity, std::unordered_set<uint64_t> &sliceIds, const void *address,
                                  uint64_t size, SliceRecord &record);

    // batch tasks
    Result ValidateBatchParams(const smem_trans_batch_params_t &param, uint64_t &batchSize);
    void ExecuteBatchTask(uint64_t batchId, const std::shared_ptr<BatchTask> &task, const SmemTransConnectionPtr &conn);
    void FinishBatchTask(uint64_t batchId, int32_t ret);

private:
    smem_trans_config_t config_{};
    std::string name_;                /* normalized local unique id, format: ip:port */
    std::string selfUrl_;             /* "tcp://ip:port" of the own config store server */
    uint16_t rpcPort_ = 0;            /* actual listening port */
    WorkerUniqueId workerUniqueId_{}; /* local worker session parsed from name_ */

    hybm_entity_t sendEntity_ = nullptr;     /* 主动建链（发起 copy）使用的 entity */
    hybm_entity_t recvEntity_ = nullptr;     /* self connection（被动建链信息交互）使用的 entity */
    uint32_t sendEntityId_ = 0;              /* process-level entity id of sendEntity_ */
    uint32_t recvEntityId_ = 0;              /* process-level entity id of recvEntity_ */
    SmemTransExchangeInfo sendEntityInfo_{}; /* send entity export info serialized into Join baseInfo */
    SmemTransExchangeInfo recvEntityInfo_{}; /* recv entity export info serialized into Join baseInfo */

    // override rank 映射表条目：同一 uniqueId 始终分配相同窗口（send/recv entity 共用），refCount 归零后映射删除、rank 可复用
    struct OverrideRankEntry {
        uint32_t rank = 0;     /* 分配给该 uniqueId 的 override rank 窗口 */
        uint32_t refCount = 0; /* 引用计数 */
    };

    std::mutex overrideRankMutex_;
    std::unordered_map<std::string, OverrideRankEntry>
        uniqueIdToRank_; /* uniqueId -> override rank 窗口（send/recv 共用），0 保留本地 */

    std::mutex memMutex_;
    std::unordered_set<uint64_t> sendSliceIds_; /* send entity 已导出的 slice ID 去重 */
    std::unordered_set<uint64_t> recvSliceIds_; /* recv entity 已导出的 slice ID 去重 */
    MultiBytes sendExportedSlices_; /* send entity 已注册 slice 记录，作为主动建链 Join externalInfo 的数据源 */
    MultiBytes recvExportedSlices_; /* recv entity 已注册 slice 记录，作为 self connection Join externalInfo 的数据源 */

    // 断链回调的存活令牌：store 生命周期长于 entry，回调捕获令牌而非依赖 entry 存活
    std::shared_ptr<std::atomic<bool>> brokenHandlerAlive_ = std::make_shared<std::atomic<bool>>(true);

    std::mutex connMutex_;
    std::condition_variable connCv_;
    std::set<std::string> pendingConnections_;
    std::map<std::string, SmemTransConnectionPtr> connections_; /* uniqueId -> 主动建链的连接 */
    SmemTransConnectionPtr selfConnection_; /* 连接本地 store server 的特殊连接，不入 connections_ */

    std::unique_ptr<ExecutorService> executor_;
    std::atomic<bool> stopping_{false};

    std::mutex batchMutex_;
    uint64_t batchIdGen_ = 0;
    std::unordered_map<uint64_t, BatchState> batchStatusMap_;
};

inline uint16_t SmemTransEntry::RpcPort() const
{
    return rpcPort_;
}
} // namespace smem
} // namespace ock

#endif // MF_SMEM_TRANS_ENTRY_H
