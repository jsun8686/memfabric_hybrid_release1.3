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
#include "smem_trans_entry_manager.h"

namespace ock {
namespace smem {
SmemTransEntryManager &SmemTransEntryManager::Instance()
{
    static SmemTransEntryManager instance;
    return instance;
}

void SmemTransEntryManager::UnInitialize()
{
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        ptr2EntryMap_.clear(); /* each entry uninitializes its connections and executor */
        entryIdx_ = 0;
    }
}

Result SmemTransEntryManager::CreateEntry(const smem_trans_config_t &config, SmemTransEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* create new trans entry */
    auto tmpEntry = SmMakeRef<SmemTransEntry>(config);
    SM_ASSERT_RETURN(tmpEntry != nullptr, SM_NEW_OBJECT_FAILED);

    ptr2EntryMap_.emplace(reinterpret_cast<uintptr_t>(tmpEntry.Get()), tmpEntry);

    /* assign out object ptr */
    entry = tmpEntry;
    SM_LOG_TRACE("create new smem trans entry success. url:" << config.url << " ptr:0x" << std::hex << entry.Get());
    return SM_OK;
}

Result SmemTransEntryManager::AllocEntityId(uint32_t &entityId)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    // hybm's MemEntityFactory dedups engines by entity id process-wide, so the id must be
    // allocated process-locally; the segment size bounds the ids of one auto-ranked rank.
    SM_VALIDATE_RETURN(entryIdx_ < HYBM_ENTITY_ID_SEGMENT_SIZE,
                       "invalid id: " << entryIdx_ << " valid range: [0, " << HYBM_ENTITY_ID_SEGMENT_SIZE << ")",
                       SM_INVALID_PARAM);
    entityId = entryIdx_++;
    return SM_OK;
}

Result SmemTransEntryManager::GetEntryByPtr(uintptr_t ptr, SmemTransEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the trans entry exists or not with lock */
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter != ptr2EntryMap_.end()) {
        entry = iter->second;
        return SM_OK;
    }

    SM_LOG_DEBUG("not found trans entry");
    return SM_OBJECT_NOT_EXISTS;
}

Result SmemTransEntryManager::RemoveEntryByPtr(uintptr_t ptr)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the trans entry exists or not with lock */
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter == ptr2EntryMap_.end()) {
        SM_LOG_DEBUG("not found trans entry");
        return SM_OBJECT_NOT_EXISTS;
    }

    /* assign to a tmp ptr and remove from map */
    auto entry = iter->second;
    ptr2EntryMap_.erase(iter);
    SM_LOG_DEBUG("remove trans entry success, entityPtr:0x" << std::hex << ptr);

    return SM_OK;
}
} // namespace smem
} // namespace ock
