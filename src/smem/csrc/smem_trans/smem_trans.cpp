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
#include <atomic>
#include "smem_common_includes.h"
#include "hybm.h"
#include "smem_trans.h"
#include "mf_rwlock.h"
#include "smem_trans_entry.h"
#include "smem_trans_entry_manager.h"
#include "smem_store_factory.h"

using namespace ock::smem;
using namespace ock::mf;

ReadWriteLock g_smemTransMutex_;
// init/uninit 线程写、各 API 读（多数在锁外），故用原子标志
std::atomic<bool> g_smemTransInited{false};

SMEM_API int32_t smem_trans_config_init(smem_trans_config_t *config)
{
    SM_VALIDATE_RETURN(config != nullptr, "Invalid config", SM_INVALID_PARAM);

    config->initTimeout = SMEM_DEFAUT_WAIT_TIME;
    config->role = SMEM_TRANS_SENDER;
    config->deviceId = UINT32_MAX;
    config->flags = 0;
    bzero(config->nic, sizeof(config->nic));
    bzero(config->url, sizeof(config->url));
    bzero(&config->hcomTlsConfig, sizeof(config->hcomTlsConfig));
    return SM_OK;
}

SMEM_API int32_t smem_trans_init(uint32_t deviceId, uint32_t flags)
{
    WriteGuard transLock(g_smemTransMutex_);

    if (g_smemTransInited) {
        SM_LOG_TRACE("smem trans initialized already");
        return SM_OK;
    }

    auto ret = hybm_init(deviceId, flags);
    if (ret != 0) {
        SM_LOG_ERROR("hybm core init failed: " << ret << " deviceId: " << deviceId << " flags: " << flags);
        return ret;
    }

    g_smemTransInited = true;
    SM_LOG_TRACE("smem trans initialized success");
    return SM_OK;
}

SMEM_API smem_trans_t smem_trans_create(const smem_trans_config_t *config, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", nullptr);
    SM_VALIDATE_RETURN(config != nullptr, "invalid config, which is null", nullptr);
    SM_VALIDATE_RETURN(strlen(config->url) != 0, "invalid config->url, which is empty", nullptr);
    WriteGuard transLock(g_smemTransMutex_);

    /* create entry */
    auto entry = SmemTransEntry::Create(*config);
    if (entry == nullptr) {
        SM_LOG_ERROR("create entity happen error, url: " << config->url << " flags: " << flags);
        return nullptr;
    }

    return reinterpret_cast<smem_trans_t>(entry.Get());
}

SMEM_API void smem_trans_destroy(smem_trans_t handle, uint32_t flags)
{
    SM_ASSERT_RET_VOID(handle != nullptr);
    WriteGuard transLock(g_smemTransMutex_);

    /* remove entry by ptr */
    auto result = SmemTransEntryManager::Instance().RemoveEntryByPtr(reinterpret_cast<uintptr_t>(handle));
    if (result == SM_OBJECT_NOT_EXISTS) {
        SM_LOG_AND_SET_LAST_ERROR("not found handle ");
        return;
    } else if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("failed to erase entry by handle ");
        return;
    }
}

SMEM_API void smem_trans_uninit(uint32_t flags)
{
    if (!g_smemTransInited) {
        SM_LOG_WARN("smem trans not initialized yet");
        return;
    }
    WriteGuard transLock(g_smemTransMutex_);

    SmemTransEntryManager::Instance().UnInitialize();
    hybm_uninit();
    ock::smem::StoreFactory::DestroyStoreAll(false);
    g_smemTransInited = false;
    SM_LOG_TRACE("smem_trans_uninit finished");
}

static int32_t GetEntryByHandle(smem_trans_t handle, SmemTransEntryPtr &entry)
{
    auto result = SmemTransEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (result != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("get entry by handle failed, handle: " << handle << " result: " << result);
        return result;
    }
    return SM_OK;
}

SMEM_API int32_t smem_trans_register_mem(smem_trans_t handle, void *address, size_t capacity, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(address != nullptr, "invalid address, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(capacity != 0, "invalid capacity, which is 0", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    /* register memory to entry */
    auto result = entry->RegisterLocalMemory(address, capacity, flags);
    if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("register local failed, result: " << result << " address: " << address
                                                                    << " capacity: " << capacity);
        return result;
    }
    return SM_OK;
}

SMEM_API int32_t smem_trans_batch_register_mem(smem_trans_t handle, void *addresses[], size_t capacities[],
                                               uint32_t count, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(addresses != nullptr, "invalid address, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(capacities != nullptr, "invalid capacities, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(count != 0, "invalid capacity, which is 0", SM_INVALID_PARAM);

    std::vector<std::pair<const void *, size_t>> regMemories;
    for (auto i = 0U; i < count; i++) {
        SM_VALIDATE_RETURN(addresses[i] != nullptr, "invalid address, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(capacities[i] != 0, "invalid capacities, which is 0", SM_INVALID_PARAM);
        regMemories.push_back(std::make_pair(addresses[i], capacities[i]));
    }

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    /* register memory to entry */
    auto result = entry->RegisterLocalMemories(regMemories, flags);
    if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("register local failed, result: " << result << " count: " << count);
        return result;
    }
    return SM_OK;
}

SMEM_API int32_t smem_trans_deregister_mem(smem_trans_t handle, void *address)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(address != nullptr, "invalid address, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    /* only maintain the local registered-memory snapshot for later replay */
    return entry->UnregisterLocalMemory(address);
}

SMEM_API int32_t smem_trans_write(smem_trans_t handle, const void *local_addr, const char *remote_unique_id,
                                  void *remote_addr, size_t data_size, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->SyncTransfer(const_cast<void *>(local_addr), remote_unique_id, remote_addr, data_size, SMEMB_COPY_L2G,
                               nullptr, flags);
}

SMEM_API int32_t smem_trans_batch_write(smem_trans_t handle, const void *local_addrs[], const char *remote_unique_id,
                                        void *remote_addrs[], size_t data_sizes[], uint32_t batch_size, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->BatchSyncTransfer(const_cast<void **>(local_addrs), remote_unique_id, remote_addrs, data_sizes,
                                    batch_size, SMEMB_COPY_L2G, nullptr, flags);
}

SMEM_API int32_t smem_trans_read(smem_trans_t handle, void *local_addr, const char *remote_unique_id,
                                 const void *remote_addr, size_t data_size, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->SyncTransfer(local_addr, remote_unique_id, const_cast<void *>(remote_addr), data_size, SMEMB_COPY_G2L,
                               nullptr, flags);
}

SMEM_API int32_t smem_trans_batch_read(smem_trans_t handle, void *local_addrs[], const char *remote_unique_id,
                                       const void *remote_addrs[], size_t data_sizes[], uint32_t batch_size,
                                       uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->BatchSyncTransfer(local_addrs, remote_unique_id, const_cast<void **>(remote_addrs), data_sizes,
                                    batch_size, SMEMB_COPY_G2L, nullptr, flags);
}

SMEM_API int32_t smem_trans_write_submit(smem_trans_t handle, const void *local_addr, const char *remote_unique_id,
                                         void *remote_addr, size_t data_size, void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(stream != nullptr, "invalid stream, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->SyncTransfer(const_cast<void *>(local_addr), remote_unique_id, remote_addr, data_size, SMEMB_COPY_L2G,
                               stream, flags);
}

SMEM_API int32_t smem_trans_read_submit(smem_trans_t handle, void *local_addr, const char *remote_unique_id,
                                        const void *remote_addr, size_t data_size, void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remote_unique_id != nullptr, "invalid remote_unique_id, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(stream != nullptr, "invalid stream, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->SyncTransfer(local_addr, remote_unique_id, const_cast<void *>(remote_addr), data_size, SMEMB_COPY_G2L,
                               stream, flags);
}

SMEM_API int32_t smem_trans_batch_write_submit(smem_trans_t handle, const void *localAddrs[],
                                               const char *remoteUniqueId, void *remoteAddrs[], size_t dataSizes[],
                                               uint32_t batchSize, void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remoteUniqueId != nullptr, "invalid remoteUniqueId, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(stream != nullptr, "invalid stream, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->BatchSyncTransfer(const_cast<void **>(localAddrs), remoteUniqueId, remoteAddrs, dataSizes, batchSize,
                                    SMEMB_COPY_L2G, stream, flags);
}

SMEM_API int32_t smem_trans_batch_read_submit(smem_trans_t handle, void *localAddrs[], const char *remoteUniqueId,
                                              const void *remoteAddrs[], size_t dataSizes[], uint32_t batchSize,
                                              void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remoteUniqueId != nullptr, "invalid remoteUniqueId, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(stream != nullptr, "invalid stream, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->BatchSyncTransfer(localAddrs, remoteUniqueId, const_cast<void **>(remoteAddrs), dataSizes, batchSize,
                                    SMEMB_COPY_G2L, stream, flags);
}

SMEM_API int32_t smem_trans_batch_copy(smem_trans_t handle, smem_trans_batch_params_t *param, uint64_t *batchId)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(param != nullptr, "invalid param, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(batchId != nullptr, "invalid batchId, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    auto ret = entry->SubmitBatchCopy(*param, *batchId);
    if (ret != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("submit batch copy failed, result: "
                                  << ret << " destUrl: " << (param->destUrl != nullptr ? param->destUrl : ""));
        return ret;
    }
    return SM_OK;
}

SMEM_API int32_t smem_trans_batch_status_query(smem_trans_t handle, uint64_t batchId, int32_t *result, uint32_t flags)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(result != nullptr, "invalid result, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    return entry->QueryBatchStatus(batchId, result, flags);
}

SMEM_API int32_t smem_trans_get_rpc_port(smem_trans_t handle, uint16_t *port)
{
    SM_VALIDATE_RETURN(g_smemTransInited, "smem trans not initialized yet", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(handle != nullptr, "invalid handle, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(port != nullptr, "invalid port, which is null", SM_INVALID_PARAM);

    /* get entry by ptr */
    SmemTransEntryPtr entry;
    if (auto result = GetEntryByHandle(handle, entry); result != SM_OK) {
        return result;
    }

    *port = entry->RpcPort();
    return SM_OK;
}
