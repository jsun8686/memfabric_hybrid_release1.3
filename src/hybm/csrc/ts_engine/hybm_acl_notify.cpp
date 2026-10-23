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
#include "hybm_acl_notify.h"

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "dl_hal_api_def.h"
#include "dl_rt_api.h"
#include "hybm_gva.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {
HybmAclNotify::~HybmAclNotify()
{
    Reset();
}

HybmAclNotify::HybmAclNotify(HybmAclNotify &&other) noexcept
{
    *this = std::move(other);
}

HybmAclNotify &HybmAclNotify::operator=(HybmAclNotify &&other) noexcept
{
    if (this == &other) {
        return *this;
    }
    Reset();
    notify_ = other.notify_;
    notifyId_ = other.notifyId_;
    regAddr_ = other.regAddr_;
    regLen_ = other.regLen_;
    offset_ = other.offset_;
    aclStream_ = other.aclStream_;
    timeoutSec_ = other.timeoutSec_;
    inited_ = other.inited_;
    other.notify_ = nullptr;
    other.notifyId_ = UINT32_MAX;
    other.regAddr_ = 0U;
    other.regLen_ = 0U;
    other.offset_ = 0U;
    other.inited_ = false;
    return *this;
}

void HybmAclNotify::Reset() noexcept
{
    if (notify_ != nullptr) {
        (void)DlAclApi::AclrtDestroyNotify(notify_);
        notify_ = nullptr;
    }
    notifyId_ = UINT32_MAX;
    regAddr_ = 0U;
    regLen_ = 0U;
    offset_ = 0U;
    inited_ = false;
}

int32_t HybmAclNotify::Init(uint64_t regBlockBase)
{
    if (inited_) {
        return BM_OK;
    }
    void *notify = nullptr;
    auto ret = DlAclApi::AclrtCreateNotify(&notify, ACL_NOTIFY_FLAG_DEVICE_ONLY);
    if (ret != BM_OK) {
        BM_LOG_ERROR("acl notify: create notify failed, ret: " << ret);
        return ret;
    }
    uint32_t notifyId = UINT32_MAX;
    ret = DlAclApi::AclrtGetNotifyId(notify, &notifyId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("acl notify: get notify id failed, ret: " << ret);
        (void)DlAclApi::AclrtDestroyNotify(notify);
        return ret;
    }
    notify_ = notify;
    notifyId_ = notifyId;
    ret = ResolveRegAddr(regBlockBase);
    if (ret != BM_OK) {
        Reset();
        return ret;
    }
    inited_ = true;
    return BM_OK;
}

int32_t HybmAclNotify::Wait()
{
    if (!inited_ || notify_ == nullptr) {
        BM_LOG_ERROR("acl notify: notify not inited, notifyId: " << notifyId_);
        return BM_ERROR;
    }
    if (aclStream_ == nullptr) {
        BM_LOG_ERROR("acl notify: acl stream is null, notifyId: " << notifyId_);
        return BM_ERROR;
    }
    auto ret = DlAclApi::AclrtWaitAndResetNotify(notify_, aclStream_, timeoutSec_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("acl notify: waitAndReset notify failed, notifyId: " << notifyId_ << " timeoutSec: " << timeoutSec_
                                                                          << " ret: " << ret);
        return ret;
    }
    ret = DlAclApi::AclrtSynchronizeStream(aclStream_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("acl notify: sync stream failed, notifyId: " << notifyId_ << " ret: " << ret);
        return ret;
    }
    return BM_OK;
}

// 地址解析：给了 RA notify 块基址就走 HAL 偏移（A2），否则问 runtime 要 device 地址（A5/STARS）
int32_t HybmAclNotify::ResolveRegAddr(uint64_t regBlockBase)
{
    if (regBlockBase == 0U) {
        return ResolveRegAddrByRt(notifyId_);
    }
    uint64_t offset = 0U;
    auto ret = ResolveRegOffsetByHal(notifyId_, offset);
    if (ret != BM_OK) {
        return ret;
    }
    offset_ = offset;
    regAddr_ = regBlockBase + offset;
    regLen_ = 0U;
    BM_LOG_DEBUG("acl notify: reg addr resolved by hal offset, notifyId: " << notifyId_ << " blockBase: 0x" << std::hex
                                                                           << regBlockBase << " offset: " << offset
                                                                           << " regAddr: 0x" << regAddr_ << std::dec);
    return BM_OK;
}

int32_t HybmAclNotify::ResolveRegAddrByRt(uint32_t notifyId)
{
    uint64_t addr = 0U;
    uint32_t len = 0U;
    rtDevResInfo resInfo{};
    resInfo.dieId = 0U;
    resInfo.procType = RT_PROCESS_HCCP;
    resInfo.resType = RT_RES_TYPE_STARS_NOTIFY_RECORD;
    resInfo.resId = notifyId;
    resInfo.flag = 0U;
    rtDevResAddrInfo addrInfo{};
    addrInfo.resAddress = &addr;
    addrInfo.len = &len;
    auto ret = DlRtApi::RtGetDevResAddress(&resInfo, &addrInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("acl notify: rtGetDevResAddress failed, notifyId: " << notifyId << " ret: " << ret);
        return ret;
    }
    if (addr == 0U || len == 0U) {
        BM_LOG_ERROR("acl notify: invalid dev res addr, notifyId: " << notifyId << " addr: " << addr
                                                                    << " len: " << len);
        return BM_ERROR;
    }
    regAddr_ = addr;
    regLen_ = len;
    offset_ = 0U;
    BM_LOG_DEBUG("acl notify: reg addr resolved by rt, notifyId: " << notifyId << " regAddr: 0x" << std::hex << addr
                                                                   << " len: " << std::dec << len);
    return BM_OK;
}

int32_t HybmAclNotify::ResolveRegOffsetByHal(uint32_t notifyId, uint64_t &offset)
{
    const uint32_t devId = static_cast<uint32_t>(HybmGetInitedLogicDeviceId());
    struct halResourceIdInputInfo detailInput {};
    detailInput.type = DRV_NOTIFY_ID;
    detailInput.tsId = 0U;
    detailInput.resourceId = notifyId;
    detailInput.res[1U] = 0U;
    struct halResourceDetailInfo detailInfo = {DRV_RES_QUERY_OFFSET, 0U, 0U, {0U}};
    auto ret = DlHalApi::HalResourceDetailQuery(devId, &detailInput, &detailInfo);
    if (ret == 0) {
        offset = detailInfo.value0;
        return BM_OK;
    }
    BM_LOG_WARN("acl notify: halResourceDetailQuery failed, devId: " << devId << " notifyId: " << notifyId
                                                                     << " ret: " << ret);
    drvNotifyInfo drvInfo = {};
    drvInfo.tsId = 0U;
    drvInfo.notifyId = notifyId;
    ret = DlHalApi::DrvNotifyIdAddrOffset(devId, &drvInfo);
    if (ret != 0) {
        BM_LOG_ERROR("acl notify: notify reg offset query failed, devId: " << devId << " notifyId: " << notifyId
                                                                           << " ret: " << ret);
        return BM_ERROR;
    }
    offset = drvInfo.devAddrOffset;
    return BM_OK;
}
} // namespace mf
} // namespace ock
