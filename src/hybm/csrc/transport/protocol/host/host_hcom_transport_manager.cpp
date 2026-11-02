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
#include "host_hcom_transport_manager.h"

#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <arpa/inet.h>
#include "dl_hcom_api.h"
#include "hybm_logger.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "host_hcom_common.h"
#include "host_hcom_helper.h"
#include "mf_tls_util.h"
#include "hybm_ptracer.h"
#include "hybm_va_manager.h"

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::host;

namespace {
#if defined(NO_XPU)
constexpr uint64_t HCOM_MAX_SLICE_SIZE = 1024 * 1024 * 1024;
constexpr uint64_t HCOM_RECV_DATA_SIZE = 1024 * 1024UL;
constexpr uint64_t HCOM_SEND_QUEUE_SIZE = 16384UL;
constexpr uint64_t HCOM_RECV_QUEUE_SIZE = 16384UL;
constexpr uint64_t HCOM_COMPLETE_QUEUE_SIZE = 8192;
constexpr uint64_t HCOM_QUEUE_PRE_POST_SIZE = 1024UL;
constexpr uint8_t HCOM_TRANS_EP_SIZE = 1;
constexpr int8_t HCOM_THREAD_PRIORITY = -20;
constexpr uint64_t UB_SEGMENT_ADDR_ALIGN_SIZE = 4096UL;
#else
constexpr uint64_t HCOM_MAX_SLICE_SIZE = 1024 * 1024UL;
constexpr uint64_t HCOM_RECV_DATA_SIZE = HCOM_MAX_SLICE_SIZE + 1024;
constexpr uint64_t HCOM_SEND_QUEUE_SIZE = 512UL;
constexpr uint64_t HCOM_RECV_QUEUE_SIZE = 512UL;
constexpr uint64_t HCOM_COMPLETE_QUEUE_SIZE = 8192;
constexpr uint64_t HCOM_QUEUE_PRE_POST_SIZE = 256UL;
constexpr uint8_t HCOM_TRANS_EP_SIZE = 1;
constexpr int8_t HCOM_THREAD_PRIORITY = -20;
#endif
const char *HCOM_RPC_SERVICE_NAME = "hybm_hcom_service";

HcomRuntimeConfig LoadHcomRuntimeConfig()
{
    HcomRuntimeConfig runtimeConfig{};
    runtimeConfig.maxSliceSize = HCOM_MAX_SLICE_SIZE;
    runtimeConfig.recvDataSize = HCOM_RECV_DATA_SIZE;
    runtimeConfig.maxSliceSize = MfEnvUtil::GetUintOrDefault(env::HCOM_MAX_SLICE_SIZE, HCOM_MAX_SLICE_SIZE);
    runtimeConfig.recvDataSize = MfEnvUtil::GetUintOrDefault(env::HCOM_RECV_DATA_SIZE, HCOM_RECV_DATA_SIZE);
    return runtimeConfig;
}

void HcomExternalLoggerAdapter(int level, const char *msg)
{
    const char *safeMsg = (msg == nullptr) ? "" : msg;
    switch (level) {
        case ock::mf::DEBUG_LEVEL:
            BM_LOG_DEBUG("[HCOM] " << safeMsg);
            break;
        case ock::mf::INFO_LEVEL:
            BM_LOG_INFO("[HCOM] " << safeMsg);
            break;
        case ock::mf::WARN_LEVEL:
            BM_LOG_WARN("[HCOM] " << safeMsg);
            break;
        case ock::mf::ERROR_LEVEL:
            BM_LOG_ERROR("[HCOM] " << safeMsg);
            break;
        default:
            BM_LOG_INFO("[HCOM-" << level << "] " << safeMsg);
            break;
    }
}
} // namespace

hybm_tls_config HcomTransportManager::tlsConfig_ = {};
char HcomTransportManager::keyPass_[KEYPASS_MAX_LEN] = {0};
std::mutex HcomTransportManager::keyPassMutex = {};
thread_local HcomCounterStreamPtr HcomTransportManager::stream_ = nullptr;

static void CopyHcomOneSideKey(const OneSideKey &from, TransportMemoryKey &to)
{
    size_t words = std::size(from.keys) + std::size(from.tokens);
#if defined(NO_XPU)
    words += (URMA_EID_LENGTH + sizeof(uint64_t) - 1U) / sizeof(uint64_t);
#endif
    to.ResizeWords(words);
    std::copy_n(from.keys, std::size(from.keys), to.keys.begin());
    std::copy_n(from.tokens, std::size(from.tokens), to.keys.begin() + std::size(from.keys));
#if defined(NO_XPU)
    auto offset = std::size(from.tokens) + std::size(from.keys);
    std::copy_n(from.eid, URMA_EID_LENGTH, to.ByteData() + offset * sizeof(uint64_t));
#endif
}

static void CopyHcomLocalKeyWords(const TransportMemoryKey &from, OneSideKey &to)
{
    if (from.keys.size() < std::size(to.keys)) {
        return;
    }
    std::copy_n(from.keys.begin(), std::size(to.keys), to.keys);
}

static void CopyHcomOneSideKey(const TransportMemoryKey &from, OneSideKey &to)
{
    const size_t offset = std::size(to.keys) + std::size(to.tokens);
    size_t needBytes = offset * sizeof(uint64_t);
#if defined(NO_XPU)
    needBytes += URMA_EID_LENGTH;
#endif
    if (from.ByteSize() < needBytes) {
        return;
    }
    std::copy_n(from.keys.begin(), std::size(to.keys), to.keys);
    std::copy_n(from.keys.begin() + std::size(to.keys), std::size(to.tokens), to.tokens);
#if defined(NO_XPU)
    std::copy_n(from.ByteData() + offset * sizeof(uint64_t), URMA_EID_LENGTH, to.eid);
#endif
}

Result HcomTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> openLock(openMutex_);
    // per-instance double-open protection (the normal flow already guarantees one open per instance in compose)
    if (rpcService_ != 0) {
        ++openRefCnt_;
        BM_LOG_INFO("hcom service already opened, increase refCnt: " << openRefCnt_);
        return BM_OK;
    }
    if (!BindSlot()) {
        BM_LOG_ERROR("Failed to allocate hcom transport slot, max instances: " << kMaxInstances);
        return BM_ERROR;
    }
    serviceName_ = std::string(HCOM_RPC_SERVICE_NAME) + "_" + std::to_string(slot_);
    auto ret = CheckTransportOptions(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to check transport options, nic: " << options.nic << " ret: " << ret);
        ReleaseSlot();
        return ret;
    }
    runtimeConfig_ = LoadHcomRuntimeConfig();
    DlHcomApi::SetExternalLogger(HcomExternalLoggerAdapter);
    Service_Options opt{};
    opt.workerGroupMode = C_SERVICE_BUSY_POLLING;
    opt.maxSendRecvDataSize = runtimeConfig_.recvDataSize;
    opt.workerThreadPriority = HCOM_THREAD_PRIORITY;
    Service_Type enumProtocolType = HostHcomHelper::HybmDopTransHcomProtocol(options.protocol, options.nic);
    ret = DlHcomApi::ServiceCreate(enumProtocolType, serviceName_.c_str(), opt, &rpcService_);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to create hcom service, nic: " << options.nic << " type: " << enumProtocolType
                                                            << " ret: " << ret);
        ReleaseSlot();
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_TRACE("Create hcom service successful, nic: " << options.nic << " type: " << enumProtocolType
                                                         << " name: " << serviceName_);
    DlHcomApi::ServiceSetHeartBeatOptions(rpcService_, 10, 3, 5); /* idle 10s, probe 3*5s, total ~25s */
    tlsConfig_ = options.tlsOption;
    DlHcomApi::ServiceSetTlsOptions(rpcService_, options.tlsOption.tlsEnable, C_SERVICE_TLS_1_3, C_SERVICE_AES_GCM_256,
                                    GetCertCallBack, GetPrivateKeyCallBack, GetCACallBack);
    DlHcomApi::SetUbsModeFunc(rpcService_, UbsHcomServiceUbcMode::C_SERVICE_HIGHBANDWIDTH);
    DlHcomApi::ServiceRegisterChannelBrokerHandler(rpcService_, EndPointBrokenTramps()[slot_], C_CHANNEL_RECONNECT,
                                                   slot_);
    DlHcomApi::ServiceRegisterHandler(rpcService_, C_SERVICE_REQUEST_RECEIVED, TransportRpcHcomRequestReceived, slot_);
    DlHcomApi::ServiceRegisterHandler(rpcService_, C_SERVICE_REQUEST_POSTED, TransportRpcHcomRequestPosted, slot_);
    DlHcomApi::ServiceRegisterHandler(rpcService_, C_SERVICE_READWRITE_DONE, TransportRpcHcomOneSideDone, slot_);

    if (enumProtocolType != Service_Type::C_SERVICE_UBC) {
        std::string ipMask = localIp_ + "/32";
        DlHcomApi::ServiceSetDeviceIpMask(rpcService_, ipMask.c_str());
    }

    ret = reconnect_.Start([this](uint32_t rankId, const std::string &nic) { return ConnectHcomChannel(rankId, nic); });
    if (ret != BM_OK) {
        BM_LOG_ERROR("start reconnect service failed: " << ret);
        DlHcomApi::ServiceDestroy(rpcService_, serviceName_.c_str());
        rpcService_ = 0;
        ReleaseSlot();
        return ret;
    }

    SetHcomServiceConfig(rpcService_);
    ret = BindServiceWithPortRetry();
    if (ret != BM_OK) {
        DlHcomApi::ServiceDestroy(rpcService_, serviceName_.c_str());
        reconnect_.Stop();
        rpcService_ = 0;
        ReleaseSlot();
        return ret;
    }
    ret = DlHcomApi::ServiceStart(rpcService_);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to start hcom service, nic: " << localNic_ << " type: " << enumProtocolType
                                                           << " ret: " << ret);
        DlHcomApi::ServiceDestroy(rpcService_, serviceName_.c_str());
        reconnect_.Stop();
        rpcService_ = 0;
        ReleaseSlot();
        return BM_DL_FUNCTION_FAILED;
    }
    bmOptype_ = static_cast<hybm_data_op_type>(options.protocol);
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    mrMutex_ = std::vector<std::mutex>(rankCount_);
    mrs_ = std::vector<std::set<HcomMemoryRegion>>(rankCount_);
    mrRefCnts_ = std::vector<std::map<uint64_t, uint32_t>>(rankCount_);
    channelMutex_ = std::vector<std::mutex>(rankCount_);
    nics_ = std::vector<std::string>(rankCount_, "");
    channels_ = std::vector<Hcom_Channel>(rankCount_, 0);
    openRefCnt_ = 1;
    return BM_OK;
}

Result HcomTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> openLock(openMutex_);
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_OK);
    // protection against closing an instance twice: only decrement the count, do not destroy service / MR /
    // reconnect thread
    if (openRefCnt_ > 1) {
        --openRefCnt_;
        BM_LOG_INFO("hcom service still used by other entities, skip destroy, refCnt: " << openRefCnt_);
        return BM_OK;
    }
    openRefCnt_ = 0;

    DlHcomApi::SetExternalLogger([]([[maybe_unused]] int level, [[maybe_unused]] const char *msg) {});
    reconnect_.Stop();
    auto service = rpcService_;
    for (uint32_t i = 0; i < rankCount_; ++i) {
        if (channels_[i] != 0) {
            DisConnectHcomChannel(i, channels_[i]);
        }
    }

    // destroy all registered MRs first to release UBContext refs before ServiceDestroy
    for (uint32_t i = 0; i < mrMutex_.size(); ++i) {
        std::unique_lock<std::mutex> lock(mrMutex_[i]);
        for (auto it = mrs_[i].begin(); it != mrs_[i].end();) {
            DlHcomApi::ServiceDestroyMemoryRegion(service, it->mr);
            it = mrs_[i].erase(it);
        }
    }

    // Remove the callback slot before destroying the service; the service name must be taken out first
    // (ReleaseSlot clears serviceName_).
    const std::string serviceName = serviceName_;
    ReleaseSlot();

    auto ret = DlHcomApi::ServiceDestroy(service, serviceName.c_str());
    if (ret != 0) {
        BM_LOG_WARN("Unable to destroy hcom service, ret: " << ret);
    }

    mf::MfTlsUtil::CloseTlsLib();
    rpcService_ = 0;
    localNic_ = "";
    localIp_ = "";
    rankId_ = UINT32_MAX;
    rankCount_ = 0;
    runtimeConfig_ = {};
    mrMutex_.clear();
    mrs_.clear();
    mrRefCnts_.clear();
    channelMutex_.clear();
    nics_.clear();
    channels_.clear();
    return BM_OK;
}

// Handling an existing MR at the same address: same address and same size is treated as a multi-entity
// replay and reused with the reference count incremented; otherwise it is reported as a range conflict.
Result HcomTransportManager::HandleExistingMemoryRegion(const TransportMemoryRegion &mr, const HcomMemoryRegion &info)
{
    if (info.addr == mr.addr && info.size == mr.size) {
        std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
        const uint32_t refCnt = ++mrRefCnts_[rankId_][mr.addr];
        BM_LOG_INFO("Mem region already registered, reuse it. addr: " << mr.addr << " size: " << mr.size
                                                                      << " refCnt: " << refCnt);
        return BM_OK;
    }
    BM_LOG_ERROR("Failed to register mem region, addr: " << mr.addr << " size: " << mr.size
                                                         << " conflicts with existing region addr: " << info.addr
                                                         << " size: " << info.size);
    return BM_ERROR;
}

#if defined(ASCEND_NPU) || defined(NVIDIA_GPU)
Result HcomTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(mr.addr != 0 && mr.size != 0, "mr.addr = " << mr.addr << ", " << "mr.size = " << mr.size,
                             BM_INVALID_PARAM);
    const bool isHbm = (mr.flags & transport::REG_MR_FLAG_HBM) != 0;
    if ((mr.flags & (transport::REG_MR_FLAG_DRAM | transport::REG_MR_FLAG_HBM)) == 0) {
        BM_LOG_WARN("Only support register dram/hbm memory, skip flag:" << mr.flags);
        return BM_OK;
    }

    HcomMemoryRegion info{};
    if (GetMemoryRegionByAddr(rankId_, mr.addr, info) == BM_OK) {
        return HandleExistingMemoryRegion(mr, info);
    }

    Service_MemoryRegion memoryRegion;
    int32_t ret = DlHcomApi::ServiceRegisterAssignMemoryRegion(rpcService_, mr.addr, mr.size, &memoryRegion);
    if (ret != 0) {
        if (isHbm) {
            // Old HDK libhcom can not register hbm to host rdma service; skip and degrade to swap path.
            BM_LOG_WARN("Failed to register hbm mem region, maybe hdk not support (ret: "
                        << ret << "), fall back to swap path, size: " << mr.size << " addr:" << std::hex << mr.addr);
            return BM_OK;
        }
        BM_LOG_ERROR("Failed to register mem region, size: " << mr.size << " addr:" << std::hex << mr.addr
                                                             << " service: " << rpcService_ << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    Service_MemoryRegionInfo memoryRegionInfo;
    ret = DlHcomApi::ServiceGetMemoryRegionInfo(memoryRegion, &memoryRegionInfo);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to get mem region info, size: " << mr.size << " service: " << rpcService_
                                                             << " ret: " << ret);
        DlHcomApi::ServiceDestroyMemoryRegion(rpcService_, memoryRegion);
        return BM_DL_FUNCTION_FAILED;
    }

    HcomMemoryRegion mrInfo{};
    mrInfo.lva = mr.addr;
    mrInfo.addr = mr.addr;
    mrInfo.size = mr.size;
    mrInfo.mr = memoryRegion;
    CopyHcomOneSideKey(memoryRegionInfo.lKey, mrInfo.lKey);
    {
        std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
        mrs_[rankId_].insert(mrInfo);
        mrRefCnts_[rankId_][mr.addr] = 1;
    }
    BM_LOG_INFO("Success to register " << (isHbm ? "hbm" : "dram") << " mr info size: " << mrInfo.size
                                       << " lKey: " << mrInfo.lKey.keys[0]);
    return BM_OK;
}
#else
Result HcomTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(mr.addr != 0 && mr.size != 0, "mr.addr = " << mr.addr << ", " << "mr.size = " << mr.size,
                             BM_INVALID_PARAM);
    if ((mr.flags & transport::REG_MR_FLAG_DRAM) == 0) {
        BM_LOG_WARN("Unable to register hcom mr, mem type flag should be dram.");
        return BM_OK;
    }

    if ((bmOptype_ == HYBM_DOP_TYPE_HOST_URMA) && (mr.addr & (UB_SEGMENT_ADDR_ALIGN_SIZE - 1)) != 0) {
        BM_LOG_ERROR("Failed to register ub mem region, whose addr must be 4k aligned");
        return BM_INVALID_PARAM;
    }

    HcomMemoryRegion info{};
    if (GetMemoryRegionByAddr(rankId_, mr.addr, info) == BM_OK) {
        return HandleExistingMemoryRegion(mr, info);
    }

    Service_MemoryRegion memoryRegion;
    int32_t ret = DlHcomApi::ServiceRegisterAssignMemoryRegion(rpcService_, mr.addr, mr.size, &memoryRegion);
    // 单rank不需要hcom,目的是在无网卡的情况下也可以测试
    if (ret != 0) {
        BM_LOG_ERROR("Failed to register mem region, size: " << mr.size << " addr:" << std::hex << mr.addr
                                                             << " service: " << rpcService_ << " ret: " << ret);
        return BM_ERROR;
    }

    Service_MemoryRegionInfo memoryRegionInfo;
    if (ret == 0) {
        ret = DlHcomApi::ServiceGetMemoryRegionInfo(memoryRegion, &memoryRegionInfo);
    }
    if (ret != 0) {
        BM_LOG_ERROR("Failed to get mem region info, size: " << mr.size << " service: " << rpcService_
                                                             << " ret: " << ret);
        DlHcomApi::ServiceDestroyMemoryRegion(rpcService_, memoryRegion);
        return BM_ERROR;
    }

    HcomMemoryRegion mrInfo{};
    mrInfo.lva = mr.addr;
    mrInfo.addr = mr.addr;
    mrInfo.size = mr.size;
    mrInfo.mr = memoryRegion;
    CopyHcomOneSideKey(memoryRegionInfo.lKey, mrInfo.lKey);
    {
        std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
        mrs_[rankId_].insert(mrInfo);
        mrRefCnts_[rankId_][mr.addr] = 1;
    }
    BM_LOG_INFO("Success to register to mr info size: " << mrInfo.size << " lKey: " << mrInfo.lKey.keys[0] << std::hex
                                                        << " laddr:" << mr.addr);
    return BM_OK;
}
#endif

Result HcomTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    BM_ASSERT_LOG_AND_RETURN(addr != 0, "addr = " << addr, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);

    std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
    auto &localMrs = mrs_[rankId_];
    for (auto it = localMrs.begin(); it != localMrs.end(); it++) {
        if (it->addr == addr) {
            // Other entities still reference this MR: only decrement the reference count, and really destroy it when
            // the last referrer deregisters
            auto cntIt = mrRefCnts_[rankId_].find(addr);
            if (cntIt != mrRefCnts_[rankId_].end() && cntIt->second > 1) {
                --cntIt->second;
                BM_LOG_INFO("Addr: " << addr << " unregister deferred, refCnt: " << cntIt->second);
                return BM_OK;
            }
            if (cntIt != mrRefCnts_[rankId_].end()) {
                mrRefCnts_[rankId_].erase(cntIt);
            }
            DlHcomApi::ServiceDestroyMemoryRegion(rpcService_, it->mr);
            localMrs.erase(it);
            BM_LOG_INFO("Addr: " << addr << " unregistered");
            return BM_OK;
        }
    }
    BM_LOG_WARN("Addr: " << addr << " not registered");
    return BM_OK;
}

bool HcomTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
    for (const auto &mrInfo : mrs_[rankId_]) {
        if (mrInfo.addr <= addr && mrInfo.addr + mrInfo.size >= addr + size) {
            return true;
        }
    }
    return false;
}

Result HcomTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    HcomMemoryRegion mrInfo{};
    if (GetMemoryRegionByAddr(rankId_, addr, mrInfo) != BM_OK) {
        BM_LOG_ERROR("Failed to query memory region, addr: 0x" << std::hex << addr << " rankId: " << rankId_);
        return BM_ERROR;
    }
    RegMemoryKey hostKey{};
    hostKey.type = TT_HCOM;
    // The reverse lookup may miss (that HVA never entered the DVA/HVA tables), in which case fall back to the
    // HVA: otherwise 0 would be taken downstream as the peer MR address and break the range matching.
    const uint64_t ownerGva = HybmVaManager::GetInstance().TransformVa(mrInfo.addr, HVM_HVA, HVM_GVA);
    hostKey.gva = (ownerGva != 0) ? ownerGva : mrInfo.addr;
    hostKey.hcomInfo.lAddress = mrInfo.addr;
    CopyHcomOneSideKey(mrInfo.lKey, hostKey.hcomInfo.lKey);
    hostKey.hcomInfo.size = mrInfo.size;
    WriteStructToKey(hostKey, key);
    BM_LOG_INFO("Success to query memory key addr:" << std::hex << mrInfo.addr << " size:" << mrInfo.size);
    return BM_OK;
}

void HcomTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr == nullptr) {
        return;
    }
    RegMemoryKey hostKey{};
    if (!ReadStructFromKey(key, hostKey) || hostKey.type != TT_HCOM) {
        return;
    }
    // trans scene: the key's gva was derived from the exporter's HVA and does not share its origin with the
    // local rebuilt GVA handed down by the data plane;
    // overwrite it with the caller address so the peer MR range matching matches the data plane.
    hostKey.gva = reinterpret_cast<uint64_t>(addr);
    WriteStructToKey(hostKey, key);
}

Result HcomTransportManager::Prepare(const HybmTransPrepareOptions &param)
{
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("Failed to update rank info ranId: " << rankId << " not match rank count: " << rankCount_);
            return BM_INVALID_PARAM;
        }
    }

    std::vector<uint32_t> toAddRanks;
    toAddRanks.reserve(options.size());
    for (const auto &item : options) {
        auto rankId = item.first;
        auto nic = item.second.nic;
        nics_[rankId] = nic;
        toAddRanks.emplace_back(rankId);
    }
    reconnect_.AddRanks(toAddRanks);

    return UpdateRankMrInfos(options);
}

Result HcomTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    BM_LOG_DEBUG("HCOM transport manager remove ranks not implements!");
    reconnect_.RemoveRanks(removedRanks);
    return BM_OK;
}

static constexpr uint32_t HCOM_CHANNEL_READY_TIMEOUT_MS = 5000;
static constexpr uint32_t HCOM_CHANNEL_POLL_INTERVAL_US = 1000;

Result HcomTransportManager::WaitChannelReady(uint32_t rankId, uint32_t timeoutMs) noexcept
{
    auto startTime = std::chrono::steady_clock::now();
    auto deadline = startTime + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
            if (channels_[rankId] != 0) {
                return BM_OK;
            }
        }
        usleep(HCOM_CHANNEL_POLL_INTERVAL_US);
    }
    return BM_ERROR;
}

Result HcomTransportManager::ConnectRank(uint32_t rankId)
{
    if (rankId >= rankCount_ || nics_[rankId].empty()) {
        return BM_OK;
    }
    if (rankId_ <= rankId) {
        // Lower rank: wait for higher rank's ServiceConnect to trigger our HCOM callback
        return WaitChannelReady(rankId, HCOM_CHANNEL_READY_TIMEOUT_MS);
    }
    TP_TRACE_BEGIN(TP_SMEM_GROUP_CONNECT_RANK);
    const auto ret = ConnectHcomChannel(rankId, nics_[rankId]);
    if (ret != BM_OK) {
        TP_TRACE_END(TP_SMEM_GROUP_CONNECT_RANK, 1);
        return ret;
    }
    // Active side: serviceConnect is complete, but wait for passive side to confirm too
    auto waitRet = WaitChannelReady(rankId, HCOM_CHANNEL_READY_TIMEOUT_MS);
    if (waitRet != BM_OK) {}
    TP_TRACE_END(TP_SMEM_GROUP_CONNECT_RANK, 0);
    return BM_OK;
}

Result HcomTransportManager::UpdateRankMrInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &opt)
{
    for (const auto &item : opt) {
        auto rankId = item.first;
        if (rankId == rankId_) {
            continue;
        }
        for (const auto &memKey : item.second.memKeys) {
            RegMemoryKey hostKey{};
            if (!ReadStructFromKey(memKey, hostKey)) {
                BM_LOG_ERROR("Failed to parse host memory key rankId: " << rankId << " bytes: " << memKey.ByteSize());
                continue;
            }
            HcomMemoryRegion mrInfo{};
            mrInfo.lva = hostKey.hcomInfo.lAddress;
            mrInfo.addr = hostKey.gva;
            mrInfo.size = hostKey.hcomInfo.size;
            if (mrInfo.size == 0) {
                continue;
            }
            if (rankId != rankId_ && (bmOptype_ & HYBM_DOP_TYPE_HOST_URMA)) {
                auto ret = DlHcomApi::ImportUrmaSegFunc(rpcService_, mrInfo.addr, mrInfo.size, &hostKey.hcomInfo.lKey);
                BM_ASSERT_LOG_AND_RETURN(ret == 0, "ret = " << ret, ret);
                BM_LOG_DEBUG("hcom returned, tokens: " << hostKey.hcomInfo.lKey.tokens[0]);
            }
            CopyHcomOneSideKey(hostKey.hcomInfo.lKey, mrInfo.lKey);
            {
                std::unique_lock<std::mutex> lock(mrMutex_[rankId]);
                mrs_[rankId].insert(mrInfo);
            }
            BM_LOG_INFO("Success to register to mr info rankId: " << rankId << " size: " << mrInfo.size
                                                                  << " lKey: " << mrInfo.lKey.keys[0]);
        }
    }
    return BM_OK;
}

Result HcomTransportManager::UpdateRankConnectInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &opt)
{
    std::vector<uint32_t> addRankList;
    for (uint32_t i = 0; i < rankCount_; ++i) {
        if (i >= rankId_) {
            break;
        }
        auto it = opt.find(i);
        if (it != opt.end()) {
            nics_[i] = it->second.nic;
            addRankList.emplace_back(i);
            BM_LOG_DEBUG("UpdateRankConnectInfos: saved nics for rank " << i << " url=" << nics_[i]);
        }
    }

    reconnect_.AddRanks(addRankList);
    return BM_OK;
}

Result HcomTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &param)
{
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("Failed to update rank info ranId: " << rankId << " not match rank count: " << rankCount_);
            return BM_INVALID_PARAM;
        }
    }
    auto ret = UpdateRankMrInfos(param.options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to update rank mr info ret: " << ret);
        return ret;
    }
    ret = UpdateRankConnectInfos(param.options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to update rank connect info ret: " << ret);
        return ret;
    }
    return BM_OK;
}

const std::string &HcomTransportManager::GetNic() const
{
    return localNic_;
}

Result HcomTransportManager::InnerReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(size <= std::numeric_limits<uint32_t>::max(),
                             "size = " << size << " > " << std::numeric_limits<uint32_t>::max(), BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;

    req.lAddress = (void *)lAddr;
    req.size = static_cast<uint32_t>(size);

    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, lAddr: is not register");
        return BM_ERROR;
    }
    CopyHcomLocalKeyWords(mr.lKey, req.lKey);
    ret = GetMemoryRegionByAddr(rankId, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " is not set");
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                               << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                               << " size: " << size);

    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    req.rAddress = (void *)rAddr;
    return DlHcomApi::ChannelGet(channel, req, nullptr);
}

Result HcomTransportManager::InnerWriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(size <= std::numeric_limits<uint32_t>::max(),
                             "size = " << size << " > " << std::numeric_limits<uint32_t>::max(), BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.lAddress = (void *)lAddr;
    req.size = static_cast<uint32_t>(size);

    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, lAddr is not register");
        return BM_ERROR;
    }
    CopyHcomLocalKeyWords(mr.lKey, req.lKey);
    ret = GetMemoryRegionByAddr(rankId, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " is not set");
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);

    auto addrOffset = rAddr - mr.addr;
    BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                << " size: " << size);
    rAddr = mr.lva + addrOffset; // rewrite to remote local va

    req.rAddress = (void *)rAddr;
    return DlHcomApi::ChannelPut(channel, req, nullptr);
}

int HcomTransportManager::PrepareThreadLocalStream()
{
    lock_.LockRead();
    if (stream_ != nullptr) {
        lock_.UnLock();
        return BM_OK;
    }
    lock_.UnLock();
    WriteGuard lockGuard(lock_);
    stream_ = std::make_shared<HostHcomCounterStream>(0);
    return BM_OK;
}

Result HcomTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.size = static_cast<uint32_t>(size);
    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                     << ", lAddr: " << VaToInfo(lAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.lKey);
    mr.lKey = {};
    ret = GetMemoryRegionByAddr(rankId, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                     << ", rAddr: " << VaToInfo(rAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                               << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                               << " lAddr:" << VaToStr(lAddr) << " rAddr: " << VaToStr(rAddr)
                                               << " size: " << size << " tokens: " << req.rKey.tokens[0]);
    ret = PrepareThreadLocalStream();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
    Channel_Callback channelCallback;
    channelCallback.arg = stream_.get();
    channelCallback.cb = ChannelAsyncCallback;
    uint64_t remain = size;
    uint64_t offset = 0;
    while (remain > 0) {
        uint32_t sliceSize = remain > runtimeConfig_.maxSliceSize ? runtimeConfig_.maxSliceSize : remain;

        req.rAddress = reinterpret_cast<void *>(rAddr + offset);
        req.lAddress = reinterpret_cast<void *>(lAddr + offset);
        req.size = sliceSize;
        stream_->SubmitTasks();
        TP_TRACE_BEGIN(TP_HYBM_HOST_RDMA_HCOM_CH_GET);
        ret = DlHcomApi::ChannelGet(channel, req, &channelCallback);
        TP_TRACE_END(TP_HYBM_HOST_RDMA_HCOM_CH_GET, ret);
        if (ret != 0) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit read task lRank:" << rankId_ << " rRank:" << rankId
                                                             << " lAddr:" << VaToStr(lAddr + offset) << "rAddr:"
                                                             << VaToStr(rAddr + offset) << " size:" << sliceSize);
            return ret;
        }
        offset += sliceSize;
        remain -= sliceSize;
    }
    return ret;
}

Result HcomTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.rAddress = reinterpret_cast<void *>(rAddr);
    req.lAddress = reinterpret_cast<void *>(lAddr);
    req.size = static_cast<uint32_t>(size);
    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                     << ", lAddr: " << VaToInfo(lAddr));
        return BM_ERROR;
    }
    CopyHcomLocalKeyWords(mr.lKey, req.lKey);
    mr.lKey = {};
    ret = GetMemoryRegionByAddr(rankId, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                     << ", rAddr: " << VaToInfo(rAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                << " lAddr:" << VaToStr(lAddr) << " rAddr: " << VaToStr(rAddr)
                                                << " size: " << size << " tokens: " << req.rKey.tokens[0]);
    ret = PrepareThreadLocalStream();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
    Channel_Callback channelCallback;
    channelCallback.arg = stream_.get();
    channelCallback.cb = ChannelAsyncCallback;
    uint64_t remain = size;
    uint64_t offset = 0;
    while (remain > 0) {
        uint32_t sliceSize = remain > runtimeConfig_.maxSliceSize ? runtimeConfig_.maxSliceSize : remain;

        req.rAddress = reinterpret_cast<void *>(rAddr + offset);
        req.lAddress = reinterpret_cast<void *>(lAddr + offset);
        req.size = sliceSize;
        stream_->SubmitTasks();
        ret = DlHcomApi::ChannelPut(channel, req, &channelCallback);
        if (ret != BM_OK) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit put task lRank:" << rankId_ << " rRank:" << rankId
                                                            << " lAddr:" << VaToStr(lAddr + offset) << "rAddr:"
                                                            << VaToStr(rAddr + offset) << " size:" << sliceSize);
            return ret;
        }
        offset += sliceSize;
        remain -= sliceSize;
    }
    return ret;
}
Result HcomTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_INFO("WriteRemoteBatchAsync start " << rankId << " rankId");
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }

    uint32_t allBatch = descriptor.counts.size();
    auto batchs = (allBatch + HCOM_IOV_BATCH_SIZE - 1) / HCOM_IOV_BATCH_SIZE; // 向上取整
    uint32_t index = 0;
    while (index < batchs) {
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (uint32_t i = index * HCOM_IOV_BATCH_SIZE; i < std::min(allBatch, (index + 1) * HCOM_IOV_BATCH_SIZE); ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.localAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            CopyHcomLocalKeyWords(mr.lKey, req.lKey);
            mr.lKey = {};
            auto rAddr = descriptor.globalAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                        << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                        << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                        << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                        << " tokens: " << req.rKey.tokens[0]);
            ret = PrepareThreadLocalStream();
            if (ret != BM_OK) {
                BM_LOG_ERROR("prepare stream error rankId: " << rankId);
                return ret;
            }

            sglReq.iov[i - index * HCOM_IOV_BATCH_SIZE] = req;
            sglReq.iovCount++;
        }
        index++;
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;

        stream_->SubmitTasks();
        BM_LOG_INFO("DlHcomApi::ChannelPutV start, sglReq iocount " << sglReq.iovCount);
        auto ret = DlHcomApi::ChannelPutV(channel, sglReq, &channelCallback);
        if (ret != BM_OK) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit put task lRank:" << rankId_ << " rRank:" << rankId);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::Synchronize(const uint32_t rankId)
{
    if (stream_ == nullptr) {
        return BM_OK;
    }
    return stream_->Synchronize(static_cast<int32_t>(rankId));
}

Result HcomTransportManager::CheckTransportOptions(const TransportOptions &options)
{
    std::string protocol;
    uint32_t basePort;
    auto ret = HostHcomHelper::AnalysisNic(options.nic, protocol, localIp_, basePort);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to check nic, nic: " << options.nic << " ret: " << ret);
        return ret;
    }
    // The auto port is basePort + rank index, so the base port must leave room for every rank below rankCount.
    constexpr uint32_t kMaxPort = std::numeric_limits<uint16_t>::max();
    if (options.rankCount == 0 || options.rankCount - 1 > kMaxPort - basePort) {
        BM_LOG_ERROR("base port " << basePort << " leaves no room for " << options.rankCount << " rank ports");
        return BM_INVALID_PARAM;
    }
    hcomAutoPort_ = basePort + options.rankId;
    nicProtocol_ = protocol;
    BM_LOG_INFO("hcom base port: " << basePort << ", hcom auto port with rank: " << hcomAutoPort_);
    localNic_ = protocol + localIp_ + ":" + std::to_string(hcomAutoPort_);
    return BM_OK;
}

Result HcomTransportManager::BindServiceWithPortRetry()
{
    // The port may be occupied by another instance or process on the same host; on bind failure retry with a
    // small port increment.
    constexpr uint32_t kMaxBindAttempts = 8;
    for (uint32_t attempt = 0; attempt < kMaxBindAttempts; ++attempt) {
        localNic_ = nicProtocol_ + localIp_ + ":" + std::to_string(hcomAutoPort_ + attempt);
        auto ret = DlHcomApi::ServiceBind(rpcService_, localNic_.c_str(), NewEndPointTramps()[slot_]);
        if (ret == 0) {
            if (attempt != 0) {
                BM_LOG_WARN("ServiceBind succeeded with offset port, nic: " << localNic_ << " attempt: " << attempt);
            }
            return BM_OK;
        }
        BM_LOG_WARN("ServiceBind failed, nic: " << localNic_ << " ret: " << ret << " attempt: " << attempt);
    }
    BM_LOG_ERROR("Failed to bind hcom service after retries, base port: " << hcomAutoPort_);
    return BM_DL_FUNCTION_FAILED;
}

// HCOM payload 里 client=建链发起方自己的 rank、server=发起方视角下的目标 rank（见 ConnectHcomChannel）。
// 两端最终都要用「本端 rank 空间里的对端编号」给 channels_ 记账：被连接的一端本端 rank 恰为 payload.server，
// 此时对端编号是 payload.client；其余情况（主动端/编号重合）用 payload.server。trans 场景下发起端 entity
// rank 与对端 override rank 不再相等，必须按此规则换算，否则被连接端把 channel 记到错误下标而等待超时。
static uint32_t PeerRankFromPayload(uint32_t localRankId, const HcomPayload &payload)
{
    return localRankId == payload.server ? payload.client : payload.server;
}

Result HcomTransportManager::OnNewEndPoint(Hcom_Channel newCh, const char *payLoad)
{
    const char *logPayLoad = (payLoad != nullptr) ? payLoad : "<null>";
    BM_LOG_DEBUG("New hcom ch, ch: " << std::hex << newCh << " payLoad: " << logPayLoad);
    uint64_t payloadNum = UINT64_MAX;
    if (payLoad == nullptr || !StrUtil::String2Uint<uint64_t>(payLoad, payloadNum)) {
        BM_LOG_ERROR("Failed to get rankId payLoad: " << logPayLoad);
        return BM_ERROR;
    }

    HcomPayload payloadUn{};
    payloadUn.payload = payloadNum;
    BM_LOG_DEBUG("new channel from " << payloadUn.client << " to " << payloadUn.server);

    auto rankId = PeerRankFromPayload(rankId_, payloadUn);
    if (rankId >= channelMutex_.size()) {
        BM_LOG_ERROR("New hcom ch with invalid rankId: " << rankId << " local rank: " << rankId_
                                                         << " payLoad: " << logPayLoad);
        return BM_ERROR;
    }
    std::unique_lock<std::mutex> locker{channelMutex_[rankId]};
    channels_[rankId] = newCh;
    locker.unlock();

    return BM_OK;
}

Result HcomTransportManager::OnEndPointBroken(Hcom_Channel ch, const char *payLoad)
{
    const char *logPayLoad = (payLoad != nullptr) ? payLoad : "<null>";
    BM_LOG_DEBUG("Broken on hcom ch, ch: " << ch << " payLoad: " << logPayLoad);
    uint64_t payloadNum = UINT64_MAX;
    if (payLoad == nullptr || !StrUtil::String2Uint<uint64_t>(payLoad, payloadNum)) {
        BM_LOG_ERROR("Failed to get rankId payLoad: " << logPayLoad);
        return BM_ERROR;
    }

    HcomPayload payloadUn{};
    payloadUn.payload = payloadNum;
    BM_LOG_DEBUG("channel: " << ch << " broken from " << payloadUn.client << " to " << payloadUn.server);

    auto rankId = PeerRankFromPayload(rankId_, payloadUn);
    HcomChannelDisconnected(rankId, ch);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomRequestReceived(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Receive hcom req, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomRequestPosted(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Post hcom req, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomOneSideDone(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Done hcom one side, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::ConnectHcomChannel(uint32_t rankId, const std::string &url)
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_HCOM_CONNECT_CH);
    {
        std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
        if (channels_[rankId] != 0) {
            BM_LOG_WARN("Stop connect to hcom service rankId: " << rankId << " url: " << url << " is connected");
            TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 0);
            return BM_OK;
        }
    }
    Hcom_Channel channel;
    Service_ConnectOptions options;
    options.mode = C_CLIENT_WORKER_POLL;
    options.clientGroupId = 0;
    options.serverGroupId = 0;
    if (StrUtil::StartWith(url, UBC_PROTOCOL_PREFIX)) {
        options.linkCount = 1UL;
    } else {
        options.linkCount = HCOM_TRANS_EP_SIZE;
    }
    HcomPayload payload{};
    payload.client = rankId_; // 发起方（本端）rank，接收方用它换算对端编号（见 PeerRankFromPayload）
    payload.server = rankId; // 本端视角下的目标 rank
    auto rankIdStr = std::to_string(payload.payload);
    std::copy_n(rankIdStr.c_str(), rankIdStr.size() + 1, options.payLoad);
    do {
        auto ret = DlHcomApi::ServiceConnect(rpcService_, url.c_str(), &channel, options);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to connect remote service, rankId" << rankId << " url: " << url << " ret: " << ret);
            TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 1);
            return BM_DL_FUNCTION_FAILED;
        }
    } while (0);
    std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
    channels_[rankId] = channel;
    lock.unlock();

    BM_LOG_DEBUG("Success to connect to hcom service rankId: " << rankId << " url: " << url
                                                               << " channel: " << (void *)channel);
    TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 0);
    return BM_OK;
}

void HcomTransportManager::HcomChannelDisconnected(uint32_t rankId, Hcom_Channel ch)
{
    BM_LOG_DEBUG("HcomChannelDisconnected for rank: " << rankId << ", channel: " << ch);
    if (rankId >= channelMutex_.size()) {
        BM_LOG_ERROR("channel disconnected with invalid rank id: " << rankId << ", channel: " << ch);
        return;
    }

    std::unique_lock<std::mutex> locker{channelMutex_[rankId]};
    channels_[rankId] = 0;
    locker.unlock();
    if (rankId >= rankId_) {
        BM_LOG_TRACE("broken channel local server side:" << rankId_ << ", reconnect by remote side: " << rankId);
        return;
    }

    auto ret = reconnect_.AddReconnectTask(rankId, nics_[rankId]);
    if (ret != BM_OK) {
        BM_LOG_ERROR("add reconnect task for rank:" << rankId << " failed: " << ret);
    }
}

void HcomTransportManager::DisConnectHcomChannel(uint32_t rankId, Hcom_Channel ch)
{
    BM_LOG_DEBUG("invoke DisConnectHcomChannel rankId: " << rankId << ", channel: " << ch);
    if (channels_.empty()) {
        return;
    }
    if (rankId >= rankCount_ || ch == 0) {
        BM_LOG_ERROR_LIMIT("Failed to remove channel invalid rankId" << rankId << " ch: " << ch);
        return;
    }
    if (rpcService_ != 0) {
        DlHcomApi::ServiceDisConnect(rpcService_, ch);
    }
}

bool HcomTransportManager::BindSlot()
{
    for (size_t i = 0; i < kMaxInstances; i++) {
        HcomTransportManager *expected = nullptr;
        if (g_slotInstances[i].compare_exchange_strong(expected, this)) {
            slot_ = i;
            return true;
        }
    }
    return false;
}

void HcomTransportManager::ReleaseSlot()
{
    if (slot_ < kMaxInstances) {
        g_slotInstances[slot_].store(nullptr, std::memory_order_release);
    }
    slot_ = kInvalidSlot;
    serviceName_.clear();
}

Result HcomTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return InnerReadRemote(rankId, lAddr, rAddr, size);
}

Result HcomTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_INFO("ReadRemoteBatchAsync start : " << rankId << " size " << descriptor.counts.size());
    BM_ASSERT_LOG_AND_RETURN(!descriptor.counts.empty(), "descriptor.counts is empty", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(rpcService_ != 0, "rpcService_ = " << rpcService_, BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    uint32_t allBatch = descriptor.counts.size();
    auto batchs = (allBatch + HCOM_IOV_BATCH_SIZE - 1) / HCOM_IOV_BATCH_SIZE; // 向上取整
    uint32_t index = 0;
    while (index < batchs) {
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (uint32_t i = index * HCOM_IOV_BATCH_SIZE; i < std::min(allBatch, (index + 1) * HCOM_IOV_BATCH_SIZE); ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.globalAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.lKey);
            mr.lKey = {};
            auto rAddr = descriptor.localAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                                       << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                       << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                       << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                       << " tokens: " << req.rKey.tokens[0]);
            ret = PrepareThreadLocalStream();
            if (ret != BM_OK) {
                BM_LOG_ERROR("prepare stream error rankId: " << rankId);
                return ret;
            }
            sglReq.iov[i - index * HCOM_IOV_BATCH_SIZE] = req;
            sglReq.iovCount++;
        }
        index++;
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;

        BM_LOG_INFO("ChannelGetV start, sglReq.iovCount " << sglReq.iovCount);
        stream_->SubmitTasks();
        TP_TRACE_BEGIN(TP_HYBM_HOST_RDMA_HCOM_CH_GET);
        auto ret = DlHcomApi::ChannelGetV(channel, sglReq, &channelCallback);
        TP_TRACE_END(TP_HYBM_HOST_RDMA_HCOM_CH_GET, ret);
        if (ret != 0) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit read task lRank:" << rankId_ << " rRank:" << rankId);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return InnerWriteRemote(rankId, lAddr, rAddr, size);
}

Result HcomTransportManager::GetMemoryRegionByAddr(const uint32_t &rankId, const uint64_t &addr, HcomMemoryRegion &mr)
{
    std::unique_lock<std::mutex> lock(mrMutex_[rankId]);
    for (const auto &mrInfo : mrs_[rankId]) {
        BM_LOG_DEBUG("Find rankId:" << rankId << std::hex << " addr:" << mrInfo.addr << " size:" << mrInfo.size);
        if (mrInfo.addr <= addr && mrInfo.addr + mrInfo.size > addr) {
            mr = mrInfo;
            return BM_OK;
        }
    }
    return BM_ERROR;
}

int HcomTransportManager::GetCACallBack(const char *name, char **caPath, char **crlPath,
                                        Hcom_PeerCertVerifyType *verifyType, Hcom_TlsCertVerify *verify)
{
    if (caPath == nullptr || crlPath == nullptr || verifyType == nullptr || verify == nullptr) {
        BM_LOG_ERROR("Invalid input");
        return 1;
    }
    *caPath = tlsConfig_.caPath;
    *crlPath = tlsConfig_.crlPath;
    *verifyType = C_VERIFY_BY_DEFAULT;
    *verify = CertVerifyCallBack;
    return 0;
}

int HcomTransportManager::GetCertCallBack(const char *name, char **certPath)
{
    if (certPath == nullptr) {
        BM_LOG_ERROR("certPath is nullptr");
        return 1;
    }
    *certPath = tlsConfig_.certPath;
    return 0;
}

int HcomTransportManager::GetPrivateKeyCallBack(const char *name, char **priKeyPath, char **keyPass,
                                                Hcom_TlsKeyPassErase *erase)
{
    std::lock_guard<std::mutex> lock(keyPassMutex);

    if (priKeyPath == nullptr || keyPass == nullptr || erase == nullptr) {
        BM_LOG_ERROR("Invalid input");
        return 1;
    }

    *priKeyPath = tlsConfig_.keyPath;

    std::ifstream fileStream;
    fileStream.open(tlsConfig_.keyPassPath);
    if (!fileStream.is_open()) {
        BM_LOG_ERROR("Failed to open keyPassFile");
        return 1;
    }
    char encryptedKeyPass[KEYPASS_MAX_LEN] = {};
    fileStream.getline(encryptedKeyPass, KEYPASS_MAX_LEN);
    fileStream.close();
    DecryptFunc func;
    if (std::string(tlsConfig_.decrypterLibPath).empty()) {
        BM_LOG_WARN("No decrypter provided, using default decrypter handler");
        func = static_cast<DecryptFunc>(MfTlsUtil::DefaultDecrypter);
    } else {
        func = MfTlsUtil::LoadDecryptFunction(tlsConfig_.decrypterLibPath);
        if (func == nullptr) {
            BM_LOG_ERROR("failed to load customized decrypt function");
            return 1;
        }
    }
    if (func(encryptedKeyPass, KEYPASS_MAX_LEN, keyPass_, KEYPASS_MAX_LEN) != 0) {
        BM_LOG_ERROR("failed to decrypt key pass");
        return 1;
    }

    *keyPass = keyPass_;
    *erase = KeyPassEraseCallBack;

    return 0;
}

int HcomTransportManager::CertVerifyCallBack(void *x509, const char *crlPath)
{
    return 0;
}

void HcomTransportManager::KeyPassEraseCallBack(char *keyPass, int len)
{
    std::lock_guard<std::mutex> lock(keyPassMutex);
    for (int i = 0; i < len; i++) {
        keyPass[i] = 0;
    }
}

void HcomTransportManager::ChannelAsyncCallback(void *arg, Service_Context context)
{
    int res = -1;
    auto ret = DlHcomApi::ContextGetResult(context, &res);
    if (ret != 0) {
        res = ret;
    }

    auto counterStream = static_cast<HostHcomCounterStream *>(arg);
    if (res == 0) {
        counterStream->FinishOne();
    } else {
        counterStream->FailedOne();
    }
}

const TransportPrivateData HcomTransportManager::GetPrivateData() const
{
    return TransportPrivateData{};
}

void HcomTransportManager::SetHcomServiceConfig(Hcom_Service service)
{
    uint16_t cqDepth = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_CQ_DEPTH, cqDepth)) {
        BM_LOG_INFO("Set hcom cq depth: " << cqDepth);
        DlHcomApi::ServiceSetCompletionQueueDepth(service, cqDepth);
    }

    uint32_t sqSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_SQ_SIZE, sqSize)) {
        BM_LOG_INFO("Set hcom sq size: " << sqSize);
        DlHcomApi::ServiceSetSendQueueSize(service, sqSize);
    }

    uint32_t rqSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_RQ_SIZE, rqSize)) {
        BM_LOG_INFO("Set hcom rq size: " << rqSize);
        DlHcomApi::ServiceSetRecvQueueSize(service, rqSize);
    }

    uint32_t prepostSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_PREPOST_SIZE, prepostSize)) {
        BM_LOG_INFO("Set hcom prepost size: " << prepostSize);
        DlHcomApi::ServiceSetQueuePrePostSize(service, prepostSize);
    }

    uint32_t maxSendRecvDataCnt = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_MAX_SEND_RECV_DATA_CNT, maxSendRecvDataCnt)) {
        BM_LOG_INFO("Set hcom max send recv data cnt: " << maxSendRecvDataCnt);
        DlHcomApi::HcomSetMaxSendRecvDataCnt(service, maxSendRecvDataCnt);
    }
}
