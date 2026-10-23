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

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cassert>
#include <limits>
#include <string>

#include "dl_acl_api.h"
#include "dl_hcomm_api.h"
#include "hybm_batch_transfer.h"
#include "hybm_copy_direction.h"
#include "hybm_logger.h"
#include "hybm_ptracer.h"
#include "hybm_stream_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "urma_topo_helper.h"
#include "hybm_va_manager.h"
#include "device_kernel_helper.h"
#include "device_urma_transport_manager.h"

#include "urma_topo_manager.h"

using namespace ock::mf;

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {
constexpr uint32_t HCOMM_NORMAL_NOTIFY_NUM = 0;
constexpr uint32_t HCOMM_CHANNEL_QOS_DEFAULT = 0U;
constexpr uint32_t HCOMM_CHANNEL_QOS_MAX = 7U;
constexpr const char *HCOMM_CHANNEL_QOS_ENV = "MF_DEVICE_UB_QOS";
constexpr const char *HYBM_DEVICE_FUNC_TRANSFER = "HybmBatchTransfer";
constexpr uint32_t HYBM_DEVICE_KERNEL_BLOCK_DIM = 1U;
constexpr uint16_t HYBM_DEVICE_KERNEL_TIMEOUT_S = 60U;
constexpr uint32_t HYBM_NOTIFY_DEFAULT_WAIT_TIME_S = 27U * 68U;
uint32_t GetHcommChannelQos()
{
    const char *rawValue = std::getenv(HCOMM_CHANNEL_QOS_ENV);
    if (rawValue == nullptr) {
        return HCOMM_CHANNEL_QOS_DEFAULT;
    }

    uint32_t qos = 0;
    if (!MfEnvUtil::GetOptionalUint(std::string(rawValue), qos) || qos > HCOMM_CHANNEL_QOS_MAX) {
        BM_LOG_WARN("device_urma environment variable " << HCOMM_CHANNEL_QOS_ENV << " has invalid value '" << rawValue
                                                        << "', valid range [0, " << HCOMM_CHANNEL_QOS_MAX
                                                        << "], fallback to default " << HCOMM_CHANNEL_QOS_DEFAULT);
        return HCOMM_CHANNEL_QOS_DEFAULT;
    }
    return qos;
}

constexpr uint32_t HYBM_NOTIFY_POOL_INVALID_INDEX = UINT32_MAX;
constexpr uint32_t HYBM_NOTIFY_POOL_MAX_SIZE = 65536U;
constexpr size_t HYBM_KERNEL_LAUNCH_BUFFER_MIN_SIZE = 4U * 1024U;

uint64_t PackNotifyFreeHead(uint32_t index, uint32_t version)
{
    return (static_cast<uint64_t>(version) << 32U) | index;
}

uint32_t NotifyFreeHeadIndex(uint64_t head)
{
    return static_cast<uint32_t>(head);
}

uint32_t NotifyFreeHeadVersion(uint64_t head)
{
    return static_cast<uint32_t>(head >> 32U);
}

static_assert(std::is_trivially_copyable<UrmaExportDesc>::value, "UrmaExportDesc must be binary serializable");
static_assert(std::is_trivially_copyable<UrmaEndpointDesc>::value,
              "UrmaEndpointDesc must be trivially copyable for memcpy serialization");

// Must NOT conflict with URMA_EXPORT_DESC_MAGIC (0xA5FAB001).
constexpr uint32_t URMA_PRIVATE_DATA_MAGIC = 0xA5FAC003U;
constexpr uint16_t URMA_PRIVATE_DATA_VERSION = 1U;

struct UrmaPrivateDataDesc {
    uint32_t magic{URMA_PRIVATE_DATA_MAGIC};
    uint16_t version{URMA_PRIVATE_DATA_VERSION};
    uint16_t payloadLen{0};
};

static_assert(std::is_trivially_copyable<UrmaEndpointDesc>::value,
              "UrmaEndpointDesc must be trivially copyable for byte serialization");

UrmaMemoryType ToUrmaMemoryType(uint32_t flags)
{
    if (flags & REG_MR_FLAG_HBM) {
        return UrmaMemoryType::DEVICE_HBM;
    }
    return UrmaMemoryType::HOST_DRAM;
}

UrmaCommMem ToUrmaMem(const TransportMemoryRegion &mr)
{
    return UrmaCommMem{mr.addr, mr.size, ToUrmaMemoryType(mr.flags)};
}

// ── Free functions migrated from deleted hcomm_transport_manager.cpp ──

CommProtocol ToHcommProtocol(UrmaProtocol protocol)
{
    if (protocol == UrmaProtocol::ROCE) {
        return COMM_PROTOCOL_ROCE;
    }
    if (protocol == UrmaProtocol::UBC_TP) {
        return COMM_PROTOCOL_UBC_TP;
    }
    if (protocol == UrmaProtocol::UBC_CTP) {
        return COMM_PROTOCOL_UBC_CTP;
    }
    if (protocol == UrmaProtocol::UBOE) {
        return COMM_PROTOCOL_UBOE;
    }
    if (protocol == UrmaProtocol::UB_RTP) {
        return COMM_PROTOCOL_UB_RTP;
    }
    return COMM_PROTOCOL_RESERVED;
}

bool GetRangeEnd(const UrmaCommMem &mem, uint64_t &end)
{
    if (mem.addr == 0 || mem.size == 0) {
        return false;
    }
    if (std::numeric_limits<uint64_t>::max() - mem.addr < mem.size) {
        return false;
    }
    end = mem.addr + mem.size;
    return true;
}

bool IsValidMem(const UrmaCommMem &mem)
{
    uint64_t end = 0;
    return (mem.type == UrmaMemoryType::HOST_DRAM || mem.type == UrmaMemoryType::DEVICE_HBM) && GetRangeEnd(mem, end);
}

bool IsEmptyMemoryKey(const TransportMemoryKey &key)
{
    return std::all_of(std::begin(key.keys), std::end(key.keys), [](uint64_t value) { return value == 0; });
}

EndpointDesc ToHcommEndpointDesc(const UrmaEndpointDesc &desc)
{
    EndpointDesc endpoint{};
    endpoint.protocol = ToHcommProtocol(desc.protocol);
    endpoint.commAddr.type = desc.type;
    std::memcpy(endpoint.commAddr.raws, desc.raws, sizeof(endpoint.commAddr.raws));
    endpoint.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    endpoint.loc.device.devPhyId = desc.devPhyId;
    endpoint.loc.device.superDevId = desc.superDevId;
    endpoint.loc.device.serverIdx = desc.serverIdx;
    endpoint.loc.device.superPodIdx = desc.superPodIdx;
    return endpoint;
}

HcommCommMem ToHcommMem(const UrmaCommMem &mem)
{
    HcommCommMem hcommMem{};
    hcommMem.type = COMM_MEM_TYPE_HOST;
    if (mem.type == UrmaMemoryType::DEVICE_HBM) {
        hcommMem.type = COMM_MEM_TYPE_DEVICE;
    }
    hcommMem.addr = reinterpret_cast<void *>(mem.addr);
    hcommMem.size = mem.size;
    return hcommMem;
}

bool SameMem(const UrmaCommMem &left, const UrmaCommMem &right)
{
    return left.addr == right.addr && left.size == right.size && left.type == right.type;
}

bool Overlaps(const UrmaCommMem &left, const UrmaCommMem &right)
{
    uint64_t leftEnd = 0;
    uint64_t rightEnd = 0;
    return left.type == right.type && GetRangeEnd(left, leftEnd) && GetRangeEnd(right, rightEnd) &&
           left.addr < rightEnd && right.addr < leftEnd;
}

// Convert HcommCommMem → UrmaCommMem (for HcommApiWrapper migration)
static UrmaCommMem ToUrmaCommMem(const HcommCommMem &mem)
{
    UrmaCommMem urmaMem{};
    urmaMem.addr = reinterpret_cast<uint64_t>(mem.addr);
    urmaMem.size = mem.size;
    urmaMem.type = (mem.type == COMM_MEM_TYPE_DEVICE) ? UrmaMemoryType::DEVICE_HBM : UrmaMemoryType::HOST_DRAM;
    return urmaMem;
}

static bool IsFM16Product()
{
    RootInfo rootInfo{};
    UrmaTopoManager::GetInstance().GetRootInfo(rootInfo);
    const std::string target = "driver/topo/950/atlas_850_2.json";
    if (!rootInfo.topoFilePath.empty() && rootInfo.topoFilePath.find(target) != std::string::npos) {
        return true;
    }
    return false;
}

static bool CheckAndGetUrmaExportDescInfo(const uint8_t *raw, uint64_t rawLen, int32_t localId,
                                          UrmaExportDesc &exportDesc, uint32_t &exportDescIndex)
{
    uint32_t offset = 0;
    uint32_t i = 0;
    constexpr uint32_t exportHeaderSize = sizeof(UrmaExportDesc);
    while (offset < rawLen) {
        if (offset + exportHeaderSize > rawLen) {
            break;
        }
        UrmaExportDesc desc{};
        std::memcpy(&desc, raw + offset, exportHeaderSize);
        uint32_t entrySize = exportHeaderSize + desc.hcommDescLen + desc.devTransFlagDescLen;
        if (offset + entrySize > rawLen) {
            return false;
        }
        if (desc.remoteId == localId || desc.remoteId == -1) {
            if (desc.magic != URMA_EXPORT_DESC_MAGIC || desc.version != URMA_EXPORT_DESC_VERSION) {
                BM_LOG_ERROR("device_urma CheckAndGetUrmaExportDescInfo invalid UrmaExportDesc magic/version in key");
                return false;
            }
            if (desc.headerSize != sizeof(UrmaExportDesc)) {
                BM_LOG_ERROR("device_urma CheckAndGetUrmaExportDescInfo UrmaExportDesc headerSize mismatch");
                return false;
            }
            if (desc.hcommDescLen == 0) {
                BM_LOG_ERROR("device_urma CheckAndGetUrmaExportDescInfo hcommDescLen is 0");
                return false;
            }
            if (desc.size == 0) {
                BM_LOG_ERROR("device_urma CheckAndGetUrmaExportDescInfo exportDesc.size is 0");
                return false;
            }
            exportDesc = desc;
            exportDescIndex = offset + exportHeaderSize;
            return true;
        }
        i++;
        offset += entrySize;
    }
    BM_LOG_ERROR("device_urma CheckAndGetUrmaExportDescInfo not found exportDesc index");
    return false;
}

bool IsSupportedMemoryFlags(uint32_t flags)
{
    const bool hasDram = (flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0;
    const bool hasHbm = (flags & REG_MR_FLAG_HBM) != 0;
    return !(hasDram && hasHbm);
}

void RecordUrmaIoMetrics(const std::vector<HcommBatchTransferDesc> &transferDescs, size_t offset, size_t count)
{
    const auto begin = transferDescs.begin() + offset;
    const auto end = begin + count;
    const auto totalSize =
        std::accumulate(begin, end, uint64_t{0}, [](uint64_t total, const HcommBatchTransferDesc &desc) {
            return total + (desc.transType == HCOMM_TRANSFER_TYPE_READ ? desc.transferInfo.read.len
                                                                       : desc.transferInfo.write.len);
        });
    TP_TRACE_RECORD(TP_HYBM_URMA_IO_COUNT, count * 1000ULL, BM_OK);
    TP_TRACE_RECORD(TP_HYBM_URMA_IO_TOTAL_SIZE, totalSize * 1000ULL, BM_OK);
}

inline bool ContainsAddressRange(const uint64_t &outerAddr, const uint64_t &outerSize, const uint64_t &innerAddr,
                                 const uint64_t &innerSize)
{
    return outerAddr <= innerAddr && (outerAddr + outerSize) >= (innerAddr + innerSize);
}

// Returns BM_INVALID_PARAM on magic/version/payloadLen/capacity mismatch.
Result ParsePrivateDataToEndpointDesc(const TransportPrivateData &privateData, const int32_t localId,
                                      UrmaEndpointDesc &outDesc)
{
    constexpr size_t headerSize = sizeof(UrmaPrivateDataDesc);
    const uint8_t *raw = privateData.key.ByteData();

    UrmaPrivateDataDesc header{};
    std::memcpy(&header, raw, headerSize);
    if (header.magic != URMA_PRIVATE_DATA_MAGIC) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc invalid magic: 0x"
                     << std::hex << header.magic << " expected: 0x" << URMA_PRIVATE_DATA_MAGIC);
        return BM_INVALID_PARAM;
    }
    if (header.version != URMA_PRIVATE_DATA_VERSION) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc unsupported version: "
                     << header.version << " expected: " << URMA_PRIVATE_DATA_VERSION);
        return BM_INVALID_PARAM;
    }
    if (header.payloadLen % sizeof(UrmaEndpointDesc) != 0) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc payloadLen mismatch: "
                     << header.payloadLen << " expected: " << sizeof(UrmaEndpointDesc));
        return BM_INVALID_PARAM;
    }
    if (headerSize + header.payloadLen > privateData.key.ByteSize()) {
        BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc payload exceeds key capacity, payloadLen: "
                     << header.payloadLen << " capacity: " << privateData.key.ByteSize());
        return BM_INVALID_PARAM;
    }
    for (size_t i = 0; i < header.payloadLen / sizeof(UrmaEndpointDesc); ++i) {
        const uint8_t *current_ptr = raw + headerSize + (sizeof(UrmaEndpointDesc) * i);
        UrmaEndpointDesc desc{};
        std::memcpy(&desc, current_ptr, sizeof(UrmaEndpointDesc));
        if (desc.remoteId == localId || desc.remoteId == -1) {
            outDesc = desc;
            return BM_OK;
        }
    }
    BM_LOG_ERROR("device_urma ParsePrivateDataToEndpointDesc not found localId :" << localId);
    return BM_ERROR;
}

bool ProtocolMatchesRequestedBits(UrmaProtocol protocol, uint32_t protocolBits)
{
    if (protocol == UrmaProtocol::UBC_CTP || protocol == UrmaProtocol::UB_RTP) {
        return (protocolBits & HYBM_DOP_TYPE_DEVICE_URMA) != 0;
    }
    if (protocol == UrmaProtocol::UBOE) {
        return (protocolBits & HYBM_DOP_TYPE_DEVICE_UBOE) != 0;
    }
    return false;
}

const UrmaProtocolTableEntry *FindUrmaProtocolTableEntry(uint32_t mainboardId)
{
    for (const auto &entry : URMA_PROTOCOL_TABLE) {
        if (entry.mainboardId == mainboardId) {
            return &entry;
        }
    }
    return nullptr;
}

Result SelectUrmaProtocol(uint32_t mainboardId, uint32_t protocolBits, UrmaProtocol &protocol)
{
    const UrmaProtocolTableEntry *entry = FindUrmaProtocolTableEntry(mainboardId);
    if (entry == nullptr) {
        BM_LOG_ERROR("device_urma unsupported mainboardId, mainboardId=" << mainboardId
                                                                         << " protocolBits=" << protocolBits);
        return BM_NOT_SUPPORTED;
    }
    for (uint32_t i = 0; i < entry->protocolCount; ++i) {
        if (ProtocolMatchesRequestedBits(entry->protocols[i], protocolBits)) {
            protocol = entry->protocols[i];
            return BM_OK;
        }
    }
    BM_LOG_ERROR("device_urma no protocol matched, mainboardId=" << mainboardId << " protocolBits=" << protocolBits
                                                                 << " rowProtocols=" << entry->protocolCount);
    return BM_NOT_SUPPORTED;
}

std::string FormatEid(const std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData)
{
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string eid;
    eid.reserve(COMM_ADDR_EID_LEN * 2U);
    for (const auto byte : eidData) {
        eid.push_back(HEX_DIGITS[(byte >> 4U) & 0x0FU]);
        eid.push_back(HEX_DIGITS[byte & 0x0FU]);
    }
    return eid;
}

std::string FormatIpAddress(CommAddrType addrType, const uint8_t *addrData)
{
    int family = AF_UNSPEC;
    if (addrType == COMM_ADDR_TYPE_IP_V4) {
        family = AF_INET;
    } else if (addrType == COMM_ADDR_TYPE_IP_V6) {
        family = AF_INET6;
    } else {
        return "<invalid>";
    }

    char ipText[INET6_ADDRSTRLEN]{};
    if (addrData == nullptr || inet_ntop(family, addrData, ipText, sizeof(ipText)) == nullptr) {
        return "<invalid>";
    }
    return ipText;
}

} // namespace

DeviceUrmaTransportManager::CompletionContext &DeviceUrmaTransportManager::GetTlsContext()
{
    thread_local CompletionContext ctx;
    return ctx;
}

DeviceUrmaTransportManager::~DeviceUrmaTransportManager()
{
    (void)CloseDevice();
}

Result DeviceUrmaTransportManager::InitLocalDeviceInfoLocked(const TransportOptions &options)
{
    int32_t userId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0 && userId >= 0,
                             "AclrtGetDevice() return=" << ret << ", output deviceId=" << userId,
                             BM_DL_FUNCTION_FAILED);

    options_ = options;
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    userDeviceId_ = static_cast<uint32_t>(userId);
    int32_t phyId = 0;
    // 实测需要使用userDeviceId
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "aclrtGetPhyDevIdByLogicDevId() return=" << ret << ", userDeviceId=" << userId,
                             BM_DL_FUNCTION_FAILED);
    BM_LOG_INFO("aclrtGetPhyDevIdByLogicDevId: userId=" << userId << ", phyId=" << phyId);
    phyDeviceId_ = static_cast<uint32_t>(phyId);

    // Get device location info
    int64_t infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SDID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SDID) return=" << ret, BM_DL_FUNCTION_FAILED);
    sdid_ = static_cast<uint32_t>(infoValue);

    infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SERVER_ID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SERVER_ID) return=" << ret, BM_DL_FUNCTION_FAILED);
    serverId_ = static_cast<uint32_t>(infoValue);

    infoValue = 0;
    ret = DlAclApi::RtGetDeviceInfo(static_cast<uint32_t>(userId), 0, INFO_TYPE_SUPER_POD_ID, &infoValue);
    BM_ASSERT_LOG_AND_RETURN(ret == 0, "RtGetDeviceInfo(INFO_TYPE_SUPER_POD_ID) return=" << ret, BM_DL_FUNCTION_FAILED);
    superPodId_ = static_cast<uint32_t>(infoValue);
    BM_LOG_INFO("local device info: userId=" << userId << ", phyId=" << phyId << " sdid=" << sdid_
                                             << ", server_id=" << serverId_ << ", superpod id=" << superPodId_);

    ret = UrmaTopoManager::GetInstance().Initialize(userId, options_.rankId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma initialize topo failed, deviceId=" << userId << " rankId=" << options_.rankId
                                                                     << " ret=" << ret);
        return ret;
    }
    mainboardId_ = UrmaTopoManager::GetInstance().GetMainboardId();
    ret = SelectUrmaProtocol(mainboardId_, options.protocol, protocol_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma SelectUrmaProtocol failed, deviceId="
                     << userId << " rankId=" << options_.rankId << " mainboardId=" << mainboardId_
                     << " protocolBits=" << options.protocol << " ret=" << ret);
        return ret;
    }
    BM_LOG_INFO("device_urma selected protocol=" << protocol_ << " mainboardId=" << mainboardId_
                                                 << " deviceId=" << userId << " rankId=" << options_.rankId);
    return BM_OK;
}

Result DeviceUrmaTransportManager::InitDeviceTransferFlagLocked()
{
    // Allocate a local flag buffer on device, initialise to 1, and register with Hcomm.
    // It is exported as an Hcomm flag descriptor in the TransportMemoryKey payload,
    // so remote peers can import it and use the resulting address as remote_flag_addr.
    // NOT inserted into localRegistrations_ — not exported/imported as a regular MR.
    int i = 0;
    void *flagPtr = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&flagPtr, sizeof(int64_t), static_cast<uint32_t>(ACL_MEM_MALLOC_NORMAL_ONLY));
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtMalloc for local flag buffer failed, ret: " << ret);
        return ret;
    }
    int64_t flagInit = 1;
    ret = DlAclApi::AclrtMemcpy(flagPtr, sizeof(int64_t), &flagInit, sizeof(int64_t), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma AclrtMemcpy init local flag buffer failed, ret: " << ret);
        (void)DlAclApi::AclrtFree(flagPtr);
        return ret;
    }
    const UrmaCommMem flagMem{reinterpret_cast<uint64_t>(flagPtr), sizeof(int64_t), UrmaMemoryType::DEVICE_HBM};
    HcommMemHandle flagHandle = nullptr;
    auto hcommFlagMem = ToHcommMem(flagMem);
    uint64_t memTag = reinterpret_cast<uint64_t>(flagPtr);
    devTransFlagPtr_ = flagPtr;
    devTransFlagSize_ = sizeof(int64_t);
    for (auto &localEndpoint : localEndpoint_) {
        ret = hcommApi_.RegisterMemory(localEndpoint, memTag, hcommFlagMem, flagHandle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma HcommMemReg for local flag buffer failed, ret: " << ret);
            return ret;
        }
        devTransFlagHcommHandle_.push_back(flagHandle);
        BM_LOG_INFO("device_urma local flag buffer allocated and registered, addr: "
                    << VaToStr(flagPtr) << " size: " << devTransFlagSize_ << " handle: " << flagHandle
                    << " endpoint:" << localEndpoint << " localId:" << localIds_[i++]);
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::BuildLocalEndpointDescLocked(UrmaProtocol protocol, UrmaEndpointDesc &localDesc)
{
    localDesc = {};
    int32_t localId = -1;
    UrmaTopoManager::GetInstance().GetLocalId(localId);
    localDesc.protocol = protocol;
    localDesc.devPhyId = phyDeviceId_;
    localDesc.localId = localId;
    localDesc.remoteId = -1;
    localDesc.superDevId = sdid_;
    localDesc.serverIdx = serverId_;
    localDesc.superPodIdx = superPodId_;

    if (protocol == UrmaProtocol::UBC_CTP || protocol == UrmaProtocol::UB_RTP) {
        std::array<uint8_t, COMM_ADDR_EID_LEN> eidData{};
        const auto ret = GetPeer2NetEid(ToHcommProtocol(protocol), eidData);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma get peer2net EID failed, protocol=" << protocol << " phyDeviceId=" << phyDeviceId_
                                                                          << " rankId=" << rankId_ << " ret=" << ret);
            return ret;
        }
        localDesc.type = COMM_ADDR_TYPE_EID;
        std::memcpy(localDesc.raws, eidData.data(), COMM_ADDR_EID_LEN);
        BM_LOG_INFO("device_urma local endpoint address protocol=" << protocol << " phyDeviceId=" << phyDeviceId_
                                                                   << " rankId=" << rankId_
                                                                   << " eid=" << FormatEid(eidData));
        return BM_OK;
    }
    if (protocol == UrmaProtocol::UBOE) {
        CommAddrType addrType = COMM_ADDR_TYPE_RESERVED;
        std::array<uint8_t, sizeof(localDesc.raws)> addrData{};
        const auto ret = GetDeviceUBoEIpAddr(phyDeviceId_, rankId_, addrType, addrData);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma get UBoE IP addr failed, protocol=UBOE phyDeviceId="
                         << phyDeviceId_ << " rankId=" << rankId_ << " ret=" << ret);
            return ret;
        }
        localDesc.type = addrType;
        const size_t copyLen = (addrType == COMM_ADDR_TYPE_IP_V4) ? sizeof(struct in_addr) : sizeof(struct in6_addr);
        std::memcpy(localDesc.raws, addrData.data(), copyLen);
        BM_LOG_INFO("device_urma local endpoint address protocol=UBOE phyDeviceId="
                    << phyDeviceId_ << " rankId=" << rankId_ << " ip=" << FormatIpAddress(addrType, addrData.data()));
        return BM_OK;
    }
    BM_LOG_ERROR("device_urma unexpected protocol=" << protocol << "(" << static_cast<int>(protocol)
                                                    << ") phyDeviceId=" << phyDeviceId_ << " rankId=" << rankId_);
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::BuildLocalEndpointsDescLocked(UrmaProtocol protocol,
                                                                 std::vector<UrmaEndpointDesc> &localDescs)
{
    if (protocol == UrmaProtocol::UBC_CTP) {
        std::unordered_map<int32_t, std::array<uint8_t, COMM_ADDR_EID_LEN>> eidInfos;
        const auto ret = UrmaTopoManager::GetInstance().GetPeer2PeerEids(eidInfos);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma GetPeer2PeerEids failed, protocol=UBC_CTP rankId=" << rankId_ << " ret=" << ret);
            return ret;
        }
        int32_t localId = -1;
        UrmaTopoManager::GetInstance().GetLocalId(localId);
        for (const auto &[index, eid] : eidInfos) {
            UrmaEndpointDesc localDesc{};
            localDesc.protocol = protocol;
            localDesc.devPhyId = phyDeviceId_;
            localDesc.localId = localId;
            localDesc.remoteId = index;
            localDesc.superDevId = sdid_;
            localDesc.serverIdx = serverId_;
            localDesc.superPodIdx = superPodId_;
            localDesc.type = COMM_ADDR_TYPE_IP_V6;
            localIds_.push_back(index);
            std::memcpy(localDesc.raws, eid.data(), COMM_ADDR_EID_LEN);
            BM_LOG_DEBUG("device_urma local endpoint address protocol=UBC_CTP phyDeviceId="
                         << phyDeviceId_ << " rankId=" << rankId_ << " eid=" << FormatEid(eid));
            localDescs.push_back(localDesc);
        }
        return BM_OK;
    }
    BM_LOG_ERROR("device_urma unexpected protocol=" << static_cast<int>(protocol) << " phyDeviceId=" << phyDeviceId_
                                                    << " rankId=" << rankId_);
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::CreateEndpointAndInitResourcesLocked(const UrmaEndpointDesc &localDesc)
{
    auto hcommDesc = ToHcommEndpointDesc(localDesc);
    HcommEndpointHandle endpoint = nullptr;
    auto ret = hcommApi_.CreateEndpoint(hcommDesc, endpoint);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CreateEndpoint failed, protocol=" << localDesc.protocol << " phyDeviceId="
                                                                    << phyDeviceId_ << " rankId=" << rankId_);
        return BM_MALLOC_FAILED;
    }
    localEndpoint_.push_back(endpoint);
    localEndpointDesc_.push_back(localDesc);
    localIds_.push_back(localDesc.localId);

    ret = InitDeviceTransferFlagLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CreateEndpointAndInitResourcesLocked InitDeviceTransferFlagLocked failed, rankId="
                     << rankId_ << " protocol=" << localDesc.protocol << " ret=" << ret);
        RollbackOpenDeviceLocked();
        return ret;
    }
    ret = EnsureDeviceKernelLoadedLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CreateEndpointAndInitResourcesLocked EnsureDeviceKernelLoadedLocked failed, rankId="
                     << rankId_ << " protocol=" << localDesc.protocol << " ret=" << ret);
        RollbackOpenDeviceLocked();
        return ret;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::CreateEndpointsAndInitResourcesLocked(const std::vector<UrmaEndpointDesc> &descs)
{
    for (auto &localDesc : descs) {
        auto hcommDesc = ToHcommEndpointDesc(localDesc);
        HcommEndpointHandle endpoint = nullptr;
        auto ret = hcommApi_.CreateEndpoint(hcommDesc, endpoint);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma CreateEndpoint failed, protocol=" << static_cast<int>(localDesc.protocol)
                                                                        << " phyDeviceId=" << phyDeviceId_
                                                                        << " rankId=" << rankId_);
            return BM_MALLOC_FAILED;
        }
        localEndpoint_.push_back(endpoint);
        localEndpointDesc_.push_back(localDesc);
    }

    auto ret = InitDeviceTransferFlagLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CreateEndpointsAndInitResourcesLocked InitDeviceTransferFlagLocked failed, rankId="
                     << rankId_ << " endpointCount=" << descs.size() << " ret=" << ret);
        RollbackOpenDeviceLocked();
        return ret;
    }
    ret = EnsureDeviceKernelLoadedLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CreateEndpointsAndInitResourcesLocked EnsureDeviceKernelLoadedLocked failed, rankId="
                     << rankId_ << " endpointCount=" << descs.size() << " ret=" << ret);
        RollbackOpenDeviceLocked();
        return ret;
    }
    return BM_OK;
}

void DeviceUrmaTransportManager::RollbackOpenDeviceLocked()
{
    if (!devTransFlagHcommHandle_.empty()) {
        for (size_t i = 0; i < devTransFlagHcommHandle_.size(); i++) {
            auto ret = hcommApi_.UnregisterMemory(localEndpoint_[i], devTransFlagHcommHandle_[i]);
            if (ret != BM_OK) {
                BM_LOG_ERROR("device_urma RollbackOpenDevice HcommMemUnreg devTransFlag failed, ret: " << ret);
            }
            devTransFlagHcommHandle_[i] = nullptr;
        }
    }
    devTransFlagHcommHandle_.clear();
    if (devTransFlagPtr_ != nullptr) {
        (void)DlAclApi::AclrtFree(devTransFlagPtr_);
        devTransFlagPtr_ = nullptr;
    }
    if (!localEndpoint_.empty()) {
        for (auto &localEndpoint : localEndpoint_) {
            if (localEndpoint != nullptr) {
                (void)hcommApi_.DestroyEndpoint(localEndpoint);
                localEndpoint = nullptr;
            }
        }
    }
    localEndpoint_.clear();
    if (!localEndpointDesc_.empty()) {
        for (auto &localEndpointDesc : localEndpointDesc_) {
            localEndpointDesc = UrmaEndpointDesc{};
        }
    }
    localEndpointDesc_.clear();
    localIds_.clear();
}

Result DeviceUrmaTransportManager::OpenEndpointResourcesLocked(const TransportOptions &options)
{
    if (protocol_ == UrmaProtocol::RESERVED) {
        BM_LOG_ERROR("device_urma OpenDevice unsupported protocol bits: " << options.protocol << ", rankId=" << rankId_
                                                                          << ", mainboardId=" << mainboardId_);
        return BM_INVALID_PARAM;
    }
    BM_LOG_INFO("device_urma OpenDevice protocol=" << protocol_ << ", rankId=" << rankId_ << ", phyDeviceId="
                                                   << phyDeviceId_ << ", mainboardId=" << mainboardId_);
    static const bool fullMesh = MfEnvUtil::GetOptionalUintOrDefault(env::TEST_MF_TRANSPORT_FULL_MESH, 0u) != 0u;
    if (fullMesh || IsFM16Product()) {
        std::vector<UrmaEndpointDesc> localDescs;
        auto ret = BuildLocalEndpointsDescLocked(protocol_, localDescs);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma build local endpoint descs failed, protocol=" << protocol_ << " rankId="
                                                                                    << rankId_ << " ret=" << ret);
            return ret;
        }
        ret = CreateEndpointsAndInitResourcesLocked(localDescs);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma create endpoints and init resources failed, protocol="
                         << protocol_ << " rankId=" << rankId_ << " endpointCount=" << localDescs.size()
                         << " ret=" << ret);
            (void)RollbackOpenDeviceLocked();
            return ret;
        }
    } else {
        UrmaEndpointDesc localDesc{};
        auto ret = BuildLocalEndpointDescLocked(protocol_, localDesc);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma build local endpoint desc failed, protocol=" << protocol_ << " rankId=" << rankId_
                                                                                   << " ret=" << ret);
            return ret;
        }
        ret = CreateEndpointAndInitResourcesLocked(localDesc);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma create endpoint and init resources failed, protocol="
                         << protocol_ << " rankId=" << rankId_ << " ret=" << ret);
            (void)RollbackOpenDeviceLocked();
            return ret;
        }
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::OpenDevice(const TransportOptions &options)
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    if (opened_) {
        return BM_OK;
    }
    if (options.rankCount == 0 || options.rankId >= options.rankCount) {
        BM_LOG_ERROR("device_urma OpenDevice: invalid rankCount or rankId");
        return BM_INVALID_PARAM;
    }
    if (DlAclApi::GetAscendSocType() != AscendSocType::ASCEND_950) {
        BM_LOG_ERROR("device_urma is only supported on Ascend950 soc, rank: " << options.rankId);
        return BM_NOT_SUPPORTED;
    }

    auto ret = InitLocalDeviceInfoLocked(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitLocalDeviceInfoLocked failed, rank=" << options.rankId << " ret: " << ret);
        return ret;
    }
    ret = OpenEndpointResourcesLocked(options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma OpenEndpointResourcesLocked failed, rank=" << rankId_ << " protocol=" << protocol_
                                                                             << " ret=" << ret);
        return ret;
    }
    ret = InitNotifyPoolLocked();
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitNotifyPoolLocked failed, rank=" << rankId_ << " ret=" << ret);
        RollbackOpenDeviceLocked();
        return ret;
    }
    opened_ = true;
    BM_LOG_INFO("device_urma OpenDevice success, rank: " << rankId_ << " rankCount: " << rankCount_
                                                         << " devPhyId: " << phyDeviceId_);
    return BM_OK;
}

Result DeviceUrmaTransportManager::EnsureDeviceKernelLoadedLocked()
{
    if (deviceKernelLoaded_) {
        return BM_OK;
    }

    auto ret = LoadDeviceKernelAndGetHandles(HYBM_DEVICE_FUNC_TRANSFER, deviceKernelHandle_, deviceFuncHandles_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma load device kernel failed, rankId=" << rankId_ << " ret=" << ret);
        return ret;
    }
    if (deviceFuncHandles_.batchTransfer == nullptr) {
        BM_LOG_ERROR(
            "device_urma invalid device kernel function handle, transfer: " << deviceFuncHandles_.batchTransfer);
        return BM_DL_FUNCTION_FAILED;
    }
    deviceKernelLoaded_ = true;
    return BM_OK;
}

DeviceUrmaTransportManager::CompletionContext *DeviceUrmaTransportManager::LookupOrCreateContextLocked()
{
    auto &ctx = GetTlsContext();
    if (ctx.initialized) {
        return &ctx;
    }

    auto ret = InitThreadContextLocked(ctx);
    if (ret != BM_OK) {
        return nullptr;
    }
    return &ctx;
}

Result DeviceUrmaTransportManager::InitThreadContextLocked(CompletionContext &ctx)
{
    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("device_urma InitThreadContextLocked GetThreadAclStream failed");
        return BM_ERROR;
    }
    ctx.stream = stream;
    ctx.initialized = true;
    return BM_OK;
}

Result DeviceUrmaTransportManager::InitNotifyResource(NotifyResource &resource)
{
    void *notify = nullptr;
    auto ret = DlAclApi::AclrtCreateNotify(&notify, ACL_NOTIFY_FLAG_DEVICE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitNotifyResource AclrtCreateNotify failed, ret: " << ret);
        return ret;
    }
    resource.notify = notify;

    uint32_t notifyId = 0;
    ret = DlAclApi::AclrtGetNotifyId(notify, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitNotifyResource AclrtGetNotifyId failed, ret: " << ret);
        CleanupNotifyResource(resource);
        return ret;
    }
    resource.notifyId = notifyId;

    uint64_t devAddr = 0;
    uint32_t devLen = 0;
    rtDevResInfo resInfo{};
    resInfo.dieId = 0U;
    resInfo.procType = RT_PROCESS_HCCP;
    resInfo.resType = RT_RES_TYPE_STARS_NOTIFY_RECORD;
    resInfo.resId = notifyId;
    resInfo.flag = 0U;
    rtDevResAddrInfo addrInfo{};
    addrInfo.resAddress = &devAddr;
    addrInfo.len = &devLen;
    ret = DlRtApi::RtGetDevResAddress(&resInfo, &addrInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma InitNotifyResource RtGetDevResAddress failed, notifyId: " << notifyId
                                                                                            << " ret: " << ret);
        CleanupNotifyResource(resource);
        return ret;
    }
    resource.notifyAddr = devAddr;
    resource.notifyLen = devLen;

    for (auto &localEndpoint : localEndpoint_) {
        const UrmaCommMem notifyMem{devAddr, devLen, UrmaMemoryType::DEVICE_HBM};
        HcommMemHandle notifyHandle = nullptr;
        auto hcommNotifyMem = ToHcommMem(notifyMem);
        ret = hcommApi_.RegisterMemory(localEndpoint, resource.notifyAddr, hcommNotifyMem, notifyHandle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma InitNotifyResource HcommMemReg failed, notifyId: " << notifyId
                                                                                         << " ret: " << ret);
            CleanupNotifyResource(resource);
            return ret;
        }
        resource.notifyHcommHandle.push_back(notifyHandle);
        BM_LOG_INFO("device_urma InitNotifyResource success, notifyId: " << notifyId << " addr: " << VaToStr(devAddr)
                                                                         << " len: " << devLen
                                                                         << " endpoint:" << localEndpoint);
    }

    return BM_OK;
}

void DeviceUrmaTransportManager::CleanupNotifyResource(NotifyResource &resource)
{
    for (size_t i = 0; i < resource.notifyHcommHandle.size(); i++) {
        if (resource.notifyHcommHandle[i] != nullptr) {
            auto ret = hcommApi_.UnregisterMemory(localEndpoint_[i], resource.notifyHcommHandle[i]);
            if (ret != BM_OK) {
                BM_LOG_WARN("device_urma CleanupNotifyResource HcommMemUnreg failed, notifyId: " << resource.notifyId
                                                                                                 << " ret: " << ret);
            }
            resource.notifyHcommHandle[i] = nullptr;
        }
    }
    resource.notifyHcommHandle.clear();
    if (resource.notify != nullptr) {
        auto ret = DlAclApi::AclrtDestroyNotify(resource.notify);
        if (ret != BM_OK) {
            BM_LOG_WARN("device_urma CleanupNotifyResource AclrtDestroyNotify failed, notifyId: " << resource.notifyId
                                                                                                  << " ret: " << ret);
        }
        resource.notify = nullptr;
    }
    resource.notifyId = 0;
    resource.notifyAddr = 0;
    resource.notifyLen = 0;
}

Result DeviceUrmaTransportManager::InitNotifyPoolLocked()
{
    notifyFreeHead_.store(PackNotifyFreeHead(HYBM_NOTIFY_POOL_INVALID_INDEX, 0U), std::memory_order_release);
    notifyPoolSize_.store(0U, std::memory_order_release);
    notifyPoolSlots_.reset(new (std::nothrow) NotifyResource *[HYBM_NOTIFY_POOL_MAX_SIZE] {});
    if (notifyPoolSlots_ == nullptr) {
        BM_LOG_ERROR(
            "device_urma InitNotifyPoolLocked allocate slot table failed, maxPoolSize: " << HYBM_NOTIFY_POOL_MAX_SIZE);
        return BM_MALLOC_FAILED;
    }
    constexpr uint32_t initialPoolSize = 1U;
    for (uint32_t index = 0; index < initialPoolSize; ++index) {
        auto *resource = new (std::nothrow) NotifyResource();
        if (resource == nullptr) {
            BM_LOG_ERROR("device_urma InitNotifyPoolLocked allocate resource failed, index: " << index);
            CleanupNotifyPoolLocked();
            return BM_MALLOC_FAILED;
        }
        auto ret = InitNotifyResource(*resource);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma InitNotifyPoolLocked init resource failed, index: " << index << " ret: " << ret);
            delete resource;
            CleanupNotifyPoolLocked();
            return ret;
        }
        resource->poolIndex = index;
        const uint32_t next = index + 1U < initialPoolSize ? index + 1U : HYBM_NOTIFY_POOL_INVALID_INDEX;
        resource->nextFree.store(next, std::memory_order_relaxed);
        notifyPoolSlots_[index] = resource;
        notifyPoolSize_.store(index + 1U, std::memory_order_release);
    }
    notifyFreeHead_.store(PackNotifyFreeHead(0U, 0U), std::memory_order_release);
    return BM_OK;
}

Result DeviceUrmaTransportManager::GrowNotifyPool()
{
    std::lock_guard<std::mutex> guard(notifyPoolGrowMutex_);
    if (NotifyFreeHeadIndex(notifyFreeHead_.load(std::memory_order_acquire)) != HYBM_NOTIFY_POOL_INVALID_INDEX) {
        return BM_OK;
    }
    const uint32_t index = notifyPoolSize_.load(std::memory_order_acquire);
    if (index >= HYBM_NOTIFY_POOL_MAX_SIZE) {
        BM_LOG_ERROR("device_urma GrowNotifyPool reaches limit, maxPoolSize: " << HYBM_NOTIFY_POOL_MAX_SIZE);
        return BM_ERROR;
    }
    auto *resource = new (std::nothrow) NotifyResource();
    if (resource == nullptr) {
        BM_LOG_ERROR("device_urma GrowNotifyPool allocate resource failed, index: " << index);
        return BM_MALLOC_FAILED;
    }
    auto ret = InitNotifyResource(*resource);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma GrowNotifyPool init resource failed, index: " << index << " ret: " << ret);
        delete resource;
        return ret;
    }
    resource->poolIndex = index;
    notifyPoolSlots_[index] = resource;
    notifyPoolSize_.store(index + 1U, std::memory_order_release);
    ReleaseNotifyResource(*resource);
    return BM_OK;
}

void DeviceUrmaTransportManager::CleanupNotifyPoolLocked()
{
    notifyFreeHead_.store(PackNotifyFreeHead(HYBM_NOTIFY_POOL_INVALID_INDEX, 0U), std::memory_order_release);
    const uint32_t poolSize = notifyPoolSize_.load(std::memory_order_acquire);
    for (uint32_t index = 0; index < poolSize; ++index) {
        CleanupNotifyResource(*notifyPoolSlots_[index]);
        delete notifyPoolSlots_[index];
        notifyPoolSlots_[index] = nullptr;
    }
    notifyPoolSlots_.reset();
    notifyPoolSize_.store(0U, std::memory_order_release);
}

Result DeviceUrmaTransportManager::AcquireNotifyResource(NotifyResource *&resource)
{
    TP_TRACE_BEGIN(TP_HYBM_URMA_ACQUIRE_NOTIFY_RESOURCE);
    for (;;) {
        uint64_t head = notifyFreeHead_.load(std::memory_order_acquire);
        while (NotifyFreeHeadIndex(head) != HYBM_NOTIFY_POOL_INVALID_INDEX) {
            const uint32_t index = NotifyFreeHeadIndex(head);
            auto *candidate = notifyPoolSlots_[index];
            const uint32_t next = candidate->nextFree.load(std::memory_order_relaxed);
            const uint64_t desired = PackNotifyFreeHead(next, NotifyFreeHeadVersion(head) + 1U);
            if (notifyFreeHead_.compare_exchange_weak(head, desired, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                resource = candidate;
                TP_TRACE_END(TP_HYBM_URMA_ACQUIRE_NOTIFY_RESOURCE, BM_OK);
                return BM_OK;
            }
        }
        auto ret = GrowNotifyPool();
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma AcquireNotifyResource grow pool failed, poolSize: "
                         << notifyPoolSize_.load(std::memory_order_acquire) << " ret: " << ret);
            TP_TRACE_END(TP_HYBM_URMA_ACQUIRE_NOTIFY_RESOURCE, ret);
            return ret;
        }
    }
}

void DeviceUrmaTransportManager::ReleaseNotifyResource(NotifyResource &resource)
{
    const uint32_t index = resource.poolIndex;
    const uint32_t poolSize = notifyPoolSize_.load(std::memory_order_acquire);
    if (notifyPoolSlots_ == nullptr || index >= poolSize || notifyPoolSlots_[index] != &resource) {
        BM_LOG_ERROR("device_urma ReleaseNotifyResource invalid resource, index: " << index
                                                                                   << " poolSize: " << poolSize);
        return;
    }
    uint64_t head = notifyFreeHead_.load(std::memory_order_acquire);
    uint64_t desired = 0;
    do {
        resource.nextFree.store(NotifyFreeHeadIndex(head), std::memory_order_relaxed);
        desired = PackNotifyFreeHead(index, NotifyFreeHeadVersion(head) + 1U);
    } while (
        !notifyFreeHead_.compare_exchange_weak(head, desired, std::memory_order_acq_rel, std::memory_order_acquire));
}

void DeviceUrmaTransportManager::CloseDeviceCleanupResourcesLocked()
{
    for (auto &rankItem : remoteRanks_) {
        const uint32_t peerRank = rankItem.first;
        auto &state = rankItem.second;
        (void)CleanupPeerRankState(state, peerRank);
    }

    CleanupNotifyPoolLocked();

    (void)CleanupLocalRegistrationsLocked();

    if (!devTransFlagHcommHandle_.empty()) {
        for (size_t i = 0; i < devTransFlagHcommHandle_.size(); i++) {
            (void)hcommApi_.UnregisterMemory(localEndpoint_[i], devTransFlagHcommHandle_[i]);
            devTransFlagHcommHandle_[i] = nullptr;
        }
    }
    devTransFlagHcommHandle_.clear();

    remoteRanks_.clear();
    {
        std::unique_lock<std::shared_mutex> faultLock(faultedRanksMutex_);
        faultedRanks_.clear();
    }

    if (!localEndpoint_.empty()) {
        for (auto &localEndpoint : localEndpoint_) {
            if (localEndpoint != nullptr) {
                (void)hcommApi_.DestroyEndpoint(localEndpoint);
                localEndpoint = nullptr;
            }
        }
    }
    localEndpoint_.clear();

    if (!localEndpointDesc_.empty()) {
        for (auto &localEndpointDesc : localEndpointDesc_) {
            localEndpointDesc = UrmaEndpointDesc{};
        }
    }
    localEndpointDesc_.clear();

    localIds_.clear();

    if (devTransFlagPtr_ != nullptr) {
        auto ret = DlAclApi::AclrtFree(devTransFlagPtr_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma CloseDeviceCleanupResources AclrtFree devTransFlag failed, ret: " << ret);
        }
        devTransFlagPtr_ = nullptr;
    }
    deviceKernelLoaded_ = false;
}

Result DeviceUrmaTransportManager::ReleasePendingTransfersLocked(std::vector<PendingTransfer> &pendingTransfers)
{
    pendingTransfers.clear();
    return BM_OK;
}

void DeviceUrmaTransportManager::ExtractRankPending(std::vector<PendingTransfer> &src, uint32_t rankId,
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

Result DeviceUrmaTransportManager::CloseDevice()
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    if (!opened_) {
        return BM_OK;
    }
    CloseDeviceCleanupResourcesLocked();
    opened_ = false;
    BM_LOG_INFO("device_urma CloseDevice success");
    return BM_OK;
}

Result DeviceUrmaTransportManager::DestroyRankChannelsAndThread(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;
    if (state.channel != 0) {
        auto hcommChan = state.channel;
        BM_LOG_INFO("device_urma calling HcommChannelDestroy, peerRank: " << peerRank << " channel: " << state.channel
                                                                          << " thread: " << state.thread);
        const auto ret = DlHcommApi::HcommChannelDestroy(&hcommChan, 1);
        if (ret != 0) {
            BM_LOG_ERROR("device_urma HcommChannelDestroy failed, peerRank: "
                         << peerRank << " channel: " << state.channel << " thread: " << state.thread
                         << " ret: " << ret);
            localResult = BM_DL_FUNCTION_FAILED;
        }
        state.channel = 0;
        state.channelDesc = {};
    }
    if (state.thread != 0) {
        auto hcommThread = state.thread;
        BM_LOG_INFO("device_urma HcommThreadFree, thread: " << state.thread);
        const auto ret = DlHcommApi::HcommThreadFree(&hcommThread, 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma HcommThreadFree failed, peerRank: " << peerRank << " thread: " << state.thread
                                                                          << " ret: " << ret);
            if (localResult == BM_OK) {
                localResult = ret;
            }
        }
        state.thread = 0;
    }
    return localResult;
}

Result DeviceUrmaTransportManager::UnimportPeerImportsAndFlag(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;
    auto importIt = state.imports.begin();
    while (importIt != state.imports.end()) {
        if (importIt->descBytes.empty()) {
            ++importIt;
            continue;
        }
        BM_LOG_INFO("device_urma HcommMemUnimport, peer: " << peerRank
                                                           << ", descBytes.size: " << importIt->descBytes.size());

        auto it = std::find(localIds_.begin(), localIds_.end(), state.remoteEndpointDesc.localId);
        auto index = 0;
        if (it != localIds_.end()) {
            index = std::distance(localIds_.begin(), it);
        }
        const auto ret = hcommApi_.UnimportMemory(localEndpoint_[index], importIt->descBytes.data(),
                                                  static_cast<uint32_t>(importIt->descBytes.size()));
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma UnimportPeerImportsAndFlag HcommMemUnimport failed, "
                         << "peerRank: " << peerRank << " descBytes.size: " << importIt->descBytes.size()
                         << " ret: " << ret);
            if (localResult == BM_OK) {
                localResult = ret;
            }
            // Keep import entry on failure — still owner-enumerable for retry
            ++importIt;
        } else {
            importIt->descBytes.clear();
            importIt = state.imports.erase(importIt);
        }
    }
    if (!state.remoteFlagDescBytes.empty()) {
        BM_LOG_INFO("device_urma HcommMemUnimport remoteFlagDescBytes, "
                    << "peerRank: " << peerRank << " remoteFlagDescBytes.size: " << state.remoteFlagDescBytes.size());
        auto it = std::find(localIds_.begin(), localIds_.end(), state.remoteEndpointDesc.localId);
        auto index = 0;
        if (it != localIds_.end()) {
            index = std::distance(localIds_.begin(), it);
        }
        const auto ret = DlHcommApi::HcommMemUnimport(localEndpoint_[index], state.remoteFlagDescBytes.data(),
                                                      static_cast<uint32_t>(state.remoteFlagDescBytes.size()));
        if (ret != 0) {
            BM_LOG_ERROR("device_urma UnimportPeerImportsAndFlag HcommMemUnimport for flag desc failed, "
                         << "peerRank: " << peerRank << " ret: " << ret);
            if (localResult == BM_OK) {
                localResult = BM_DL_FUNCTION_FAILED;
            }
        } else {
            state.remoteFlagDescBytes.clear();
            state.remoteFlagAddr = 0;
            state.remoteFlagSize = 0;
        }
    } else {
        state.remoteFlagAddr = 0;
        state.remoteFlagSize = 0;
    }
    return localResult;
}

Result DeviceUrmaTransportManager::CleanupPeerRankState(RemoteRankState &state, uint32_t peerRank)
{
    Result localResult = BM_OK;

    const auto retChan = DestroyRankChannelsAndThread(state, peerRank);
    if (retChan != BM_OK && localResult == BM_OK) {
        localResult = retChan;
    }
    const auto retImports = UnimportPeerImportsAndFlag(state, peerRank);
    if (retImports != BM_OK && localResult == BM_OK) {
        localResult = retImports;
    }
    return localResult;
}

Result DeviceUrmaTransportManager::CleanupLocalRegistrationsLocked()
{
    Result localResult = BM_OK;
    for (auto &item : localRegistrations_) {
        for (size_t i = 0; i < item.second.handle.size(); i++) {
            if (item.second.handle[i] != nullptr && i < localEndpoint_.size() && localEndpoint_[i] != nullptr) {
                auto ret = hcommApi_.UnregisterMemory(localEndpoint_[i], item.second.handle[i]);
                if (ret != BM_OK) {
                    BM_LOG_ERROR("device_urma CleanupLocalRegistrationsLocked HcommMemUnreg global handle failed, "
                                 << "addr: " << VaToStr(item.first) << " ret: " << ret << " index :" << i);
                    if (localResult == BM_OK)
                        localResult = ret;
                }
                item.second.handle[i] = nullptr;
            }
        }
        item.second.handle.clear();
    }
    localRegistrations_.clear();
    return localResult;
}

Result DeviceUrmaTransportManager::FindLocalRegistrationLocked(uint64_t addr, uint64_t size,
                                                               LocalRegistration *registration) const
{
    auto pos = localRegistrations_.upper_bound(addr);
    if (pos == localRegistrations_.begin()) {
        BM_LOG_ERROR("device_urma FindLocalRegistrationLocked: no registration found, addr: " << VaToStr(addr)
                                                                                              << " size: " << size);
        return BM_INVALID_PARAM;
    }
    --pos;

    const bool containsRange = ContainsAddressRange(pos->second.mr.addr, pos->second.mr.size, addr, size);
    if (!containsRange) {
        BM_LOG_ERROR("device_urma FindLocalRegistrationLocked: registration does not contain range, addr: "
                     << VaToStr(addr) << " size: " << size << " registeredAddr: " << VaToStr(pos->second.mr.addr)
                     << " registeredSize: " << pos->second.mr.size);
        return BM_INVALID_PARAM;
    }
    if (registration != nullptr) {
        *registration = pos->second;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::CorrectLocalRegAddressLocked(uint64_t addr, uint64_t size,
                                                                uint64_t &correctedAddr) const
{
    correctedAddr = addr;
    LocalRegistration registration{};
    auto ret = FindLocalRegistrationLocked(addr, size, &registration);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma CorrectLocalRegAddressLocked: local addr not registered, rankId: "
                     << rankId_ << " addr: " << VaToStr(addr) << " size: " << size << " ret: " << ret);
        return ret;
    }
    const bool isDram = (registration.mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0;
    if (!isDram || registration.deviceVa == 0) {
        return BM_OK;
    }
    const uint64_t offset = addr - registration.mr.addr;
    const uint64_t corrected = registration.deviceVa + offset;
    if (corrected < registration.deviceVa) {
        BM_LOG_ERROR("device_urma CorrectLocalRegAddressLocked: addr overflow, rankId: "
                     << rankId_ << " deviceVa: " << VaToStr(registration.deviceVa) << " offset: " << offset);
        return BM_INVALID_PARAM;
    }
    correctedAddr = corrected;
    return BM_OK;
}

Result DeviceUrmaTransportManager::FindRemoteRegistrationLocked(uint32_t rankId, uint64_t addr, uint64_t size,
                                                                const RemoteRegistration **registration) const
{
    const auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        BM_LOG_ERROR("device_urma FindRemoteRegistrationLocked rank not found, rankId=" << rankId << " remoteRanksSize="
                                                                                        << remoteRanks_.size());
        return BM_NOT_CONNECTED;
    }
    for (const auto &remote : rankIt->second.imports) {
        const bool containsRange = ContainsAddressRange(remote.addr, remote.size, addr, size);
        if (containsRange) {
            if (registration != nullptr) {
                *registration = &remote;
            }
            return BM_OK;
        }
    }
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    BM_VALIDATE_RETURN(mr.addr != 0 && mr.size != 0, "device_urma RegisterMemoryRegion: mr.addr or mr.size is 0",
                       BM_INVALID_PARAM);
    if (!IsSupportedMemoryFlags(mr.flags)) {
        BM_LOG_ERROR("device_urma RegisterMemoryRegion: unsupported memory flags: " << mr.flags);
        return BM_INVALID_PARAM;
    }
    UrmaCommMem mem = ToUrmaMem(mr);
    if (!IsValidMem(mem)) {
        BM_LOG_ERROR("device_urma RegisterMemoryRegion: invalid memory");
        return BM_INVALID_PARAM;
    }

    // localEndpoint_ must be created by OpenDevice before any memory registration
    if (localEndpoint_.empty()) {
        BM_LOG_ERROR("device_urma localEndpoint_ is null, cannot register memory");
        return BM_NOT_INITIALIZED;
    }

    auto item = localRegistrations_.find(mr.addr);
    if (item != localRegistrations_.end()) {
        if (item->second.mr.size != mr.size) {
            BM_LOG_ERROR("device_urma register memory conflict, addr: " << std::hex << mr.addr);
            return BM_ERROR;
        }
        item->second.refCount++;
        return BM_OK;
    }

    LocalRegistration registration{};
    registration.mr = mr;
    registration.memTag = mr.addr;
    registration.refCount = 1;

    if ((mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0) {
        const uint64_t dva = HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA);
        if (dva != 0) {
            registration.deviceVa = dva;
            mem.addr = dva;
        } else {
            BM_LOG_WARN("device_urma RegisterMemoryRegion: DRAM addr " << VaToStr(mr.addr)
                                                                       << " has no DVA mapping, using HVA");
        }
    }

    for (size_t i = 0; i < localEndpoint_.size(); i++) {
        HcommMemHandle hcommHandle = nullptr;
        auto hcommRegMem = ToHcommMem(mem);
        auto ret = hcommApi_.RegisterMemory(localEndpoint_[i], registration.memTag, hcommRegMem, hcommHandle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma HcommMemReg failed, addr: " << VaToStr(mr.addr) << " size: " << mr.size
                                                                  << " ret: " << ret);
            for (size_t registered = 0; registered < registration.handle.size(); registered++) {
                (void)hcommApi_.UnregisterMemory(localEndpoint_[registered], registration.handle[registered]);
            }
            return ret;
        }
        registration.handle.push_back(hcommHandle);
        BM_LOG_INFO("device_urma registerMem success, addr: " << VaToStr(mem.addr) << " len: " << mem.size
                                                              << " endpoint:" << localEndpoint_[i]
                                                              << " localId:" << localIds_[i]);
    }

    try {
        localRegistrations_.emplace(mr.addr, registration);
    } catch (...) {
        for (size_t i = 0; i < registration.handle.size(); i++) {
            (void)hcommApi_.UnregisterMemory(localEndpoint_[i], registration.handle[i]);
            BM_LOG_ERROR("device_urma failed to cache local registration, addr: "
                         << VaToStr(mr.addr) << " size: " << mr.size << " handle: " << registration.handle[i]);
        }
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    auto item = localRegistrations_.find(addr);
    if (item == localRegistrations_.end()) {
        BM_LOG_WARN("device_urma unregister skipped for unknown addr: " << std::hex << addr);
        return BM_OK;
    }
    if (item->second.refCount > 1) {
        item->second.refCount--;
        return BM_OK;
    }

    Result finalRet = BM_OK;
    for (size_t i = 0; i < item->second.handle.size(); i++) {
        auto retUnreg = hcommApi_.UnregisterMemory(localEndpoint_[i], item->second.handle[i]);
        if (retUnreg != BM_OK) {
            BM_LOG_ERROR("device_urma HcommMemUnreg failed for global handle, addr: " << std::hex << addr
                                                                                      << " ret: " << retUnreg);
            if (finalRet == BM_OK) {
                finalRet = retUnreg;
            }
        }
        item->second.handle[i] = nullptr;
    }
    item->second.handle.clear();
    localRegistrations_.erase(item);
    return finalRet;
}

bool DeviceUrmaTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return FindLocalRegistrationLocked(addr, size, nullptr) == BM_OK;
}

Result DeviceUrmaTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    LocalRegistration registration{};
    auto ret = FindLocalRegistrationLocked(addr, 1, &registration);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma QueryMemoryKey addr is not registered: " << std::hex << addr);
        return ret;
    }

    if (localEndpoint_.empty() || registration.handle.empty()) {
        BM_LOG_ERROR("device_urma QueryMemoryKey addr has no HCOMM handle: " << std::hex << addr);
        return BM_ERROR;
    }

    // Guard: devTransFlagHcommHandle_ must be valid for flag export
    if (devTransFlagHcommHandle_.empty()) {
        BM_LOG_ERROR("device_urma QueryMemoryKey devTransFlagHcommHandle_ is null, cannot export flag");
        return BM_ERROR;
    }

    if (localEndpoint_.size() > 1) {
        key.ResizeWords(DEVICE_URMA_MAX_EXPORT_KEY_LENGTH + DEVICE_URMA_EXPORT_KEY_INFO);
    } else {
        key.ResizeWords(DEVICE_URMA_MAX_EXPORT_KEY_LENGTH);
    }
    constexpr uint32_t exportHeaderSize = sizeof(UrmaExportDesc);
    uint8_t *payload = reinterpret_cast<uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
    uint32_t totalPayloadLen = 0;

    // Traverse localEndpoint_
    for (size_t i = 0; i < localEndpoint_.size(); ++i) {
        // Export HCOMM memory descriptor
        const uint8_t *memDesc = nullptr;
        uint32_t memDescLen = 0;
        ret = hcommApi_.ExportMemory(localEndpoint_[i], registration.handle[i], memDesc, memDescLen);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma QueryMemoryKey HcommMemExport failed for endpoint["
                         << i << "], addr: " << std::hex << addr << " ret: " << ret);
            return ret;
        }

        // Export flag descriptor
        void *flagDescRaw = nullptr;
        uint32_t flagDescLenRaw = 0;
        if (i < devTransFlagHcommHandle_.size()) {
            ret = DlHcommApi::HcommMemExport(localEndpoint_[i], devTransFlagHcommHandle_[i], &flagDescRaw,
                                             &flagDescLenRaw);
            if (ret != BM_OK) {
                BM_LOG_ERROR("device_urma QueryMemoryKey flag HcommMemExport failed for endpoint["
                             << i << "], ret: " << ret);
                return BM_DL_FUNCTION_FAILED;
            }
            if (flagDescRaw == nullptr || flagDescLenRaw == 0) {
                BM_LOG_ERROR("device_urma QueryMemoryKey flag export returned null or zero length for endpoint["
                             << i << "]");
                return BM_ERROR;
            }
        }
        const uint8_t *flagDesc = static_cast<const uint8_t *>(flagDescRaw);
        const uint32_t flagDescLen = flagDescLenRaw;

        // Capacity check
        uint32_t thisEntrySize = exportHeaderSize + memDescLen + flagDescLen;
        if (totalPayloadLen + thisEntrySize > DEVICE_URMA_EXPORT_KEY_DATA_BYTES) {
            BM_LOG_ERROR("device_urma QueryMemoryKey export payload exceeds key capacity after endpoint["
                         << i << "], totalPayloadLen: " << totalPayloadLen << " thisEntrySize: " << thisEntrySize
                         << " capacity: " << DEVICE_URMA_EXPORT_KEY_DATA_BYTES);
            return BM_ERROR;
        }

        // Build the UrmaExportDesc and Serialize
        UrmaExportDesc exportDesc{};
        exportDesc.headerSize = exportHeaderSize;
        exportDesc.memoryType = ToUrmaMemoryType(registration.mr.flags);
        exportDesc.memTag = registration.memTag;
        exportDesc.addr = registration.deviceVa != 0 ? registration.deviceVa : registration.mr.addr;
        exportDesc.size = registration.mr.size;
        exportDesc.hcommDescLen = memDescLen;
        exportDesc.devTransFlagDescLen = flagDescLen;

        uint8_t *entryPayload = payload + totalPayloadLen;
        std::memcpy(entryPayload, &exportDesc, exportHeaderSize);
        std::memcpy(entryPayload + exportHeaderSize, memDesc, memDescLen);
        if (flagDescLen > 0) {
            std::memcpy(entryPayload + exportHeaderSize + memDescLen, flagDesc, flagDescLen);
        }

        totalPayloadLen += thisEntrySize;
    }

    // Add key headers
    key.keys[0] = URMA_EXPORT_DESC_MAGIC;
    uint64_t gva = HybmVaManager::GetInstance().TransformVa(registration.mr.addr, HVM_HVA, HVM_GVA);
    key.keys[1] = (gva != 0) ? gva : registration.mr.addr;

    return BM_OK;
}

void DeviceUrmaTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    if (addr != nullptr) {
        if (key.keys.size() <= DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS) {
            BM_LOG_ERROR("device_urma UpdateMemoryKey key too small, words: " << key.keys.size());
            return;
        }
        key.keys[1] = reinterpret_cast<uint64_t>(addr);
    }
}

Result DeviceUrmaTransportManager::ImportRemoteMemKeysLocked(uint32_t peerRank, RemoteRankState &state,
                                                             const std::vector<TransportMemoryKey> &memKeys)
{
    if (memKeys.empty()) {
        return BM_OK;
    }
    if (localEndpoint_.empty()) {
        BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked localEndpoint_ is null, peer: " << peerRank);
        return BM_NOT_INITIALIZED;
    }

    // Collect newly imported registrations in a local vector;
    // commit to state.imports only after all keys succeed.
    std::vector<RemoteRegistration> newImports;
    bool flagImportedInThisCall = false;
    auto rollbackNewImports = [&]() {
        auto index = 0;
        if (flagImportedInThisCall && !state.remoteFlagDescBytes.empty()) {
            auto it = std::find(localIds_.begin(), localIds_.end(), state.remoteEndpointDesc.localId);
            if (it != localIds_.end()) {
                index = std::distance(localIds_.begin(), it);
            }
            (void)DlHcommApi::HcommMemUnimport(localEndpoint_[index], state.remoteFlagDescBytes.data(),
                                               static_cast<uint32_t>(state.remoteFlagDescBytes.size()));
            state.remoteFlagDescBytes.clear();
            state.remoteFlagAddr = 0;
            state.remoteFlagSize = 0;
            flagImportedInThisCall = false;
        }
        for (const auto &ni : newImports) {
            if (!ni.descBytes.empty()) {
                (void)hcommApi_.UnimportMemory(localEndpoint_[index], ni.descBytes.data(),
                                               static_cast<uint32_t>(ni.descBytes.size()));
            }
        }
        newImports.clear();
    };

    for (const auto &key : memKeys) {
        if (IsEmptyMemoryKey(key)) {
            BM_LOG_DEBUG("device_urma ImportRemoteMemKeysLocked skip empty memory key, peer: " << peerRank);
            continue;
        }
        // --- 0. Validate payload capacity before touching header/payload words ---
        if (key.ByteSize() < DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t) + sizeof(UrmaExportDesc)) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked key too small, peer: " << peerRank
                                                                                       << " bytes: " << key.ByteSize());
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }
        // --- 1. Validate top-level magic ---
        if (key.keys[0] != URMA_EXPORT_DESC_MAGIC) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked invalid key magic 0x"
                         << std::hex << key.keys[0] << ", peer: " << peerRank << ", expected: 0x"
                         << URMA_EXPORT_DESC_MAGIC);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        const uint64_t remoteAddr = key.keys[1];
        if (remoteAddr == 0) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked zero addr(" << remoteAddr
                                                                            << ") in key, peer: " << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 2. Parse UrmaExportDesc header from payload after header slots ---
        auto i = std::find(localIds_.begin(), localIds_.end(), state.remoteEndpointDesc.localId);
        auto index = 0;
        if (i != localIds_.end()) {
            index = std::distance(localIds_.begin(), i);
        }
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(&key.keys[DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS]);
        size_t rawLength = key.ByteSize() - (DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t));
        UrmaExportDesc exportDesc;
        uint32_t exportDescIndex = 0;
        int32_t localId = -1;
        UrmaTopoManager::GetInstance().GetLocalId(localId);
        if (!CheckAndGetUrmaExportDescInfo(raw, rawLength, localId, exportDesc, exportDescIndex)) {
            BM_LOG_ERROR(
                "device_urma ImportRemoteMemKeysLocked CheckAndGetUrmaExportDescInfo failed, peer: " << peerRank);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }
        const uint64_t remoteSize = exportDesc.size;
        const uint64_t memTag = exportDesc.memTag;
        const uint64_t memDescLen = sizeof(UrmaExportDesc) + exportDesc.hcommDescLen;

        // Validate total payload fits within the key data area
        const size_t payloadCapacity = key.ByteSize() - DEVICE_URMA_EXPORT_KEY_HEADER_SLOTS * sizeof(uint64_t);
        if (memDescLen + exportDesc.devTransFlagDescLen > payloadCapacity) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked total payload exceeds key capacity, "
                         << "peer: " << peerRank << " memDescLen: " << memDescLen
                         << " flagDescLen: " << exportDesc.devTransFlagDescLen << " capacity: " << payloadCapacity);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 3. Idempotency: skip if already imported (by memTag) ---
        auto it = std::find_if(state.imports.begin(), state.imports.end(),
                               [memTag](const auto &r) { return r.memTag == memTag; });
        if (it != state.imports.end()) {
            BM_LOG_DEBUG("device_urma ImportRemoteMemKeysLocked skip duplicate memTag: " << memTag
                                                                                         << ", peer: " << peerRank);
            continue;
        }

        // --- 4. Protocol compatibility check before import ---
        if (state.remoteEndpointDesc.protocol != localEndpointDesc_[index].protocol) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked protocol mismatch, peer: "
                         << peerRank << " remote protocol: " << state.remoteEndpointDesc.protocol
                         << " local protocol: " << localEndpointDesc_[index].protocol);
            rollbackNewImports();
            return BM_INVALID_PARAM;
        }

        // --- 5. HcommMemImport using global localEndpoint_ ---
        // raw points to UrmaExportDesc + hcommDesc; pass only the hcomm descriptor
        const uint8_t *hcommImportDesc = raw + exportDescIndex;
        const uint32_t hcommImportDescLen = exportDesc.hcommDescLen;
        HcommCommMem hcommView{};
        auto ret = hcommApi_.ImportMemory(localEndpoint_[index], hcommImportDesc, hcommImportDescLen, hcommView);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked HcommMemImport failed, memTag: "
                         << memTag << " peer: " << peerRank << " ret: " << ret);
            rollbackNewImports();
            return ret;
        }

        // --- 6. Build RemoteRegistration (store only hcomm desc, not UrmaExportDesc header) ---
        RemoteRegistration remote{};
        remote.addr = remoteAddr;
        remote.size = remoteSize;
        remote.memTag = memTag;
        remote.descBytes.assign(hcommImportDesc, hcommImportDesc + hcommImportDescLen);
        remote.view = ToUrmaCommMem(hcommView);
        newImports.emplace_back(std::move(remote));

        // --- 7. Import flag desc from UrmaExportDesc (if present and not yet imported for this peer) ---
        if (exportDesc.devTransFlagDescLen > 0 && !flagImportedInThisCall && state.remoteFlagAddr == 0) {
            const uint8_t *flagRaw = raw + exportDescIndex + exportDesc.hcommDescLen;
            HcommCommMem flagOutMem{};
            const auto flagRet =
                DlHcommApi::HcommMemImport(localEndpoint_[index], flagRaw, exportDesc.devTransFlagDescLen, &flagOutMem);
            if (flagRet != 0) {
                BM_LOG_ERROR("device_urma ImportRemoteMemKeysLocked HcommMemImport for flag failed, peer: "
                             << peerRank << " ret: " << flagRet);
                rollbackNewImports();
                return BM_DL_FUNCTION_FAILED;
            }
            if (flagOutMem.type == COMM_MEM_TYPE_INVALID) {
                BM_LOG_ERROR(
                    "device_urma ImportRemoteMemKeysLocked flag import returned invalid type, peer: " << peerRank);
                rollbackNewImports();
                return BM_INVALID_PARAM;
            }
            state.remoteFlagAddr = reinterpret_cast<uint64_t>(flagOutMem.addr);
            state.remoteFlagSize = flagOutMem.size;
            state.remoteFlagDescBytes.assign(flagRaw, flagRaw + exportDesc.devTransFlagDescLen);
            flagImportedInThisCall = true;
            BM_LOG_INFO("device_urma ImportRemoteMemKeysLocked imported flag, peer: "
                        << peerRank << " flagAddr: " << VaToStr(state.remoteFlagAddr)
                        << " flagSize: " << state.remoteFlagSize << " descLen: " << exportDesc.devTransFlagDescLen);
        }

        BM_LOG_INFO("device_urma ImportRemoteMemKeysLocked imported mem, peer: "
                    << peerRank << " memTag: " << memTag << " addr: " << VaToStr(remoteAddr) << " size: " << remoteSize
                    << " view: " << VaToStr(hcommView.addr));
    }

    state.imports.insert(state.imports.end(), newImports.begin(), newImports.end());
    return BM_OK;
}

Result DeviceUrmaTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    BM_VALIDATE_RETURN(!localEndpoint_.empty(),
                       "evice_urma Prepare failed: localEndpoint_ is null (OpenDevice may not have completed)",
                       BM_NOT_INITIALIZED);
    std::set<uint32_t> pendingCreatedPeerRanks;
    for (const auto &item : options.options) {
        const uint32_t peerRank = item.first;
        if (peerRank >= rankCount_) {
            BM_LOG_ERROR("device_urma Prepare invalid peerRank: " << peerRank << " rankCount: " << rankCount_);
            RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
            return BM_INVALID_PARAM;
        }
        if (peerRank == rankId_) {
            BM_LOG_WARN("device_urma Prepare skipping self rank: " << peerRank);
            continue;
        }
        auto &state = remoteRanks_[peerRank];
        // peer 重新 Prepare 视为恢复，解除故障标记，否则重连后无法再通信
        UnmarkRankFaulted(peerRank);

        // 1. Parse peer UrmaEndpointDesc from privateData
        UrmaEndpointDesc peerDesc{};
        int32_t localId = -1;
        UrmaTopoManager::GetInstance().GetLocalId(localId);
        auto ret = ParsePrivateDataToEndpointDesc(item.second.privateData, localId, peerDesc);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma Prepare failed to parse peer endpoint desc, peer: " << peerRank);
            RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
            return ret;
        }

        // 2. Idempotency: if thread and channel already exist, compare with stored remoteEndpointDesc.
        //    If unchanged → skip resource creation; if changed → reject (no hot-replace).
        if (state.channel != 0 && state.thread != 0) {
            if (std::memcmp(&state.remoteEndpointDesc, &peerDesc, sizeof(UrmaEndpointDesc)) != 0) {
                BM_LOG_ERROR(
                    "device_urma Prepare peer endpoint desc changed but hot-replace not supported, peer: " << peerRank);
                RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
                return BM_INVALID_PARAM;
            }
            BM_LOG_INFO("device_urma Prepare reusing existing channel/thread for peer: " << peerRank);
        } else {
            // 3. Convert peer UrmaEndpointDesc → HCOMM EndpointDesc for HcommChannelDesc.remoteEndpoint
            EndpointDesc hcommRemoteEndpoint = ToHcommEndpointDesc(peerDesc);

            // 4. Build a single HcommChannelDesc for this peer
            HcommChannelDesc channelDesc{};
            HcommChannelDescInit(&channelDesc, 1); // not in DlHcommApi namespace, direct C function
            channelDesc.role = (rankId_ > peerRank) ? HCOMM_SOCKET_ROLE_CLIENT : HCOMM_SOCKET_ROLE_SERVER;
            channelDesc.remoteEndpoint = hcommRemoteEndpoint;
            channelDesc.notifyNum = HCOMM_NORMAL_NOTIFY_NUM;
            channelDesc.exchangeAllMems = true;     // 填true, 不用管memHandles了, remoteEndpoint要填对
            channelDesc.qos = GetHcommChannelQos(); // 0-7, 值越大优先级越高。
            BM_LOG_INFO("device_urma Prepare channelDesc qos: " << channelDesc.qos);

            // 5. Allocate one thread per peer (use temporary variable for safe rollback)
            HcommThreadHandle threadHandle = 0;
            ret = DlHcommApi::HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &HCOMM_NORMAL_NOTIFY_NUM, &threadHandle);
            if (ret != BM_OK) {
                BM_LOG_ERROR("device_urma Prepare HcommThreadAlloc failed, peer: "
                             << peerRank << " engine: " << COMM_ENGINE_AICPU_TS << " ret: " << ret);
                RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
                return ret;
            }
            if (threadHandle == 0) {
                BM_LOG_ERROR("device_urma Prepare HcommThreadAlloc returned invalid thread, peer: " << peerRank);
                RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
                return BM_DL_FUNCTION_FAILED;
            }

            // 6. Create one channel per peer (temporary variable for safe rollback)
            HcommChannelHandle channelHandle = 0;
            BM_LOG_INFO("device_urma Prepare HcommChannelCreate peerRank: " << peerRank << ", channelDesc.role: "
                                                                            << channelDesc.role);
            auto it = std::find(localIds_.begin(), localIds_.end(), peerDesc.localId);
            auto index = 0;
            if (it != localIds_.end()) {
                index = std::distance(localIds_.begin(), it);
            }
            ret = DlHcommApi::HcommChannelCreate(localEndpoint_[index], COMM_ENGINE_AICPU, &channelDesc, 1,
                                                 &channelHandle);
            if (ret != 0) {
                BM_LOG_ERROR("device_urma Prepare HcommChannelCreate failed, peer: "
                             << peerRank << " peerDev=" << peerDesc.devPhyId << " peerServer=" << peerDesc.serverIdx
                             << " peerSuperPod=" << peerDesc.superPodIdx << " localDev=" << phyDeviceId_
                             << " ret: " << ret);
                auto rollbackThread = threadHandle;
                // No stream sync needed: thread was just allocated, no RemoteIO launched during Prepare.
                (void)DlHcommApi::HcommThreadFree(&rollbackThread, 1);
                RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
                return BM_DL_FUNCTION_FAILED;
            }
            if (channelHandle == 0) {
                BM_LOG_ERROR("device_urma Prepare HcommChannelCreate returned invalid channel, peer: " << peerRank);
                auto rollbackThread = threadHandle;
                // No stream sync needed: thread was just allocated, no RemoteIO launched during Prepare.
                (void)DlHcommApi::HcommThreadFree(&rollbackThread, 1);
                RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
                return BM_DL_FUNCTION_FAILED;
            }

            // 7. Persist new resources to state
            state.remoteEndpointDesc = peerDesc;
            state.hasEndpointDesc = true;
            state.thread = threadHandle;
            state.channelDesc = channelDesc;
            state.channel = channelHandle;
            pendingCreatedPeerRanks.insert(peerRank);

            BM_LOG_INFO("device_urma Prepare created channel/thread, peer: "
                        << peerRank << " peerDev=" << peerDesc.devPhyId << " peerServer=" << peerDesc.serverIdx
                        << " peerSuperPod=" << peerDesc.superPodIdx << " localDev=" << phyDeviceId_
                        << " thread: " << threadHandle << " channel: " << channelHandle);
        }
    }

    for (const auto &item : options.options) {
        const auto peerRank = item.first;
        if (peerRank == rankId_) {
            continue;
        }
        auto &state = remoteRanks_.at(peerRank);
        const bool resourcesCreatedInThisCall = pendingCreatedPeerRanks.count(peerRank) != 0U;

        // 8. Import remote memory keys (always, even if channel/thread were reused)
        auto ret = ImportRemoteMemKeysLocked(peerRank, state, options.options.at(peerRank).memKeys);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma Prepare ImportRemoteMemKeysLocked failed, peer: " << peerRank);
            RollbackPendingPrepareResourcesLocked(pendingCreatedPeerRanks);
            return ret;
        }

        BM_LOG_INFO("device_urma Prepare success, peer: " << peerRank << " thread: " << state.thread << " channel: "
                                                          << state.channel << " imports: " << state.imports.size());
        if (resourcesCreatedInThisCall) {
            pendingCreatedPeerRanks.erase(peerRank);
        }
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::ConnectRank(uint32_t rankId)
{
    std::shared_lock<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    BM_VALIDATE_RETURN(rankId < rankCount_,
                       "device_urma ConnectRank rank: " << rankId << " out of range, rankCount: " << rankCount_,
                       BM_INVALID_PARAM);

    std::vector<HcommChannelReadyPeer> channelPeers;
    channelPeers.reserve(remoteRanks_.size());
    for (const auto &item : remoteRanks_) {
        if (item.first != rankId_ && item.second.channel != 0) {
            channelPeers.push_back({item.second.channel, item.first});
        }
    }
    // Keep channel handles alive until the wrapper finishes polling.
    return HcommApiWrapper::WaitForChannelsReady(channelPeers);
}

void DeviceUrmaTransportManager::RollbackPendingPrepareResourcesLocked(const std::set<uint32_t> &peerRanks)
{
    for (const auto peerRank : peerRanks) {
        auto stateIt = remoteRanks_.find(peerRank);
        if (stateIt == remoteRanks_.end()) {
            continue;
        }
        const auto ret = CleanupPeerRankState(stateIt->second, peerRank);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma Prepare rollback pending resources failed, localRank: "
                         << rankId_ << " peerRank: " << peerRank << " ret: " << ret);
        }
        remoteRanks_.erase(stateIt);
    }
}

void DeviceUrmaTransportManager::MarkRanksFaulted(const std::vector<uint32_t> &ranks)
{
    std::unique_lock<std::shared_mutex> lock(faultedRanksMutex_);
    for (auto rankId : ranks) {
        if (faultedRanks_.insert(rankId).second) {
            BM_LOG_INFO("device_urma mark peer rank faulted, localRank: " << rankId_ << " peerRank: " << rankId);
        }
    }
}

void DeviceUrmaTransportManager::UnmarkRankFaulted(uint32_t rankId)
{
    std::unique_lock<std::shared_mutex> lock(faultedRanksMutex_);
    if (faultedRanks_.erase(rankId) != 0) {
        BM_LOG_INFO("device_urma unmark peer rank faulted, localRank: " << rankId_ << " peerRank: " << rankId);
    }
}

bool DeviceUrmaTransportManager::IsRankFaulted(uint32_t rankId) const
{
    std::shared_lock<std::shared_mutex> lock(faultedRanksMutex_);
    return faultedRanks_.count(rankId) != 0;
}

Result DeviceUrmaTransportManager::RemoveRankLocked(uint32_t rankId)
{
    auto rankIt = remoteRanks_.find(rankId);
    if (rankIt == remoteRanks_.end()) {
        BM_LOG_WARN("device_urma RemoveRankLocked rank not found, localRank: "
                    << rankId_ << " peerRank: " << rankId << " remoteRanksSize: " << remoteRanks_.size());
        return BM_OK;
    }
    auto &state = rankIt->second;
    BM_LOG_INFO("device_urma RemoveRankLocked found entry, localRank: "
                << rankId_ << " peerRank: " << rankId << " channel: " << state.channel << " thread: " << state.thread);
    Result finalRet = DestroyRankChannelsAndThread(state, rankId);

    const auto retImports = UnimportPeerImportsAndFlag(state, rankId);
    if (retImports != BM_OK && finalRet == BM_OK) {
        finalRet = retImports;
    }
    remoteRanks_.erase(rankIt);
    return finalRet;
}

Result DeviceUrmaTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    BM_LOG_INFO("device_urma RemoveRanks called, localRank: " << rankId_ << " ranks: " << removedRanks.size()
                                                              << " opened: " << opened_);
    {
        std::shared_lock<std::shared_mutex> guard(mutex_);
        BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    }

    // 关键：先在不持 mutex_ 的前提下标记故障并中止所有 memfabric 线程 stream。
    // 若有数据线程正阻塞在共享锁保护的 notify/stream 等待中，此处 abort 可让其立即返回
    // ACL_ERROR_RT_STREAM_ABORT，避免被拖到 device task timeout(107020) 污染整卡 context。
    // 若先取独占锁再 abort，会被该 shared 持有者阻塞，永远到不了 abort。
    MarkRanksFaulted(removedRanks);
    HybmStreamManager::AbortAllThreadAclStreams();

    std::lock_guard<std::shared_mutex> guard(mutex_);
    Result finalRet = BM_OK;
    for (auto rankId : removedRanks) {
        BM_LOG_INFO("device_urma RemoveRanks calling RemoveRankLocked, rankId: " << rankId);
        auto ret = RemoveRankLocked(rankId);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma RemoveRanks RemoveRankLocked failed, localRank: " << rankId_ << " peerRank: "
                                                                                        << rankId << " ret: " << ret);
            if (finalRet == BM_OK) {
                finalRet = ret;
            }
        }
    }
    if (finalRet != BM_OK) {
        BM_LOG_ERROR("device_urma RemoveRanks cleanup failed, localRank: "
                     << rankId_ << " removedRanks: " << removedRanks.size() << " ret: " << finalRet);
    }
    return finalRet;
}

Result DeviceUrmaTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    bool needFallback = false;
    {
        std::lock_guard<std::shared_mutex> guard(mutex_);
        BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
        if (localEndpoint_.empty()) {
            BM_LOG_ERROR("device_urma UpdateRankOptions failed: localEndpoint_ is null");
            return BM_NOT_INITIALIZED;
        }
        for (const auto &item : options.options) {
            const uint32_t peerRank = item.first;
            if (peerRank >= rankCount_) {
                BM_LOG_ERROR("device_urma UpdateRankOptions invalid peerRank: " << peerRank
                                                                                << " rankCount: " << rankCount_);
                return BM_INVALID_PARAM;
            }
            if (peerRank == rankId_) {
                BM_LOG_WARN("device_urma UpdateRankOptions skipping self rank: " << peerRank);
                continue;
            }
            auto stateIt = remoteRanks_.find(peerRank);
            if (stateIt == remoteRanks_.end()) {
                BM_LOG_INFO("device_urma UpdateRankOptions peer rank " << peerRank
                                                                       << " not prepared yet, need to Prepare");
                needFallback = true;
                break;
            }
            auto &state = stateIt->second;
            if (state.channel == 0 || state.thread == 0) {
                BM_LOG_WARN("device_urma UpdateRankOptions peer rank "
                            << peerRank << " has no channel/thread, fallback to Prepare");
                needFallback = true;
                break;
            }
        }
        if (!needFallback) {
            // Keep validation and import in one exclusive remote-rank lock cycle so a rank cannot exit between them.
            for (const auto &item : options.options) {
                const uint32_t peerRank = item.first;
                if (peerRank == rankId_) {
                    continue;
                }
                auto &state = remoteRanks_.at(peerRank);
                auto ret = ImportRemoteMemKeysLocked(peerRank, state, item.second.memKeys);
                if (ret != BM_OK) {
                    BM_LOG_ERROR("device_urma UpdateRankOptions ImportRemoteMemKeysLocked failed, peer: " << peerRank);
                    return ret;
                }
                BM_LOG_INFO("device_urma UpdateRankOptions success, peer: " << peerRank);
            }
            return BM_OK;
        }
    } // Lifecycle and remote-rank locks released before Prepare acquires them.

    BM_LOG_INFO("device_urma UpdateRankOptions falling back to Prepare for new dynamic ranks");
    return Prepare(options);
}

const std::string &DeviceUrmaTransportManager::GetNic() const
{
    static const std::string emptyNic;
    return emptyNic;
}

const TransportPrivateData DeviceUrmaTransportManager::GetPrivateData() const
{
    std::lock_guard<std::shared_mutex> guard(mutex_);
    TransportPrivateData data{};
    if (localEndpoint_.empty()) {
        BM_LOG_ERROR("device_urma GetPrivateData called before localEndpoint_ is ready, returning empty");
        return data; // empty data, peer's ParsePrivateDataToEndpointDesc will reject with magic mismatch
    }
    UrmaPrivateDataDesc header{};
    header.payloadLen = static_cast<uint16_t>(sizeof(UrmaEndpointDesc) * localEndpoint_.size());
    data.key.ResizeBytes(sizeof(UrmaPrivateDataDesc) + sizeof(UrmaEndpointDesc) * localEndpoint_.size());
    uint8_t *raw = data.key.ByteData();
    std::memcpy(raw, &header, sizeof(UrmaPrivateDataDesc));
    std::memcpy(raw + sizeof(UrmaPrivateDataDesc), localEndpointDesc_.data(),
                sizeof(UrmaEndpointDesc) * localEndpoint_.size());
    return data;
}

//单IO
Result DeviceUrmaTransportManager::StageAndLaunchTransfer(CompletionContext &ctx, RemoteRankState &state, bool isRead,
                                                          const std::vector<uint64_t> &localVec,
                                                          const std::vector<uint64_t> &remoteVec,
                                                          const std::vector<uint64_t> &sizeVec, uint32_t rankId)
{
    std::vector<HcommBatchTransferDesc> transferDescs;
    transferDescs.reserve(sizeVec.size());
    for (size_t i = 0; i < sizeVec.size(); ++i) {
        HcommBatchTransferDesc desc{};
        auto *local = reinterpret_cast<void *>(localVec[i]);
        auto *remote = reinterpret_cast<void *>(remoteVec[i]);
        desc.transType = isRead ? HCOMM_TRANSFER_TYPE_READ : HCOMM_TRANSFER_TYPE_WRITE;
        if (isRead) {
            desc.transferInfo.read = {sizeVec[i], local, remote};
        } else {
            desc.transferInfo.write = {sizeVec[i], remote, local};
        }
        transferDescs.push_back(desc);
    }
    const std::vector<uint32_t> rankIds{rankId};
    const std::vector<uint32_t> rankStartIdx{0U};
    const std::vector<uint32_t> rankListNum{static_cast<uint32_t>(transferDescs.size())};
    const std::vector<HcommThreadHandle> threads{state.thread};
    const std::vector<HcommChannelHandle> channels{state.channel};
    std::vector<HcommBatchTransferDesc> markerDescs;
    std::vector<NotifyResource *> notifyResources;
    auto ret = PrepareMultiRankMarkersLocked(rankIds, markerDescs, notifyResources);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma StageAndLaunchTransfer PrepareMultiRankMarkersLocked failed, rankId="
                     << rankId << " rankCount=" << rankIds.size() << " ret=" << ret);
        return ret;
    }
    ret = StageAndLaunchMultiTransfer(ctx, transferDescs, 0U, transferDescs.size(), rankIds, rankStartIdx, rankListNum,
                                      threads, channels, &markerDescs, &notifyResources);
    for (auto *resource : notifyResources) {
        if (resource != nullptr) {
            ReleaseNotifyResource(*resource);
        }
    }
    return ret;
}

Result DeviceUrmaTransportManager::RemoteIo(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write)
{
    if (size == 0) {
        return BM_OK;
    }
    if (IsRankFaulted(rankId)) {
        BM_LOG_WARN("device_urma RemoteIo skip faulted rank, localRank: " << rankId_ << " peerRank: " << rankId);
        return BM_NOT_CONNECTED;
    }
    std::shared_lock<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    const RemoteRegistration *remote = nullptr;
    auto ret = FindRemoteRegistrationLocked(rankId, rAddr, size, &remote);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma remote address is not prepared, rank: " << rankId << " addr: " << std::hex << rAddr);
        return ret;
    }
    uint64_t correctedLAddr = lAddr;
    ret = CorrectLocalRegAddressLocked(lAddr, size, correctedLAddr);
    if (ret != BM_OK) {
        return ret;
    }
    auto &state = remoteRanks_[rankId];
    if (state.channel == 0 || state.thread == 0) {
        BM_LOG_ERROR("RemoteIo no channel/thread, rankId: " << rankId << " channel: " << state.channel
                                                            << " thread: " << state.thread << std::hex << " rAddr: 0x"
                                                            << rAddr << std::dec << " size: " << size);
        return BM_NOT_CONNECTED;
    }
    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("device_urma RemoteIo LookupOrCreateContextLocked failed");
        return BM_ERROR;
    }
    const auto translatedRemoteAddr = remote->view.addr + (rAddr - remote->addr);
    const bool isRead = !write;

    TP_TRACE_BEGIN(TP_HYBM_URMA_LAUNCH_TRANSFER);
    ret = StageAndLaunchTransfer(*ctx, state, isRead, {correctedLAddr}, {translatedRemoteAddr}, {size}, rankId);
    TP_TRACE_END(TP_HYBM_URMA_LAUNCH_TRANSFER, ret);
    return ret;
}

Result DeviceUrmaTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = ReadRemoteAsync(rankId, lAddr, rAddr, size);
    if (ret != BM_OK) {
        return ret;
    }
    return Synchronize(rankId);
}

Result DeviceUrmaTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    auto ret = WriteRemoteAsync(rankId, lAddr, rAddr, size);
    if (ret != BM_OK) {
        return ret;
    }
    return Synchronize(rankId);
}

Result DeviceUrmaTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    TP_TRACE_BEGIN(TP_HYBM_URMA_REMOTE_IO_READ);
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, false);
    TP_TRACE_END(TP_HYBM_URMA_REMOTE_IO_READ, ret);
    return ret;
}

Result DeviceUrmaTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    TP_TRACE_BEGIN(TP_HYBM_URMA_REMOTE_IO_WRITE);
    auto ret = RemoteIo(rankId, lAddr, rAddr, size, true);
    TP_TRACE_END(TP_HYBM_URMA_REMOTE_IO_WRITE, ret);
    return ret;
}

Result DeviceUrmaTransportManager::ValidateMultiRankBatchLocked(const hybm_batch_copy_params &params,
                                                                hybm_data_copy_direction direction,
                                                                const RankGroupMap &groupMap) const
{
    const auto directionValue = static_cast<int32_t>(direction);
    const bool validDirection = directionValue >= static_cast<int32_t>(HYBM_LOCAL_HOST_TO_GLOBAL_HOST) &&
                                directionValue < static_cast<int32_t>(HYBM_DATA_COPY_DIRECTION_AUTO);
    if (!validDirection || params.batchSize == 0 || params.sources == nullptr || params.destinations == nullptr ||
        params.dataSizes == nullptr) {
        BM_LOG_ERROR("device_urma invalid raw multi-rank batch, batchSize: " << params.batchSize
                                                                             << " direction: " << direction);
        return BM_INVALID_PARAM;
    }
    for (const auto &[p2pInfo, indices] : groupMap) {
        const bool containsLocal = p2pInfo.first == rankId_ || p2pInfo.second == rankId_;
        if (!containsLocal) {
            BM_LOG_ERROR("device_urma multi-rank group excludes local rank, localRank: "
                         << rankId_ << " srcRank: " << p2pInfo.first << " destRank: " << p2pInfo.second);
            return BM_INVALID_PARAM;
        }
        for (uint32_t index : indices) {
            if (index >= params.batchSize) {
                BM_LOG_ERROR("device_urma multi-rank index out of range, index: "
                             << index << " batchSize: " << params.batchSize << " srcRank: " << p2pInfo.first
                             << " destRank: " << p2pInfo.second);
                return BM_INVALID_PARAM;
            }
            if (params.sources[index] == nullptr || params.destinations[index] == nullptr) {
                BM_LOG_ERROR("device_urma multi-rank address is null, index: " << index << " srcRank: " << p2pInfo.first
                                                                               << " destRank: " << p2pInfo.second);
                return BM_INVALID_PARAM;
            }
        }
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::ResolveLocalAddressLocked(uint64_t addr, uint64_t size, uint64_t &correctedAddr,
                                                             bool &registered) const
{
    correctedAddr = addr;
    registered = false;
    auto pos = localRegistrations_.upper_bound(addr);
    if (pos == localRegistrations_.begin()) {
        return BM_OK;
    }
    --pos;
    const auto &registration = pos->second;
    if (!ContainsAddressRange(registration.mr.addr, registration.mr.size, addr, size)) {
        return BM_OK;
    }
    registered = true;
    const bool isDram = (registration.mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0;
    if (isDram && registration.deviceVa != 0) {
        const uint64_t offset = addr - registration.mr.addr;
        if (registration.deviceVa > std::numeric_limits<uint64_t>::max() - offset) {
            BM_LOG_ERROR("device_urma local DVA overflow, localRank: "
                         << rankId_ << " deviceVa: " << VaToStr(registration.deviceVa) << " offset: " << offset);
            return BM_INVALID_PARAM;
        }
        correctedAddr = registration.deviceVa + offset;
    }
    if (size > std::numeric_limits<uint64_t>::max() - correctedAddr) {
        BM_LOG_ERROR("device_urma local address range overflow, localRank: "
                     << rankId_ << " localAddr: " << VaToStr(correctedAddr) << " size: " << size);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::ResolveRemoteAddressLocked(uint32_t remoteRank, uint64_t remoteAddr, uint64_t size,
                                                              uint64_t &correctedAddr) const
{
    const RemoteRegistration *remote = nullptr;
    auto ret = FindRemoteRegistrationLocked(remoteRank, remoteAddr, size, &remote);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma remote address is not imported, remoteRank: "
                     << remoteRank << " remoteAddr: " << VaToStr(remoteAddr) << " size: " << size << " ret: " << ret);
        return ret;
    }
    const uint64_t offset = remoteAddr - remote->addr;
    if (remote->view.addr > std::numeric_limits<uint64_t>::max() - offset) {
        BM_LOG_ERROR("device_urma imported view address overflow, remoteRank: "
                     << remoteRank << " viewAddr: " << VaToStr(remote->view.addr) << " offset: " << offset);
        return BM_INVALID_PARAM;
    }
    correctedAddr = remote->view.addr + offset;
    if (size > std::numeric_limits<uint64_t>::max() - correctedAddr) {
        BM_LOG_ERROR("device_urma imported view range overflow, remoteRank: "
                     << remoteRank << " peerAddr: " << VaToStr(correctedAddr) << " size: " << size);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

HcommBatchTransferDesc DeviceUrmaTransportManager::BuildTransferDesc(uint64_t localAddr, uint64_t remoteAddr,
                                                                     uint64_t size, bool isRead)
{
    HcommBatchTransferDesc desc{};
    auto *local = reinterpret_cast<void *>(localAddr);
    auto *peer = reinterpret_cast<void *>(remoteAddr);
    desc.transType = isRead ? HCOMM_TRANSFER_TYPE_READ : HCOMM_TRANSFER_TYPE_WRITE;
    if (isRead) {
        desc.transferInfo.read = {size, local, peer};
    } else {
        desc.transferInfo.write = {size, peer, local};
    }
    return desc;
}

Result DeviceUrmaTransportManager::PrepareMultiRankMarkersLocked(const std::vector<uint32_t> &rankIds,
                                                                 std::vector<HcommBatchTransferDesc> &markerDescs,
                                                                 std::vector<NotifyResource *> &notifyResources)
{
    //给ranks准备notify descs
    notifyResources.assign(rankIds.size(), nullptr);
    markerDescs.clear();
    markerDescs.reserve(rankIds.size());
    for (uint32_t rankId : rankIds) {
        const auto &state = remoteRanks_.at(rankId);
        if (state.remoteFlagAddr == 0 || state.remoteFlagSize == 0) {
            BM_LOG_ERROR("device_urma multi-rank remote flag is invalid, rankId: "
                         << rankId << " remoteFlagAddr: " << VaToStr(state.remoteFlagAddr)
                         << " remoteFlagSize: " << state.remoteFlagSize);
            return BM_NOT_CONNECTED;
        }
    }
    //每个rank申请一个notify
    for (size_t i = 0; i < rankIds.size(); ++i) {
        auto ret = AcquireNotifyResource(notifyResources[i]);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma multi-rank acquire notify failed, rankId: " << rankIds[i] << " ret: " << ret);
            for (auto *resource : notifyResources) {
                if (resource != nullptr) {
                    ReleaseNotifyResource(*resource);
                }
            }
            return ret;
        }
        const auto &state = remoteRanks_.at(rankIds[i]);
        //构造notify descs
        markerDescs.push_back(BuildTransferDesc(notifyResources[i]->notifyAddr, state.remoteFlagAddr,
                                                notifyResources[i]->notifyLen, true));
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::ResolveMultiRankIoLocked(const hybm_batch_copy_params &params,
                                                            hybm_data_copy_direction direction,
                                                            const std::pair<uint32_t, uint32_t> &p2pInfo,
                                                            uint32_t index, RankTransferDescriptors &descriptors,
                                                            RankGroupMap &unregisteredGroups) const
{
    const bool isWrite = p2pInfo.first == rankId_;
    const uint32_t remoteRank = isWrite ? p2pInfo.second : p2pInfo.first;
    const uint64_t size = params.dataSizes[index];
    if (size == 0) {
        return BM_OK;
    }
    const uint64_t originalLocal =
        reinterpret_cast<uint64_t>(isWrite ? params.sources[index] : params.destinations[index]);
    const auto memType = isWrite ? HybmDirectionSrcMemType[direction] : HybmDirectionDestMemType[direction];
    const uint32_t outputType = memType == HYBM_MEM_TYPE_HOST ? HVM_HVA : HVM_DVA;
    const uint64_t transformed = HybmVaManager::GetInstance().TransformVa(originalLocal, HVM_GVA, outputType);
    const uint64_t localAddr = transformed == 0 ? originalLocal : transformed;
    uint64_t correctedLocal = localAddr;
    bool registered = false;
    //先处理本地，地址范围+dva
    auto ret = ResolveLocalAddressLocked(localAddr, size, correctedLocal, registered);
    if (ret != BM_OK) {
        return ret;
    }
    if (!registered) {
        unregisteredGroups[p2pInfo].push_back(index);
        return BM_OK;
    }
    const uint64_t remoteAddr =
        reinterpret_cast<uint64_t>(isWrite ? params.destinations[index] : params.sources[index]);
    //处理远端，地址范围+范围
    uint64_t correctedRemote = 0;
    ret = ResolveRemoteAddressLocked(remoteRank, remoteAddr, size, correctedRemote);
    if (ret != BM_OK) {
        return ret;
    }
    //根据read/write构造descs
    descriptors[remoteRank].push_back(BuildTransferDesc(correctedLocal, correctedRemote, size, !isWrite));
    return BM_OK;
}

Result DeviceUrmaTransportManager::ResolveMultiRankGroupLocked(
    const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
    const std::pair<uint32_t, uint32_t> &p2pInfo, const std::vector<uint32_t> &indices,
    RankTransferDescriptors &descriptors, std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups) const
{
    TP_TRACE_BEGIN(TP_HYBM_URMA_RESOLVE_MULTI_RANK_GROUP);
    if (p2pInfo.first == rankId_ && p2pInfo.second == rankId_) {
        localIndices.insert(localIndices.end(), indices.begin(), indices.end());
        TP_TRACE_END(TP_HYBM_URMA_RESOLVE_MULTI_RANK_GROUP, BM_OK);
        return BM_OK;
    }
    for (uint32_t index : indices) {
        auto ret = ResolveMultiRankIoLocked(params, direction, p2pInfo, index, descriptors, unregisteredGroups);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma failed to resolve multi-rank IO, ret: " << ret << " index: " << index
                                                                              << " srcRank: " << p2pInfo.first
                                                                              << " destRank: " << p2pInfo.second);
            TP_TRACE_END(TP_HYBM_URMA_RESOLVE_MULTI_RANK_GROUP, ret);
            return ret;
        }
    }
    TP_TRACE_END(TP_HYBM_URMA_RESOLVE_MULTI_RANK_GROUP, BM_OK);
    return BM_OK;
}

Result DeviceUrmaTransportManager::FlattenMultiRankDescriptorsLocked(
    const RankTransferDescriptors &descriptors, std::vector<HcommBatchTransferDesc> &transferDescs,
    std::vector<uint32_t> &rankIds, std::vector<uint32_t> &rankStartIdx, std::vector<uint32_t> &rankListNum,
    std::vector<HcommThreadHandle> &threads, std::vector<HcommChannelHandle> &channels) const
{
    std::vector<uint32_t> sortedRanks;
    sortedRanks.reserve(descriptors.size());
    size_t totalCount = 0;
    for (const auto &[rankId, rankDescs] : descriptors) {
        if (rankDescs.size() > transferDescs.max_size() - totalCount) {
            BM_LOG_ERROR("device_urma raw multi-rank batch capacity exceeded, rankId: " << rankId);
            return BM_INVALID_PARAM;
        }
        totalCount += rankDescs.size();
        sortedRanks.push_back(rankId);
    }
    if (totalCount > UINT32_MAX) {
        BM_LOG_ERROR("device_urma raw multi-rank batch exceeds index limit, totalCount: " << totalCount);
        return BM_INVALID_PARAM;
    }
    transferDescs.reserve(totalCount);
    std::sort(sortedRanks.begin(), sortedRanks.end());
    for (uint32_t rankId : sortedRanks) {
        const auto stateIt = remoteRanks_.find(rankId);
        if (stateIt == remoteRanks_.end() || stateIt->second.thread == 0 || stateIt->second.channel == 0) {
            BM_LOG_ERROR("device_urma raw multi-rank channel not ready, rankId: " << rankId);
            return BM_NOT_CONNECTED;
        }
        const auto &rankDescs = descriptors.at(rankId);
        rankIds.push_back(rankId);
        rankStartIdx.push_back(static_cast<uint32_t>(transferDescs.size()));
        rankListNum.push_back(static_cast<uint32_t>(rankDescs.size()));
        threads.push_back(stateIt->second.thread);
        channels.push_back(stateIt->second.channel);
        transferDescs.insert(transferDescs.end(), rankDescs.begin(), rankDescs.end());
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::ResolveAndFlattenMultiRankBatchLocked(
    const hybm_batch_copy_params &params, hybm_data_copy_direction direction, const RankGroupMap &groupMap,
    std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups,
    std::vector<HcommBatchTransferDesc> &transferDescs, std::vector<uint32_t> &rankIds,
    std::vector<uint32_t> &rankStartIdx, std::vector<uint32_t> &rankListNum, std::vector<HcommThreadHandle> &threads,
    std::vector<HcommChannelHandle> &channels) const
{
    RankTransferDescriptors descriptors;
    TP_TRACE_BEGIN(TP_HYBM_URMA_RESOLVE_BATCH_IO);
    for (const auto &[p2pInfo, indices] : groupMap) {
        auto ret = ResolveMultiRankGroupLocked(params, direction, p2pInfo, indices, descriptors, localIndices,
                                               unregisteredGroups);
        if (ret != BM_OK) {
            TP_TRACE_END(TP_HYBM_URMA_RESOLVE_BATCH_IO, ret);
            return ret;
        }
    }
    TP_TRACE_END(TP_HYBM_URMA_RESOLVE_BATCH_IO, BM_OK);
    TP_TRACE_BEGIN(TP_HYBM_URMA_FLATTEN_MULTI_RANK_DESCRIPTORS);
    auto ret = FlattenMultiRankDescriptorsLocked(descriptors, transferDescs, rankIds, rankStartIdx, rankListNum,
                                                 threads, channels);
    TP_TRACE_END(TP_HYBM_URMA_FLATTEN_MULTI_RANK_DESCRIPTORS, ret);
    if (ret != BM_OK || transferDescs.size() <= HCOMM_BATCH_TRANSFER_MAX_DESC_NUM) {
        return ret;
    }
    BM_LOG_ERROR("device_urma multi-rank slice exceeds kernel limit, batchSize: "
                 << transferDescs.size() << " rankNum: " << rankIds.size()
                 << " limit: " << HCOMM_BATCH_TRANSFER_MAX_DESC_NUM);
    return BM_INVALID_PARAM;
}

Result DeviceUrmaTransportManager::StageAndLaunchMultiTransfer(
    CompletionContext &ctx, const std::vector<HcommBatchTransferDesc> &transferDescs, size_t transferOffset,
    size_t transferCount, const std::vector<uint32_t> &rankIds, const std::vector<uint32_t> &rankStartIdx,
    const std::vector<uint32_t> &rankListNum, const std::vector<HcommThreadHandle> &threads,
    const std::vector<HcommChannelHandle> &channels, const std::vector<HcommBatchTransferDesc> *markerDescs,
    std::vector<NotifyResource *> *notifyResources)
{
    if (markerDescs == nullptr || notifyResources == nullptr || markerDescs->size() != rankIds.size() ||
        notifyResources->size() != rankIds.size()) {
        BM_LOG_ERROR("device_urma StageAndLaunchMultiTransfer invalid notify arguments, rankNum: "
                     << rankIds.size() << " markerCount: " << (markerDescs == nullptr ? 0U : markerDescs->size())
                     << " notifyCount: " << (notifyResources == nullptr ? 0U : notifyResources->size()));
        return BM_INVALID_PARAM;
    }
    for (size_t i = 0; i < notifyResources->size(); ++i) {
        if ((*notifyResources)[i] == nullptr) {
            BM_LOG_ERROR("device_urma StageAndLaunchMultiTransfer null notify resource, rankId: " << rankIds[i]
                                                                                                  << " index: " << i);
            return BM_INVALID_PARAM;
        }
    }
    if (rankIds.size() > ctx.pendingTransfers.max_size() - ctx.pendingTransfers.size()) {
        BM_LOG_ERROR("device_urma StageAndLaunchMultiTransfer pending capacity exceeded, pendingCount: "
                     << ctx.pendingTransfers.size() << " rankNum: " << rankIds.size());
        return BM_INVALID_PARAM;
    }
    try {
        ctx.pendingTransfers.reserve(ctx.pendingTransfers.size() + rankIds.size());
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("device_urma StageAndLaunchMultiTransfer reserve pending failed, pendingCount: "
                     << ctx.pendingTransfers.size() << " rankNum: " << rankIds.size());
        return BM_MALLOC_FAILED;
    }
    //准备传输的descs，包括marker
    TP_TRACE_BEGIN(TP_HYBM_URMA_STAGE_PREPARE_BUFFERS);
    auto ret = PrepareKernelLaunchBuffers(transferDescs, transferOffset, transferCount, rankIds, rankStartIdx,
                                          rankListNum, threads, channels, ctx.launchBuffers, markerDescs);
    TP_TRACE_END(TP_HYBM_URMA_STAGE_PREPARE_BUFFERS, ret);
    if (ret != BM_OK) {
        return ret;
    }
    RecordUrmaIoMetrics(transferDescs, transferOffset, transferCount);

    std::vector<std::unique_lock<std::mutex>> rankLocks;
    rankLocks.reserve(rankIds.size());
    TP_TRACE_BEGIN(TP_HYBM_URMA_LOCK_RANKS);
    for (uint32_t rankId : rankIds) {
        rankLocks.emplace_back(remoteRanks_.at(rankId).rankMutex);
    }
    TP_TRACE_END(TP_HYBM_URMA_LOCK_RANKS, BM_OK);
    //一系列的launch
    TP_TRACE_BEGIN(TP_HYBM_URMA_STAGE_LAUNCH_BATCH);
    ret = LaunchDeviceKernelBatch(ctx.launchBuffers, transferCount, rankIds.size());
    TP_TRACE_END(TP_HYBM_URMA_STAGE_LAUNCH_BATCH, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma StageAndLaunchMultiTransfer kernel failed, transferCount: "
                     << transferCount << " rankNum: " << rankIds.size() << " ret: " << ret);
        return ret;
    }
    for (size_t i = 0; i < rankIds.size(); ++i) {
        const uint32_t notifyPoolIndex = (*notifyResources)[i]->poolIndex;
        //交给pendingtransfer管理
        (*notifyResources)[i] = nullptr;
        //给pendingTransfers加上notify id
        ctx.pendingTransfers.push_back(PendingTransfer{rankIds[i], true, notifyPoolIndex});
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::LaunchMultiRankBatchLocked(const std::vector<HcommBatchTransferDesc> &transferDescs,
                                                              const std::vector<uint32_t> &rankIds,
                                                              const std::vector<uint32_t> &rankStartIdx,
                                                              const std::vector<uint32_t> &rankListNum,
                                                              const std::vector<HcommThreadHandle> &threads,
                                                              const std::vector<HcommChannelHandle> &channels,
                                                              std::set<uint32_t> &batchRanks)
{
    for (uint32_t rankId : rankIds) {
        if (IsRankFaulted(rankId)) {
            BM_LOG_WARN("device_urma LaunchMultiRankBatchLocked skip faulted rank, localRank: "
                        << rankId_ << " peerRank: " << rankId);
            return BM_NOT_CONNECTED;
        }
    }
    CompletionContext *ctx = LookupOrCreateContextLocked();
    if (ctx == nullptr) {
        BM_LOG_ERROR("device_urma raw multi-rank context creation failed, rankNum: " << rankIds.size());
        return BM_ERROR;
    }
    std::set<uint32_t> currentBatchRanks;
    try {
        currentBatchRanks.insert(rankIds.begin(), rankIds.end());
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("device_urma multi-rank slice rank allocation failed, rankNum: " << rankIds.size());
        return BM_MALLOC_FAILED;
    }
    std::vector<NotifyResource *> notifyResources;
    std::vector<HcommBatchTransferDesc> markerDescs;
    TP_TRACE_BEGIN(TP_HYBM_URMA_LAUNCH_TRANSFER);
    TP_TRACE_BEGIN(TP_HYBM_URMA_PREPARE_MULTI_RANK_MARKERS);
    auto ret = PrepareMultiRankMarkersLocked(rankIds, markerDescs, notifyResources);
    TP_TRACE_END(TP_HYBM_URMA_PREPARE_MULTI_RANK_MARKERS, ret);
    if (ret != BM_OK) {
        TP_TRACE_END(TP_HYBM_URMA_LAUNCH_TRANSFER, ret);
        return ret;
    }
    try {
        ret = StageAndLaunchMultiTransfer(*ctx, transferDescs, 0U, transferDescs.size(), rankIds, rankStartIdx,
                                          rankListNum, threads, channels, &markerDescs, &notifyResources);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("device_urma multi-rank slice launch allocation failed, batchSize: "
                     << transferDescs.size() << " rankNum: " << rankIds.size());
        ret = BM_MALLOC_FAILED;
    }
    TP_TRACE_END(TP_HYBM_URMA_LAUNCH_TRANSFER, ret);
    for (auto *resource : notifyResources) {
        if (resource != nullptr) {
            ReleaseNotifyResource(*resource);
        }
    }
    if (ret == BM_OK) {
        batchRanks.swap(currentBatchRanks);
    }
    return ret;
}

Result DeviceUrmaTransportManager::TransferRemoteBatchAsync(
    const hybm_batch_copy_params &params, hybm_data_copy_direction direction, const RankGroupMap &groupMap,
    std::vector<uint32_t> &localIndices, RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks)
{
    localIndices.clear();
    unregisteredGroups.clear();
    batchRanks.clear();
    std::shared_lock<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    TP_TRACE_BEGIN(TP_HYBM_URMA_VALIDATE_MULTI_RANK_BATCH);
    auto ret = ValidateMultiRankBatchLocked(params, direction, groupMap);
    TP_TRACE_END(TP_HYBM_URMA_VALIDATE_MULTI_RANK_BATCH, ret);
    if (ret != BM_OK) {
        return ret;
    }
    std::vector<HcommBatchTransferDesc> transferDescs;
    std::vector<uint32_t> rankIds, rankStartIdx, rankListNum;
    std::vector<HcommThreadHandle> threads;
    std::vector<HcommChannelHandle> channels;
    ret = ResolveAndFlattenMultiRankBatchLocked(params, direction, groupMap, localIndices, unregisteredGroups,
                                                transferDescs, rankIds, rankStartIdx, rankListNum, threads, channels);
    if (ret != BM_OK || transferDescs.empty()) {
        return ret;
    }
    return LaunchMultiRankBatchLocked(transferDescs, rankIds, rankStartIdx, rankListNum, threads, channels, batchRanks);
}

Result DeviceUrmaTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_ERROR("device_urma uses multirank batchcopy now, WriteRemoteBatchAsync is not supported, rankId: "
                 << rankId << " batchSize: " << descriptor.counts.size());
    return BM_NOT_SUPPORTED;
}

Result DeviceUrmaTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_ERROR("device_urma uses multirank batchcopy now, ReadRemoteBatchAsync is not supported, rankId: "
                 << rankId << " batchSize: " << descriptor.counts.size());
    return BM_NOT_SUPPORTED;
}

aclrtFuncHandle DeviceUrmaTransportManager::GetDeviceKernelFunc() const
{
    return deviceFuncHandles_.batchTransfer;
}

Result DeviceUrmaTransportManager::ReleaseDeviceTransferBuffers(DeviceTransferBuffers &buffers)
{
    if (buffers.base == nullptr) {
        buffers = DeviceTransferBuffers{};
        return BM_OK;
    }

    TP_TRACE_BEGIN(TP_HYBM_URMA_RELEASE_FREE_BUFFER);
    auto ret = DlAclApi::AclrtFree(buffers.base);
    TP_TRACE_END(TP_HYBM_URMA_RELEASE_FREE_BUFFER, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma ReleaseDeviceTransferBuffers AclrtFree failed, base: "
                     << VaToStr(buffers.base) << " capacity: " << buffers.capacity << " ret: " << ret);
        return ret;
    }
    buffers = DeviceTransferBuffers{};
    return BM_OK;
}

Result DeviceUrmaTransportManager::EnsureKernelLaunchBufferCapacity(DeviceTransferBuffers &buffers,
                                                                    size_t requiredBytes)
{
    if (requiredBytes <= buffers.capacity) {
        return BM_OK;
    }
    const size_t maxCapacity = std::numeric_limits<size_t>::max();
    const size_t doubledCapacity = buffers.capacity <= maxCapacity / 2U ? buffers.capacity * 2U : requiredBytes;
    const size_t newCapacity = std::max({HYBM_KERNEL_LAUNCH_BUFFER_MIN_SIZE, requiredBytes, doubledCapacity});
    try {
        buffers.hostBuffer.resize(newCapacity);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("device_urma EnsureKernelLaunchBufferCapacity resize host buffer failed, requiredBytes: "
                     << requiredBytes << " oldCapacity: " << buffers.capacity << " newCapacity: " << newCapacity);
        return BM_MALLOC_FAILED;
    }
    void *newBase = nullptr;
    auto ret = DlAclApi::AclrtMalloc(&newBase, newCapacity, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma EnsureKernelLaunchBufferCapacity AclrtMalloc failed, requiredBytes: "
                     << requiredBytes << " oldCapacity: " << buffers.capacity << " newCapacity: " << newCapacity
                     << " ret: " << ret);
        return ret;
    }
    if (buffers.base != nullptr) {
        ret = DlAclApi::AclrtFree(buffers.base);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma EnsureKernelLaunchBufferCapacity AclrtFree old buffer failed, base: "
                         << VaToStr(buffers.base) << " oldCapacity: " << buffers.capacity << " ret: " << ret);
            auto cleanupRet = DlAclApi::AclrtFree(newBase);
            if (cleanupRet != BM_OK) {
                BM_LOG_ERROR("device_urma EnsureKernelLaunchBufferCapacity cleanup new buffer failed, base: "
                             << VaToStr(newBase) << " newCapacity: " << newCapacity << " ret: " << cleanupRet);
            }
            return ret;
        }
    }
    buffers.base = newBase;
    buffers.capacity = newCapacity;
    return BM_OK;
}

Result DeviceUrmaTransportManager::PrepareKernelLaunchBuffers(
    const std::vector<HcommBatchTransferDesc> &transferDescs, size_t transferOffset, size_t transferCount,
    const std::vector<uint32_t> &rankIds, const std::vector<uint32_t> &rankStartIdx,
    const std::vector<uint32_t> &rankListNum, const std::vector<HcommThreadHandle> &threads,
    const std::vector<HcommChannelHandle> &channels, DeviceTransferBuffers &outBuffers,
    const std::vector<HcommBatchTransferDesc> *markerDescs)
{
    const bool invalidTransferRange =
        transferOffset > transferDescs.size() || transferCount > transferDescs.size() - transferOffset;
    const size_t batchSize = transferCount;
    const size_t rankNum = rankIds.size();
    if (invalidTransferRange || batchSize == 0 || batchSize > HCOMM_BATCH_TRANSFER_MAX_DESC_NUM || rankNum == 0 ||
        rankNum > UINT32_MAX || rankStartIdx.size() != rankNum || rankListNum.size() != rankNum ||
        threads.size() != rankNum || channels.size() != rankNum ||
        (markerDescs != nullptr && markerDescs->size() != rankNum)) {
        BM_LOG_ERROR("device_urma PrepareKernelLaunchBuffers invalid array size, batchSize: "
                     << batchSize << " transferOffset: " << transferOffset
                     << " transferDescCount: " << transferDescs.size() << " rankNum: " << rankNum
                     << " startCount: " << rankStartIdx.size() << " listCount: " << rankListNum.size()
                     << " threadCount: " << threads.size() << " channelCount: " << channels.size()
                     << " markerCount: " << (markerDescs == nullptr ? 0U : markerDescs->size()));
        return BM_INVALID_PARAM;
    }
    for (size_t i = 0; i < rankNum; ++i) {
        const size_t start = rankStartIdx[i];
        const size_t count = rankListNum[i];
        if (start > batchSize || count > batchSize - start) {
            BM_LOG_ERROR("device_urma PrepareKernelLaunchBuffers invalid rank range, rankId: "
                         << rankIds[i] << " start: " << start << " count: " << count << " batchSize: " << batchSize);
            return BM_INVALID_PARAM;
        }
    }

    const size_t descBytes = batchSize * sizeof(HcommBatchTransferDesc);
    const size_t threadBytes = rankNum * sizeof(HcommThreadHandle);
    const size_t channelBytes = rankNum * sizeof(HcommChannelHandle);
    const size_t rankBytes = rankNum * sizeof(uint32_t);
    const size_t markerBytes = markerDescs == nullptr ? 0U : rankNum * sizeof(HcommBatchTransferDesc);
    const size_t totalBytes = descBytes + markerBytes + threadBytes + channelBytes + rankBytes * 3U;
    TP_TRACE_BEGIN(TP_HYBM_URMA_ENSURE_LAUNCH_BUFFER_CAPACITY);
    auto ret = EnsureKernelLaunchBufferCapacity(outBuffers, totalBytes);
    TP_TRACE_END(TP_HYBM_URMA_ENSURE_LAUNCH_BUFFER_CAPACITY, ret);
    if (ret != BM_OK) {
        return ret;
    }

    // Descriptors already contain final HCOMM addresses and transfer types.
    auto *hostBase = outBuffers.hostBuffer.data();
    auto *deviceBase = static_cast<uint8_t *>(outBuffers.base);
    outBuffers.transferDescs = deviceBase;
    std::memcpy(hostBase, transferDescs.data() + transferOffset, descBytes);
    outBuffers.markerDescs = nullptr;

    // Rank-level section: handles and ranges.
    size_t offset = descBytes;
    auto append = [&](void *&deviceView, const void *data, size_t bytes) {
        deviceView = deviceBase + offset;
        std::memcpy(hostBase + offset, data, bytes);
        offset += bytes;
    };
    if (markerDescs != nullptr) {
        append(outBuffers.markerDescs, markerDescs->data(), markerBytes);
    }
    append(outBuffers.threadList, threads.data(), threadBytes);
    append(outBuffers.channelList, channels.data(), channelBytes);
    append(outBuffers.rankIdList, rankIds.data(), rankBytes);
    append(outBuffers.rankStartIdxList, rankStartIdx.data(), rankBytes);
    append(outBuffers.rankListNumList, rankListNum.data(), rankBytes);

    TP_TRACE_BEGIN(TP_HYBM_URMA_COPY_LAUNCH_BUFFER_H2D);
    ret = DlAclApi::AclrtMemcpy(outBuffers.base, outBuffers.capacity, hostBase, totalBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    TP_TRACE_END(TP_HYBM_URMA_COPY_LAUNCH_BUFFER_H2D, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma PrepareKernelLaunchBuffers AclrtMemcpy failed, totalBytes: "
                     << totalBytes << " capacity: " << outBuffers.capacity << " batchSize: " << batchSize
                     << " rankNum: " << rankNum << " ret: " << ret);
    }
    return ret;
}

Result DeviceUrmaTransportManager::LaunchDeviceKernelBatch(const DeviceTransferBuffers &buffers, size_t batchSize,
                                                           size_t rankNum)
{
    HybmBatchTransferParam args{};
    args.rank_num = static_cast<uint32_t>(rankNum);
    args.rank_id_list = static_cast<uint32_t *>(buffers.rankIdList);
    args.rank_start_idx_list = static_cast<uint32_t *>(buffers.rankStartIdxList);
    args.rank_list_num_list = static_cast<uint32_t *>(buffers.rankListNumList);
    args.thread_list = static_cast<HcommThreadHandle *>(buffers.threadList);
    args.channel_list = static_cast<HcommChannelHandle *>(buffers.channelList);
    args.total_list_num = static_cast<uint32_t>(batchSize);
    args.transfer_descs = static_cast<HcommBatchTransferDesc *>(buffers.transferDescs);
    args.marker_descs = static_cast<HcommBatchTransferDesc *>(buffers.markerDescs);
    aclrtArgsHandle argsHandle = nullptr;
    auto funcHandle = GetDeviceKernelFunc();
    auto ret = DlAclApi::AclrtKernelArgsInit(funcHandle, &argsHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsInit failed, ret: " << ret);
        return ret;
    }
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, &args, sizeof(args), &paramHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsAppend failed, ret: " << ret);
        return ret;
    }
    ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtKernelArgsFinalize failed, ret: " << ret);
        return ret;
    }
    void *stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch GetThreadAclStream failed");
        return BM_DL_FUNCTION_FAILED;
    }

    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = HYBM_NOTIFY_DEFAULT_WAIT_TIME_S;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;

    TP_TRACE_BEGIN(TP_HYBM_URMA_KERNEL_LAUNCH);
    ret = DlAclApi::AclrtLaunchKernelWithConfig(funcHandle, HYBM_DEVICE_KERNEL_BLOCK_DIM, stream, &cfg, argsHandle,
                                                nullptr);
    TP_TRACE_END(TP_HYBM_URMA_KERNEL_LAUNCH, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma LaunchDeviceKernelBatch AclrtLaunchKernelWithConfig failed, kernel: "
                     << HYBM_DEVICE_FUNC_TRANSFER << " ret: " << ret);
        return ret;
    }

    TP_TRACE_BEGIN(TP_HYBM_URMA_KERNEL_LAUNCH_SYNC_STREAM);
    ret = DlAclApi::AclrtSynchronizeStream(stream);
    TP_TRACE_END(TP_HYBM_URMA_KERNEL_LAUNCH_SYNC_STREAM, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("device_urma launch kernel AclrtSynchronizeStream failed, ret: " << ret);
        return ret;
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::Synchronize(uint32_t rankId)
{
    return SynchronizeRanks({rankId});
}

Result DeviceUrmaTransportManager::SynchronizePreSubmittedNotifiesLocked(CompletionContext &ctx,
                                                                         const std::set<uint32_t> &rankIds)
{
    std::vector<PendingTransfer *> targets;
    for (auto &pending : ctx.pendingTransfers) {
        // 数据kernel已经下发并且有notify
        if (rankIds.count(pending.rankId) != 0 && pending.inFlight && pending.notifyPoolIndex != UINT32_MAX) {
            targets.push_back(&pending);
        }
    }
    Result waitRet = BM_OK;
    //记录是否有waitnotify排入stream
    bool waitQueued = false;
    for (auto *pending : targets) {
        const uint32_t index = pending->notifyPoolIndex;
        const uint32_t poolSize = notifyPoolSize_.load(std::memory_order_acquire);
        if (notifyPoolSlots_ == nullptr || index >= poolSize || notifyPoolSlots_[index] == nullptr) {
            BM_LOG_ERROR("device_urma invalid pending notify, rankId: " << pending->rankId << " index: " << index
                                                                        << " poolSize: " << poolSize);
            for (auto *target : targets) {
                target->notifyPoolIndex = UINT32_MAX;
            }
            return BM_ERROR;
        }
        // 取回notify
        auto *resource = notifyPoolSlots_[index];
        TP_TRACE_BEGIN(TP_HYBM_URMA_WAIT_RESET_NOTIFY);
        auto ret = DlAclApi::AclrtWaitAndResetNotify(resource->notify, ctx.stream, HYBM_DEVICE_KERNEL_TIMEOUT_S);
        TP_TRACE_END(TP_HYBM_URMA_WAIT_RESET_NOTIFY, ret);
        if (ret != BM_OK) {
            BM_LOG_ERROR("device_urma batch notify wait enqueue failed, rankId: "
                         << pending->rankId << " notifyId: " << resource->notifyId << " ret: " << ret);
            waitRet = waitRet == BM_OK ? ret : waitRet;
        } else {
            //至少一个入队
            waitQueued = true;
        }
    }
    Result syncRet = BM_OK;
    if (waitQueued) {
        TP_TRACE_BEGIN(TP_HYBM_URMA_SYNC_STREAM);
        syncRet = DlAclApi::AclrtSynchronizeStream(ctx.stream);
        TP_TRACE_END(TP_HYBM_URMA_SYNC_STREAM, syncRet);
    }
    // stream 因 peer 故障被中止：归一化为可降级错误，避免上层把 107023 当致命错误处理
    if (syncRet == ACL_ERROR_RT_STREAM_ABORT) {
        BM_LOG_WARN("device_urma stream aborted on peer fault, treat as recoverable, localRank: "
                    << rankId_ << " rankNum: " << rankIds.size());
        syncRet = BM_TIMEOUT;
    }
    if (waitRet == ACL_ERROR_RT_STREAM_ABORT) {
        waitRet = BM_TIMEOUT;
    }
    if (waitRet != BM_OK || syncRet != BM_OK) {
        //入队失败or同步失败
        for (auto *pending : targets) {
            // notify做隔离，到closedevice释放
            pending->notifyPoolIndex = UINT32_MAX;
        }
        BM_LOG_ERROR("device_urma batch notify synchronize failed, rankNum: " << rankIds.size() << " waitRet: "
                                                                              << waitRet << " syncRet: " << syncRet);
        return waitRet != BM_OK ? waitRet : syncRet;
    }
    for (auto *pending : targets) {
        //同步成功则pending解除记录+notify归还
        auto *resource = notifyPoolSlots_[pending->notifyPoolIndex];
        pending->notifyPoolIndex = UINT32_MAX;
        ReleaseNotifyResource(*resource);
    }
    for (uint32_t rankId : rankIds) {
        std::vector<PendingTransfer> completed;
        TP_TRACE_BEGIN(TP_HYBM_URMA_EXTRACT_RANK_PENDING);
        ExtractRankPending(ctx.pendingTransfers, rankId, completed);
        TP_TRACE_END(TP_HYBM_URMA_EXTRACT_RANK_PENDING, BM_OK);
        TP_TRACE_BEGIN(TP_HYBM_URMA_RELEASE_PENDING_TRANSFERS);
        auto releaseRet = ReleasePendingTransfersLocked(completed);
        TP_TRACE_END(TP_HYBM_URMA_RELEASE_PENDING_TRANSFERS, releaseRet);
    }
    return BM_OK;
}

Result DeviceUrmaTransportManager::SynchronizeRanks(const std::set<uint32_t> &rankIds)
{
    // 故障 rank 直接快速失败；在取锁前判定，避免与正在退出的阻塞线程抢 shared_lock
    for (uint32_t rankId : rankIds) {
        if (IsRankFaulted(rankId)) {
            BM_LOG_WARN("device_urma SynchronizeRanks skip faulted rank, localRank: " << rankId_
                                                                                      << " peerRank: " << rankId);
            return BM_NOT_CONNECTED;
        }
    }
    std::shared_lock<std::shared_mutex> guard(mutex_);
    BM_VALIDATE_RETURN(opened_, "device_urma transport manager is not opened", BM_ERROR);
    for (uint32_t rankId : rankIds) {
        if (remoteRanks_.find(rankId) == remoteRanks_.end()) {
            BM_LOG_ERROR("device_urma SynchronizeRanks rank not found: " << rankId);
            return BM_NOT_CONNECTED;
        }
    }
    auto &ctx = GetTlsContext();
    if (!ctx.initialized) {
        return BM_OK;
    }
    for (uint32_t rankId : rankIds) {
        for (const auto &pending : ctx.pendingTransfers) {
            if (pending.rankId != rankId) {
                continue;
            }
            if (!pending.inFlight || pending.notifyPoolIndex == UINT32_MAX) {
                BM_LOG_ERROR("device_urma SynchronizeRanks invalid pending, rankId: "
                             << rankId << " inFlight: " << pending.inFlight
                             << " notifyPoolIndex: " << pending.notifyPoolIndex);
                return BM_ERROR;
            }
        }
    }
    TP_TRACE_BEGIN(TP_HYBM_URMA_SYNC_RANK_PENDING);
    auto ret = SynchronizePreSubmittedNotifiesLocked(ctx, rankIds);
    TP_TRACE_END(TP_HYBM_URMA_SYNC_RANK_PENDING, ret);
    return ret;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
