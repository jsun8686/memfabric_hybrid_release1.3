/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#include "hcomm_api_wrapper.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dl_hcomm_api.h"
#include "hybm_logger.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

// ── Library lifecycle ──

Result HcommApiWrapper::LoadHcomm()
{
    return DlHcommApi::LoadLibrary();
}

void HcommApiWrapper::UnloadHcomm()
{
    DlHcommApi::CleanupLibrary();
}

// ── Endpoint ──

Result HcommApiWrapper::CreateEndpoint(const EndpointDesc &desc, HcommEndpointHandle &handle) const
{
    EndpointHandle ep = nullptr;
    auto ret = DlHcommApi::HcommEndpointCreate(&desc, &ep);
    if (ret != 0) {
        BM_LOG_ERROR("HcommEndpointCreate failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    handle = ep;
    return BM_OK;
}

Result HcommApiWrapper::DestroyEndpoint(HcommEndpointHandle handle) const
{
    if (handle == nullptr) {
        return BM_OK;
    }
    auto ret = DlHcommApi::HcommEndpointDestroy(handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommEndpointDestroy failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

// ── Memory helpers ──

bool HcommApiWrapper::GetRangeEnd(uint64_t addr, uint64_t size, uint64_t &end)
{
    if (addr == 0 || size == 0) {
        return false;
    }
    if (std::numeric_limits<uint64_t>::max() - addr < size) {
        return false;
    }
    end = addr + size;
    return true;
}

bool HcommApiWrapper::IsValidMem(const HcommCommMem &mem)
{
    uint64_t end = 0;
    return (mem.type == COMM_MEM_TYPE_HOST || mem.type == COMM_MEM_TYPE_DEVICE) &&
           GetRangeEnd(reinterpret_cast<uint64_t>(mem.addr), mem.size, end);
}

bool HcommApiWrapper::SameMem(const HcommMemEntry &entry, const HcommCommMem &mem)
{
    return entry.addr == reinterpret_cast<uint64_t>(mem.addr) && entry.size == mem.size && entry.memType == mem.type;
}

bool HcommApiWrapper::Overlaps(const HcommMemEntry &entry, const HcommCommMem &mem)
{
    if (entry.memType != mem.type) {
        return false;
    }
    uint64_t entryEnd = 0;
    uint64_t memEnd = 0;
    uint64_t memAddr = reinterpret_cast<uint64_t>(mem.addr);
    return GetRangeEnd(entry.addr, entry.size, entryEnd) && GetRangeEnd(memAddr, mem.size, memEnd) &&
           entry.addr < memEnd && memAddr < entryEnd;
}

// ── Memory registration (cached) ──

Result HcommApiWrapper::RegisterMemory(HcommEndpointHandle endpoint, MemTag memTag, const HcommCommMem &mem,
                                       HcommMemHandleType &handle)
{
    if (endpoint == nullptr) {
        BM_LOG_ERROR("RegisterMemory: endpoint is null");
        return BM_INVALID_PARAM;
    }
    if (!IsValidMem(mem)) {
        BM_LOG_ERROR("RegisterMemory: invalid memory, addr: " << std::hex << mem.addr << " size: " << mem.size);
        return BM_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    const EndpointMemTagKey tagKey{endpoint, memTag};

    // Check if tag already registered on this endpoint
    auto tagIt = tagIndex_.find(tagKey);
    if (tagIt != tagIndex_.end()) {
        auto &entry = tagIt->second;
        if (entry == nullptr) {
            BM_LOG_ERROR("RegisterMemory: tag index points to non-existent memEntry, endpoint: "
                         << endpoint << " memTag: " << memTag);
            return BM_ERROR;
        }
        if (!SameMem(*entry, mem)) {
            BM_LOG_ERROR("RegisterMemory: memTag conflict, endpoint: " << endpoint << " memTag: " << memTag);
            return BM_ERROR;
        }
        entry->refCount++;
        handle = entry->handle;
        return BM_OK;
    }

    // Check overlap with existing registrations on the same endpoint
    for (const auto &item : memEntries_) {
        if (item.second != nullptr && item.second->endpoint == endpoint && Overlaps(*item.second, mem)) {
            BM_LOG_ERROR("RegisterMemory: memory range overlaps existing MR, endpoint: "
                         << endpoint << " addr: " << std::hex << mem.addr << " size: " << mem.size);
            return BM_ERROR;
        }
    }

    // Register with HCOMM
    HcommMemHandleType hcommHandle = nullptr;
    const std::string tag = std::to_string(memTag);
    int ret = DlHcommApi::HcommMemReg(endpoint, tag.c_str(), &mem, &hcommHandle);
    if (ret != 0) {
        BM_LOG_ERROR("RegisterMemory HcommMemReg failed, endpoint: " << endpoint << " addr: " << std::hex << mem.addr
                                                                     << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("RegisterMemory HcommMemReg success endpoint: " << endpoint << " addr: " << std::hex << mem.addr);

    try {
        auto entry = std::make_shared<HcommMemEntry>();
        entry->endpoint = endpoint;
        entry->handle = hcommHandle;
        entry->tag = memTag;
        entry->addr = reinterpret_cast<uint64_t>(mem.addr);
        entry->size = mem.size;
        entry->memType = mem.type;
        entry->refCount = 1;

        const EndpointMemHandleKey handleKey{endpoint, entry->handle};
        auto entryInserted = memEntries_.emplace(handleKey, entry);
        auto tagInserted = tagIndex_.emplace(tagKey, entry);
        if (!entryInserted.second || !tagInserted.second) {
            memEntries_.erase(handleKey);
            tagIndex_.erase(tagKey);
            int deregRet = DlHcommApi::HcommMemUnreg(endpoint, hcommHandle);
            if (deregRet != 0) {
                BM_LOG_ERROR("RegisterMemory: rollback HcommMemUnreg failed, endpoint: " << endpoint
                                                                                         << " ret: " << deregRet);
            }
            return BM_ERROR;
        }

        handle = entry->handle;
        return BM_OK;
    } catch (...) {
        int deregRet = DlHcommApi::HcommMemUnreg(endpoint, hcommHandle);
        if (deregRet != 0) {
            BM_LOG_ERROR("RegisterMemory: exception rollback HcommMemUnreg failed, endpoint: " << endpoint
                                                                                               << " ret: " << deregRet);
        }
        return BM_MALLOC_FAILED;
    }
}

Result HcommApiWrapper::UnregisterMemory(HcommEndpointHandle endpoint, HcommMemHandleType handle)
{
    if (endpoint == nullptr || handle == nullptr) {
        BM_LOG_ERROR("UnregisterMemory: endpoint or handle is null");
        return BM_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    auto entryIt = memEntries_.find(EndpointMemHandleKey{endpoint, handle});
    if (entryIt == memEntries_.end() || entryIt->second == nullptr) {
        BM_LOG_ERROR("UnregisterMemory: handle not found in cache, endpoint: " << endpoint << " handle: " << handle);
        return BM_INVALID_PARAM;
    }

    auto entry = entryIt->second;
    if (entry->refCount > 1) {
        entry->refCount--;
        return BM_OK;
    }

    const auto ret = DlHcommApi::HcommMemUnreg(endpoint, entry->handle);
    if (ret != 0) {
        BM_LOG_ERROR("UnregisterMemory HcommMemUnreg failed, endpoint: " << endpoint << " handle: " << entry->handle
                                                                         << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    tagIndex_.erase(EndpointMemTagKey{entry->endpoint, entry->tag});
    memEntries_.erase(entryIt);
    return BM_OK;
}

Result HcommApiWrapper::ExportMemory(HcommEndpointHandle endpoint, HcommMemHandleType handle, const uint8_t *&desc,
                                     uint32_t &descLen)
{
    if (endpoint == nullptr || handle == nullptr) {
        BM_LOG_ERROR("ExportMemory: endpoint or handle is null");
        return BM_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    auto entryIt = memEntries_.find(EndpointMemHandleKey{endpoint, handle});
    if (entryIt == memEntries_.end() || entryIt->second == nullptr) {
        BM_LOG_ERROR("ExportMemory: handle not found in cache, endpoint: " << endpoint << " handle: " << handle);
        return BM_INVALID_PARAM;
    }

    auto entry = entryIt->second;
    if (!entry->exportCacheValid) {
        void *hcommDesc = nullptr;
        uint32_t hcommDescLen = 0;
        auto ret = DlHcommApi::HcommMemExport(endpoint, entry->handle, &hcommDesc, &hcommDescLen);
        if (ret != 0) {
            BM_LOG_ERROR("ExportMemory HcommMemExport failed, ret: " << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        if (hcommDesc == nullptr || hcommDescLen == 0) {
            BM_LOG_ERROR("ExportMemory: hcommDesc is null or empty after HcommMemExport");
            return BM_ERROR;
        }

        try {
            std::vector<uint8_t> bytes(hcommDescLen);
            std::memcpy(bytes.data(), hcommDesc, hcommDescLen);
            entry->exportCache.swap(bytes);
            entry->exportCacheValid = true;
        } catch (...) {
            return BM_MALLOC_FAILED;
        }
    }

    desc = entry->exportCache.data();
    descLen = static_cast<uint32_t>(entry->exportCache.size());
    return BM_OK;
}

Result HcommApiWrapper::ImportMemory(HcommEndpointHandle endpoint, const uint8_t *desc, uint32_t descLen,
                                     HcommCommMem &outMem) const
{
    auto ret = DlHcommApi::HcommMemImport(endpoint, desc, descLen, &outMem);
    if (ret != 0) {
        BM_LOG_ERROR("ImportMemory HcommMemImport failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    if (outMem.type == COMM_MEM_TYPE_INVALID) {
        BM_LOG_ERROR("ImportMemory returned invalid memory type, endpoint: "
                     << endpoint << " descLen: " << descLen << " addr: " << outMem.addr << " size: " << outMem.size
                     << " type: " << outMem.type);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result HcommApiWrapper::UnimportMemory(HcommEndpointHandle endpoint, const uint8_t *desc, uint32_t descLen) const
{
    auto ret = DlHcommApi::HcommMemUnimport(endpoint, desc, descLen);
    if (ret != 0) {
        BM_LOG_WARN("UnimportMemory HcommMemUnimport failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

// ── Thread ──

Result HcommApiWrapper::AllocThread(CommEngine engine, uint32_t notifyNum, ThreadHandle &thread)
{
    ThreadHandle t = 0;
    auto ret = DlHcommApi::HcommThreadAlloc(engine, 1, &notifyNum, &t);
    if (ret != 0) {
        BM_LOG_ERROR("HcommThreadAlloc failed, engine=" << engine << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    thread = t;
    return BM_OK;
}

Result HcommApiWrapper::FreeThread(ThreadHandle &thread)
{
    if (thread == 0) {
        return BM_OK;
    }
    auto ret = DlHcommApi::HcommThreadFree(&thread, 1);
    if (ret != 0) {
        BM_LOG_ERROR("HcommThreadFree failed, thread: " << thread << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    thread = 0;
    return BM_OK;
}

// ── Channel ──

Result HcommApiWrapper::CreateChannel(HcommEndpointHandle endpoint, CommEngine engine, HcommChannelDesc &desc,
                                      ChannelHandle &channel)
{
    ChannelHandle ch = 0;
    auto ret = DlHcommApi::HcommChannelCreate(endpoint, engine, &desc, 1, &ch);
    if (ret != 0) {
        BM_LOG_ERROR("HcommChannelCreate failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    channel = ch;
    return BM_OK;
}

Result HcommApiWrapper::DestroyChannel(ChannelHandle &channel)
{
    if (channel == 0) {
        return BM_OK;
    }
    auto ret = DlHcommApi::HcommChannelDestroy(&channel, 1);
    if (ret != 0) {
        BM_LOG_ERROR("HcommChannelDestroy failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    channel = 0;
    return BM_OK;
}

Result HcommApiWrapper::GetChannelStatus(ChannelHandle &channel, int32_t &status)
{
    return DlHcommApi::HcommChannelGetStatus(&channel, 1, &status);
}

namespace {
constexpr int32_t HCOMM_CHANNEL_READY = 0;
constexpr int32_t HCOMM_CHANNEL_IN_PROGRESS = 1;
constexpr int32_t HCOMM_CHANNEL_FAILED = 2;
constexpr int32_t HCOMM_CHANNEL_TIMEOUT = 3;
constexpr auto HCOMM_CHANNEL_POLL_INTERVAL = std::chrono::milliseconds(100);

std::string DescribePeerRanks(const std::vector<HcommChannelReadyPeer> &peerRanks)
{
    std::ostringstream stream;
    stream << "[";
    for (size_t index = 0; index < peerRanks.size(); ++index) {
        if (index != 0U) {
            stream << ", ";
        }
        stream << peerRanks[index].peerRank;
    }
    stream << "]";
    return stream.str();
}
} // namespace

Result HcommApiWrapper::WaitForChannelsReady(const std::vector<HcommChannelReadyPeer> &peerRanks,
                                             std::chrono::milliseconds timeout)
{
    if (peerRanks.empty()) {
        return BM_OK;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + timeout;
    auto pendingPeerRanks = peerRanks;

    while (!pendingPeerRanks.empty() && std::chrono::steady_clock::now() < deadline) {
        // Poll every pending channel once per round. Blocking on one channel can form a cycle when peers choose
        // different first channels, even though all channel handles have already been created.
        for (auto peerIt = pendingPeerRanks.begin(); peerIt != pendingPeerRanks.end();) {
            int32_t channelStatus = HCOMM_CHANNEL_IN_PROGRESS;
            auto ret = DlHcommApi::HcommChannelGetStatus(&peerIt->channel, 1, &channelStatus);
            if (ret != 0) {
                BM_LOG_ERROR("HcommChannelGetStatus failed, channel: " << peerIt->channel << " peer: "
                                                                       << peerIt->peerRank << " ret: " << ret);
                return BM_DL_FUNCTION_FAILED;
            }
            if (channelStatus == HCOMM_CHANNEL_READY) {
                peerIt = pendingPeerRanks.erase(peerIt);
                continue;
            }
            if (channelStatus == HCOMM_CHANNEL_FAILED) {
                BM_LOG_ERROR("channel FAILED, channel: " << peerIt->channel << " peer: " << peerIt->peerRank);
                return BM_NOT_CONNECTED;
            }
            if (channelStatus == HCOMM_CHANNEL_TIMEOUT) {
                BM_LOG_ERROR("channel TIMEOUT, channel: " << peerIt->channel << " peer: " << peerIt->peerRank);
                return BM_TIMEOUT;
            }
            if (channelStatus != HCOMM_CHANNEL_IN_PROGRESS) {
                BM_LOG_ERROR("channel unknown status, channel: " << peerIt->channel << " peer: " << peerIt->peerRank
                                                                 << " status: " << channelStatus);
                return BM_DL_FUNCTION_FAILED;
            }
            ++peerIt;
        }
        if (!pendingPeerRanks.empty()) {
            const auto now = std::chrono::steady_clock::now();
            BM_LOG_INFO_LIMIT("pendingPeerCount: " << pendingPeerRanks.size() << ","
                                                   << " In Progress PeerRanks: "
                                                   << DescribePeerRanks(pendingPeerRanks));
            const auto nextPoll = now + HCOMM_CHANNEL_POLL_INTERVAL;
            std::this_thread::sleep_until(nextPoll < deadline ? nextPoll : deadline);
        }
    }

    for (const auto &peer : pendingPeerRanks) {
        BM_LOG_ERROR("channel wait TIMEOUT, channel: " << peer.channel << " peerRank: " << peer.peerRank
                                                       << " timeout_ms: " << timeout.count()
                                                       << " status: " << HCOMM_CHANNEL_IN_PROGRESS);
    }
    return pendingPeerRanks.empty() ? BM_OK : BM_TIMEOUT;
}

// ── Raw (bypass cache) ──

Result HcommApiWrapper::RawMemReg(HcommEndpointHandle endpoint, const char *tag, const HcommCommMem &mem,
                                  HcommMemHandleType &handle)
{
    auto ret = DlHcommApi::HcommMemReg(endpoint, tag, &mem, &handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemReg failed, addr: " << std::hex << mem.addr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result HcommApiWrapper::RawMemUnreg(HcommEndpointHandle endpoint, HcommMemHandleType handle)
{
    if (handle == nullptr) {
        return BM_OK;
    }
    auto ret = DlHcommApi::HcommMemUnreg(endpoint, handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemUnreg failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

Result HcommApiWrapper::RawMemExport(HcommEndpointHandle endpoint, HcommMemHandleType handle, void *&desc,
                                     uint32_t &descLen)
{
    return DlHcommApi::HcommMemExport(endpoint, handle, &desc, &descLen);
}

Result HcommApiWrapper::RawMemImport(HcommEndpointHandle endpoint, const void *desc, uint32_t descLen,
                                     HcommCommMem &outMem)
{
    return DlHcommApi::HcommMemImport(endpoint, desc, descLen, &outMem);
}

Result HcommApiWrapper::RawMemUnimport(HcommEndpointHandle endpoint, const void *desc, uint32_t descLen)
{
    return DlHcommApi::HcommMemUnimport(endpoint, desc, descLen);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
