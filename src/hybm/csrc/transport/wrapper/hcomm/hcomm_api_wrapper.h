/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 *
 * HcommApiWrapper — unified entry point for all HCOMM API calls.
 * Wraps DlHcommApi with error handling, caching, and resource management.
 * All HCOMM-based transport managers use this class, not DlHcommApi directly.
 * Folds URMA's HcommTransportManager caching (memEntries, exportCache, refCount).
 */
#ifndef MF_HYBRID_HCOMM_API_WRAPPER_H
#define MF_HYBRID_HCOMM_API_WRAPPER_H

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "dl_hcomm_api.h"
#include "hybm_transport_common.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

using HcommEndpointHandle = void *;
using HcommMemHandleType = void *;

struct HcommChannelReadyPeer {
    ChannelHandle channel{0};
    uint32_t peerRank{0};
};

// ─────────────────────────────────────────────────────────────────
// MemEntry: cached memory registration entry
// ─────────────────────────────────────────────────────────────────
struct HcommMemEntry {
    HcommEndpointHandle endpoint{nullptr};
    HcommMemHandleType handle{nullptr};
    uint64_t tag{0};
    uint64_t addr{0};
    uint64_t size{0};
    CommMemType memType{COMM_MEM_TYPE_INVALID};
    uint32_t refCount{0};
    bool exportCacheValid{false};
    std::vector<uint8_t> exportCache{};
};

// Registration caches are scoped per endpoint: the same memTag / memory range may be
// registered on multiple endpoints, and each endpoint owns an independent handle.
struct EndpointMemTagKey {
    HcommEndpointHandle endpoint{nullptr};
    uint64_t memTag{0};

    bool operator==(const EndpointMemTagKey &other) const
    {
        return endpoint == other.endpoint && memTag == other.memTag;
    }
};

struct EndpointMemTagKeyHash {
    std::size_t operator()(const EndpointMemTagKey &key) const
    {
        return std::hash<const void *>()(key.endpoint) ^ (std::hash<uint64_t>()(key.memTag) << 1U);
    }
};

struct EndpointMemHandleKey {
    HcommEndpointHandle endpoint{nullptr};
    HcommMemHandleType handle{nullptr};

    bool operator==(const EndpointMemHandleKey &other) const
    {
        return endpoint == other.endpoint && handle == other.handle;
    }
};

struct EndpointMemHandleKeyHash {
    std::size_t operator()(const EndpointMemHandleKey &key) const
    {
        return std::hash<const void *>()(key.endpoint) ^ (std::hash<const void *>()(key.handle) << 1U);
    }
};

// ─────────────────────────────────────────────────────────────────
// HcommApiWrapper
// ─────────────────────────────────────────────────────────────────
class HcommApiWrapper {
public:
    HcommApiWrapper() = default;

    // Library lifecycle
    static Result LoadHcomm();
    static void UnloadHcomm();

    // ── Endpoint ──
    Result CreateEndpoint(const EndpointDesc &desc, HcommEndpointHandle &handle) const;
    Result DestroyEndpoint(HcommEndpointHandle handle) const;

    // ── Memory Registration (with caching, refCount, exportCache) ──
    // Tag = hash of the registration tag string
    using MemTag = uint64_t;

    // Full registration with caching: validates mem, tracks refCount,
    // detects overlaps, caches export descriptors.
    Result RegisterMemory(HcommEndpointHandle endpoint, MemTag memTag, const HcommCommMem &mem,
                          HcommMemHandleType &handle);
    Result UnregisterMemory(HcommEndpointHandle endpoint, HcommMemHandleType handle);

    // Cached export: returns cached descriptor if available.
    Result ExportMemory(HcommEndpointHandle endpoint, HcommMemHandleType handle, const uint8_t *&desc,
                        uint32_t &descLen);
    Result ImportMemory(HcommEndpointHandle endpoint, const uint8_t *desc, uint32_t descLen,
                        HcommCommMem &outMem) const;
    Result UnimportMemory(HcommEndpointHandle endpoint, const uint8_t *desc, uint32_t descLen) const;

    // ── Thread ──
    static Result AllocThread(CommEngine engine, uint32_t notifyNum, ThreadHandle &thread);
    static Result FreeThread(ThreadHandle &thread);

    // ── Channel ──
    static Result CreateChannel(HcommEndpointHandle endpoint, CommEngine engine, HcommChannelDesc &desc,
                                ChannelHandle &channel);
    static Result DestroyChannel(ChannelHandle &channel);
    static Result GetChannelStatus(ChannelHandle &channel, int32_t &status);
    static Result WaitForChannelsReady(const std::vector<HcommChannelReadyPeer> &peerRanks,
                                       std::chrono::milliseconds timeout = std::chrono::hours(2));

    // ── Raw (bypass cache) — for flag buffers, one-shot registration ──
    static Result RawMemReg(HcommEndpointHandle endpoint, const char *tag, const HcommCommMem &mem,
                            HcommMemHandleType &handle);
    static Result RawMemUnreg(HcommEndpointHandle endpoint, HcommMemHandleType handle);
    static Result RawMemExport(HcommEndpointHandle endpoint, HcommMemHandleType handle, void *&desc, uint32_t &descLen);
    static Result RawMemImport(HcommEndpointHandle endpoint, const void *desc, uint32_t descLen, HcommCommMem &outMem);
    static Result RawMemUnimport(HcommEndpointHandle endpoint, const void *desc, uint32_t descLen);

private:
    static bool GetRangeEnd(uint64_t addr, uint64_t size, uint64_t &end);
    static bool IsValidMem(const HcommCommMem &mem);
    static bool SameMem(const HcommMemEntry &entry, const HcommCommMem &mem);
    static bool Overlaps(const HcommMemEntry &entry, const HcommCommMem &mem);

    mutable std::mutex mutex_;
    std::unordered_map<EndpointMemTagKey, std::shared_ptr<HcommMemEntry>, EndpointMemTagKeyHash> tagIndex_;
    std::unordered_map<EndpointMemHandleKey, std::shared_ptr<HcommMemEntry>, EndpointMemHandleKeyHash> memEntries_;
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HCOMM_API_WRAPPER_H
