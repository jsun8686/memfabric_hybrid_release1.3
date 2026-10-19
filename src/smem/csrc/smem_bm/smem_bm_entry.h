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
#ifndef MEMFABRIC_HYBRID_SMEM_BM_ENTRY_H
#define MEMFABRIC_HYBRID_SMEM_BM_ENTRY_H

#include <future>
#include <string>
#include <unordered_map>

#include "hybm_def.h"
#include "smem_thread_pool.h"
#include "smem_common_includes.h"
#include "smem_group_manager.h"
#include "smem_config_store.h"
#include "smem_tcp_config_store.h"
#include "smem_bm.h"

namespace ock {
namespace smem {
struct SmemBmEntryOptions {
    uint32_t id;
    uint32_t rank;
    uint32_t rankSize;
    uint64_t controlOperationTimeout;
};

struct SmemBmConsistencyConfig {
    uint64_t maxHBMSize;
    uint64_t maxDRAMSize;
    bool enable56BitsGva;

    SmemBmConsistencyConfig() : maxHBMSize(0), maxDRAMSize(0), enable56BitsGva(false) {}

    explicit SmemBmConsistencyConfig(const hybm_options &options)
        : maxHBMSize(options.maxHBMSize), maxDRAMSize(options.maxDRAMSize), enable56BitsGva(options.enable56BitsGva)
    {}

    [[nodiscard]] std::string ToStr() const
    {
        std::stringstream ss;
        ss << "maxHBMSize=" << maxHBMSize << ", maxDRAMSize=" << maxDRAMSize << "enable56BitsGva" << enable56BitsGva;
        return ss.str();
    }
};

class SmemBmEntry : public SmReferable {
public:
    explicit SmemBmEntry(const SmemBmEntryOptions &options, const StorePtr &store)
        : options_(options), _configStore(store), coreOptions_{}, entityInfo_{}
    {}

    ~SmemBmEntry() override
    {
        Uninitialize();
    };

    int32_t Initialize(const hybm_options &options);

    void Uninitialize();

    Result Join(uint32_t flags);

    Result Update(uint32_t flags);

    Result Leave(uint32_t flags);

    Result ExtendLocalMem(smem_bm_mem_type memType, uint64_t size);

    Result DataCopy(const void *src, void *dest, uint64_t size, smem_bm_copy_type t, void *stream, uint32_t flags);

    Result DataCopyBatch(smem_batch_copy_params *params, smem_bm_copy_type t, uint32_t flags);

    Result Wait();

    Result RegisterMem(uint64_t addr, uint64_t size);

    Result UnRegisterMem(uint64_t addr);

    uint32_t Id() const;

    uint32_t GetRankIdByGva(void *gva);

    const hybm_options &GetCoreOptions() const;

    void *GetGvaAddress() const;
    void *GetHostGvaAddress() const;
    void *GetDeviceGvaAddress() const;

    uint64_t GetRealDRAMSize() const;
    uint64_t GetRealHBMSize() const;

    /**
     * @brief 由本 rank 申请的内存推导角色：DRAM/HBM 都不申请即为 Proxy。
     *
     * Proxy 不贡献内存，group manager 会让两个 Proxy rank 之间不建链（分离部署链路剪枝），
     * 其余角色组合保持全互联。
     */
    static uint8_t DeriveRankRole(uint64_t localDRAMSize, uint64_t localHBMSize) noexcept
    {
        return (localDRAMSize == 0 && localHBMSize == 0) ? SMEM_GROUP_RANK_ROLE_PROXY : SMEM_GROUP_RANK_ROLE_PROVIDER;
    }

private:
    bool AddrInHostGva(const void *address, uint64_t size);
    bool AddrInDeviceGva(const void *address, uint64_t size);
    [[nodiscard]] bool CheckRankConfigConsistency(const hybm_options &options) const;
    Result AllocDramMemBySlice(hybm_entity_t entity, uint64_t totalSize, uint32_t flags);
    Result AllocDramMemBestEffort(hybm_entity_t entity, uint64_t maxSize, uint32_t flags);
    Result AllocDramMem(hybm_entity_t entity, const hybm_options &options, uint32_t flags);

    smem_bm_mem_type GetHybmMemTypeFromGva(const void *addr, uint64_t size);
    Result CheckJoined() const;
    hybm_data_copy_direction TransToHybmDirection(const smem_bm_copy_type &smemDirect, const void *src,
                                                  uint64_t srcSize, const void *dest, uint64_t destSize);
    int SetupGroupManagerCallbacks() noexcept;
    int OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) noexcept;
    std::vector<ock::smem::LinkStateEntry> OnQueryLinkState() noexcept;
    int OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept;

    // Group-manager callback plumbing (extracted to keep SetupGroupManagerCallbacks small)
    int RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;
    int RegisterExecutorCallbacks(SmemGroupManagerClient *executor, SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;

    // Validate peer payload sizes (extracted from OnAddToWhitelist)
    int ImportPeerEntities(const std::vector<RankFullInfo> &others) noexcept;
    int ImportPeerSlices(const std::vector<RankFullInfo> &others) noexcept;

private:
    /* hot used variables */
    bool inited_ = false;
    bool joined_ = false;
    std::mutex mutex_;
    hybm_entity_t entity_ = nullptr;
    void *hostGva_ = nullptr;
    void *deviceGva_ = nullptr;

    /* non-hot used variables */
    SmemBmEntryOptions options_;
    hybm_options coreOptions_;
    StorePtr _configStore;
    hybm_exchange_info entityInfo_;
    std::vector<hybm_mem_slice_t> slices_;
    std::vector<hybm_exchange_info> sliceInfos_;
    uint64_t realDRAMSize_ = 0;
    uint64_t realHBMSize_ = 0;
    std::map<uint64_t, std::pair<uint64_t, hybm_mem_slice_t>> registedSlice_;
    // 由本 rank 实际申请的内存推导（Initialize 时确定）：DRAM/HBM 都不申请即为 PROXY 角色，
    // 该 rank 与其它 PROXY rank 之间不建链（详见 SmemGroupManagerServer 的链路剪枝）。
    uint8_t rankRole_{SMEM_GROUP_RANK_ROLE_UNKNOWN};

    std::shared_ptr<std::promise<void>> joinComplete_;
};
using SmemBmEntryPtr = SmRef<SmemBmEntry>;

inline uint32_t SmemBmEntry::Id() const
{
    return options_.id;
}

inline const hybm_options &SmemBmEntry::GetCoreOptions() const
{
    return coreOptions_;
}

inline void *SmemBmEntry::GetGvaAddress() const
{
    return deviceGva_;
}

inline void *SmemBmEntry::GetHostGvaAddress() const
{
    return hostGva_;
}

inline void *SmemBmEntry::GetDeviceGvaAddress() const
{
    return deviceGva_;
}

inline uint64_t SmemBmEntry::GetRealDRAMSize() const
{
    return realDRAMSize_;
}

inline uint64_t SmemBmEntry::GetRealHBMSize() const
{
    return realHBMSize_;
}

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_BM_ENTRY_H
