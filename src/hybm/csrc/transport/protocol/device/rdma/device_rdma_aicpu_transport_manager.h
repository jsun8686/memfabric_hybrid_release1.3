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

#ifndef MF_HYBRID_DEVICE_RDMA_AICPU_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_RDMA_AICPU_TRANSPORT_MANAGER_H

#include "device_rdma_transport_manager.h"
#include "dl_acl_api.h"
#include "hybm_acl_notify.h"
#include "hybm_rdma_aicpu.h"
#include "hybm_kernel_hepler.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

struct AicpuStream {
    void *stream = nullptr;
    uint32_t sqId = 0U;
    uint32_t streamId = 0U;
};

class RdmaAicpuTransportManager : public RdmaTransportManager {
public:
    ~RdmaAicpuTransportManager() override;

    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    Result Synchronize(uint32_t rankId) override;
    int RemoteIO(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write, bool sync) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

private:
    using AicpuSegment = ock::mf::aicpu::RdmaSegment;

    struct AicpuPendingBatch {
        bool pending = false;
        uint64_t outAddr = 0U;
        uint32_t expectedPosted = 0U;
        uint32_t sqId = 0U;
        uint32_t sqStreamId = 0U;
        uint32_t devId = 0U;
    };

    int ResolveAicpuBatchAddresses(uint32_t rankId, const CopyDescriptor &descriptor,
                                   std::vector<AicpuSegment> &segments);
    Result InitDeviceContext(const TransportOptions &options, int32_t &userId);
    int RunAicpuIo(uint32_t rankId, void *stream, ock::mf::aicpu::RdmaAicpuParam &param, uint32_t expectedPosted);
    int ReadAndCheckAicpuOut(uint32_t rankId);
    int RemoteIoBatchAicpu(uint32_t rankId, const std::vector<AicpuSegment> &segments, bool write);
    int SubmitAicpuBatchChunk(uint32_t rankId, const std::vector<AicpuSegment> &segments, bool write);
    int BuildBatchParam(uint32_t rankId, const std::vector<AicpuSegment> &segments, uint64_t qpHandle,
                        void *deviceSegments, bool write, ock::mf::aicpu::RdmaAicpuParam &param);
    int StageBatchSegments(const std::vector<AicpuSegment> &segments, void *&deviceSegments);
    int ComputeDoorbellAddr(uint32_t rankId, uint64_t &dbAddr);
    int FillDoorbellParam(uint32_t rankId, ock::mf::aicpu::RdmaAicpuParam &param);
    int EnsureAicpuNotify();
    int LaunchAicpuBatch(uint32_t rankId, void *stream, ock::mf::aicpu::RdmaAicpuParam &param, uint32_t count);

    aclrtBinHandle aicpuRdmaBin_{nullptr};
    aclrtFuncHandle aicpuRdmaPostSend_{nullptr};
    static thread_local HybmAclNotifyPtr aicpuNotify_;
    static thread_local AicpuPendingBatch aicpuPending_;
    static thread_local AicpuStream aicpuStream_;
    uint32_t aicpuMaxSendNum_{UN1024};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_DEVICE_RDMA_AICPU_TRANSPORT_MANAGER_H
