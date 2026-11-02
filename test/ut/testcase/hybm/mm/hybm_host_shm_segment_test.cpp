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

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>
#include <vector>

#define private   public
#define protected public
#include "hybm_host_shm_segment.h"
#undef private
#undef protected

#include "hybm_ex_info_transfer.h"
#include "hybm_va_manager.h"

#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

using namespace ock::mf;

class HybmHostShmSegmentTest : public testing::Test {
protected:
    static MemSegmentOptions MakeOptions(uint64_t size = HYBM_LARGE_PAGE_SIZE * 2UL, uint32_t rankCnt = 3U,
                                         uint32_t rankId = 1U)
    {
        MemSegmentOptions options{};
        options.segType = HYBM_MST_DRAM;
        options.size = size;
        options.maxSize = size;
        options.rankCnt = rankCnt;
        options.rankId = rankId;
        return options;
    }

    static void SetManualMemoryWindow(HybmHostShmSegment &segment, std::vector<uint8_t> &window)
    {
        segment.globalVirtualAddress_ = window.data();
        segment.totalVirtualSize_ = window.size();
        segment.localVirtualBase_ = segment.globalVirtualAddress_ + segment.options_.size * segment.options_.rankId;
    }

    static void ResetManualMemoryWindow(HybmHostShmSegment &segment)
    {
        segment.globalVirtualAddress_ = nullptr;
        segment.totalVirtualSize_ = 0;
        segment.localVirtualBase_ = nullptr;
    }

    void SetUp() override
    {
        GlobalMockObject::reset();
        // 清理前序用例/suite 遗留的 VA 登记，避免 AllocLocalMemory 因地址重叠失败
        HybmVaManager::GetInstance().ClearAll();
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    }
};

TEST_F(HybmHostShmSegmentTest, ValidateOptions_CoversSuccessAndInvalidBranches)
{
    auto options = MakeOptions();
    HybmHostShmSegment segment(options, 0);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    options = MakeOptions();
    options.segType = HYBM_MST_HBM;
    HybmHostShmSegment badType(options, 0);
    EXPECT_EQ(badType.ValidateOptions(), BM_INVALID_PARAM);

    options = MakeOptions();
    options.maxSize = 0;
    HybmHostShmSegment zeroMaxSize(options, 0);
    EXPECT_EQ(zeroMaxSize.ValidateOptions(), BM_INVALID_PARAM);

    options = MakeOptions();
    options.maxSize = HYBM_LARGE_PAGE_SIZE / 2UL;
    HybmHostShmSegment misalignedSize(options, 0);
    EXPECT_EQ(misalignedSize.ValidateOptions(), BM_INVALID_PARAM);

    options = MakeOptions((1ULL << 63U), 2U, 0U);
    HybmHostShmSegment overflow(options, 0);
    EXPECT_EQ(overflow.ValidateOptions(), BM_INVALID_PARAM);
}

TEST_F(HybmHostShmSegmentTest, AllocLocalMemory_CoversValidUnalignedAndOverflow)
{
    auto options = MakeOptions(HYBM_LARGE_PAGE_SIZE * 2UL, 2U, 1U);
    HybmHostShmSegment segment(options, 0);
    std::vector<uint8_t> window(options.size * options.rankCnt, 0U);
    SetManualMemoryWindow(segment, window);

    MemSlicePtr slice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->index_, 0U);
    EXPECT_EQ(slice->size_, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(slice->vAddress_, reinterpret_cast<uint64_t>(segment.localVirtualBase_));
    EXPECT_EQ(slice->gva_, reinterpret_cast<uint64_t>(segment.globalVirtualAddress_) + options.size * options.rankId);

    MemSlicePtr unalignedSlice;
    EXPECT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE / 2UL, unalignedSlice), BM_INVALID_PARAM);

    MemSlicePtr overflowSlice;
    EXPECT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE * 2UL, overflowSlice), BM_INVALID_PARAM);

    ResetManualMemoryWindow(segment);
}

TEST_F(HybmHostShmSegmentTest, ExportSlice_CoversNullUnknownSerializeAndCacheReuse)
{
    auto options = MakeOptions(HYBM_LARGE_PAGE_SIZE * 2UL, 2U, 1U);
    HybmHostShmSegment segment(options, 0);
    std::vector<uint8_t> window(options.size * options.rankCnt, 0U);
    SetManualMemoryWindow(segment, window);

    std::string exInfo;
    EXPECT_EQ(segment.Export(nullptr, exInfo), BM_INVALID_PARAM);

    auto unknownSlice =
        std::make_shared<MemSlice>(9U, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, reinterpret_cast<uint64_t>(window.data()),
                                   reinterpret_cast<uint64_t>(window.data()), HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(segment.Export(unknownSlice, exInfo), BM_INVALID_PARAM);

    MemSlicePtr localSlice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, localSlice), BM_OK);
    ASSERT_NE(localSlice, nullptr);

    ASSERT_EQ(segment.Export(localSlice, exInfo), BM_OK);
    ASSERT_EQ(segment.exportMap_.count(localSlice->index_), 1U);

    ShmExportInfo info{};
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Deserialize(exInfo, info), BM_OK);
    EXPECT_EQ(info.magic, DRAM_SLICE_EXPORT_INFO_MAGIC);
    EXPECT_EQ(info.version, EXPORT_INFO_VERSION);
    EXPECT_EQ(info.mappingOffset, 0U);
    EXPECT_EQ(info.sliceIndex, localSlice->index_);
    EXPECT_EQ(info.rankId, options.rankId);
    EXPECT_EQ(info.size, localSlice->size_);
    EXPECT_EQ(info.pageTblType, MEM_PT_TYPE_SVM);
    EXPECT_EQ(info.memSegType, HYBM_MST_DRAM);
    EXPECT_EQ(info.exchangeType, HYBM_INFO_EXG_IN_NODE);
    EXPECT_FALSE(info.useHugetlbfs);
    EXPECT_FALSE(info.useMemfd);
    EXPECT_EQ(info.memfdFd, -1);

    std::string cached = "cache_miss";
    ASSERT_EQ(segment.Export(localSlice, cached), BM_OK);
    EXPECT_EQ(cached, exInfo);

    ResetManualMemoryWindow(segment);
}

TEST_F(HybmHostShmSegmentTest, Import_ReturnsInvalidParamWhenDeserializeFails)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    std::vector<std::string> allExInfo{"bad_format"};

    EXPECT_EQ(segment.Import(allExInfo, nullptr, HYBM_IMPORT_RANK_INVALID), BM_INVALID_PARAM);
}

TEST_F(HybmHostShmSegmentTest, Import_ReturnsInvalidParamWhenMagicIsInvalid)
{
    HybmHostShmSegment segment(MakeOptions(), 0);

    ShmExportInfo info{};
    info.magic = 0x1234ULL;
    info.rankId = 0U;
    std::string exInfo;
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(info, exInfo), BM_OK);

    EXPECT_EQ(segment.Import({exInfo}, nullptr, HYBM_IMPORT_RANK_INVALID), BM_INVALID_PARAM);
}

TEST_F(HybmHostShmSegmentTest, Import_KeepsAllSlicesDedupDeferredToMmap)
{
    HybmHostShmSegment segment(MakeOptions(), 0);

    ShmExportInfo first{};
    first.rankId = 0U;
    first.sliceIndex = 1U;
    first.mappingOffset = 128U;
    first.size = HYBM_LARGE_PAGE_SIZE;
    first.useHugetlbfs = true;

    ShmExportInfo secondSlice = first;
    secondSlice.sliceIndex = 2U;
    secondSlice.mappingOffset = 256U;

    ShmExportInfo trueDuplicate = first;

    ShmExportInfo third{};
    third.rankId = 2U;
    third.sliceIndex = 3U;
    third.mappingOffset = 512U;
    third.size = HYBM_LARGE_PAGE_SIZE;
    third.useHugetlbfs = true;

    std::string ex1;
    std::string ex2;
    std::string ex3;
    std::string ex4;
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(first, ex1), BM_OK);
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(secondSlice, ex2), BM_OK);
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(trueDuplicate, ex3), BM_OK);
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(third, ex4), BM_OK);

    ASSERT_EQ(segment.Import({ex1, ex2, ex3, ex4}, nullptr, HYBM_IMPORT_RANK_INVALID), BM_OK);
    ASSERT_EQ(segment.imports_.size(), 4U);
    EXPECT_EQ(segment.imports_[0].rankId, 0U);
    EXPECT_EQ(segment.imports_[0].sliceIndex, 1U);
    EXPECT_EQ(segment.imports_[1].rankId, 0U);
    EXPECT_EQ(segment.imports_[1].sliceIndex, 2U);
    EXPECT_EQ(segment.imports_[2].rankId, 0U);
    EXPECT_EQ(segment.imports_[2].sliceIndex, 1U);
    EXPECT_EQ(segment.imports_[3].rankId, 2U);
    EXPECT_EQ(segment.imports_[3].sliceIndex, 3U);
    EXPECT_EQ(segment.importedSources_.at(0U).useHugetlbfs, true);
    EXPECT_EQ(segment.importedSources_.at(0U).useMemfd, false);
    EXPECT_EQ(segment.importedSources_.at(2U).useHugetlbfs, true);
    EXPECT_EQ(segment.importedSources_.at(2U).useMemfd, false);
}

TEST_F(HybmHostShmSegmentTest, ReleaseSliceMemory_CoversNullUnknownAndValid)
{
    auto options = MakeOptions(HYBM_LARGE_PAGE_SIZE * 2UL, 2U, 1U);
    HybmHostShmSegment segment(options, 0);
    std::vector<uint8_t> window(options.size * options.rankCnt, 0U);
    SetManualMemoryWindow(segment, window);

    EXPECT_EQ(segment.ReleaseSliceMemory(nullptr), BM_INVALID_PARAM);

    auto unknownSlice =
        std::make_shared<MemSlice>(100U, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, 0U, 0U, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(segment.ReleaseSliceMemory(unknownSlice), BM_INVALID_PARAM);

    MemSlicePtr localSlice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, localSlice), BM_OK);
    ASSERT_NE(localSlice, nullptr);
    EXPECT_EQ(segment.ReleaseSliceMemory(localSlice), BM_OK);

    ResetManualMemoryWindow(segment);
}

TEST_F(HybmHostShmSegmentTest, GetExportSliceSize_ReturnsShmExportInfoSize)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    size_t size = 0U;

    EXPECT_EQ(segment.GetExportSliceSize(size), BM_OK);
    EXPECT_EQ(size, sizeof(ShmExportInfo));
}

TEST_F(HybmHostShmSegmentTest, UnReserveMemorySpace_NoCrash)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    auto ret = segment.UnReserveMemorySpace();
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(HybmHostShmSegmentTest, MemoryInRange_NoInit_ReturnsFalse)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    EXPECT_FALSE(segment.MemoryInRange(reinterpret_cast<void *>(0x1000), 0));
}

TEST_F(HybmHostShmSegmentTest, ReleaseSliceMemory_NullSlice_NoCrash)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    auto ret = segment.ReleaseSliceMemory(nullptr);
    EXPECT_NE(ret, BM_OK);
}

// ─────────────────────────────────────────────────────────────────
// 以下用例覆盖真实 POSIX 路径（/dev/shm + mmap），无需设备 mock
// ─────────────────────────────────────────────────────────────────

namespace {
class HybmHostShmVaGuard {
public:
    HybmHostShmVaGuard()
    {
        HybmVaManager::GetInstance().ClearAll();
        (void)HybmVaManager::GetInstance().Initialize(AscendSocType::ASCEND_910B);
    }

    ~HybmHostShmVaGuard()
    {
        HybmVaManager::GetInstance().ClearAll();
    }
};

void RemoveShmFileIfExists(const std::string &path)
{
    (void)unlink(path.c_str());
}

void CreateShmFile(const std::string &path, uint64_t size)
{
    RemoveShmFileIfExists(path);
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    ASSERT_GE(fd, 0) << "open shm file failed: " << path;
    ASSERT_EQ(ftruncate(fd, static_cast<off_t>(size)), 0);
    ASSERT_EQ(close(fd), 0);
}
} // namespace

TEST_F(HybmHostShmSegmentTest, ReserveMemorySpace_FullRealShmFlow)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    const std::string shmPath = "/dev/shm/memfabric_hybrid_" + std::to_string(options.rankId);
    RemoveShmFileIfExists(shmPath);

    HybmHostShmSegment segment(options, 0);
    void *addr = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&addr), BM_OK);
    ASSERT_NE(addr, nullptr);
    EXPECT_EQ(segment.globalVirtualAddress_, addr);
    EXPECT_NE(segment.localShmFd_, -1);

    struct stat st {};
    EXPECT_EQ(stat(shmPath.c_str(), &st), 0);

    // 空 imports_ 时 Mmap 直接成功
    EXPECT_EQ(segment.Mmap(), BM_OK);

    // 真实 AllocLocalMemory + AddVaInfo
    MemSlicePtr slice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    ASSERT_NE(slice, nullptr);

    // GetMemSlice 命中与未命中（quiet 两种取值）
    EXPECT_EQ(segment.GetMemSlice(slice->ConvertToId(), false), slice);
    EXPECT_EQ(segment.GetMemSlice(reinterpret_cast<hybm_mem_slice_t>(0xDEADBEEF), true), nullptr);
    EXPECT_EQ(segment.GetMemSlice(reinterpret_cast<hybm_mem_slice_t>(0xDEADBEEF), false), nullptr);

    // 内存范围判定
    EXPECT_TRUE(segment.MemoryInRange(slice->vAddress_, 1));
    EXPECT_FALSE(segment.MemoryInRange(reinterpret_cast<void *>(0x1UL), 1));
    EXPECT_TRUE(segment.IsLocalRange(segment.localVirtualBase_, 1));
    EXPECT_FALSE(segment.IsLocalRange(segment.localVirtualBase_ + options.size + 1, 1));

    // 清理：FreeMemory → close fd + unlink + munmap + FreeReserveGva
    EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
    EXPECT_EQ(stat(shmPath.c_str(), &st), -1);
}

TEST_F(HybmHostShmSegmentTest, MapImportedShm_WithRealPeerFiles)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    const std::string shmPath1 = "/dev/shm/memfabric_hybrid_0";
    const std::string shmPath2 = "/dev/shm/memfabric_hybrid_2";
    CreateShmFile(shmPath1, options.size);
    CreateShmFile(shmPath2, options.size);

    HybmHostShmSegment segment(options, 0);
    void *addr = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&addr), BM_OK);

    // 通过真实 Import 填充 imports_（rank 0/2）
    ShmExportInfo info0{};
    info0.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    info0.rankId = 0U;
    info0.size = options.size;
    info0.useHugetlbfs = false;
    ShmExportInfo info2{};
    info2.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    info2.rankId = 2U;
    info2.size = options.size;
    info2.useHugetlbfs = false;
    std::string ex0;
    std::string ex2;
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(info0, ex0), BM_OK);
    ASSERT_EQ(LiteralExInfoTranslater<ShmExportInfo>{}.Serialize(info2, ex2), BM_OK);
    ASSERT_EQ(segment.Import({ex0, ex2}, nullptr, HYBM_IMPORT_RANK_INVALID), BM_OK);
    ASSERT_EQ(segment.imports_.size(), 2U);

    // Mmap 真实映射两个 peer shm
    ASSERT_EQ(segment.Mmap(), BM_OK);
    auto expectedGva = [&segment, &options](uint32_t rankId) -> uint64_t {
        return reinterpret_cast<uint64_t>(segment.globalVirtualAddress_) + options.maxSize * rankId;
    };
    EXPECT_EQ(segment.mappedGvaMem_.count(expectedGva(0U)), 1U);
    EXPECT_EQ(segment.mappedGvaMem_.count(expectedGva(2U)), 1U);
    EXPECT_EQ(segment.importedShmFds_.count(0U), 1U);
    EXPECT_EQ(segment.importedShmFds_.count(2U), 1U);

    // 重复 Mmap 幂等
    ASSERT_EQ(segment.Mmap(), BM_OK);

    // 已映射 rank 直接调用 MapImportedShm → 提前返回
    EXPECT_EQ(segment.MapImportedShm(info0), BM_OK);
    EXPECT_EQ(segment.MapImportedShm(info2), BM_OK);

    // 仅移除 rank0
    EXPECT_EQ(segment.RemoveImported({0U}), BM_OK);
    EXPECT_EQ(segment.mappedGvaMem_.count(expectedGva(0U)), 0U);
    EXPECT_EQ(segment.mappedGvaMem_.count(expectedGva(2U)), 1U);

    // 移除未映射的 rank 无副作用
    EXPECT_EQ(segment.RemoveImported({5U}), BM_OK);

    // Unmap 剩余
    EXPECT_EQ(segment.Unmap(), BM_OK);
    EXPECT_TRUE(segment.mappedGvaMem_.empty());
    EXPECT_TRUE(segment.importedShmFds_.empty());

    EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
    RemoveShmFileIfExists(shmPath1);
    RemoveShmFileIfExists(shmPath2);
}

TEST_F(HybmHostShmSegmentTest, MapLocalShm_StaleFileCleanup)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    const std::string shmPath = "/dev/shm/memfabric_hybrid_" + std::to_string(options.rankId);
    // 预置未加锁的陈旧 shm 文件，触发 EEXIST 清理路径
    CreateShmFile(shmPath, options.size);

    HybmHostShmSegment segment(options, 0);
    void *addr = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&addr), BM_OK);
    EXPECT_NE(segment.localShmFd_, -1);

    struct stat st {};
    EXPECT_EQ(stat(shmPath.c_str(), &st), 0);
    EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
    EXPECT_EQ(stat(shmPath.c_str(), &st), -1);
}

TEST_F(HybmHostShmSegmentTest, ExportAndGetExportSliceSize)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    std::string exInfo;
    EXPECT_EQ(segment.Export(exInfo), BM_OK);

    size_t size = 0U;
    EXPECT_EQ(segment.GetExportSliceSize(size), BM_OK);
    EXPECT_EQ(size, sizeof(ShmExportInfo));
}

TEST_F(HybmHostShmSegmentTest, RegisterMemory_And_ReleaseValidSlice)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    HybmHostShmSegment segment(options, 0);

    MemSlicePtr slice;
    void *regAddr = reinterpret_cast<void *>(0x700000000000UL);
    ASSERT_EQ(segment.RegisterMemory(regAddr, 4096U, slice), BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->vAddress_, reinterpret_cast<uint64_t>(regAddr));

    EXPECT_EQ(segment.ReleaseSliceMemory(slice), BM_OK);

    // index 相同但对象不同 → 不匹配
    auto otherSlice = std::make_shared<MemSlice>(slice->index_, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, 0U, 0U, 0U);
    EXPECT_NE(segment.ReleaseSliceMemory(otherSlice), BM_OK);
}

TEST_F(HybmHostShmSegmentTest, GetShmFilePath_Overloads)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    segment.useHugetlbfs_ = false;
    EXPECT_EQ(segment.GetShmFilePath(7U), "/dev/shm/memfabric_hybrid_7");
    EXPECT_EQ(segment.GetShmFilePath(7U, false), "/dev/shm/memfabric_hybrid_7");
    EXPECT_EQ(segment.GetShmFilePath(7U, true), "/dev/hugepages/memfabric_hybrid_7");
}

TEST_F(HybmHostShmSegmentTest, TryHugetlbfsAvailable_NoCrash)
{
    HybmHostShmSegment segment(MakeOptions(), 0);
    // 环境相关（本机 HugePages=0 → false），仅验证不崩溃且路径自洽
    auto available = segment.TryHugetlbfsAvailable();
    EXPECT_EQ(segment.GetShmFilePath(1U, available), segment.GetShmFilePath(1U, segment.useHugetlbfs_));
}

TEST_F(HybmHostShmSegmentTest, Mmap_ImportedShmErrorPaths)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    HybmHostShmSegment segment(options, 0);
    void *addr = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&addr), BM_OK);

    // 自 rank 的 import 被跳过
    ShmExportInfo self{};
    self.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    self.rankId = options.rankId;
    self.size = options.size;
    self.useHugetlbfs = false;
    segment.imports_.push_back(self);
    ASSERT_EQ(segment.Mmap(), BM_OK);
    EXPECT_TRUE(segment.imports_.empty());

    // peer shm 文件大小不足 → 失败
    const std::string path2 = "/dev/shm/memfabric_hybrid_2";
    CreateShmFile(path2, options.size - 4096U);
    ShmExportInfo badSize{};
    badSize.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    badSize.rankId = 2U;
    badSize.size = options.size;
    badSize.useHugetlbfs = false;
    segment.imports_.push_back(badSize);
    EXPECT_NE(segment.Mmap(), BM_OK);

    // shm 路径是目录 → open 返回非 ENOENT 错误 → 立即失败
    const std::string path3 = "/dev/shm/memfabric_hybrid_3";
    ASSERT_EQ(mkdir(path3.c_str(), 0755), 0); // 0755
    segment.imports_.clear();
    ShmExportInfo dirAsFile{};
    dirAsFile.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    dirAsFile.rankId = 3U;
    dirAsFile.size = options.size;
    dirAsFile.useHugetlbfs = false;
    segment.imports_.push_back(dirAsFile);
    EXPECT_NE(segment.Mmap(), BM_OK);

    // peer shm 文件不存在 → 重试后失败
    segment.imports_.clear();
    ShmExportInfo missing{};
    missing.magic = DRAM_SLICE_EXPORT_INFO_MAGIC;
    missing.rankId = 4U;
    missing.size = options.size;
    missing.useHugetlbfs = false;
    segment.imports_.push_back(missing);
    EXPECT_NE(segment.Mmap(), BM_OK);

    RemoveShmFileIfExists(path2);
    ASSERT_EQ(rmdir(path3.c_str()), 0);
    EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
}

TEST_F(HybmHostShmSegmentTest, MapLocalShm_StaleLockHeld_ReturnsError)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    const std::string shmPath = "/dev/shm/memfabric_hybrid_" + std::to_string(options.rankId);
    RemoveShmFileIfExists(shmPath);
    // 模拟其他进程正持有 shm 文件锁
    int heldFd = open(shmPath.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    ASSERT_GE(heldFd, 0);
    ASSERT_EQ(flock(heldFd, LOCK_EX), 0);

    HybmHostShmSegment segment(options, 0);
    void *addr = nullptr;
    // MapLocalShm 失败 → ReserveMemorySpace 回滚
    EXPECT_NE(segment.ReserveMemorySpace(&addr), BM_OK);
    EXPECT_EQ(segment.globalVirtualAddress_, nullptr);
    EXPECT_EQ(segment.localShmFd_, -1);

    ASSERT_EQ(close(heldFd), 0);
    RemoveShmFileIfExists(shmPath);
}

TEST_F(HybmHostShmSegmentTest, AllocLocalMemory_AddVaInfoFails_RollsBack)
{
    HybmHostShmVaGuard vaGuard;

    auto options = MakeOptions();
    HybmHostShmSegment segment(options, 0);
    std::vector<uint8_t> window(options.size * options.rankCnt, 0U);
    SetManualMemoryWindow(segment, window);

    using AddVaInfoMemberFn =
        ock::mf::Result (HybmVaManager::*)(const BaseAllocatedGvaInfo &, bool, ock::mf::AddressOwnership);
    auto addVaInfoPtr = static_cast<AddVaInfoMemberFn>(&HybmVaManager::AddVaInfo);
    MOCKER_CPP(addVaInfoPtr,
               ock::mf::Result(*)(HybmVaManager *, const BaseAllocatedGvaInfo &, bool, ock::mf::AddressOwnership))
        .stubs()
        .will(returnValue(BM_ERROR));

    MemSlicePtr slice;
    EXPECT_NE(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    EXPECT_EQ(slice, nullptr);
    EXPECT_TRUE(segment.slices_.empty());

    ResetManualMemoryWindow(segment);
}
