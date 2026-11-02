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
#include "device_rdma_aicpu_transport_manager.h"

#include <cstdint>
#include <mutex>
#include <vector>

#include "hybm_common_include.h"
#include "dl_acl_api.h"
#include "dl_rt_api.h"
#include "aicpu_joinable_ranks_qp_manager.h"
#include "hybm_kernel_hepler.h"
#include "hybm_stream_manager.h"
#include "hybm_gva.h"
#include "hybm_ptracer.h"
#include "device_nic_helper.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

constexpr uint32_t DB_DIE_ID_MASK = 0x00FF0000U;
constexpr uint32_t DB_DIE_ID_SHIFT = 16U;
constexpr uint32_t ACL_STREAM_DEVICE_USE_ONLY = 0x00000020U;
constexpr unsigned long RT_ASCEND910B1_ROCEE_BASE_ADDR = 0x2000000000UL;
constexpr unsigned long RT_ASCEND910B1_ROCEE_VF_DB_CFG0_REG = 0x230UL;

thread_local HybmAclNotifyPtr RdmaAicpuTransportManager::aicpuNotify_ = nullptr;
thread_local RdmaAicpuTransportManager::AicpuPendingBatch RdmaAicpuTransportManager::aicpuPending_ = {};
thread_local AicpuStream RdmaAicpuTransportManager::aicpuStream_ = {};

namespace {
class DeviceBuffer {
public:
    ~DeviceBuffer()
    {
        if (dev_ != nullptr) {
            DlAclApi::AclrtFree(dev_);
        }
    }

    bool Ensure(size_t bytes)
    {
        if (bytes <= cap_) {
            return true;
        }
        Release();
        if (DlAclApi::AclrtMalloc(&dev_, bytes, 0U) != BM_OK) {
            dev_ = nullptr;
            return false;
        }
        cap_ = bytes;
        return true;
    }

    void *Device() const
    {
        return dev_;
    }

private:
    void Release()
    {
        if (dev_ != nullptr) {
            DlAclApi::AclrtFree(dev_);
            dev_ = nullptr;
        }
        cap_ = 0U;
    }

    void *dev_{nullptr};
    size_t cap_{0U};
};

int CreateAicpuStream(AicpuStream &as)
{
    void *stream = nullptr;
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&stream, 0, ACL_STREAM_DEVICE_USE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: create doorbell acl stream failed, ret: " << ret);
        return ret;
    }
    uint32_t sqId = 0U;
    ret = DlRtApi::RtStreamGetSqid(stream, &sqId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: get doorbell sqid failed, ret: " << ret);
        DlAclApi::AclrtDestroyStream(stream);
        return ret;
    }
    int32_t streamId = 0;
    ret = DlAclApi::AclrtStreamGetId(stream, &streamId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: get doorbell stream id failed, sqId: " << sqId << " ret: " << ret);
        DlAclApi::AclrtDestroyStream(stream);
        return ret;
    }
    as.stream = stream;
    as.sqId = sqId;
    as.streamId = static_cast<uint32_t>(streamId);
    BM_LOG_DEBUG("aicpu rdma: doorbell acl stream ready, stream: " << as.stream << " streamId: " << as.streamId
                                                                   << " sqId: " << as.sqId);
    return BM_OK;
}
} // namespace

RdmaAicpuTransportManager::~RdmaAicpuTransportManager()
{
    aicpuNotify_ = nullptr;
}

Result RdmaAicpuTransportManager::InitDeviceContext(const TransportOptions &options, int32_t &userId)
{
    int32_t logicId = -1;
    int32_t phyId = -1;
    auto ret = DlAclApi::AclrtGetDevice(&userId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0 && userId >= 0,
                             "AclrtGetDevice() return=" << ret << ", output deviceId=" << userId,
                             BM_DL_FUNCTION_FAILED);

    ret = DlAclApi::RtGetLogicDevIdByUserDevId(userId, &logicId);
    BM_ASSERT_LOG_AND_RETURN(ret == 0 && logicId >= 0,
                             "RtGetLogicDevIdByUserDevId() return=" << ret << ", output deviceId=" << logicId,
                             BM_DL_FUNCTION_FAILED);

    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userId, &phyId);
    if (ret != 0) {
        BM_LOG_WARN("aclrtGetPhyDevIdByLogicDevId ret=" << ret << " use logicId=" << logicId);
        phyId = logicId;
    }
    BM_LOG_DEBUG("aclrtGetPhyDevIdByLogicDevId: userId=" << userId << ", logicId=" << logicId << ", phyId=" << phyId);
    deviceId_ = static_cast<uint32_t>(phyId);
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    qpReadyCache_.reset(new std::atomic<bool>[rankCount_]());
    role_ = options.role;
    scene_ = options.scene;
    ret = ParseDeviceNic(options.nic, devicePort_);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "parse input nic(" << options.nic << ") failed!", BM_INVALID_PARAM);
    return BM_OK;
}

Result RdmaAicpuTransportManager::OpenDevice(const TransportOptions &options)
{
    int32_t userId = -1;
    std::unique_lock<std::mutex> unique_lock(mutex_);
    BM_LOG_DEBUG("begin to open device with " << options);
    auto ret = InitDeviceContext(options, userId);
    if (ret != BM_OK) {
        return ret;
    }

    if (!PrepareOpenDevice(userId, deviceId_, rankCount_, deviceIp_, rdmaHandle_)) {
        BM_LOG_ERROR("PrepareOpenDevice failed.");
        return BM_ERROR;
    }

    nicInfo_ = GenerateDeviceNic(deviceIp_, devicePort_);
    BM_ASSERT_LOG_AND_RETURN(
        !nicInfo_.empty(),
        "GenerateDeviceNic failed, deviceIp=" << DescribeIPv4(deviceIp_) << ", devicePort=" << devicePort_, BM_ERROR);

    sockaddr_in deviceAddr{};
    deviceAddr.sin_family = AF_INET;
    deviceAddr.sin_addr = deviceIp_;
    deviceAddr.sin_port = devicePort_;
    BM_LOG_INFO("use aicpu joinable ranks qp manager, rankId: " << rankId_ << " rankCount: " << rankCount_);
    qpManager_ =
        std::make_shared<AicpuJoinableRanksQpManager>(userId, deviceId_, rankId_, rankCount_, deviceAddr, scene_);

    auto loadRet = LoadDeviceKernelAndGetHandles(aicpuRdmaBin_, aicpuRdmaPostSend_, "HybmRdmaAicpuPostSend");
    if (loadRet != BM_OK) {
        BM_LOG_ERROR("load rdma aicpu kernel failed, rankId: " << rankId_ << " ret: " << loadRet);
        return loadRet;
    }
    BM_LOG_INFO("aicpu rdma enabled, rankId: " << rankId_);

    auto rtRet = DlRtApi::LoadLibrary();
    if (rtRet != BM_OK) {
        BM_LOG_ERROR("load rt api failed, rankId: " << rankId_ << " ret: " << rtRet);
        return rtRet;
    }

    deviceChipInfo_ = std::make_shared<DeviceChipInfo>(userId);
    ret = deviceChipInfo_->Init();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "device info init failed: " << ret, ret);

    ret = InitStreamNotifyBuf();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "notify init failed: " << ret, ret);
    ranksMRs_.resize(rankCount_);
    rankMutex_ = std::vector<std::mutex>(rankCount_);
    aicpuMaxSendNum_ = static_cast<AicpuJoinableRanksQpManager *>(qpManager_.get())->GetSendWrDepth() - UN40;
    BM_LOG_TRACE("open device with " << options << " success, aicpuMaxSendNum_:" << aicpuMaxSendNum_);
    return BM_OK;
}

Result RdmaAicpuTransportManager::CloseDevice()
{
    aicpuNotify_ = nullptr;
    aicpuPending_ = AicpuPendingBatch{};
    if (aicpuStream_.stream != nullptr) {
        DlAclApi::AclrtDestroyStream(aicpuStream_.stream);
        aicpuStream_.stream = nullptr;
        aicpuStream_.sqId = 0U;
        aicpuStream_.streamId = 0U;
    }
    return RdmaTransportManager::CloseDevice();
}

Result RdmaAicpuTransportManager::Synchronize(uint32_t rankId)
{
    if (!aicpuPending_.pending) {
        return BM_OK;
    }
    if (aicpuNotify_ == nullptr || !aicpuNotify_->Inited()) {
        BM_LOG_ERROR("aicpu rdma: notify not ready, a batch is still pending, rankId: " << rankId);
        return BM_ERROR;
    }
    auto ret = aicpuNotify_->Wait();
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: wait notify failed, rankId: " << rankId << " notifyId: "
                                                                << aicpuNotify_->GetNotifyId() << " ret: " << ret);
        (void)ReadAndCheckAicpuOut(rankId);
        return ret;
    }
    aicpuPending_.pending = false;
    return BM_OK;
}

int RdmaAicpuTransportManager::ReadAndCheckAicpuOut(uint32_t rankId)
{
    aicpu::RdmaAicpuOut out{};
    auto ret = DlAclApi::AclrtMemcpy(&out, sizeof(out), reinterpret_cast<void *>(aicpuPending_.outAddr), sizeof(out),
                                     ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: read back out buffer failed, devOut: " << aicpuPending_.outAddr << " ret: " << ret);
        return ret;
    }
    if (out.doorbellRet != BM_OK || out.posted != aicpuPending_.expectedPosted) {
        BM_LOG_ERROR("aicpu rdma: device doorbell failed, rankId: "
                     << rankId << " sqId: " << aicpuPending_.sqId << " streamId: " << aicpuPending_.sqStreamId
                     << " devId: " << aicpuPending_.devId << " localDevId: " << out.localDevId << " sqBase: 0x"
                     << std::hex << out.sqBaseAddr << std::dec << " sqDepth: " << out.sqDepth
                     << " sqTail: " << out.sqTail << " doorbellRet: " << out.doorbellRet << " posted: " << out.posted
                     << " expected: " << aicpuPending_.expectedPosted);
        return BM_ERROR;
    }
    return BM_OK;
}

int RdmaAicpuTransportManager::RemoteIO(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write,
                                        bool sync)
{
    if (qpManager_ == nullptr) {
        BM_LOG_ERROR("aicpu RemoteIO(): connection manager not created.");
        return BM_ERROR;
    }
    BM_ASSERT_LOG_AND_RETURN(rankId < rankMutex_.size(), "rankId out of range, rankId: " << rankId, BM_ERROR);

    CopyDescriptor descriptor{};
    descriptor.localAddrs.push_back(reinterpret_cast<void *>(lAddr));
    descriptor.globalAddrs.push_back(reinterpret_cast<void *>(rAddr));
    descriptor.counts.push_back(size);

    std::vector<AicpuSegment> segments;
    auto ret = ResolveAicpuBatchAddresses(rankId, descriptor, segments);
    if (ret != BM_OK) {
        return ret;
    }
    if (segments.empty()) {
        return BM_OK;
    }

    ret = RemoteIoBatchAicpu(rankId, segments, write);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: io failed, rankId: " << rankId << " size: " << size << " write: " << write
                                                       << " ret: " << ret);
        return ret;
    }
    if (sync) {
        ret = Synchronize(rankId);
    }
    return ret;
}

Result RdmaAicpuTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    if (qpManager_ == nullptr) {
        BM_LOG_ERROR("ReadRemoteBatchAsync(): connection manager not created.");
        return BM_ERROR;
    }
    if (descriptor.counts.empty()) {
        return BM_OK;
    }
    BM_ASSERT_LOG_AND_RETURN(rankId < rankMutex_.size(), "rankId out of range, rankId: " << rankId, BM_INVALID_PARAM);
    std::vector<AicpuSegment> segments;
    auto ret = ResolveAicpuBatchAddresses(rankId, descriptor, segments);
    if (ret != BM_OK) {
        return ret;
    }
    if (segments.empty()) {
        return BM_OK;
    }
    TP_TRACE_BEGIN(TP_HYBM_DEV_RDMA_AICPU_BATCH_IO);
    ret = RemoteIoBatchAicpu(rankId, segments, false);
    TP_TRACE_END(TP_HYBM_DEV_RDMA_AICPU_BATCH_IO, ret);
    return ret;
}

Result RdmaAicpuTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    if (qpManager_ == nullptr) {
        BM_LOG_ERROR("WriteRemoteBatchAsync(): connection manager not created.");
        return BM_ERROR;
    }
    if (descriptor.counts.empty()) {
        return BM_OK;
    }
    BM_ASSERT_LOG_AND_RETURN(rankId < rankMutex_.size(), "rankId out of range, rankId: " << rankId, BM_INVALID_PARAM);
    std::vector<AicpuSegment> segments;
    auto ret = ResolveAicpuBatchAddresses(rankId, descriptor, segments);
    if (ret != BM_OK) {
        return ret;
    }
    if (segments.empty()) {
        return BM_OK;
    }
    TP_TRACE_BEGIN(TP_HYBM_DEV_RDMA_AICPU_BATCH_IO);
    ret = RemoteIoBatchAicpu(rankId, segments, true);
    TP_TRACE_END(TP_HYBM_DEV_RDMA_AICPU_BATCH_IO, ret);
    return ret;
}

int RdmaAicpuTransportManager::ResolveAicpuBatchAddresses(uint32_t rankId, const CopyDescriptor &descriptor,
                                                          std::vector<AicpuSegment> &segments)
{
    const auto batchSize = descriptor.counts.size();
    segments.clear();
    segments.reserve(batchSize);

    ReadGuard lockGuard(lock_);
    for (size_t i = 0; i < batchSize; ++i) {
        const uint64_t lAddr = reinterpret_cast<uint64_t>(descriptor.localAddrs[i]);
        const uint64_t rAddr = reinterpret_cast<uint64_t>(descriptor.globalAddrs[i]);
        const uint64_t size = descriptor.counts[i];
        if (size == 0) {
            continue;
        }
        AicpuSegment seg{};
        uint64_t localAddr = 0;
        uint32_t lkey = 0;
        auto ret = GetRegAddress(registerMRS_, lAddr, size, true, localAddr, lkey);
        if (ret != BM_OK) {
            BM_LOG_ERROR("aicpu rdma batch: local addr not registered, rankId: "
                         << rankId << " index: " << i << std::hex << " lAddr: 0x" << lAddr << std::dec);
            return ret;
        }
        if (rankId >= ranksMRs_.size() || ranksMRs_[rankId].empty()) {
            BM_LOG_ERROR("aicpu rdma batch: remote rank not prepared, rankId: " << rankId << " index: " << i);
            return BM_INVALID_PARAM;
        }
        uint64_t remoteAddr = 0;
        uint32_t rkey = 0;
        ret = GetRegAddress(ranksMRs_[rankId], rAddr, size, false, remoteAddr, rkey);
        if (ret != BM_OK) {
            BM_LOG_ERROR("aicpu rdma batch: remote addr not registered, rankId: "
                         << rankId << " index: " << i << std::hex << " rAddr: 0x" << rAddr << std::dec);
            return ret;
        }
        seg.localAddr = localAddr;
        seg.remoteAddr = remoteAddr;
        seg.len = size;
        seg.localLkey = lkey;
        seg.remoteRkey = rkey;
        segments.push_back(seg);
    }
    return BM_OK;
}

int RdmaAicpuTransportManager::RunAicpuIo(uint32_t rankId, void *stream, aicpu::RdmaAicpuParam &param,
                                          uint32_t expectedPosted)
{
    auto ret = LaunchAicpuKernel(aicpuRdmaPostSend_, stream, &param, sizeof(param));
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: launch post task failed, rankId: " << rankId << " descCount: " << param.descCount
                                                                     << " ret: " << ret);
        return ret;
    }
    // 只下发不等待：notify 尾段由算子提交，等待放到 Synchronize()，与算子在同一条 ACL 流上一次同步
    aicpuPending_.outAddr = param.outAddr;
    aicpuPending_.expectedPosted = expectedPosted;
    aicpuPending_.sqId = param.sqId;
    aicpuPending_.sqStreamId = param.sqStreamId;
    aicpuPending_.devId = param.devId;
    aicpuPending_.pending = true;
    return BM_OK;
}

// doorbell 地址只取决于该 rank 的 dbIndex 与芯片/裸片信息。
int RdmaAicpuTransportManager::ComputeDoorbellAddr(uint32_t rankId, uint64_t &dbAddr)
{
    if (rankId >= rankCount_) {
        BM_LOG_ERROR("aicpu rdma: rankId out of range, rankId: " << rankId);
        return BM_ERROR;
    }
    if (deviceChipInfo_ == nullptr) {
        BM_LOG_ERROR("aicpu rdma: deviceChipInfo_ is nullptr, rankId: " << rankId);
        return BM_ERROR;
    }
    auto *aiMgr = dynamic_cast<AicpuJoinableRanksQpManager *>(qpManager_.get());
    if (aiMgr == nullptr) {
        BM_LOG_ERROR("aicpu rdma: not an AI QP manager, no dbIndex, rankId: " << rankId);
        return BM_ERROR;
    }
    const uint32_t dbIndex = aiMgr->GetDbIndex(rankId);
    if (dbIndex == AicpuJoinableRanksQpManager::INVALID_DB_INDEX) {
        BM_LOG_ERROR("aicpu rdma: db index not found, rankId: " << rankId);
        return BM_ERROR;
    }
    const uint32_t dieId = (dbIndex & DB_DIE_ID_MASK) >> DB_DIE_ID_SHIFT;
    dbAddr = RT_ASCEND910B1_ROCEE_BASE_ADDR + RT_ASCEND910B1_ROCEE_VF_DB_CFG0_REG +
             deviceChipInfo_->GetChipOffset() * static_cast<uint64_t>(deviceChipInfo_->GetChipId()) +
             deviceChipInfo_->GetDieOffset() * dieId + deviceChipInfo_->GetChipAddr();
    if (dbAddr == 0ULL) {
        BM_LOG_ERROR("aicpu rdma: invalid doorbell address, rankId: " << rankId << " dbIndex: " << dbIndex);
        return BM_ERROR;
    }
    return BM_OK;
}

int RdmaAicpuTransportManager::FillDoorbellParam(uint32_t rankId, aicpu::RdmaAicpuParam &param)
{
    if (aicpuStream_.stream == nullptr) {
        auto createRet = CreateAicpuStream(aicpuStream_);
        if (createRet != BM_OK) {
            BM_LOG_ERROR("aicpu rdma: doorbell stream unavailable, rankId: " << rankId << " ret: " << createRet);
            return createRet;
        }
    }

    uint64_t doorbellAddr = 0U;
    auto ret = ComputeDoorbellAddr(rankId, doorbellAddr);
    if (ret != BM_OK) {
        return ret;
    }

    param.doorbellAddr = doorbellAddr;
    param.sqId = aicpuStream_.sqId;
    param.tsId = 0U;
    param.devId = static_cast<uint32_t>(HybmGetInitedLogicDeviceId());
    param.sqStreamId = aicpuStream_.streamId;
    BM_LOG_DEBUG("aicpu rdma doorbell param, rankId: " << rankId << " sqId: " << aicpuStream_.sqId << " streamId: "
                                                       << aicpuStream_.streamId << " devId: " << param.devId
                                                       << " dbAddr: 0x" << std::hex << doorbellAddr << std::dec);
    return BM_OK;
}

int RdmaAicpuTransportManager::EnsureAicpuNotify()
{
    if (aicpuNotify_ != nullptr && aicpuNotify_->Inited()) {
        return BM_OK;
    }
    auto stream = HybmStreamManager::GetThreadAclStream();
    BM_ASSERT_LOG_AND_RETURN(stream != nullptr, "aicpu rdma: get thread acl stream failed", BM_ERROR);

    auto notify = std::make_shared<HybmAclNotify>(stream);
    BM_ASSERT_LOG_AND_RETURN(notify != nullptr, "aicpu rdma: create notify failed", BM_ERROR);
    auto ret = notify->Init(notifyInfo_.notifyAddr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma: notify init failed, blockBase: 0x" << std::hex << notifyInfo_.notifyAddr << std::dec
                                                                     << " ret: " << ret);
        return ret;
    }
    aicpuNotify_ = notify;
    return BM_OK;
}

int RdmaAicpuTransportManager::BuildBatchParam(uint32_t rankId, const std::vector<AicpuSegment> &segments,
                                               uint64_t qpHandle, void *deviceSegments, bool write,
                                               aicpu::RdmaAicpuParam &param)
{
    thread_local DeviceBuffer outBuffer;
    constexpr size_t OUT_BYTES = sizeof(aicpu::RdmaAicpuOut);
    if (!outBuffer.Ensure(OUT_BYTES)) {
        BM_LOG_ERROR("aicpu rdma: alloc out buffer failed, rankId: " << rankId);
        return BM_MALLOC_FAILED;
    }
    const uint32_t count = static_cast<uint32_t>(segments.size());
    const auto &remoteMr = notifyRemoteInfo_[rankId];
    param.outAddr = reinterpret_cast<uint64_t>(outBuffer.Device());
    param.abiVersion = aicpu::HYBM_RDMA_AICPU_ABI_VERSION;
    param.qpHandle = qpHandle;
    param.opcode = write ? aicpu::HYBM_RDMA_OP_WRITE : aicpu::HYBM_RDMA_OP_READ;
    param.descAddr = reinterpret_cast<uint64_t>(deviceSegments);
    param.descCount = count;
    // 本批 WQE 的 wrId 基准（数据 count 个 + notify 1 个），按批偏移保证全局唯一
    param.wrId = wrIdx_.fetch_add(static_cast<uint64_t>(count) + 1U);
    param.notifyLen = notifyInfo_.len;
    param.notifyLocalLkey = notifyInfo_.notifyLkey;
    param.notifyRemoteRkey = remoteMr.second;
    param.notifyLocalAddr = aicpuNotify_->GetRegAddr();
    param.notifyRemoteAddr = remoteMr.first;
    return FillDoorbellParam(rankId, param);
}

int RdmaAicpuTransportManager::RemoteIoBatchAicpu(uint32_t rankId, const std::vector<AicpuSegment> &segments,
                                                  bool write)
{
    BM_ASSERT_LOG_AND_RETURN(rankId < rankMutex_.size(), "aicpu rdma batch: rankId out of range, rankId: " << rankId,
                             BM_INVALID_PARAM);
    std::unique_lock<std::mutex> rankLock(rankMutex_[rankId]);
    uint32_t remain = static_cast<uint32_t>(segments.size());
    uint32_t offset = 0U;
    while (remain > 0U) {
        const uint32_t chunkSize = aicpuMaxSendNum_ < remain ? aicpuMaxSendNum_ : remain;
        const uint32_t end = offset + chunkSize;
        const std::vector<AicpuSegment> chunk(segments.begin() + offset, segments.begin() + end);
        auto ret = SubmitAicpuBatchChunk(rankId, chunk, write);
        if (ret != BM_OK) {
            BM_LOG_ERROR("aicpu rdma batch: chunk failed, rankId: " << rankId << " offset: " << offset
                                                                    << " chunkCount: " << chunkSize << " count: "
                                                                    << segments.size() << " ret: " << ret);
            return ret;
        }
        remain -= chunkSize;
        offset += chunkSize;
    }
    // 等待完成，因为kernel执行是异步的，如果这里不同步的话，会出现多线程操作一个QP的问题，当前只能退化成同步调用
    const auto syncRet = Synchronize(rankId);
    if (syncRet != BM_OK) {
        BM_LOG_ERROR("aicpu rdma batch: wait last chunk failed, rankId: " << rankId << " count: " << segments.size()
                                                                          << " ret: " << syncRet);
        return syncRet;
    }
    return BM_OK;
}

int RdmaAicpuTransportManager::SubmitAicpuBatchChunk(uint32_t rankId, const std::vector<AicpuSegment> &segments,
                                                     bool write)
{
    const auto count = static_cast<uint32_t>(segments.size());
    if (count == 0U) {
        return BM_OK;
    }
    BM_ASSERT_LOG_AND_RETURN(Synchronize(rankId) == BM_OK, "aicpu rdma: wait last pending batch failed", BM_ERROR);
    uint64_t qpHandle = qpManager_->GetDeviceQpAddr(rankId);
    if (qpHandle == 0U) {
        BM_LOG_ERROR("aicpu rdma batch: device qp addr is 0, rankId: " << rankId);
        return BM_ERROR;
    }

    void *deviceSegments = nullptr;
    auto ret = StageBatchSegments(segments, deviceSegments);
    if (ret != BM_OK) {
        return ret;
    }

    auto stream = HybmStreamManager::GetThreadAclStream();
    if (stream == nullptr) {
        BM_LOG_ERROR("aicpu rdma batch: get thread acl stream failed, rankId: " << rankId << " count: " << count);
        return BM_ERROR;
    }

    // notify 尾段：把固定的完成 notify READ 一并交给算子（一次 kernel 下发"数据 + notify"），
    ret = EnsureAicpuNotify();
    if (ret != BM_OK) {
        return ret;
    }
    if (rankId >= notifyRemoteInfo_.size() || notifyRemoteInfo_[rankId].second == 0U) {
        BM_LOG_ERROR("aicpu rdma batch: remote notify source not ready, rankId: " << rankId);
        return BM_ERROR;
    }

    aicpu::RdmaAicpuParam param{};
    ret = BuildBatchParam(rankId, segments, qpHandle, deviceSegments, write, param);
    if (ret != BM_OK) {
        return ret;
    }

    ret = LaunchAicpuBatch(rankId, stream, param, count);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma batch: io failed, rankId: " << rankId << " count: " << count << " ret: " << ret);
        return ret;
    }
    BM_LOG_DEBUG("aicpu rdma batch done, rankId: " << rankId << " count: " << count << " write: " << write);
    return BM_OK;
}

int RdmaAicpuTransportManager::LaunchAicpuBatch(uint32_t rankId, void *stream, aicpu::RdmaAicpuParam &param,
                                                uint32_t count)
{
    // rankMutex_[rankId] 由调用方 RemoteIoBatchAicpu 持有到该批完成，这里不再加锁（std::mutex 非递归）
    TP_TRACE_BEGIN(TP_HYBM_DEV_RDMA_AICPU_LAUNCH);
    auto ret = RunAicpuIo(rankId, stream, param, count + 1U);
    TP_TRACE_END(TP_HYBM_DEV_RDMA_AICPU_LAUNCH, ret);
    return ret;
}

int RdmaAicpuTransportManager::StageBatchSegments(const std::vector<AicpuSegment> &segments, void *&deviceSegments)
{
    const size_t bytes = segments.size() * sizeof(AicpuSegment);
    thread_local DeviceBuffer buffer;
    if (!buffer.Ensure(bytes)) {
        BM_LOG_ERROR("aicpu rdma batch: alloc segment buffer failed, count: " << segments.size()
                                                                              << " bytes: " << bytes);
        return BM_MALLOC_FAILED;
    }

    auto ret = DlAclApi::AclrtMemcpy(buffer.Device(), bytes, segments.data(), bytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aicpu rdma batch: copy segments to device failed, count: " << segments.size() << " bytes: "
                                                                                 << bytes << " ret: " << ret);
        return ret;
    }
    deviceSegments = buffer.Device();
    return BM_OK;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
