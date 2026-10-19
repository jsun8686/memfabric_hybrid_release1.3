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

#ifndef MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dl_hcomm_api.h"
#include "dl_rt_api.h"
#include "hcomm_api_wrapper.h"
#include "hybm_transport_manager.h"
#include "hybm_kernel_hepler.h"
#include "urma_topo_def.h"

struct HybmOneSideOpParam;

namespace ock {
namespace mf {
namespace transport {
namespace device {

// ─────────────────────────────────────────────────────────────────
// URMA-specific type definitions (migrated from hcomm_transport_manager.h)
// ─────────────────────────────────────────────────────────────────
constexpr uint32_t URMA_EXPORT_DESC_MAGIC = 0xA5FAB001U;
constexpr uint16_t URMA_EXPORT_DESC_VERSION = 1U;
constexpr uint32_t DEVICE_URMA_MAX_EXPORT_KEY_LENGTH = KEY_SIZE * 6;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS = 2;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_INFO = 1256;
constexpr uint32_t DEVICE_URMA_EXPORT_KEY_DATA_BYTES =
    (DEVICE_URMA_MAX_EXPORT_KEY_LENGTH - DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS + DEVICE_URMA_EXPORT_KEY_INFO) *
    sizeof(uint64_t);
constexpr void *INVALID_MEM_HANDLE = nullptr;

using UrmaMemTag = uint64_t;
using HcommChannelHandle = ock::mf::ChannelHandle;
using HcommThreadHandle = ock::mf::ThreadHandle;

enum UrmaProtocol {
    RESERVED = -1,
    HCCS = 0,
    ROCE = 1,
    PCIE = 2,
    SIO = 3,
    UBC_CTP = 4,
    UBC_TP = 5,
    UB_MEM = 6,
    UBOE = 7,
    UB_RTP = 9,
};

inline std::ostream &operator<<(std::ostream &stream, UrmaProtocol protocol)
{
    switch (protocol) {
        case UrmaProtocol::RESERVED:
            return stream << "RESERVED";
        case UrmaProtocol::HCCS:
            return stream << "HCCS";
        case UrmaProtocol::ROCE:
            return stream << "ROCE";
        case UrmaProtocol::PCIE:
            return stream << "PCIE";
        case UrmaProtocol::SIO:
            return stream << "SIO";
        case UrmaProtocol::UBC_CTP:
            return stream << "UBC_CTP";
        case UrmaProtocol::UBC_TP:
            return stream << "UBC_TP";
        case UrmaProtocol::UB_MEM:
            return stream << "UB_MEM";
        case UrmaProtocol::UBOE:
            return stream << "UBOE";
        case UrmaProtocol::UB_RTP:
            return stream << "UB_RTP";
        default:
            return stream << "UNKNOWN";
    }
}

// 机型 × 协议支持表：第一维 mainboardId，第二维协议；顺序即优先级
// 选择规则：用户请求的协议族（DEVICE_URMA 命中 UBC_CTP/UB_RTP，DEVICE_UBOE 命中 UBOE）
// 与行内协议取交集，按行序取第一个。
constexpr uint32_t URMA_PROTOCOL_TABLE_MAX_PROTOCOLS = 2;

struct UrmaProtocolTableEntry {
    uint32_t mainboardId;
    UrmaProtocol protocols[URMA_PROTOCOL_TABLE_MAX_PROTOCOLS];
    uint32_t protocolCount;
};

constexpr UrmaProtocolTableEntry URMA_PROTOCOL_TABLE[] = {
    {MAIN_BOARD_ID_POD, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_POD_2D, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_SERVER_TYPE1, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_SERVER_350L, {UBC_CTP}, 1},
    {MAIN_BOARD_ID_SERVER_8PMESH, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_SERVER_8PMESH_UBOE, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_SERVER_8PMESH_NOSP, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_SERVER_8PMESH_NOSP_UBOE, {UBC_CTP, UBOE}, 2},
    {MAIN_BOARD_ID_POD_FLEX, {UB_RTP}, 1},     // 950 SuperPod PC16
    {MAIN_BOARD_ID_POD_FLEX_RTP, {UB_RTP}, 1}, // 950 SuperPod PC16
};

enum UrmaMemoryType : uint16_t {
    HOST_DRAM = 0,
    DEVICE_HBM = 1,
    INVALID_BUTT = 2,
};

struct UrmaEndpointDesc {
    uint32_t devPhyId{0};
    int32_t localId{0};
    int32_t remoteId{-1};
    uint32_t superDevId{0};
    uint32_t serverIdx{0};
    uint32_t superPodIdx{0};
    UrmaProtocol protocol{UrmaProtocol::RESERVED};
    CommAddrType type{COMM_ADDR_TYPE_RESERVED};
    uint8_t raws[URMA_ENDPOINT_RAW_LEN]{};
};

struct UrmaCommMem {
    uint64_t addr{0};
    uint64_t size{0};
    UrmaMemoryType type{UrmaMemoryType::INVALID_BUTT};
};

struct UrmaExportDesc {
    uint32_t magic{URMA_EXPORT_DESC_MAGIC};
    uint16_t version{URMA_EXPORT_DESC_VERSION};
    uint16_t headerSize{0};
    int32_t remoteId{-1};
    UrmaMemoryType memoryType{UrmaMemoryType::INVALID_BUTT};
    UrmaMemTag memTag{0};
    uint64_t addr{0};
    uint64_t size{0};
    uint32_t hcommDescLen{0};
    uint32_t devTransFlagDescLen{0};
};

class DeviceUrmaTransportManager final : public transport::TransportManager {
public:
    DeviceUrmaTransportManager() = default;

    ~DeviceUrmaTransportManager() override;

    // Initialization/open/close lifecycle
    Result OpenDevice(const TransportOptions &options) override;

    Result CloseDevice() override;

    // Local/remote registration and key export/import
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;

    Result UnregisterMemoryRegion(uint64_t addr) override;

    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;

    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    // Prepare/rank/connection/private data
    Result Prepare(const HybmTransPrepareOptions &options) override;

    Result ConnectRank(uint32_t rankId) override;

    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;

    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;

    const std::string &GetNic() const override;

    const TransportPrivateData GetPrivateData() const override;

    // Data transfer/copy
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const RankGroupMap &groupMap, std::vector<uint32_t> &localIndices,
                                    RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks) override;

    // Sync stream
    Result Synchronize(uint32_t rankId) override;

    Result SynchronizeRanks(const std::set<uint32_t> &rankIds) override;

private:
    struct LocalRegistration {
        TransportMemoryRegion mr{};
        std::vector<HcommMemHandle> handle;
        UrmaMemTag memTag{0};
        uint32_t refCount{0};
        uint64_t deviceVa{0};
    };

    struct RemoteRegistration {
        uint64_t addr{0};
        uint64_t size{0};
        uint64_t memTag{0};
        std::vector<uint8_t> descBytes{};
        UrmaCommMem view{};
    };

    struct RemoteRankState {
        std::mutex rankMutex{};
        UrmaEndpointDesc remoteEndpointDesc{};
        bool hasEndpointDesc{false};
        // 本 rank 侧用于与该 peer 通信的 channel 描述符
        HcommChannelDesc channelDesc{};
        // 本 rank 侧用于与该 peer 通信的 channel handle
        HcommChannelHandle channel{0};
        // 本 rank 侧用于与该 peer 通信的 thread
        HcommThreadHandle thread{0};
        std::vector<RemoteRegistration> imports{};
        // Remote peer's notify record address (from TransportMemoryKey header during
        // ImportRemoteMemKeysLocked). Used as remote_flag_addr in kernel launch args.
        uint64_t remoteFlagAddr{0};
        uint64_t remoteFlagSize{0};
        // Raw hcommDesc bytes for the remote flag, used for HcommMemUnimport during cleanup.
        std::vector<uint8_t> remoteFlagDescBytes{};
    };

    struct RemoteMemKey {
        uint64_t addr;
        uint64_t size;
    };

    using RankTransferDescriptors = std::unordered_map<uint32_t, std::vector<HcommBatchTransferDesc>>;

    // Device kernel launch helpers (moved from removed HcomUrmaTransportAdapter)
    struct DeviceTransferBuffers {
        void *base{nullptr};
        size_t capacity{0};
        std::vector<uint8_t> hostBuffer{};
        void *rankIdList{nullptr};
        void *rankStartIdxList{nullptr};
        void *rankListNumList{nullptr};
        void *threadList{nullptr};
        void *channelList{nullptr};
        void *transferDescs{nullptr};
        void *markerDescs{nullptr};
    };

    struct NotifyResource;

    // A remote transfer awaiting completion; owned by CompletionContext.
    struct PendingTransfer {
        uint32_t rankId{0};
        bool inFlight{false};
        uint32_t notifyPoolIndex{UINT32_MAX};
    };

    // Notify 池中的单个资源；只与当前 manager/device 生命周期绑定，不与 Host 线程绑定。
    struct NotifyResource {
        void *notify{nullptr};                         // ACL Notify 对象
        uint32_t notifyId{0};                          // Notify 对应的设备资源 ID
        uint64_t notifyAddr{0};                        // Notify record 的设备地址
        uint64_t notifyLen{0};                         // Notify record 的长度
        std::vector<HcommMemHandle> notifyHcommHandle; // Notify record 的 HCOMM 注册句柄
        uint32_t poolIndex{UINT32_MAX};                // 资源在池中的稳定下标
        std::atomic<uint32_t> nextFree{UINT32_MAX};    // 无锁空闲栈中的下一个槽位
    };

    // Per-thread async completion context.
    struct CompletionContext {
        void *stream{nullptr}; // non-owning ACL stream, compared at each launch/sync
        // Data-kernel launch parameters are reused after each launch stream synchronization.
        DeviceTransferBuffers launchBuffers{};
        bool initialized{false};
        // All in-flight transfers; emptied by Synchronize.
        std::vector<PendingTransfer> pendingTransfers{};
    };

    // Initialization/open/close helpers and lifecycle
    Result InitLocalDeviceInfoLocked(const TransportOptions &options);
    Result OpenEndpointResourcesLocked(const TransportOptions &options);
    Result BuildLocalEndpointDescLocked(UrmaProtocol protocol, UrmaEndpointDesc &localDesc);
    Result BuildLocalEndpointsDescLocked(UrmaProtocol protocol, std::vector<UrmaEndpointDesc> &localDesc);
    Result CreateEndpointAndInitResourcesLocked(const UrmaEndpointDesc &localDesc);
    Result CreateEndpointsAndInitResourcesLocked(const std::vector<UrmaEndpointDesc> &descs);
    Result InitDeviceTransferFlagLocked();
    void RollbackOpenDeviceLocked();
    Result EnsureDeviceKernelLoadedLocked();
    static Result DestroyRankChannelsAndThread(RemoteRankState &state, uint32_t peerRank);
    Result UnimportPeerImportsAndFlag(RemoteRankState &state, uint32_t peerRank);
    Result CleanupPeerRankState(RemoteRankState &state, uint32_t peerRank);
    Result CleanupLocalRegistrationsLocked();

    // Local/remote registration and key export/import
    Result FindLocalRegistrationLocked(uint64_t addr, uint64_t size, LocalRegistration *registration) const;
    Result CorrectLocalRegAddressLocked(uint64_t addr, uint64_t size, uint64_t &correctedAddr) const;
    Result FindRemoteRegistrationLocked(uint32_t rankId, uint64_t addr, uint64_t size,
                                        const RemoteRegistration **registration) const;
    Result ImportRemoteMemKeysLocked(uint32_t peerRank, RemoteRankState &state,
                                     const std::vector<TransportMemoryKey> &memKeys);

    // Prepare/rank/connection/private data
    void RollbackPendingPrepareResourcesLocked(const std::set<uint32_t> &peerRanks);
    Result RemoveRankLocked(uint32_t rankId);

    // Data transfer/copy
    Result RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write);
    Result StageAndLaunchTransfer(CompletionContext &ctx, RemoteRankState &state, bool isRead,
                                  const std::vector<uint64_t> &localVec, const std::vector<uint64_t> &remoteVec,
                                  const std::vector<uint64_t> &sizeVec, uint32_t rankId);

    Result ValidateMultiRankBatchLocked(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                        const RankGroupMap &groupMap) const;
    Result ResolveMultiRankGroupLocked(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                       const std::pair<uint32_t, uint32_t> &p2pInfo,
                                       const std::vector<uint32_t> &indices, RankTransferDescriptors &descriptors,
                                       std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups) const;
    Result ResolveMultiRankIoLocked(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const std::pair<uint32_t, uint32_t> &p2pInfo, uint32_t index,
                                    RankTransferDescriptors &descriptors, RankGroupMap &unregisteredGroups) const;
    Result ResolveLocalAddressLocked(uint64_t addr, uint64_t size, uint64_t &correctedAddr, bool &registered) const;
    Result ResolveRemoteAddressLocked(uint32_t remoteRank, uint64_t remoteAddr, uint64_t size,
                                      uint64_t &correctedAddr) const;
    static HcommBatchTransferDesc BuildTransferDesc(uint64_t localAddr, uint64_t remoteAddr, uint64_t size,
                                                    bool isRead);
    Result PrepareMultiRankMarkersLocked(const std::vector<uint32_t> &rankIds,
                                         std::vector<HcommBatchTransferDesc> &markerDescs,
                                         std::vector<NotifyResource *> &notifyResources);
    Result FlattenMultiRankDescriptorsLocked(const RankTransferDescriptors &descriptors,
                                             std::vector<HcommBatchTransferDesc> &transferDescs,
                                             std::vector<uint32_t> &rankIds, std::vector<uint32_t> &rankStartIdx,
                                             std::vector<uint32_t> &rankListNum,
                                             std::vector<HcommThreadHandle> &threads,
                                             std::vector<HcommChannelHandle> &channels) const;
    Result ResolveAndFlattenMultiRankBatchLocked(const hybm_batch_copy_params &params,
                                                 hybm_data_copy_direction direction, const RankGroupMap &groupMap,
                                                 std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups,
                                                 std::vector<HcommBatchTransferDesc> &transferDescs,
                                                 std::vector<uint32_t> &rankIds, std::vector<uint32_t> &rankStartIdx,
                                                 std::vector<uint32_t> &rankListNum,
                                                 std::vector<HcommThreadHandle> &threads,
                                                 std::vector<HcommChannelHandle> &channels) const;
    Result LaunchMultiRankBatchLocked(const std::vector<HcommBatchTransferDesc> &transferDescs,
                                      const std::vector<uint32_t> &rankIds, const std::vector<uint32_t> &rankStartIdx,
                                      const std::vector<uint32_t> &rankListNum,
                                      const std::vector<HcommThreadHandle> &threads,
                                      const std::vector<HcommChannelHandle> &channels, std::set<uint32_t> &batchRanks);
    Result StageAndLaunchMultiTransfer(CompletionContext &ctx, const std::vector<HcommBatchTransferDesc> &transferDescs,
                                       size_t transferOffset, size_t transferCount,
                                       const std::vector<uint32_t> &rankIds, const std::vector<uint32_t> &rankStartIdx,
                                       const std::vector<uint32_t> &rankListNum,
                                       const std::vector<HcommThreadHandle> &threads,
                                       const std::vector<HcommChannelHandle> &channels,
                                       const std::vector<HcommBatchTransferDesc> *markerDescs,
                                       std::vector<NotifyResource *> *notifyResources);

    // Each thread uses only one manager.
    static CompletionContext &GetTlsContext();

    // Per-thread context lifecycle.
    CompletionContext *LookupOrCreateContextLocked();
    Result InitThreadContextLocked(CompletionContext &ctx);

    // 对端 rank 退出后的快速失败：故障 rank 集合用独立锁，避免与长时间持 shared_lock 的数据线程争用 mutex_
    void MarkRanksFaulted(const std::vector<uint32_t> &ranks);
    void UnmarkRankFaulted(uint32_t rankId);
    bool IsRankFaulted(uint32_t rankId) const;

    // Notify 池生命周期及无锁申请/归还；未转入 pending 的资源由调用方归还，pending 同步失败时隔离至 CloseDevice。
    Result InitNotifyPoolLocked();
    Result GrowNotifyPool();
    Result InitNotifyResource(NotifyResource &resource);
    void CleanupNotifyResource(NotifyResource &resource);
    void CleanupNotifyPoolLocked();
    Result AcquireNotifyResource(NotifyResource *&resource);
    void ReleaseNotifyResource(NotifyResource &resource);

    // CloseDevice helpers
    void CloseDeviceCleanupResourcesLocked();

    // Device kernel buffer management
    aclrtFuncHandle GetDeviceKernelFunc() const;
    static Result ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers);
    static Result EnsureKernelLaunchBufferCapacity(DeviceTransferBuffers &buffers, size_t requiredBytes);
    static Result ReleasePendingTransfersLocked(std::vector<PendingTransfer> &pendingTransfers);
    // Move all entries matching rankId from src to dst
    static void ExtractRankPending(std::vector<PendingTransfer> &src, uint32_t rankId,
                                   std::vector<PendingTransfer> &dst);
    Result SynchronizePreSubmittedNotifiesLocked(CompletionContext &ctx, const std::set<uint32_t> &rankIds);

    // Device kernel launch helpers
    Result PrepareKernelLaunchBuffers(const std::vector<HcommBatchTransferDesc> &transferDescs, size_t transferOffset,
                                      size_t transferCount, const std::vector<uint32_t> &rankIds,
                                      const std::vector<uint32_t> &rankStartIdx,
                                      const std::vector<uint32_t> &rankListNum,
                                      const std::vector<HcommThreadHandle> &threads,
                                      const std::vector<HcommChannelHandle> &channels,
                                      DeviceTransferBuffers &outBuffers,
                                      const std::vector<HcommBatchTransferDesc> *markerDescs = nullptr);
    // Device kernel launch (builds args, configures and launches)
    Result LaunchDeviceKernelBatch(const DeviceTransferBuffers &buffers, size_t batchSize, size_t rankNum);
    mutable std::shared_mutex mutex_{};
    bool opened_{false};
    uint32_t rankId_{0};
    uint32_t rankCount_{0};
    uint32_t userDeviceId_{0};
    uint32_t logicDeviceId_{0};
    uint32_t phyDeviceId_{0};
    uint32_t sdid_{0};
    uint32_t serverId_{0};
    uint32_t superPodId_{0};
    uint32_t mainboardId_{0};
    UrmaProtocol protocol_{UrmaProtocol::RESERVED};
    TransportOptions options_{};
    HcommApiWrapper hcommApi_;
    std::vector<uint32_t> localIds_;
    std::vector<HcommEndpointHandle> localEndpoint_;
    std::vector<UrmaEndpointDesc> localEndpointDesc_;

    // Device kernel launch state
    bool deviceKernelLoaded_{false};
    aclrtBinHandle deviceKernelHandle_{nullptr};
    DeviceFuncHandles deviceFuncHandles_{};
    // Local flag buffer allocated via AclrtMalloc in OpenDevice, initialised to 1.
    void *devTransFlagPtr_{nullptr};
    uint64_t devTransFlagSize_{0};
    std::vector<HcommMemHandle> devTransFlagHcommHandle_;
    std::unique_ptr<NotifyResource *[]> notifyPoolSlots_{}; // manager 级池，下标到稳定资源地址的只增映射
    std::atomic<uint32_t> notifyPoolSize_{0};               // 已初始化的 Notify 池槽位数量
    std::mutex notifyPoolGrowMutex_{};                      // 池耗尽时串行扩容，不进入正常申请路径
    std::atomic<uint64_t> notifyFreeHead_{UINT32_MAX};      // 高 32 位为版本号，低 32 位为空闲槽位下标

    // lock 粒度跟随 mutex_
    std::map<uint64_t, LocalRegistration> localRegistrations_{};
    std::unordered_map<uint32_t, RemoteRankState> remoteRanks_{};
    // 故障 rank 集合：独立锁，与 mutex_ 无嵌套反序
    mutable std::shared_mutex faultedRanksMutex_{};
    std::set<uint32_t> faultedRanks_{};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_DEVICE_URMA_TRANSPORT_MANAGER_H
