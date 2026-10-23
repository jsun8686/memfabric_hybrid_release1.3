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

#include "device_rdma_hcomm_transport_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "hybm_logger.h"
#include "hybm_stream_manager.h"
#include "hybm_va_manager.h"
#include "urma_topo_helper.h"
#include "hybm_batch_transfer.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {

constexpr int32_t CHANNEL_STATUS_POLL_INTERVAL_MS = 1;
constexpr int32_t CHANNEL_CONNECT_TIMEOUT_MS = 30000;
// 本端 flag 缓冲初值：device(AICPU kernel) 路径用它做完成确认；
// host RoCE 路径已不再读对端 flag，完成确认只依赖 fence。
constexpr uint64_t TRANSFER_FLAG_DONE = 1ULL;
constexpr const char *HYBM_DEVICE_FUNC_READ = "HybmBatchRead";
constexpr const char *HYBM_DEVICE_FUNC_WRITE = "HybmBatchWrite";

// A5(950) host RoCE 专用：把 channel desc 升到 HCOMM ABI v4 并归零 v4 新增字段
// （srcPortList 若保持 memset 的 0xFF 即野指针）。v4 新字段都在 union raw 区内，
// sizeof/偏移与 v3 完全一致，因此这里只改 version，不影响 DEVICE/URMA 等其它场景。
void UpgradeChannelDescToV4(HcommChannelDesc &chDesc)
{
    chDesc.header.version = HCOMM_CHANNEL_VERSION_V4;
    chDesc.roceAttr.qpThreshold = 0U;
    chDesc.roceAttr.cqAttrFlags = 0U;
    chDesc.roceAttr.srcPortList = nullptr;
    chDesc.roceAttr.sqDepth = 0U;
    chDesc.roceAttr.scqDepth = 0U;
}

bool IsValidMem(const TransportMemoryRegion &mr)
{
    return mr.addr != 0 && mr.size != 0;
}

// Read NPU IP from EID file (devPhyId:IP format, one per line).
// This matches the old MF_DEVICE_URMA_EID_FILE format used before develop0717.
static Result GetDeviceUrmaIpAddrFromFile(uint32_t phyDeviceId, uint32_t rankId, const char *eidPath, char *ipBuf,
                                          size_t ipBufLen)
{
    std::ifstream eidFile(eidPath);
    if (!eidFile.is_open()) {
        BM_LOG_ERROR("cannot open EID file: " << eidPath);
        return BM_ERROR;
    }
    std::string line;
    while (std::getline(eidFile, line)) {
        if (line.empty()) {
            continue;
        }
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }
        auto idStr = line.substr(0, colonPos);
        char *end = nullptr;
        auto devId = static_cast<uint32_t>(std::strtoul(idStr.c_str(), &end, 10));
        if (*end != '\0') {
            continue;
        }
        if (devId != phyDeviceId) {
            continue;
        }
        std::string ipStr = line.substr(colonPos + 1);
        auto trimLeft = ipStr.find_first_not_of(" \t");
        if (trimLeft != std::string::npos) {
            ipStr = ipStr.substr(trimLeft);
        }
        auto trimRight = ipStr.find_last_not_of(" \t\r\n");
        if (trimRight != std::string::npos) {
            ipStr = ipStr.substr(0, trimRight + 1);
        }
        if (ipStr.length() >= ipBufLen) {
            return BM_ERROR;
        }
        std::memcpy(ipBuf, ipStr.c_str(), ipStr.length() + 1);
        BM_LOG_DEBUG("NPU IP from EID file: phyId=" << phyDeviceId << " ip=" << ipBuf);
        return BM_OK;
    }
    BM_LOG_ERROR("devPhyId " << phyDeviceId << " not found in EID file: " << eidPath);
    return BM_INVALID_PARAM;
}

// Query NPU device IP via hccn_tool with 910B3-compatible syntax:
//   hccn_tool -i <devId> -ip -g
// This is different from the A5 syntax used in GetDeviceUBoEIpAddr.
static Result GetDeviceIpViaHccnTool(uint32_t phyDeviceId, struct in_addr &addr)
{
    // Find hccn_tool path
    std::string toolPath;
    if (access("/usr/local/Ascend/driver/tools/hccn_tool", X_OK) == 0) {
        toolPath = "/usr/local/Ascend/driver/tools/hccn_tool";
    } else {
        toolPath = "hccn_tool";
    }

    const std::string cmd = toolPath + " -i " + std::to_string(phyDeviceId) + " -ip -g";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return BM_ERROR;
    }

    // Parse output like: "ipaddr: 10.20.0.6 netmask: 255.255.255.0"
    static constexpr size_t LINE_BUF_SIZE = 256;
    char buf[LINE_BUF_SIZE];
    while (fgets(buf, static_cast<int>(sizeof(buf)), pipe) != nullptr) {
        const std::string line(buf);
        auto pos = line.find("ipaddr:");
        if (pos == std::string::npos) {
            continue;
        }
        std::string ipStr = line.substr(pos + 7); // skip "ipaddr:"
        auto trimLeft = ipStr.find_first_not_of(" \t");
        if (trimLeft != std::string::npos) {
            ipStr = ipStr.substr(trimLeft);
        }
        auto trimRight = ipStr.find_last_not_of(" \t\r\n");
        if (trimRight != std::string::npos) {
            ipStr = ipStr.substr(0, trimRight + 1);
        }
        if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
            pclose(pipe);
            BM_LOG_DEBUG("NPU IP via hccn_tool(910B3): dev=" << phyDeviceId << " ip=" << ipStr);
            return BM_OK;
        }
    }
    pclose(pipe);
    return BM_ERROR;
}

// 解析 "ens<slot>f<port>"，其它命名返回 false。
static bool ParseEnsIfName(const char *name, unsigned &slot, unsigned &port)
{
    constexpr unsigned MAX_IDX = 255U;
    if (name == nullptr || std::strncmp(name, "ens", 3) != 0) {
        return false;
    }
    const char *p = name + 3;
    char *end = nullptr;
    const unsigned long s = std::strtoul(p, &end, 10);
    if (end == p || *end != 'f' || s > MAX_IDX) {
        return false;
    }
    p = end + 1;
    const unsigned long f = std::strtoul(p, &end, 10);
    if (end == p || *end != '\0' || f > MAX_IDX) {
        return false;
    }
    slot = static_cast<unsigned>(s);
    port = static_cast<unsigned>(f);
    return true;
}

// A5(950) host RoCE：用 getifaddrs 取「本 device 对应网口」的 IPv4，不依赖 /etc/hccn.conf
// （后者 / hccn_tool 仍作为后续兜底保留）。
//
// 网口命名 ens<slot>f<port> 固定，但「每 slot 几个 func」因机型而异（2 卡各 4 口 / 4 卡各 2 口），
// 写死任何一种都会在另一类机型上选错口（如 4 卡各 2 口机型上 phyId=4 会被算成 ens1f0，实际是 ens2f0）。
// 因此这里从本机实际存在的网口反推：每 slot 口数 = port 最大值 + 1，再按 phyId 顺序切分 slot/port。
static Result GetIpViaGetifaddrs(uint32_t phyDeviceId, struct in_addr &addr)
{
    const char *forced = std::getenv("MF_HOST_ROCE_IFNAME");
    std::vector<std::string> candidates;
    if (forced != nullptr && forced[0] != '\0') {
        candidates.emplace_back(forced);
    }

    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        BM_LOG_ERROR("GetIpViaGetifaddrs: getifaddrs failed, errno=" << errno);
        return BM_ERROR;
    }

    unsigned maxPort = 0;
    bool sawEns = false;
    for (auto *p = ifap; p != nullptr; p = p->ifa_next) {
        unsigned slot = 0;
        unsigned port = 0;
        if (!ParseEnsIfName(p->ifa_name, slot, port)) {
            continue;
        }
        sawEns = true;
        maxPort = std::max(maxPort, port);
    }
    if (sawEns) {
        const unsigned portsPerSlot = maxPort + 1U;
        char ifName[IFNAMSIZ]{};
        (void)snprintf(ifName, sizeof(ifName), "ens%uf%u", phyDeviceId / portsPerSlot, phyDeviceId % portsPerSlot);
        candidates.emplace_back(ifName);
    }

    Result ret = BM_INVALID_PARAM;
    for (const auto &want : candidates) {
        for (auto *p = ifap; p != nullptr; p = p->ifa_next) {
            if (p->ifa_name == nullptr || p->ifa_addr == nullptr) {
                continue;
            }
            if (want != p->ifa_name || p->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            addr = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr)->sin_addr;
            char ipStr[INET_ADDRSTRLEN]{};
            (void)inet_ntop(AF_INET, &addr, ipStr, sizeof(ipStr));
            BM_LOG_INFO("GetIpViaGetifaddrs: phyId=" << phyDeviceId << " ifname=" << want << " ip=" << ipStr);
            ret = BM_OK;
            break;
        }
        if (ret == BM_OK) {
            break;
        }
    }
    if (ret != BM_OK) {
        const std::string first = candidates.empty() ? std::string("(none)") : candidates[0];
        BM_LOG_WARN("GetIpViaGetifaddrs: no AF_INET for phyId=" << phyDeviceId << " (tried " << candidates.size()
                                                                << " ifname(s), e.g. " << first << ")");
    }
    freeifaddrs(ifap);
    return ret;
}

} // namespace

DeviceRdmaHcommTransportManager::~DeviceRdmaHcommTransportManager()
{
    (void)CloseDevice();
}

// ============================================================================
// AICPU kernel loading
// ============================================================================

Result DeviceRdmaHcommTransportManager::LoadDeviceKernel()
{
    std::lock_guard<std::mutex> guard(deviceKernelMutex_);
    if (deviceFuncHandles_.batchWrite != nullptr && deviceFuncHandles_.batchRead != nullptr) {
        return BM_OK;
    }
    return LoadDeviceKernelAndValidate(HYBM_DEVICE_FUNC_READ, HYBM_DEVICE_FUNC_WRITE, deviceKernelHandle_,
                                       deviceFuncHandles_);
}

// ============================================================================
// Endpoint helpers
// ============================================================================

Result DeviceRdmaHcommTransportManager::BuildLocalEndpointDesc(EndpointDesc &desc) const
{
    desc.protocol = COMM_PROTOCOL_ROCE;
    if (hostRocePath_) {
        // A5(950) 上 ROCE 只有 HOST 形态可用：ROCE + DEVICE 会落到 AicpuTsRoceEndpoint，
        // 该类只允许 910B/910_93；ROCE + HOST 落到 CpuRoceEndpoint，明确支持 950/960，
        // 且内部走 HccpPeerManager(PEER 模式)，直接用主机侧的 RoCE 网口（无需 NPU 侧地址）。
        desc.loc.locType = ENDPOINT_LOC_TYPE_HOST;
        // 注意：EndpointLoc 里 device/host 是 union，共用同一段内存。
        // HOST 形态只能填 host.id（CpuRoceEndpoint 内部是 kHostResourceId=0），
        // 填 device.* 会覆盖成 host.id = phyDeviceId，导致两端 host id 不一致。
        desc.loc.host.id = 0;
    } else {
        desc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
        desc.loc.device.devPhyId = phyDeviceId_;
        desc.loc.device.superDevId = sdid_;
        desc.loc.device.serverIdx = serverId_;
        desc.loc.device.superPodIdx = superPodId_;
    }

    // Get NPU-side IP address for HCOMM endpoint creation.
    {
        // 0) A5(950) host RoCE：先读 /etc/hccn.conf 拿 phyId->IP 的权威映射（不依赖网口命名，
        //    也不 fork hccn_tool）；读不到再按 MF_HOST_ROCE_IFNAME / 本机网口拓扑推算 ens<slot>f<port>；
        //    都失败则继续走下面的原有链路（hccn_tool 等兜底保留）。
        if (hostRocePath_) {
            CommAddrType confAddrType = COMM_ADDR_TYPE_RESERVED;
            std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> confAddrBytes{};
            if (GetDeviceIpFromHccnConf(phyDeviceId_, rankId_, confAddrType, confAddrBytes) == BM_OK) {
                desc.commAddr.type = confAddrType;
                std::memcpy(desc.commAddr.raws, confAddrBytes.data(), sizeof(desc.commAddr.raws));
                return BM_OK;
            }

            struct in_addr nicIpv4 {};
            if (GetIpViaGetifaddrs(phyDeviceId_, nicIpv4) == BM_OK) {
                desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
                desc.commAddr.addr = nicIpv4;
                return BM_OK;
            }
        }

        // 1) Try GetDeviceUBoEIpAddr (A5 hccn_tool syntax: -g -ip -i <dev> -d bond<dev>)
        CommAddrType addrType = COMM_ADDR_TYPE_RESERVED;
        std::array<uint8_t, URMA_ENDPOINT_RAW_LEN> addrBytes{};
        if (GetDeviceUBoEIpAddr(phyDeviceId_, rankId_, addrType, addrBytes) == BM_OK) {
            desc.commAddr.type = addrType;
            std::memcpy(desc.commAddr.raws, addrBytes.data(), sizeof(desc.commAddr.raws));
            return BM_OK;
        }

        // 2) Try hccn_tool with 910B3-compatible syntax: hccn_tool -i <dev> -ip -g
        struct in_addr ipv4 {};
        if (GetDeviceIpViaHccnTool(phyDeviceId_, ipv4) == BM_OK) {
            desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
            desc.commAddr.addr = ipv4;
            return BM_OK;
        }

        // 3) Last resort: MF_DEVICE_URMA_EID_FILE (devPhyId:IP per line)
        const char *eidPath = std::getenv("MF_DEVICE_URMA_EID_FILE");
        if (eidPath != nullptr && eidPath[0] != '\0') {
            char ipBuf[INET_ADDRSTRLEN]{};
            if (GetDeviceUrmaIpAddrFromFile(phyDeviceId_, rankId_, eidPath, ipBuf, sizeof(ipBuf)) == BM_OK) {
                if (inet_pton(AF_INET, ipBuf, &desc.commAddr.addr) == 1) {
                    desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
                    return BM_OK;
                }
            }
        }
    }

    BM_LOG_ERROR("cannot determine NPU-side IP for phyDeviceId=" << phyDeviceId_);
    return BM_INVALID_PARAM;
}

// ============================================================================
// OpenDevice / CloseDevice
// ============================================================================

Result DeviceRdmaHcommTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (opened_) {
        return BM_OK;
    }
    if (options.rankCount == 0 || options.rankId >= options.rankCount) {
        BM_LOG_ERROR("invalid rankCount or rankId");
        return BM_INVALID_PARAM;
    }

    // Ensure HCOMM library is loaded
    auto loadRet = HcommApiWrapper::LoadHcomm();
    if (loadRet != BM_OK) {
        BM_LOG_ERROR("DlHcommApi::LoadLibrary failed, ret: " << loadRet);
        return loadRet;
    }

    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    localNic_ = options.nic;

    // A5(950) 的 ROCE 没有 DEVICE 形态端点（AicpuTsRoceEndpoint 只支持 910B/910_93），走主机侧
    // RoCE：HOST 端点 + COMM_ENGINE_CPU + 主机侧 Nbi 提交（不下发 AICPU 算子）；其它 SoC 保持
    // 原有 DEVICE 形态实现，行为不变。
    hostRocePath_ = (DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_950);

    // Get local device info
    auto ret = InitLocalDeviceInfo(phyDeviceId_, sdid_, serverId_, superPodId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("InitLocalDeviceInfo failed, rank=" << rankId_);
        return ret;
    }

    // Build local EndpointDesc and create HCOMM endpoint (always RoCE)
    EndpointDesc desc{};
    ret = BuildLocalEndpointDesc(desc);
    if (ret != BM_OK) {
        return ret;
    }
    EndpointHandle handle = nullptr;
    ret = hcommApi_.CreateEndpoint(desc, handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommEndpointCreate failed, ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    localEndpoint_ = handle;
    localEndpointDesc_ = desc;
    BM_LOG_INFO("HcommEndpointCreate success, handle=" << handle << " protocol=ROCE addrType=" << desc.commAddr.type);

    if (hostRocePath_) {
        // 取本端监听端口，随 private data 交换给对端建 channel 用；
        // libhcomm 未提供该符号时回退为 0（DEVICE 形态不需要端口交换）。
        uint32_t listenPort = 0;
        auto portRet = DlHcommApi::HcommEndpointGetListenPort(localEndpoint_, &listenPort);
        if (portRet == 0) {
            localListenPort_ = listenPort;
            BM_LOG_INFO("local endpoint listen port: " << localListenPort_);
        } else {
            BM_LOG_WARN("HcommEndpointGetListenPort unavailable, ret=" << portRet << ", fallback to port 0");
        }
    }

    // Load AICPU kernel (仅 DEVICE 形态的 launch 需要；A5 host RoCE 数据面走主机侧 Nbi，不用算子，
    // 因此这里缺 kernel 文件/加载失败时不应阻塞 host 路径)
    ret = LoadDeviceKernel();
    if (ret != BM_OK) {
        if (hostRocePath_) {
            BM_LOG_WARN("LoadDeviceKernel failed, ret: " << ret
                                                         << ", but A5 host RoCE path does not use the AICPU kernel,"
                                                            " continue.");
        } else {
            BM_LOG_ERROR("LoadDeviceKernel failed, ret: " << ret);
            (void)hcommApi_.DestroyEndpoint(localEndpoint_);
            localEndpoint_ = nullptr;
            return ret;
        }
    }

    // Initialize transfer flag buffer (for kernel notify/confirm read-back)
    ret = InitTransferFlagLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("InitTransferFlagLocked failed, ret: " << ret);
        (void)hcommApi_.DestroyEndpoint(localEndpoint_);
        localEndpoint_ = nullptr;
        return ret;
    }

    openGeneration_ = std::make_shared<OpenGeneration>();
    openGeneration_->id = 1;
    opened_ = true;
    BM_LOG_INFO("DeviceRdmaHcommTransportManager OpenDevice success, rank: "
                << rankId_ << " rankCount: " << rankCount_ << " protocol: ROCE" << " nic: " << localNic_);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    if (buffers.dstList != nullptr) {
        (void)DlAclApi::AclrtFree(buffers.dstList);
    }
    buffers.dstList = nullptr;
    buffers.srcList = nullptr;
    buffers.lenList = nullptr;
}

// ============================================================================
// Transfer flag buffer (kernel notify/confirm)
// ============================================================================

Result DeviceRdmaHcommTransportManager::InitTransferFlagLocked()
{
    if (localFlagPtr_ != nullptr) {
        return BM_OK;
    }
    if (hostRocePath_) {
        // flag 用 host 内存（页对齐）+ COMM_MEM_TYPE_HOST 注册，
        // 这样本端可以 CPU 直接轮询它的值，对端也可以直接拿它的地址做 RDMA 读。
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            BM_LOG_ERROR("sysconf(_SC_PAGESIZE) failed");
            return BM_ERROR;
        }
        void *hostFlag = nullptr;
        if (posix_memalign(&hostFlag, static_cast<size_t>(pageSize), localFlagSize_) != 0 || hostFlag == nullptr) {
            BM_LOG_ERROR("posix_memalign for local flag buffer failed, size: " << localFlagSize_);
            return BM_MALLOC_FAILED;
        }
        *reinterpret_cast<uint64_t *>(hostFlag) = TRANSFER_FLAG_DONE;
        localFlagPtr_ = hostFlag;

        HcommCommMem flagMem{};
        flagMem.type = COMM_MEM_TYPE_HOST;
        flagMem.addr = localFlagPtr_;
        flagMem.size = localFlagSize_;
        auto ret = hcommApi_.RawMemReg(localEndpoint_, "__hybm_transfer_flag__", flagMem, localFlagHandle_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HcommMemReg for local flag buffer failed, ret: " << ret);
            free(localFlagPtr_);
            localFlagPtr_ = nullptr;
            return ret;
        }

        BM_LOG_INFO("local flag buffer allocated and registered, addr: " << localFlagPtr_ << " size: " << localFlagSize_
                                                                         << " handle: " << localFlagHandle_
                                                                         << " type: HOST");
        return BM_OK;
    }

    auto ret = DlAclApi::AclrtMalloc(
        &localFlagPtr_, localFlagSize_,
        static_cast<aclrtMemMallocPolicy>(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_ONLY));
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMalloc for local flag buffer failed, ret: " << ret);
        return ret;
    }
    uint64_t flagInit = 1;
    ret = DlAclApi::AclrtMemcpy(localFlagPtr_, localFlagSize_, &flagInit, localFlagSize_, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AclrtMemcpy init local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }
    HcommCommMem flagMem{};
    flagMem.type = COMM_MEM_TYPE_DEVICE;
    flagMem.addr = localFlagPtr_;
    flagMem.size = localFlagSize_;
    ret = hcommApi_.RawMemReg(localEndpoint_, "__hybm_transfer_flag__", flagMem, localFlagHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HcommMemReg for local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }

    // Cache the exported flag descriptor for embedding into memory keys
    void *exportDesc = nullptr;
    uint32_t exportLen = 0;
    ret = hcommApi_.RawMemExport(localEndpoint_, localFlagHandle_, exportDesc, exportLen);
    if (ret != BM_OK || exportDesc == nullptr || exportLen == 0) {
        BM_LOG_ERROR("HcommMemExport for local flag failed, ret: " << ret);
        (void)hcommApi_.RawMemUnreg(localEndpoint_, localFlagHandle_);
        localFlagHandle_ = nullptr;
        (void)DlAclApi::AclrtFree(localFlagPtr_);
        localFlagPtr_ = nullptr;
        return ret;
    }
    // Export descriptor is owned by Hcomm — copy locally
    localFlagExportDesc_.resize(exportLen);
    std::memcpy(localFlagExportDesc_.data(), exportDesc, exportLen);
    localFlagExportLen_ = exportLen;

    BM_LOG_INFO("local flag buffer allocated and registered, addr: " << localFlagPtr_ << " size: " << localFlagSize_
                                                                     << " handle: " << localFlagHandle_
                                                                     << " exportLen: " << exportLen);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::DestroyTransferFlagLocked()
{
    if (localFlagHandle_ != nullptr) {
        (void)hcommApi_.RawMemUnreg(localEndpoint_, localFlagHandle_);
        localFlagHandle_ = nullptr;
    }
    if (localFlagPtr_ != nullptr) {
        if (hostRocePath_) {
            free(localFlagPtr_); // host 侧 posix_memalign 的内存不能交给 AclrtFree
        } else {
            (void)DlAclApi::AclrtFree(localFlagPtr_);
        }
        localFlagPtr_ = nullptr;
    }
    localFlagExportDesc_.clear();
    localFlagExportLen_ = 0;
}

// ============================================================================
// CloseDevice
// ============================================================================

Result DeviceRdmaHcommTransportManager::CloseDevice()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    Result finalRet = BM_OK;

    // Destroy all peer resources
    for (auto &item : remoteRanks_) {
        // Unimport peer descriptors from the HCOMM endpoint before tearing down,
        // so the endpoint table does not carry stale entries across reopen.
        for (const auto &desc : item.second.importedDescs) {
            if (desc.empty()) {
                continue;
            }
            (void)hcommApi_.RawMemUnimport(localEndpoint_, desc.data(), static_cast<uint32_t>(desc.size()));
        }
        auto ret = DestroyPeerResources(item.first);
        if (ret != BM_OK && finalRet == BM_OK) {
            finalRet = ret;
        }
    }
    remoteRanks_.clear();

    // Unregister local memory
    for (auto &item : localRegistrations_) {
        if (item.second.handle != nullptr) {
            auto ret = hcommApi_.RawMemUnreg(localEndpoint_, item.second.handle);
            if (ret != 0 && finalRet == BM_OK) {
                finalRet = BM_DL_FUNCTION_FAILED;
            }
        }
    }
    localRegistrations_.clear();

    // Clean up all CompletionContexts (notify/stream/pending)
    for (auto &ctx : registry_) {
        if (ctx == nullptr) {
            continue;
        }
        if (ctx->notifyHcommHandle != nullptr) {
            (void)hcommApi_.RawMemUnreg(localEndpoint_, ctx->notifyHcommHandle);
            ctx->notifyHcommHandle = nullptr;
        }
        for (auto &pt : ctx->pendingTransfers) {
            ReleaseDeviceTransferBuffers(pt.buffers);
        }
        ctx->pendingTransfers.clear();
        if (ctx->notify != nullptr) {
            (void)DlAclApi::AclrtDestroyNotify(ctx->notify);
            ctx->notify = nullptr;
        }
        // ctx->stream is a thread-local shared FAST stream owned by HybmStreamManager;
        // it must NOT be destroyed here (device_urma does the same).
        ctx->stream = nullptr;
    }
    registry_.clear();
    openGeneration_.reset();

    // Destroy local flag buffer
    DestroyTransferFlagLocked();

    // Destroy local endpoint
    if (localEndpoint_ != nullptr) {
        auto ret = hcommApi_.DestroyEndpoint(localEndpoint_);
        if (ret != 0 && finalRet == BM_OK) {
            finalRet = BM_DL_FUNCTION_FAILED;
        }
        localEndpoint_ = nullptr;
    }

    deviceKernelHandle_ = nullptr;
    deviceFuncHandles_ = DeviceFuncHandles{};
    opened_ = false;
    BM_LOG_INFO("DeviceRdmaHcommTransportManager CloseDevice");
    return finalRet;
}

// ============================================================================
// Memory registration
// ============================================================================

Result DeviceRdmaHcommTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("transport not opened");
        return BM_ERROR;
    }
    if (!IsValidMem(mr)) {
        BM_LOG_ERROR("invalid memory region");
        return BM_INVALID_PARAM;
    }

    auto it = localRegistrations_.find(mr.addr);
    if (it != localRegistrations_.end()) {
        it->second.refCount++;
        return BM_OK;
    }

    // Follow device_urma pattern: convert HVA to DVA for DRAM memory.
    // host RoCE 形态不走这个转换：DRAM/ACL_DRAM 就是主机内存，直接用 HVA + COMM_MEM_TYPE_HOST 注册，
    // HCOMM/HCCP 内部自己做 HVA->设备地址转换；传 HalHostRegister 的 DVA 反而会注册失败。
    uint64_t regAddr = mr.addr;
    CommMemType memType = (mr.flags & REG_MR_FLAG_HBM) ? COMM_MEM_TYPE_DEVICE : COMM_MEM_TYPE_HOST;
    if (!hostRocePath_ && (mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0) {
        const uint64_t dva = HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA);
        if (dva != 0) {
            regAddr = dva;
            memType = COMM_MEM_TYPE_DEVICE;
            BM_LOG_INFO("DRAM addr 0x" << std::hex << mr.addr << " mapped to DVA 0x" << dva << std::dec);
        } else {
            BM_LOG_WARN("DRAM addr 0x" << std::hex << mr.addr << " has no DVA mapping, using HVA");
        }
    }

    HcommCommMem hcommMem{};
    hcommMem.type = memType;
    hcommMem.addr = reinterpret_cast<void *>(regAddr);
    hcommMem.size = mr.size;

    HcommMemHandle handle = nullptr;
    auto ret = hcommApi_.RawMemReg(localEndpoint_, std::to_string(mr.addr).c_str(), hcommMem, handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemReg failed, addr: 0x" << std::hex << regAddr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    LocalMemEntry entry{};
    entry.handle = handle;
    entry.addr = regAddr;
    entry.size = mr.size;
    entry.refCount = 1;
    localRegistrations_.emplace(mr.addr, entry);

    BM_LOG_INFO("HcommMemReg success, addr: 0x" << std::hex << regAddr << " handle: " << handle);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_WARN("addr not registered: " << std::hex << addr);
        return BM_OK;
    }
    if (it->second.refCount > 1) {
        it->second.refCount--;
        return BM_OK;
    }

    auto ret = hcommApi_.RawMemUnreg(localEndpoint_, it->second.handle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemUnreg failed, addr: " << std::hex << addr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    localRegistrations_.erase(it);
    return BM_OK;
}

bool DeviceRdmaHcommTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Range match (consistent with legacy native RDMA / URMA / host_hcom):
    // a sub-range inside a registered region counts as registered.
    for (const auto &[regAddr, entry] : localRegistrations_) {
        if (addr >= regAddr && (addr - regAddr) <= entry.size && size <= entry.size - (addr - regAddr)) {
            return true;
        }
    }
    return false;
}

Result DeviceRdmaHcommTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = localRegistrations_.find(addr);
    if (it == localRegistrations_.end()) {
        BM_LOG_ERROR("addr not registered: " << std::hex << addr);
        return BM_INVALID_PARAM;
    }

    void *memDesc = nullptr;
    uint32_t descLen = 0;
    auto ret = hcommApi_.RawMemExport(localEndpoint_, it->second.handle, memDesc, descLen);
    if (ret != 0) {
        BM_LOG_ERROR("HcommMemExport failed, addr: " << std::hex << addr << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    // HCOMM mem descriptor lives in slots [0, TRANS_KEY_DEV_SLOT2); slot 2 holds metadata; slot 3+ holds flag data.
    constexpr uint32_t maxDescBytes = TRANS_KEY_DEV_SLOT2 * sizeof(uint64_t);
    if (descLen > maxDescBytes) {
        BM_LOG_ERROR("HcommMemExport descriptor too large, addr: " << std::hex << addr << " descLen: " << descLen
                                                                   << " max: " << maxDescBytes);
        return BM_ERROR;
    }
    const size_t flagWords = (localFlagExportLen_ + sizeof(uint64_t) - 1U) / sizeof(uint64_t);
    key.ResizeWords(TRANS_KEY_DEV_SLOT3 + flagWords);
    std::memcpy(key.ByteData(), memDesc, descLen);
    // Store metadata in slot 2 via HcommKeyMeta struct (device_urma-compatible pattern)
    auto &meta = HcommKeyMetaAt(key);
    meta.addr = it->second.addr;
    meta.size = it->second.size;
    meta.descLen = static_cast<uint64_t>(descLen);
    // gvaBase must be the peer's GVA base (not HVA). addr here is the HVA
    // (realSlice->vAddress_), so translate HVA->GVA; in the HBM model GVA==HVA
    // and TransformVa returns the original value.
    const uint64_t peerGva = HybmVaManager::GetInstance().TransformVa(addr, HVM_HVA, HVM_GVA);
    meta.gvaBase = (peerGva != 0) ? peerGva : addr;
    if (hostRocePath_) {
        // 主机侧 RoCE：flag 地址随 private data 交换（对端直接读它判完成），
        // 不走 memory key 里的 flag 描述符，保持 0 避免对端走已废弃的 MemImport 路径。
        meta.flagLen = 0;
    } else {
        BM_LOG_DEBUG("QueryMemoryKey localFlagHandle_=" << localFlagHandle_
                                                        << " localFlagExportLen_=" << localFlagExportLen_
                                                        << " descSize=" << localFlagExportDesc_.size());
        if (localFlagHandle_ != nullptr) {
            const uint32_t flagSlot = HCOMM_KEY_FLAG_SLOT;
            meta.flagLen = static_cast<uint64_t>(localFlagExportLen_);
            BM_LOG_DEBUG("QueryMemoryKey flagExportLen=" << localFlagExportLen_ << " descSize="
                                                         << localFlagExportDesc_.size() << " flagWords=" << flagWords);
            if (localFlagExportLen_ > 0 && !localFlagExportDesc_.empty()) {
                std::memcpy(&key.keys[flagSlot], localFlagExportDesc_.data(), localFlagExportLen_);
                BM_LOG_DEBUG("QueryMemoryKey flag data copied, len=" << localFlagExportLen_);
            } else {
                BM_LOG_DEBUG("QueryMemoryKey flag memcpy skipped: len=" << localFlagExportLen_
                                                                        << " empty=" << localFlagExportDesc_.empty());
            }
        } else {
            meta.flagLen = 0;
            BM_LOG_DEBUG("QueryMemoryKey localFlagHandle_ is nullptr!");
        }
    }

    BM_LOG_DEBUG("QueryMemoryKey addr=0x" << std::hex << addr << " export descLen=" << descLen << std::dec);
    return BM_OK;
}

void DeviceRdmaHcommTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr != nullptr) {
        EnsureHcommKeyMeta(key);
        auto &meta = HcommKeyMetaAt(key);
        meta.addr = reinterpret_cast<uint64_t>(addr);
        // gvaBase shares its origin with addr: QueryMemoryKey derives it from the reverse HVA lookup (the owner GVA
        // under multiple entities), so it has to be rewritten as well; otherwise the offset base of
        // ConvertRemoteGvaToDva (rGva - peerGva) would not match the data-plane address.
        meta.gvaBase = reinterpret_cast<uint64_t>(addr);
    }
}

// ============================================================================
// Connection prepare
// ============================================================================

Result DeviceRdmaHcommTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!opened_) {
        BM_LOG_ERROR("transport not opened");
        return BM_ERROR;
    }

    for (const auto &item : options.options) {
        const uint32_t peerRank = item.first;
        if (peerRank >= rankCount_) {
            BM_LOG_ERROR("invalid peerRank: " << peerRank);
            return BM_INVALID_PARAM;
        }
        if (peerRank == rankId_) {
            continue;
        }

        // 私有数据按形态分流：DEVICE 用 v1（与 master 一致），A5 host RoCE 用 v2（额外带 flagAddr）。
        const TransportMemoryKey &privKey = item.second.privateData.key;
        RdmaHcommPeerPrivateInfo peerData{};
        if (hostRocePath_) {
            RdmaHcommPrivateDataHost hostData{};
            if (!ReadStructFromKey(privKey, hostData) || hostData.magic != RDMA_HCOMM_PRIVATE_DATA_MAGIC ||
                hostData.version != RDMA_HCOMM_PRIVATE_DATA_VERSION_HOST) {
                BM_LOG_ERROR("invalid host private data from peer: " << peerRank << " bytes: " << privKey.ByteSize());
                return BM_INVALID_PARAM;
            }
            peerData.addrType = hostData.addrType;
            std::memcpy(peerData.addrBytes, hostData.addrBytes, sizeof(peerData.addrBytes));
            peerData.devPhyId = hostData.devPhyId;
            peerData.sdid = hostData.sdid;
            peerData.listenPort = hostData.listenPort;
            peerData.flagAddr = hostData.flagAddr;
        } else {
            RdmaHcommPrivateData devData{};
            if (!ReadStructFromKey(privKey, devData) || devData.magic != RDMA_HCOMM_PRIVATE_DATA_MAGIC ||
                devData.version != RDMA_HCOMM_PRIVATE_DATA_VERSION) {
                BM_LOG_ERROR("invalid private data from peer: " << peerRank << " bytes: " << privKey.ByteSize());
                return BM_INVALID_PARAM;
            }
            peerData.addrType = devData.addrType;
            std::memcpy(peerData.addrBytes, devData.addrBytes, sizeof(peerData.addrBytes));
            peerData.devPhyId = devData.devPhyId;
            peerData.sdid = devData.sdid;
            peerData.listenPort = devData.listenPort;
        }

        // 如果有远端内存 key，先 import，再创建通道
        // 如果没有（对端尚未注册内存），等 UpdateRankOptions 带 memKeys 后再处理
        if (!item.second.memKeys.empty()) {
            auto ret = ImportPeerMems(peerRank, item.second.memKeys);
            if (ret != BM_OK) {
                BM_LOG_ERROR("ImportPeerMems failed for peer: " << peerRank << " ret: " << ret);
                return ret;
            }
            BM_LOG_DEBUG("Prepare has memKeys for rank=" << peerRank << ", creating channel now");
            ret = CreatePeerResources(peerRank, peerData);
            if (ret != BM_OK) {
                BM_LOG_ERROR("CreatePeerResources failed for peer: " << peerRank);
                return ret;
            }
        }
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::CreatePeerResources(uint32_t peerRank, const RdmaHcommPeerPrivateInfo &peerData)
{
    auto &state = remoteRanks_[peerRank];

    if (state.channel != 0 || state.thread != 0) {
        return BM_OK;
    }

    // Build remote EndpointDesc from peer data
    EndpointDesc remoteDesc{};
    remoteDesc.protocol = COMM_PROTOCOL_ROCE;
    remoteDesc.commAddr.type = peerData.addrType;
    std::memcpy(remoteDesc.commAddr.raws, peerData.addrBytes, sizeof(remoteDesc.commAddr.raws));
    if (hostRocePath_) {
        remoteDesc.loc.locType = ENDPOINT_LOC_TYPE_HOST;
        remoteDesc.loc.host.id = 0; // 同本端：HOST 形态不写 device.*（union 会覆盖 host.id）
    } else {
        remoteDesc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
        remoteDesc.loc.device.devPhyId = peerData.devPhyId;
        remoteDesc.loc.device.superDevId = peerData.sdid;
        remoteDesc.loc.device.serverIdx = serverId_;
        remoteDesc.loc.device.superPodIdx = superPodId_;
    }
    state.remoteEndpointDesc = remoteDesc;
    if (hostRocePath_) {
        // 对端 flag 地址随 private data 交换，完成性判定时直接对它做一次 RDMA 读
        state.remoteFlagAddr = peerData.flagAddr;
        state.remoteFlagSize = DEVICE_RDMA_HCOMM_FLAG_SIZE;
        BM_LOG_INFO("CreatePeerResources peer rank=" << peerRank << " remoteFlagAddr=0x" << std::hex
                                                     << state.remoteFlagAddr << std::dec);
    }

    // Allocate thread
    uint32_t notifyNum = DEFAULT_NOTIFY_NUM;
    ThreadHandle threadHandle = 0;
    // HOST 形态的 endpoint 必须搭配 COMM_ENGINE_CPU：channel 层据此选 HostCpuRoceChannel
    const CommEngine threadEngine = hostRocePath_ ? COMM_ENGINE_CPU : COMM_ENGINE_AICPU_TS;
    auto ret = HcommApiWrapper::AllocThread(threadEngine, notifyNum, threadHandle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommThreadAlloc failed for peer: " << peerRank << " ret: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    state.thread = threadHandle;

    // Build channel descriptor
    HcommChannelDesc chDesc{};
    HcommChannelDescInit(&chDesc, 1);
    chDesc.role = (rankId_ > peerRank) ? HCOMM_SOCKET_ROLE_CLIENT : HCOMM_SOCKET_ROLE_SERVER;
    chDesc.remoteEndpoint = remoteDesc;
    chDesc.notifyNum = 1;
    chDesc.exchangeAllMems = true;

    // Build channelName: consistent on both ends (sorted rank pair)
    const uint32_t lowerRank = (rankId_ < peerRank) ? rankId_ : peerRank;
    const uint32_t higherRank = (rankId_ < peerRank) ? peerRank : rankId_;
    channelNameStore_ = "hybm_" + std::to_string(lowerRank) + "_" + std::to_string(higherRank);
    chDesc.channelName = channelNameStore_.c_str();

    chDesc.roceAttr.tc = DEVICE_RDMA_HCOMM_DEFAULT_TC;
    chDesc.roceAttr.sl = DEVICE_RDMA_HCOMM_DEFAULT_SL;
    chDesc.roceAttr.queueNum = 1;

    if (hostRocePath_) {
        // A5 host RoCE：channel desc 升到 ABI v4（仅该路径）
        UpgradeChannelDescToV4(chDesc);
        // 显式设置重传参数，不设会留下 0xFF 的脏值
        chDesc.roceAttr.retryCnt = DEVICE_RDMA_HCOMM_DEFAULT_RETRY_CNT;
        chDesc.roceAttr.retryInterval = DEVICE_RDMA_HCOMM_DEFAULT_RETRY_INTERVAL_MS;
        // qos 保持 unset（kQosUnset 语义）
        chDesc.qos = 0xFFFFFFFFU;
        // port 语义是「监听端口或目标端口」：SERVER 用本端监听端口，CLIENT 用对端监听端口
        chDesc.port =
            static_cast<uint16_t>(chDesc.role == HCOMM_SOCKET_ROLE_SERVER ? localListenPort_ : peerData.listenPort);
        BM_LOG_INFO("CreatePeerResources: role=" << (chDesc.role == HCOMM_SOCKET_ROLE_SERVER ? "SERVER" : "CLIENT")
                                                 << " localPort=" << localListenPort_ << " peerListenPort="
                                                 << peerData.listenPort << " for peer rank=" << peerRank);
    } else {
        // Set port from peer's listen port (critical for QP to enter RTS state)
        chDesc.port = static_cast<uint16_t>(peerData.listenPort);
        BM_LOG_INFO("CreatePeerResources: using peer listenPort=" << peerData.listenPort
                                                                  << " for peer rank=" << peerRank);
    }

    ChannelHandle channelHandle = 0;
    // HOST 形态的 channel 必须用 COMM_ENGINE_CPU，DEVICE 形态用 COMM_ENGINE_AICPU
    const CommEngine channelEngine = hostRocePath_ ? COMM_ENGINE_CPU : COMM_ENGINE_AICPU;
    ret = hcommApi_.CreateChannel(localEndpoint_, channelEngine, chDesc, channelHandle);
    if (ret != 0) {
        BM_LOG_ERROR("HcommChannelCreate failed for peer: " << peerRank << " ret: " << ret);
        (void)HcommApiWrapper::FreeThread(threadHandle);
        state.thread = 0;
        return BM_DL_FUNCTION_FAILED;
    }
    state.channel = channelHandle;

    // Wait for channel to connect
    int32_t status = -1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CHANNEL_CONNECT_TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        auto chk = hcommApi_.GetChannelStatus(channelHandle, status);
        if (chk == 0 && status == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CHANNEL_STATUS_POLL_INTERVAL_MS));
    }
    if (status != 0) {
        BM_LOG_ERROR("channel connect timeout for peer: " << peerRank << " status: " << status);
        (void)hcommApi_.DestroyChannel(channelHandle);
        (void)HcommApiWrapper::FreeThread(threadHandle);
        state.channel = 0;
        state.thread = 0;
        return BM_TIMEOUT;
    }

    BM_LOG_INFO("CreatePeerResources success for peer: "
                << peerRank << " thread: " << threadHandle << " channel: " << channelHandle << " role: "
                << (chDesc.role == HCOMM_SOCKET_ROLE_CLIENT ? "CLIENT" : "SERVER") << " port: " << chDesc.port);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::DestroyPeerResources(uint32_t peerRank)
{
    auto it = remoteRanks_.find(peerRank);
    if (it == remoteRanks_.end()) {
        return BM_OK;
    }
    auto &state = it->second;

    // stream/notify/context are per-thread (CompletionContext), not per-peer.
    // They are destroyed in CloseDevice via registry_ cleanup.
    if (state.channel != 0) {
        (void)hcommApi_.DestroyChannel(state.channel);
        state.channel = 0;
    }
    if (state.thread != 0) {
        (void)HcommApiWrapper::FreeThread(state.thread);
        state.thread = 0;
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::ImportPeerMems(uint32_t peerRank,
                                                       const std::vector<TransportMemoryKey> &memKeys)
{
    // 幂等：peerDva 在 import 成功后设置。Prepare（644 行）可能对同一 peer
    // 多次调用（如后续 UpdateRankOptions 带 memKeys 再入），重复 import 会让 HCOMM 返回
    // "memDesc already imported"（HCCL_E_AGAIN），这里直接跳过已导入的 peer。
    if (remoteRanks_[peerRank].peerDva != 0) {
        return BM_OK;
    }
    for (const auto &key : memKeys) {
        if (key.keys.size() < TRANS_KEY_DEV_SLOT2 + KEY_SIZE) {
            BM_LOG_DEBUG("ImportPeerMems skip rank=" << peerRank << " key too small: " << key.keys.size());
            continue;
        }
        // Read metadata from slot 2 via HcommKeyMeta struct; fall back to the legacy slot index if empty
        const auto &meta = HcommKeyMetaAt(key);
        uint64_t peerAddr = meta.addr;
        uint64_t peerSize = meta.size;
        if (peerAddr == 0 || peerSize == 0) {
            if (HCOMM_KEY_LEGACY_ADDR_IDX < key.keys.size() && HCOMM_KEY_LEGACY_SIZE_IDX < key.keys.size()) {
                peerAddr = key.keys[HCOMM_KEY_LEGACY_ADDR_IDX];
                peerSize = key.keys[HCOMM_KEY_LEGACY_SIZE_IDX];
            }
            if (peerAddr == 0 || peerSize == 0) {
                continue;
            }
        }
        const uint64_t descLen = meta.descLen;
        const uint64_t flagLen = meta.flagLen;
        const uint64_t gvaBase = meta.gvaBase;
        BM_LOG_DEBUG("ImportPeerMems rank=" << peerRank << " peerAddr=0x" << std::hex << peerAddr << " peerSize=0x"
                                            << peerSize << " descLen=" << std::dec << descLen);

        if (peerAddr == 0 || peerSize == 0 || descLen == 0) {
            BM_LOG_DEBUG("ImportPeerMems skip rank=" << peerRank << " (no valid HCOMM descriptor in memKeys)");
            continue;
        }
        if (descLen > key.ByteSize()) {
            BM_LOG_ERROR("ImportPeerMems rank=" << peerRank << " descLen: " << descLen
                                                << " exceeds key bytes: " << key.ByteSize());
            return BM_INVALID_PARAM;
        }

        const void *memDesc = key.ByteData();
        BM_LOG_DEBUG("ImportPeerMems descLen=" << descLen << " meta={addr=0x" << std::hex << meta.addr << " size=0x"
                                               << meta.size << " descLen=" << std::dec << meta.descLen << " gvaBase=0x"
                                               << std::hex << meta.gvaBase << std::dec << " flagLen=" << meta.flagLen
                                               << "}");

        HcommCommMem outMem{};
        auto ret = RawMemImportWithRetry(localEndpoint_, memDesc, static_cast<uint32_t>(descLen), peerRank, outMem);
        if (ret != 0) {
            BM_LOG_ERROR("HcommMemImport failed for peer: " << peerRank << " ret: " << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_DEBUG("ImportPeerMems success for rank=" << peerRank << " outMem addr=0x" << std::hex << outMem.addr
                                                        << " size=0x" << outMem.size << std::dec
                                                        << " type=" << outMem.type);

        // Store peer DVA/GVA for address translation
        auto &state = remoteRanks_[peerRank];
        state.peerDva = reinterpret_cast<uint64_t>(outMem.addr);
        state.peerDvaSize = outMem.size;
        state.peerGva = gvaBase;
        // Save the imported descriptor so RemoveRanks can Unimport it and keep the
        // HCOMM endpoint table in sync (avoid "memDesc already imported" on re-join).
        state.importedDescs.emplace_back(static_cast<const uint8_t *>(memDesc),
                                         static_cast<const uint8_t *>(memDesc) + descLen);
        BM_LOG_INFO("ImportPeerMems stored peer rank=" << peerRank << " peerDva=0x" << std::hex << state.peerDva
                                                       << " peerGva=0x" << state.peerGva << " peerSize=0x"
                                                       << state.peerDvaSize << std::dec);
        if (hostRocePath_) {
            // 对端 flag 地址不走 memory key，改由 private data 交换（见 CreatePeerResources）。
        } else {
            // Import flag descriptor from slot 3+ (if present)
            if (flagLen > 0 && flagLen <= (key.ByteSize() - HCOMM_KEY_FLAG_SLOT * sizeof(uint64_t))) {
                const void *flagDesc = reinterpret_cast<const void *>(&key.keys[HCOMM_KEY_FLAG_SLOT]);
                HcommCommMem flagOutMem{};
                auto flagRet = RawMemImportWithRetry(localEndpoint_, flagDesc, static_cast<uint32_t>(flagLen), peerRank,
                                                     flagOutMem);
                if (flagRet == 0 && flagOutMem.type != COMM_MEM_TYPE_INVALID) {
                    state.remoteFlagAddr = reinterpret_cast<uint64_t>(flagOutMem.addr);
                    state.remoteFlagSize = flagOutMem.size;
                    state.importedDescs.emplace_back(static_cast<const uint8_t *>(flagDesc),
                                                     static_cast<const uint8_t *>(flagDesc) + flagLen);
                    BM_LOG_INFO("ImportPeerMems imported flag for peer=" << peerRank << " addr=0x" << std::hex
                                                                         << state.remoteFlagAddr << std::dec
                                                                         << " size=" << state.remoteFlagSize);
                }
            }
        }
    }
    return BM_OK;
}

int32_t DeviceRdmaHcommTransportManager::RawMemImportWithRetry(HcommEndpointHandle endpoint, const void *desc,
                                                               uint32_t descLen, uint32_t peerRank,
                                                               HcommCommMem &outMem) const
{
    constexpr int kImportMaxRetry = 10;
    constexpr int kImportRetrySleepUs = 1000;
    constexpr int32_t kHcclEAgain = 20; // HCCL_E_AGAIN：资源暂不可用（并发 import 冲突）
    for (int attempt = 0; attempt < kImportMaxRetry; ++attempt) {
        auto ret = hcommApi_.RawMemImport(endpoint, desc, descLen, outMem);
        if (ret == 0) {
            return ret;
        }
        if (ret != kHcclEAgain) {
            return ret;
        }
        // HCCL_E_AGAIN：并发 join 时多进程对同一对端 import 的瞬时冲突，重试可成功
        BM_LOG_WARN("HcommMemImport AGAIN, retry attempt=" << (attempt + 1) << "/" << kImportMaxRetry
                                                           << " peer: " << peerRank << " ret: " << ret);
        usleep(kImportRetrySleepUs);
    }
    return kHcclEAgain;
}

// ============================================================================
// Remove ranks
// ============================================================================

Result DeviceRdmaHcommTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto rankId : removedRanks) {
        auto it = remoteRanks_.find(rankId);
        if (it != remoteRanks_.end()) {
            // Unimport peer memDesc/flagDesc from the HCOMM endpoint BEFORE dropping
            // local state, otherwise a later re-import (e.g. join retry after a rank
            // reconnect) hits "memDesc already imported" (HCCL_E_AGAIN) forever.
            for (const auto &desc : it->second.importedDescs) {
                if (desc.empty()) {
                    continue;
                }
                auto ret = hcommApi_.RawMemUnimport(localEndpoint_, desc.data(), static_cast<uint32_t>(desc.size()));
                if (ret != BM_OK) {
                    BM_LOG_WARN("RemoveRanks RawMemUnimport failed for peer: " << rankId << " ret: " << ret);
                }
            }
        }
        DestroyPeerResources(rankId);
        remoteRanks_.erase(rankId);
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}

const std::string &DeviceRdmaHcommTransportManager::GetNic() const
{
    return localNic_;
}

const TransportPrivateData DeviceRdmaHcommTransportManager::GetPrivateData() const
{
    TransportPrivateData data{};
    if (localEndpoint_ == nullptr) {
        return data;
    }

    if (hostRocePath_) {
        // A5 host RoCE（v2）：对端建 channel 需要本端监听端口；flag 地址也一并带过去（对端直接读它判完成）
        RdmaHcommPrivateDataHost priv{};
        priv.protocol = COMM_PROTOCOL_ROCE;
        priv.addrType = localEndpointDesc_.commAddr.type;
        std::memcpy(priv.addrBytes, localEndpointDesc_.commAddr.raws, sizeof(priv.addrBytes));
        priv.devPhyId = phyDeviceId_;
        priv.sdid = sdid_;
        priv.listenPort = localListenPort_;
        priv.flagAddr = reinterpret_cast<uint64_t>(localFlagPtr_);
        WriteStructToKey(priv, data.key);
        return data;
    }

    // DEVICE 形态（v1，与 master 一致）：listenPort/flagAddr 保持 0（channel 建链靠对端 endpoint；flag 走 memory key）
    RdmaHcommPrivateData priv{};
    priv.protocol = COMM_PROTOCOL_ROCE;
    priv.addrType = localEndpointDesc_.commAddr.type;
    std::memcpy(priv.addrBytes, localEndpointDesc_.commAddr.raws, sizeof(priv.addrBytes));
    priv.devPhyId = phyDeviceId_;
    priv.sdid = sdid_;

    WriteStructToKey(priv, data.key);
    return data;
}

// ============================================================================
// AICPU kernel launch
// ============================================================================

// ============================================================================
// GVA → DVA address conversion for HCOMM RDMA writes/reads
// HCOMM registers DVA addresses, but the framework passes GVA.
// ============================================================================

uint64_t DeviceRdmaHcommTransportManager::ConvertGvaToDva(uint64_t gva) const
{
    // localRegistrations_ is keyed by HVA (mr.addr), while callers pass GVA.
    // In HBM model GVA == HVA so the direct lookup works; in DRAM + 56-bit GVA
    // model GVA (0x1000...) differs from HVA (0x2800...), so translate GVA→HVA
    // first before looking up the registered range.
    const uint64_t hva = HybmVaManager::GetInstance().TransformVa(gva, HVM_GVA, HVM_HVA);
    const uint64_t lookupKey = (hva != 0) ? hva : gva;
    BM_LOG_DEBUG("ConvertGvaToDva gva=0x" << std::hex << gva << " -> hva=0x" << hva << std::dec);

    // Look up in localRegistrations_ (keyed by HVA, value stores DVA)
    auto it = localRegistrations_.find(lookupKey);
    if (it != localRegistrations_.end()) {
        return it->second.addr + (lookupKey - it->first);
    }
    // Fallback: linear scan for offset within a registered range
    for (const auto &[regGva, entry] : localRegistrations_) {
        if (lookupKey >= regGva && lookupKey < regGva + entry.size) {
            return entry.addr + (lookupKey - regGva);
        }
    }
    BM_LOG_WARN("ConvertGvaToDva: no mapping for GVA=0x" << std::hex << gva << std::dec
                                                         << ", using raw value (may fail if HOST DRAM)");
    return gva;
}

// Convert remote GVA to DVA using the peer's registered base.
// state.peerGva stores the peer's GVA base (from QueryMemoryKey gvaBase), and
// callers pass GVA. So remote DVA = peerDva + (rGva - peerGva). For memory that
// the VaManager maps GVA→DVA (local HBM), use that mapping first.
uint64_t DeviceRdmaHcommTransportManager::ConvertRemoteGvaToDva(const RemoteRankState &state, uint64_t rGva) const
{
    uint64_t remoteDva = rGva;

    // Local memory with an existing GVA→DVA mapping (e.g. HBM, or imported
    // memory the VaManager registered with a DVA).
    const uint64_t dvaViaMgr = HybmVaManager::GetInstance().TransformVa(rGva, HVM_GVA, HVM_DVA);
    if (dvaViaMgr != 0) {
        return dvaViaMgr;
    }

    // Remote peer memory: peerGva is the peer's GVA base (0x1000...), peerDva is
    // the imported DVA of that base. rGva is a GVA in the peer's space.
    if (state.peerDva != 0 && state.peerGva != 0) {
        const int64_t gvaOffset = static_cast<int64_t>(rGva) - static_cast<int64_t>(state.peerGva);
        if (gvaOffset >= 0 && static_cast<uint64_t>(gvaOffset) < state.peerDvaSize) {
            remoteDva = state.peerDva + static_cast<uint64_t>(gvaOffset);
        }
    }
    return remoteDva;
}

// ============================================================================
// TLS binding & per-thread CompletionContext
// ============================================================================

std::vector<DeviceRdmaHcommTransportManager::ContextBinding> &DeviceRdmaHcommTransportManager::GetTlsBindings()
{
    static thread_local std::vector<ContextBinding> bindings;
    return bindings;
}

DeviceRdmaHcommTransportManager::CompletionContext *DeviceRdmaHcommTransportManager::FindCurrentContextLocked() const
{
    auto &bindings = GetTlsBindings();
    for (auto &b : bindings) {
        if (!b.owner.expired()) {
            auto ctx = b.ctx.lock();
            if (ctx != nullptr) {
                return ctx.get();
            }
        }
    }
    return nullptr;
}

DeviceRdmaHcommTransportManager::CompletionContext *DeviceRdmaHcommTransportManager::LookupOrCreateContextLocked()
{
    auto existing = FindCurrentContextLocked();
    if (existing != nullptr) {
        return existing;
    }
    auto ctx = std::make_shared<CompletionContext>();
    auto ret = CreateAndPublishContextLocked(ctx);
    if (ret != BM_OK) {
        return nullptr;
    }
    return ctx.get();
}

Result DeviceRdmaHcommTransportManager::CreateAndPublishContextLocked(std::shared_ptr<CompletionContext> &ctx)
{
    // AICPU kernel launch requires a FAST_LAUNCH | FAST_SYNC stream (same as device_urma).
    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("CreateAndPublishContextLocked GetThreadAclStream failed");
        return BM_DL_FUNCTION_FAILED;
    }
    ctx->stream = stream;

    auto ret = EnsureContextInitLocked(*ctx);
    if (ret != BM_OK) {
        ctx->stream = nullptr;
        return ret;
    }

    auto &bindings = GetTlsBindings();
    ContextBinding binding{};
    binding.owner = openGeneration_;
    binding.ctx = ctx;
    bindings.push_back(binding);
    {
        std::lock_guard<std::mutex> regGuard(mutex_);
        registry_.push_back(ctx);
    }

    BM_LOG_INFO("CreateAndPublishContextLocked success, notifyId=" << ctx->notifyId);
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::EnsureContextInitLocked(CompletionContext &ctx)
{
    if (ctx.initialized) {
        return BM_OK;
    }

    void *notify = nullptr;
    auto ret = DlAclApi::AclrtCreateNotify(&notify, ACL_NOTIFY_FLAG_DEVICE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("EnsureContextInitLocked AclrtCreateNotify failed, ret=" << ret);
        return ret;
    }
    ctx.notify = notify;

    uint32_t notifyId = 0;
    ret = DlAclApi::AclrtGetNotifyId(notify, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("EnsureContextInitLocked AclrtGetNotifyId failed, ret=" << ret);
        (void)DlAclApi::AclrtDestroyNotify(notify);
        ctx.notify = nullptr;
        return ret;
    }
    ctx.notifyId = notifyId;
    // RtGetDevResAddress is STARS-only (A5), not available on A2.
    // notifyAddr/notifyLen stay 0; notify works via notifyId (use_notify_record=1).
    // notifyHcommHandle stays nullptr; CloseDevice skips unregistration.

    ctx.initialized = true;
    BM_LOG_INFO("EnsureContextInitLocked success, notifyId=" << notifyId);
    return BM_OK;
}

// ============================================================================
// Pending transfer management
// ============================================================================

void DeviceRdmaHcommTransportManager::ExtractRankPending(std::vector<PendingTransfer> &src, uint32_t rankId,
                                                         std::vector<PendingTransfer> &dst)
{
    auto it = src.begin();
    while (it != src.end()) {
        if (it->rankId == rankId) {
            dst.push_back(std::move(*it));
            it = src.erase(it);
        } else {
            ++it;
        }
    }
}

void DeviceRdmaHcommTransportManager::RestoreRankPending(std::vector<PendingTransfer> &src,
                                                         std::vector<PendingTransfer> &dst)
{
    for (auto &p : src) {
        dst.push_back(std::move(p));
    }
    src.clear();
}

Result DeviceRdmaHcommTransportManager::SynchronizeContextLocked(void * /* notify */, void *stream,
                                                                 std::vector<PendingTransfer> &pendingTransfers)
{
    auto syncRet = DlAclApi::AclrtSynchronizeStream(stream);
    if (syncRet != BM_OK) {
        BM_LOG_WARN("SynchronizeContextLocked AclrtSynchronizeStream failed, ret=" << syncRet);
    }
    for (auto &pt : pendingTransfers) {
        device::ReleaseDeviceTransferBuffers(pt.buffers);
    }
    pendingTransfers.clear();
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::SynchronizeRankPendingLocked(CompletionContext &ctx, RemoteRankState &state,
                                                                     uint32_t rankId, bool hasInFlight)
{
    std::vector<PendingTransfer> rankPending{};
    std::lock_guard<std::mutex> rankLock(state.rankMutex);
    ExtractRankPending(ctx.pendingTransfers, rankId, rankPending);

    // Data kernel already ensured data consistency via WRITE → RDMA READ flag (QP ordering).
    // No marker kernel needed; just drain the stream and release buffers.
    auto syncRet = SynchronizeContextLocked(ctx.notify, ctx.stream, rankPending);
    if (syncRet != BM_OK) {
        RestoreRankPending(rankPending, ctx.pendingTransfers);
    }
    return syncRet;
}

// ============================================================================
// Transfer staging
// ============================================================================

Result DeviceRdmaHcommTransportManager::StageAndLaunchTransfer(CompletionContext &ctx, RemoteRankState &state,
                                                               bool isRead, const std::vector<uint64_t> &localVec,
                                                               const std::vector<uint64_t> &remoteVec,
                                                               const std::vector<uint64_t> &sizeVec, uint32_t rankId)
{
    std::lock_guard<std::mutex> rankLock(state.rankMutex);

    if (hostRocePath_) {
        // HOST 形态的 RoCE endpoint 走主机侧提交：不下发 AICPU 算子。
        // 用 Nbi 变体提交（thread 传 0）：
        // cpu_primitives_c_adpt 里阻塞版 *OnThread 会先判 IS_PLUGIN_HANDLE，非插件 channel 退回 legacy 路径
        // (HcclRemoteWrite -> Transport::pimpl_)，未安装 NIC plugin 时必然失败；Nbi 变体直接走 channel 的 nic ops。
        // 一批 N 个 IO 连续提交，SQ(send queue) 可能被这一批 WR 打满：ibv_post_send 返回 ENOMEM 时
        // HCOMM 归一成 HCCL_E_AGAIN(20)。先把已提交的排空
        // （fence 会等 CQE 到齐并把 wqeNums_ 归零、腾出 SQ），再重试这一笔，避免整批直接失败。
        constexpr int32_t kHcclEAgain = 20;
        constexpr int64_t kSubmitRetryTimeoutMs = 60 * 1000;
        const auto submitRetryDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSubmitRetryTimeoutMs);

        int32_t submitRet = 0;
        for (size_t i = 0; i < localVec.size(); ++i) {
            while (true) {
                if (isRead) {
                    submitRet =
                        DlHcommApi::HcommReadNbiOnThread(0, state.channel, reinterpret_cast<void *>(localVec[i]),
                                                         reinterpret_cast<const void *>(remoteVec[i]), sizeVec[i]);
                } else {
                    submitRet =
                        DlHcommApi::HcommWriteNbiOnThread(0, state.channel, reinterpret_cast<void *>(remoteVec[i]),
                                                          reinterpret_cast<const void *>(localVec[i]), sizeVec[i]);
                }
                if (submitRet != kHcclEAgain) {
                    break; // 成功，或非「暂不可用」类错误：这一层不重试
                }
                if (std::chrono::steady_clock::now() >= submitRetryDeadline) {
                    BM_LOG_ERROR("StageAndLaunchTransfer submit retry timeout, rank: " << rankId << " idx: " << i);
                    break;
                }
                const int32_t drainRet = DlHcommApi::HcommChannelFenceOnThread(0, state.channel);
                if (drainRet != 0) {
                    BM_LOG_ERROR("StageAndLaunchTransfer fence-before-retry failed, rank: " << rankId
                                                                                            << " ret: " << drainRet);
                    submitRet = drainRet;
                    break;
                }
            }
            if (submitRet != 0) {
                BM_LOG_ERROR("StageAndLaunchTransfer " << (isRead ? "read" : "write")
                                                       << " submit failed, rank: " << rankId << " idx: " << i
                                                       << " size: " << sizeVec[i] << " ret: " << submitRet);
                break;
            }
        }
        if (submitRet != 0) {
            return BM_DL_FUNCTION_FAILED;
        }

        BM_LOG_DEBUG("StageAndLaunchTransfer submit done, rank: " << rankId << (isRead ? " read" : " write")
                                                                  << " count: " << localVec.size());
        const int32_t fenceRet = DlHcommApi::HcommChannelFenceOnThread(0, state.channel);
        if (fenceRet != 0) {
            BM_LOG_ERROR("StageAndLaunchTransfer HcommChannelFenceOnThread failed, rank: " << rankId
                                                                                           << " ret: " << fenceRet);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_DEBUG("StageAndLaunchTransfer fence done, rank: " << rankId);

        // 完成性判定只依赖 fence：HcommChannelFenceOnThread 会忙等本端 sendCq，直到本次提交的
        // 所有 WR 都拿到 CQE（RC QP 下即对端已 ACK、数据已落地）。原先在 fence 之后又读一次
        // 对端 flag 属于二次确认，多一个 RDMA READ 往返，去掉以降低每笔延迟。
        BM_LOG_DEBUG("StageAndLaunchTransfer done, rank: " << rankId << " isRead: " << isRead
                                                           << " count: " << localVec.size());
        return BM_OK;
    }

    DeviceTransferBuffers buffers{};
    auto ret = device::LaunchDeviceKernel(
        KernelLaunchConfig{isRead ? deviceFuncHandles_.batchRead : deviceFuncHandles_.batchWrite, state.thread,
                           state.channel, ctx.stream, isRead, static_cast<uint32_t>(localVec.size()), localVec.data(),
                           remoteVec.data(), sizeVec.data(), state.remoteFlagAddr,
                           reinterpret_cast<uint64_t>(localFlagPtr_), static_cast<uint32_t>(localFlagSize_)},
        buffers);
    if (ret != BM_OK) {
        return ret;
    }

    // Synchronize immediately after launch to keep only one kernel running on the
    // shared HCOMM thread/channel at a time. Concurrent kernels interleave on the
    // AICPU engine thread and corrupt HCOMM's thread-local BatchMode state
    // (g_threadLaunchCtx), which fails the kernel with aicpu error 0x2a. This
    // matches the synchronous model of device_urma; the flag read-back inside the
    // kernel still provides write ordering (QP ordering) on A2 where Fence is no-op.
    const auto syncRet = DlAclApi::AclrtSynchronizeStream(ctx.stream);
    device::ReleaseDeviceTransferBuffers(buffers);
    if (syncRet != BM_OK) {
        BM_LOG_ERROR("StageAndLaunchTransfer AclrtSynchronizeStream failed, rank: " << rankId << " ret: " << syncRet);
        return syncRet;
    }
    return BM_OK;
}

// ============================================================================
// Data transfer
// ============================================================================

Result DeviceRdmaHcommTransportManager::RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size,
                                                 bool isRead)
{
    if (size == 0) {
        return BM_OK;
    }
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0 || state.thread == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    // Convert GVA to DVA for HCOMM: RDMA uses registered DVA addresses
    const uint64_t localDva = ConvertGvaToDva(lAddr);
    const uint64_t remoteDva = ConvertRemoteGvaToDva(state, rAddr);

    BM_LOG_DEBUG("RemoteIo rank=" << rankId << (isRead ? " READ" : " WRITE") << " lGVA=0x" << std::hex << lAddr
                                  << " lDVA=0x" << localDva << " rGVA=0x" << rAddr << " rDVA=0x" << remoteDva
                                  << " size=0x" << size << std::dec);

    // TLS: look up or create per-thread CompletionContext
    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("RemoteIo LookupOrCreateContextLocked failed");
        return BM_MALLOC_FAILED;
    }

    return StageAndLaunchTransfer(*ctx, state, isRead, {localDva}, {remoteDva}, {size}, rankId);
}

Result DeviceRdmaHcommTransportManager::RemoteIoBatch(uint32_t rankId, const CopyDescriptor &descriptor, bool isRead)
{
    if (descriptor.counts.empty()) {
        return BM_OK;
    }
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0 || state.thread == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("RemoteIoBatch LookupOrCreateContextLocked failed");
        return BM_MALLOC_FAILED;
    }

    // A5(950) host RoCE：一次提交的 WR 数受 QP send queue 深度限制（实测约 2048），超过会把 SQ
    // 打满、ibv_post_send 失败，因此按 SQ 安全上限切片，每片一次提交 + 各自 fence。
    // DEVICE 形态(含 A2/A3)仍逐笔下发一片，保持原有的 kernel 下发粒度与提交行为不变。
    const size_t maxWrPerBatch = hostRocePath_ ? 1024U : 1U;
    const size_t total = descriptor.counts.size();
    for (size_t beg = 0; beg < total; beg += maxWrPerBatch) {
        const size_t n = std::min(maxWrPerBatch, total - beg);
        std::vector<uint64_t> localVec(n);
        std::vector<uint64_t> remoteVec(n);
        std::vector<uint64_t> counts(n);
        for (size_t i = 0; i < n; ++i) {
            // Convert GVA to DVA for HCOMM RDMA
            localVec[i] = ConvertGvaToDva(reinterpret_cast<uint64_t>(descriptor.localAddrs[beg + i]));
            remoteVec[i] = ConvertRemoteGvaToDva(state, reinterpret_cast<uint64_t>(descriptor.globalAddrs[beg + i]));
            counts[i] = descriptor.counts[beg + i];
        }
        auto ret = StageAndLaunchTransfer(*ctx, state, isRead, localVec, remoteVec, counts, rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("RemoteIoBatch chunk failed, rank: " << rankId << " beg: " << beg << " n: " << n
                                                              << " ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

// ============================================================================
// Data transfer public API (overrides from TransportManager)
// ============================================================================

Result DeviceRdmaHcommTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, true);
    if (ret == BM_OK) {
        ret = Synchronize(rankId);
    }
    return ret;
}

Result DeviceRdmaHcommTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, false);
    if (ret == BM_OK) {
        ret = Synchronize(rankId);
    }
    return ret;
}

Result DeviceRdmaHcommTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, true);
}

Result DeviceRdmaHcommTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return RemoteIo(rankId, lAddr, rAddr, size, false);
}

Result DeviceRdmaHcommTransportManager::Synchronize(uint32_t rankId)
{
    auto it = remoteRanks_.find(rankId);
    if (it == remoteRanks_.end()) {
        BM_LOG_ERROR("rank not connected: " << rankId);
        return BM_NOT_CONNECTED;
    }
    auto &state = it->second;
    if (state.channel == 0) {
        BM_LOG_ERROR("peer resources not ready for rank: " << rankId);
        return BM_NOT_INITIALIZED;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    CompletionContext *currentCtx = FindCurrentContextLocked();
    if (currentCtx == nullptr) {
        // No TLS context for current thread; warn if rank has pending in other contexts
        bool otherCtxHasPending = false;
        for (const auto &ctxSp : registry_) {
            if (!ctxSp) {
                continue;
            }
            for (const auto &pt : ctxSp->pendingTransfers) {
                if (pt.rankId == rankId) {
                    otherCtxHasPending = true;
                    break;
                }
            }
        }
        if (otherCtxHasPending) {
            BM_LOG_ERROR("Synchronize: rank " << rankId << " has pending in another context");
            return BM_ERROR;
        }
        return BM_OK;
    }

    // Check if current context has pending for target rank
    bool needSync = false;
    for (const auto &pt : currentCtx->pendingTransfers) {
        if (pt.rankId == rankId) {
            needSync = true;
            break;
        }
    }
    if (needSync) {
        return SynchronizeRankPendingLocked(*currentCtx, state, rankId, false);
    }

    // No pending in current context. Check other contexts.
    for (const auto &ctxSp : registry_) {
        if (!ctxSp || ctxSp.get() == currentCtx) {
            continue;
        }
        for (const auto &pt : ctxSp->pendingTransfers) {
            if (pt.rankId == rankId) {
                BM_LOG_ERROR("Synchronize: rank " << rankId << " has pending in another context");
                return BM_ERROR;
            }
        }
    }
    return BM_OK;
}

Result DeviceRdmaHcommTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, false);
}

Result DeviceRdmaHcommTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    return RemoteIoBatch(rankId, descriptor, true);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
