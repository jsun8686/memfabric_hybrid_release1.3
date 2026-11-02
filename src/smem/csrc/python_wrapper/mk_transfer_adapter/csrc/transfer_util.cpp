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
#include <cctype>
#include <string>

#include "adapter_logger.h"
#include "mf_env_define.h"
#include "smem.h"
#include "transfer_util.h"

namespace ock {
namespace adapter {

int32_t pytransfer_create_config_store(const char *storeUrl)
{
    const std::string &mfLevel = mf::env::MF_LOG_LEVEL;
    if (mfLevel.size() == 1) {
        unsigned char c = static_cast<unsigned char>(mfLevel[0]);
        if (std::isdigit(c)) {
            int level = c - '0';
            smem_set_log_level(level);
        }
    }
    // default: disable tls
    smem_set_conf_store_tls(false, nullptr, 0);
    int ret = smem_create_config_store(storeUrl, SMEM_STORE_SKIP_RECOVER);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_create_config_store happen error, ret=" << ret);
    }
    return ret;
}

int32_t pytransfer_set_log_level(int level)
{
    int ret = smem_set_log_level(level);
    if (ret != 0) {
        ADAPTER_LOG_ERROR("SMEM API smem_set_log_level happen error, ret=" << ret);
        return ret;
    }
    return 0;
}

int32_t pytransfer_set_conf_store_tls(bool enable, std::string &tls_info)
{
    return smem_set_conf_store_tls(enable, tls_info.c_str(), tls_info.size());
}

} // namespace adapter
} // namespace ock
