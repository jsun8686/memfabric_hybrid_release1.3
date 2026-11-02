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
#include "hybm_host_shm_segment.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "hybm_ex_info_transfer.h"
#include "hybm_logger.h"
#include "hybm_va_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {

uint64_t HybmHostShmSegment::sUsedOffset_ = 0U;

namespace {
constexpr const char *HOST_SHM_HUGEPAGE_DIR = "/dev/hugepages";
constexpr const char *HOST_SHM_FALLBACK_DIR = "/dev/shm";
constexpr unsigned long HOST_SHM_HUGETLBFS_MAGIC = 0x958458f6UL;
constexpr uint32_t HOST_SHM_IMPORT_OPEN_RETRY_TIMES = 50U;
constexpr uint32_t HOST_SHM_IMPORT_OPEN_RETRY_INTERVAL_US = 10000U;

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif

// 直接走 syscall，避免依赖 glibc 版本提供 memfd_create 包装函数
int MemfdCreate(const char *name, unsigned int flags) noexcept
{
#if defined(SYS_memfd_create)
    return static_cast<int>(::syscall(SYS_memfd_create, name, flags));
#else
    errno = ENOSYS;
    return -1;
#endif
}

bool IsHugetlbfsMounted() noexcept
{
    struct statfs fsInfo {};
    if (statfs(HOST_SHM_HUGEPAGE_DIR, &fsInfo) != 0) {
        BM_LOG_WARN("Unable to access hugepage dir " << HOST_SHM_HUGEPAGE_DIR << " reason:" << errno << ", "
                                                     << SafeStrError(errno) << ", will fallback to /dev/shm");
        return false;
    }
    if (static_cast<unsigned long>(fsInfo.f_type) != HOST_SHM_HUGETLBFS_MAGIC) {
        BM_LOG_WARN("Hugepage dir " << HOST_SHM_HUGEPAGE_DIR << " is not hugetlbfs, fs type:" << std::hex
                                    << static_cast<unsigned long>(fsInfo.f_type) << ", will fallback to /dev/shm");
        return false;
    }
    return true;
}

bool HasAvailableHugePages() noexcept
{
    std::ifstream memInfoFile("/proc/meminfo");
    if (!memInfoFile.is_open()) {
        BM_LOG_WARN("Unable to open /proc/meminfo, will fallback to /dev/shm");
        return false;
    }

    std::string line;
    while (std::getline(memInfoFile, line)) {
        std::istringstream lineStream(line);
        std::string key;
        uint64_t value = 0;

        if (!(lineStream >> key >> value)) {
            continue;
        }

        if (key != "HugePages_Free:") {
            continue;
        }

        if (value == 0) {
            BM_LOG_WARN("HugePages_Free is 0, will fallback to /dev/shm");
            return false;
        }
        return true;
    }

    BM_LOG_WARN("Unable to parse HugePages_Free from /proc/meminfo, will fallback to /dev/shm");
    return false;
}
} // namespace

HybmHostShmSegment::~HybmHostShmSegment()
{
    FreeMemory();
}

Result HybmHostShmSegment::ValidateOptions() noexcept
{
    if (options_.segType != HYBM_MST_DRAM || options_.maxSize == 0 || (options_.maxSize % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("Validate options error type(" << options_.segType << ") maxSize(" << options_.maxSize << ") size("
                                                    << options_.size);
        return BM_INVALID_PARAM;
    }
    if (UINT64_MAX / options_.maxSize < options_.rankCnt) {
        BM_LOG_ERROR("Validate options error rankCnt(" << options_.rankCnt << ") maxSize(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result HybmHostShmSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(ValidateOptions() == BM_OK, "Failed to validate options.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(globalVirtualAddress_ == nullptr, "Already prepare virtual memory.", BM_NOT_INITIALIZED);
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "Invalid param, address is NULL.", BM_INVALID_PARAM);
    uint64_t totalSize = options_.rankCnt * options_.maxSize;
    uint64_t localSize = options_.enable56BitsGva ? options_.maxSize : totalSize;
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(
        {totalSize, localSize, HYBM_MEM_TYPE_HOST, options_.enable56BitsGva});
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "Invalid reserved gva.", BM_ERROR);
    void *startAddr = reinterpret_cast<void *>(gvaInfo.va[HVM_GVA]);
    if (!options_.enable56BitsGva) {
        void *mapped = mmap(startAddr, totalSize, PROT_NONE,
                            MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);
        if (mapped == MAP_FAILED || (uint64_t)mapped != (uint64_t)startAddr) {
            BM_LOG_ERROR("Failed to mmap size:" << totalSize << " addr:" << startAddr << " ret:" << mapped
                                                << " error: " << errno);
            return BM_ERROR;
        }
    }
    globalVirtualAddress_ = (uint8_t *)startAddr;
    totalVirtualSize_ = totalSize;
    localVirtualBase_ = options_.enable56BitsGva ? (uint8_t *)gvaInfo.va[HVM_DVA]
                                                 : globalVirtualAddress_ + options_.maxSize * options_.rankId;
    allocatedSize_ = 0UL;
    sliceCount_ = 0;
    auto ret = MapLocalShm();
    if (ret != BM_OK) {
        munmap(globalVirtualAddress_, totalVirtualSize_);
        globalVirtualAddress_ = nullptr;
        totalVirtualSize_ = 0;
        localVirtualBase_ = nullptr;
        return ret;
    }
    *address = globalVirtualAddress_;
    return BM_OK;
}

Result HybmHostShmSegment::UnReserveMemorySpace() noexcept
{
    BM_LOG_INFO("un-reserve memory space, reclaiming " << totalVirtualSize_ << " bytes.");
    FreeMemory();
    return BM_OK;
}

Result HybmHostShmSegment::AllocLocalMemory(uint64_t size, std::shared_ptr<MemSlice> &slice) noexcept
{
    if ((size % HYBM_LARGE_PAGE_SIZE) != 0UL || size + allocatedSize_ > options_.size) {
        BM_LOG_ERROR("invalid allocate memory size : " << size << ", now used " << allocatedSize_ << " of "
                                                       << options_.size);
        return BM_INVALID_PARAM;
    }
    void *sliceAddr = localVirtualBase_ + allocatedSize_;
    auto gva = reinterpret_cast<uint64_t>(globalVirtualAddress_ + options_.maxSize * options_.rankId + allocatedSize_);
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(sliceAddr), size);
    slices_.emplace(slice->index_, slice);
    // onlyGva=true: this segment always records DVA as 0 and does no device-side DVA translation, so only
    // the GVA table is needed. Previously options_.rankId was passed (uint32_t implicitly converted to bool):
    // rankId == 0 degraded to writing all three tables, inserting a DVA row keyed 0 into allocatedMap_[HVM_DVA]
    // and polluting the range lookup.
    auto ret = HybmVaManager::GetInstance().AddVaInfo(
        {gva, 0, reinterpret_cast<uint64_t>(sliceAddr), size, HYBM_MEM_TYPE_HOST}, true);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AddVaInfo failed for slice(idx:" << slice->index_ << "), ret:" << ret);
        slices_.erase(slice->index_);
        slice = nullptr;
        return ret;
    }
    allocatedSize_ += size;
    BM_LOG_INFO("allocate slice(idx:" << slice->index_ << ", size:" << slice->size_ << " va:" << sliceAddr << ").");
    return BM_OK;
}

Result HybmHostShmSegment::Export(std::string &exInfo) noexcept
{
    return BM_OK;
}

Result HybmHostShmSegment::Export(const std::shared_ptr<MemSlice> &slice, std::string &exInfo) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("Export slice is nullptr");
        return BM_INVALID_PARAM;
    }
    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end() || pos->second.slice != slice) {
        BM_LOG_ERROR("Export slice not found in slices_, sliceIdx: " << slice->index_);
        return BM_INVALID_PARAM;
    }
    auto exp = exportMap_.find(slice->index_);
    if (exp != exportMap_.end()) {
        exInfo = exp->second;
        return BM_OK;
    }
    ShmExportInfo info;
    info.mappingOffset = slice->vAddress_ - (uint64_t)(localVirtualBase_);
    info.sliceIndex = static_cast<uint32_t>(slice->index_);
    info.rankId = options_.rankId;
    info.size = slice->size_;
    info.pageTblType = MEM_PT_TYPE_SVM;
    info.memSegType = HYBM_MST_DRAM;
    info.exchangeType = HYBM_INFO_EXG_IN_NODE;
    info.useHugetlbfs = useHugetlbfs_;
    info.useMemfd = useMemfd_;
    info.memfdPid = useMemfd_ ? static_cast<int32_t>(getpid()) : 0;
    info.memfdFd = useMemfd_ ? localShmFd_ : -1;
    auto ret = LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        return BM_ERROR;
    }
    exportMap_[slice->index_] = exInfo;
    return BM_OK;
}

Result HybmHostShmSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[],
                                  uint32_t overrideRank) noexcept
{
    (void)overrideRank; // rank 覆盖仅 trans 场景（AsymmetricMemSegment）需要
    LiteralExInfoTranslater<ShmExportInfo> translator;
    std::vector<ShmExportInfo> deserializedInfos{allExInfo.size()};
    for (auto i = 0U; i < allExInfo.size(); i++) {
        auto desRet = translator.Deserialize(allExInfo[i], deserializedInfos[i]);
        if (desRet != 0) {
            BM_LOG_ERROR("Deserialize imported info failed, i: " << i << " ret: " << desRet);
            return BM_INVALID_PARAM;
        }
        if (deserializedInfos[i].magic != DRAM_SLICE_EXPORT_INFO_MAGIC) {
            return BM_INVALID_PARAM;
        }
    }
    try {
        for (const auto &info : deserializedInfos) {
            imports_.push_back(info);
            importedSources_[info.rankId] = {info.useHugetlbfs, info.useMemfd, info.memfdPid, info.memfdFd};
        }
    } catch (...) {
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result HybmHostShmSegment::Mmap() noexcept
{
    for (const auto &im : imports_) {
        if (im.rankId == options_.rankId || im.size == 0) {
            continue;
        }
        auto ret = MapImportedShm(im);
        if (ret != BM_OK) {
            return ret;
        }
    }
    imports_.clear();
    return BM_OK;
}

Result HybmHostShmSegment::Unmap() noexcept
{
    for (auto gva : mappedGvaMem_) {
        HybmVaManager::GetInstance().RemoveOneVaInfo(gva);
    }
    mappedGvaMem_.clear();
    CloseImportedShmFds();
    return BM_OK;
}

MemSlicePtr HybmHostShmSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    auto index = MemSlice::GetIndexFrom(slice);
    auto pos = slices_.find(index);
    if (pos == slices_.end() || !pos->second.slice->ValidateId(slice)) {
        if (!quiet) {
            BM_LOG_ERROR("cannot find slice by id: " << index);
        }
        return nullptr;
    }
    return pos->second.slice;
}

bool HybmHostShmSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    return !(begin < globalVirtualAddress_ ||
             reinterpret_cast<const uint8_t *>(begin) + size > globalVirtualAddress_ + totalVirtualSize_);
}

bool HybmHostShmSegment::IsLocalRange(const void *begin, uint64_t size) const noexcept
{
    return !(begin < localVirtualBase_ ||
             reinterpret_cast<const uint8_t *>(begin) + size > localVirtualBase_ + options_.size);
}

void HybmHostShmSegment::FreeMemory() noexcept
{
    (void)Unmap();
    if (localShmFd_ >= 0) {
        close(localShmFd_);
        localShmFd_ = -1;
        // memfd 为匿名文件，close 即释放，无路径可 unlink
        if (!useMemfd_) {
            auto shmPath = GetShmFilePath(options_.rankId);
            if (unlink(shmPath.c_str()) != 0 && errno != ENOENT) {
                BM_LOG_ERROR("Failed to unlink local shm file " << shmPath << " error:" << errno << " "
                                                                << SafeStrError(errno));
                if (errno == EBUSY || errno == EACCES) {
                    BM_LOG_WARN("File may remain on filesystem, consider manual cleanup");
                }
            }
        }
        useMemfd_ = false;
    }
    localVirtualBase_ = nullptr;
    if (globalVirtualAddress_ != nullptr) {
        (void)munmap(globalVirtualAddress_, totalVirtualSize_);
        HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uintptr_t>(globalVirtualAddress_));
        globalVirtualAddress_ = nullptr;
    }
}

Result HybmHostShmSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    auto ret = BM_OK;
    for (auto rankId : ranks) {
        RemapRemoteAsReserved(rankId);
        auto fdPos = importedShmFds_.find(rankId);
        if (fdPos != importedShmFds_.end()) {
            close(fdPos->second);
            importedShmFds_.erase(fdPos);
        }
    }
    return ret;
}

Result HybmHostShmSegment::RegisterMemory(const void *addr, uint64_t size, std::shared_ptr<MemSlice> &slice) noexcept
{
    auto va = reinterpret_cast<uint64_t>(addr);
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, va, va, size);
    slices_.emplace(slice->index_, slice);
    return BM_OK;
}

Result HybmHostShmSegment::ReleaseSliceMemory(const std::shared_ptr<MemSlice> &slice) noexcept
{
    if (slice == nullptr) {
        return BM_INVALID_PARAM;
    }
    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end() || pos->second.slice != slice) {
        return BM_INVALID_PARAM;
    }
    HybmVaManager::GetInstance().RemoveOneVaInfo(slice->vAddress_, HVM_HVA);
    slices_.erase(pos);
    return BM_OK;
}

Result HybmHostShmSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(ShmExportInfo);
    return BM_OK;
}

bool HybmHostShmSegment::TryHugetlbfsAvailable() noexcept
{
    return IsHugetlbfsMounted() && HasAvailableHugePages();
}

std::string HybmHostShmSegment::GetShmFilePath(uint32_t rankId) const noexcept
{
    return GetShmFilePath(rankId, useHugetlbfs_);
}

std::string HybmHostShmSegment::GetShmFilePath(uint32_t rankId, bool useHugetlbfs) const noexcept
{
    const char *baseDir = useHugetlbfs ? HOST_SHM_HUGEPAGE_DIR : HOST_SHM_FALLBACK_DIR;
    return std::string(baseDir) + "/memfabric_hybrid_" + std::to_string(rankId);
}

bool HybmHostShmSegment::TryMapLocalMemfdHuge() noexcept
{
    auto name = "memfabric_hybrid_" + std::to_string(options_.rankId);
    int fd = MemfdCreate(name.c_str(), MFD_HUGETLB);
    if (fd < 0) {
        BM_LOG_WARN("memfd_create(MFD_HUGETLB) failed, error:" << errno << " " << SafeStrError(errno)
                                                               << ", fallback to file-based shm");
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(options_.size)) != 0) {
        BM_LOG_WARN("Failed to truncate memfd hugepage size:" << options_.size << " error:" << errno << " "
                                                              << SafeStrError(errno) << ", fallback to file-based shm");
        close(fd);
        return false;
    }
    // memfd 为匿名文件，不存在具名冲突，无需 flock；MAP_POPULATE 触发大页分配，
    // 大页池不足时 mmap 失败并回退到文件方式
    void *mapped =
        mmap(localVirtualBase_, options_.size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_POPULATE, fd, 0);
    if (mapped == MAP_FAILED || mapped != localVirtualBase_) {
        BM_LOG_WARN("Failed to mmap memfd hugepage addr: "
                    << static_cast<void *>(localVirtualBase_) << " size: " << options_.size << " ret: " << mapped
                    << " error: " << errno << " " << SafeStrError(errno) << ", fallback to file-based shm");
        close(fd);
        return false;
    }
    localShmFd_ = fd;
    BM_LOG_INFO("MapLocalShm success via memfd hugepage: rankId=" << options_.rankId
                                                                  << " addr=" << static_cast<void *>(localVirtualBase_)
                                                                  << " size=" << options_.size << " fd=" << fd);
    return true;
}

Result HybmHostShmSegment::MapLocalShm() noexcept
{
    if (options_.size == 0) {
        BM_LOG_INFO("MapLocalShm success: rankId=" << options_.rankId << " size=" << options_.size << " skip it");
        return BM_OK;
    }
    if (MfEnvUtil::GetOptionalUintOrDefault(env::MF_HOST_SHM_USE_MEMFD, 0u) == 1u && TryMapLocalMemfdHuge()) {
        useMemfd_ = true;
        useHugetlbfs_ = true;
        return BM_OK;
    }
    useMemfd_ = false;
    useHugetlbfs_ = TryHugetlbfsAvailable();
    auto shmPath = GetShmFilePath(options_.rankId);
    BM_LOG_INFO("MapLocalShm start: rankId=" << options_.rankId << " size=" << options_.size
                                             << " useHugetlbfs=" << useHugetlbfs_ << " shmPath=" << shmPath);
    auto openAndLockLocalShm = [&shmPath]() noexcept -> int {
        int shmFd = open(shmPath.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
        if (shmFd < 0) {
            return -1;
        }
        if (flock(shmFd, LOCK_EX | LOCK_NB) != 0) {
            auto lockErr = errno;
            close(shmFd);
            errno = lockErr;
            return -1;
        }
        return shmFd;
    };

    localShmFd_ = openAndLockLocalShm();
    if (localShmFd_ < 0 && errno == EEXIST) {
        BM_LOG_INFO("Local shm file already exists, trying cleanup: " << shmPath);
        int staleShmFd = open(shmPath.c_str(), O_RDWR);
        if (staleShmFd < 0) {
            BM_LOG_ERROR("Failed to open existing shm file " << shmPath << " error:" << errno << " "
                                                             << SafeStrError(errno));
            return BM_ERROR;
        }
        if (flock(staleShmFd, LOCK_EX | LOCK_NB) != 0) {
            auto lockErr = errno;
            close(staleShmFd);
            BM_LOG_ERROR("Local shm file is already in use " << shmPath << " error:" << lockErr << " "
                                                             << SafeStrError(lockErr));
            return BM_ERROR;
        }
        if (unlink(shmPath.c_str()) != 0) {
            auto unlinkErr = errno;
            close(staleShmFd);
            BM_LOG_ERROR("Failed to cleanup stale shm file " << shmPath << " error:" << unlinkErr << " "
                                                             << SafeStrError(unlinkErr));
            return BM_ERROR;
        }
        close(staleShmFd);
        localShmFd_ = openAndLockLocalShm();
    }
    if (localShmFd_ < 0) {
        BM_LOG_ERROR("Failed to open local shm file " << shmPath << " size:" << options_.size << " error:" << errno
                                                      << " " << SafeStrError(errno));
        return BM_ERROR;
    }
    BM_LOG_INFO("Local shm file opened and locked: " << shmPath << " fd=" << localShmFd_);
    if (ftruncate(localShmFd_, static_cast<off_t>(options_.size)) != 0) {
        BM_LOG_ERROR("Failed to truncate local shm file " << shmPath << " size:" << options_.size << " error:" << errno
                                                          << " " << SafeStrError(errno));
        close(localShmFd_);
        localShmFd_ = -1;
        (void)unlink(shmPath.c_str());
        return BM_ERROR;
    }
    BM_LOG_INFO("Local shm file truncated: " << shmPath << " size=" << options_.size);
    void *mapped = mmap(localVirtualBase_, options_.size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_POPULATE,
                        localShmFd_, 0);
    if (mapped == MAP_FAILED || mapped != localVirtualBase_) {
        BM_LOG_ERROR("Failed to mmap local shm file " << shmPath << " addr:" << static_cast<void *>(localVirtualBase_)
                                                      << " size:" << options_.size << " ret:" << mapped
                                                      << " error:" << errno << " " << SafeStrError(errno));
        close(localShmFd_);
        localShmFd_ = -1;
        (void)unlink(shmPath.c_str());
        return BM_ERROR;
    }
    BM_LOG_INFO("MapLocalShm success: rankId=" << options_.rankId << " addr=" << static_cast<void *>(localVirtualBase_)
                                               << " size=" << options_.size << " useHugetlbfs=" << useHugetlbfs_
                                               << " fd=" << localShmFd_);
    return BM_OK;
}

uint64_t HybmHostShmSegment::ReserveLva(const ShmExportInfo &im) noexcept
{
    if (!options_.enable56BitsGva) {
        return reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * im.rankId + im.mappingOffset;
    }
    auto memType = im.magic == HBM_SLICE_EXPORT_INFO_MAGIC ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    const auto info = HybmVaManager::GetInstance().AllocReserveLva(im.size, HVM_DVA, memType);
    const auto reservedLva = info.va[HVM_DVA];
    if (reservedLva == 0) {
        BM_LOG_ERROR("AllocReserveLva failed rank:" << im.rankId << " size:" << im.size);
    }
    return reservedLva;
}

int HybmHostShmSegment::OpenImportedShmFd(uint32_t rankId) noexcept
{
    auto fdIt = importedShmFds_.find(rankId);
    if (fdIt != importedShmFds_.end()) {
        return fdIt->second;
    }
    bool remoteUseHugetlbfs = useHugetlbfs_;
    bool remoteUseMemfd = false;
    int32_t remotePid = 0;
    int32_t remoteFd = -1;
    auto flagIt = importedSources_.find(rankId);
    if (flagIt != importedSources_.end()) {
        remoteUseHugetlbfs = flagIt->second.useHugetlbfs;
        remoteUseMemfd = flagIt->second.useMemfd;
        remotePid = flagIt->second.pid;
        remoteFd = flagIt->second.fd;
    }
    // memfd 无文件系统路径，经 /proc/<pid>/fd/<fd> 打开对端的 memfd 引用同一块共享大页内存
    std::string shmPath;
    if (remoteUseMemfd && remotePid > 0 && remoteFd >= 0) {
        shmPath = "/proc/" + std::to_string(remotePid) + "/fd/" + std::to_string(remoteFd);
    } else {
        shmPath = GetShmFilePath(rankId, remoteUseHugetlbfs);
    }
    constexpr uint32_t extendedRetryTimes = 100U;
    for (uint32_t attempt = 0U; attempt < extendedRetryTimes; ++attempt) {
        int fd = open(shmPath.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            importedShmFds_[rankId] = fd;
            return fd;
        }
        if (errno != ENOENT) {
            BM_LOG_ERROR("Failed to open imported shm file " << shmPath << " rank:" << rankId << " error:" << errno
                                                             << " " << SafeStrError(errno));
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(HOST_SHM_IMPORT_OPEN_RETRY_INTERVAL_US));
    }
    BM_LOG_ERROR("Imported shm file not ready after retry "
                 << shmPath << " rank:" << rankId << " retries:" << extendedRetryTimes << " interval(us):"
                 << HOST_SHM_IMPORT_OPEN_RETRY_INTERVAL_US << " last error:" << errno << " " << SafeStrError(errno));
    return -1;
}

Result HybmHostShmSegment::MapImportedShm(const ShmExportInfo &im) noexcept
{
    auto rankId = im.rankId;
    auto remoteGva =
        reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * im.rankId + im.mappingOffset;
    if (mappedGvaMem_.find(remoteGva) != mappedGvaMem_.end()) {
        return BM_OK;
    }
    int fd = OpenImportedShmFd(rankId);
    if (fd < 0) {
        return BM_ERROR;
    }
    struct stat fileStat {};
    if (fstat(fd, &fileStat) != 0 || static_cast<uint64_t>(fileStat.st_size) < (im.size + im.mappingOffset)) {
        BM_LOG_ERROR("Imported shm file size mismatch rank:" << rankId << " expected:" << (im.size + im.mappingOffset)
                                                             << " got:" << fileStat.st_size);
        return BM_ERROR;
    }
    auto remoteLva = reinterpret_cast<void *>(ReserveLva(im));
    if (remoteLva == nullptr) {
        BM_LOG_ERROR("Failed to reserve lva rank:" << im.rankId << " size:" << im.size);
        return BM_ERROR;
    }
    void *mapped = mmap(remoteLva, im.size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, im.mappingOffset);
    if (mapped == MAP_FAILED || mapped != remoteLva) {
        BM_LOG_ERROR("Failed to mmap imported shm rank:" << rankId << " gva:" << reinterpret_cast<void *>(remoteGva)
                                                         << " lva:" << remoteLva << " size:" << im.size
                                                         << " offset:" << im.mappingOffset << " ret:" << mapped
                                                         << " error:" << errno << " " << SafeStrError(errno));
        return BM_ERROR;
    }
    mappedGvaMem_.insert(remoteGva);
    auto ret = HybmVaManager::GetInstance().AddVaInfo(
        {remoteGva, 0, reinterpret_cast<uint64_t>(remoteLva), im.size, HYBM_MEM_TYPE_HOST}, false,
        AddressOwnership::Imported);
    BM_LOG_INFO("MapImportedShm: ret:" << ret << " rankId=" << im.rankId << " addr=" << remoteGva << " size=" << im.size
                                       << " useHugetlbfs=" << im.useHugetlbfs << " fd=" << fd);
    if (ret != BM_OK) {
        BM_LOG_ERROR("AddVaInfo failed for rank " << rankId);
        return ret;
    }
    return BM_OK;
}

Result HybmHostShmSegment::RemapRemoteAsReserved(uint32_t rankId) noexcept
{
    auto gvaBase = reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * rankId;
    auto it = mappedGvaMem_.lower_bound(gvaBase);
    auto st = it;
    while (it != mappedGvaMem_.end() && *it < gvaBase + options_.maxSize) {
        HybmVaManager::GetInstance().RemoveOneVaInfo(*it);
        ++it;
    }
    if (st != it) {
        mappedGvaMem_.erase(st, it);
    }
    return BM_OK;
}

void HybmHostShmSegment::CloseImportedShmFds() noexcept
{
    for (auto &item : importedShmFds_) {
        close(item.second);
    }
    importedShmFds_.clear();
}

} // namespace mf
} // namespace ock
