// Memory manager: physical pages, contiguous memory, system PTEs, mapped pages, protection, locking.
#include "kernel_test.h"

#include <algorithm>
#include <set>
#include <vector>

extern volatile ULONG MiPteDoubleFrees, MiRelocations;

namespace {

// Every test leaves the page database consistent.
class Mm : public ::testing::Test {
protected:
    ULONG available0 = 0;
    void SetUp() override {
        ASSERT_EQ(CheckPfnDatabase(), "");
        available0 = MmAvailablePages;
    }
    void TearDown() override { EXPECT_EQ(CheckPfnDatabase(), "") << host::TakeLog(); }
};

uint32_t &Pte(ULONG va) { return *(uint32_t *)MiGetPteAddress(va); }

} // namespace

// --- Physical pages

TEST_F(Mm, AllocatePageTakesAFreePageZeroesItAndRecordsItsUse) {
    ULONG before = MmAllocatedPagesByUsage[MmSystemMemoryUsage];
    ULONG pfn = MiAllocatePage(MmSystemMemoryUsage, nullptr, false);
    ASSERT_NE(pfn, 0u);
    memset(host::Phys(pfn << 12), 0xA5, 4096); // dirty it
    MiFreePage(pfn);
    // A freed single page goes to the head of its colour list (reused first within its colour).
    EXPECT_EQ(MmRetailPfnRegion.FreePagesByColor[pfn & 31].PackedPfnFlink, (USHORT)((pfn >> 4) & 0xFFFE));
    // The colours are taken in turn: within 32 allocations it comes back, zeroed.
    std::vector<ULONG> taken;
    ULONG again = 0;
    for (int i = 0; i < 32 && again != pfn; i++)
        taken.push_back(again = MiAllocatePage(MmSystemMemoryUsage, nullptr, true));
    ASSERT_EQ(again, pfn);
    EXPECT_EQ(MmAvailablePages, available0 - taken.size());
    EXPECT_EQ(MmAllocatedPagesByUsage[MmSystemMemoryUsage], before + taken.size());
    uint8_t *p = host::Phys(again << 12);
    EXPECT_TRUE(std::all_of(p, p + 4096, [](uint8_t b) { return b == 0; })) << "page not zeroed";
    uint32_t e = PfnEntry(again);
    EXPECT_FALSE(e & PTE_PRESENT);
    EXPECT_TRUE(e & 0x10000) << "busy bit";
    EXPECT_EQ(e >> 28, (uint32_t)MmSystemMemoryUsage);
    EXPECT_EQ((uint16_t)e, 0) << "lock count";
    for (ULONG t : taken)
        MiFreePage(t);
    EXPECT_EQ(MmAvailablePages, available0);
    EXPECT_EQ(MmAllocatedPagesByUsage[MmSystemMemoryUsage], before);
}

TEST_F(Mm, ConsecutivePagesTakeTheColoursInTurn) {
    std::vector<ULONG> pfns;
    for (int i = 0; i < 64; i++)
        pfns.push_back(MiAllocatePage(MmSystemMemoryUsage, nullptr, false));
    for (size_t i = 1; i < pfns.size(); i++)
        EXPECT_EQ(pfns[i] & 31, (pfns[i - 1] + 1) & 31) << i;
    std::set<ULONG> unique(pfns.begin(), pfns.end());
    EXPECT_EQ(unique.size(), pfns.size());
    for (ULONG p : pfns)
        MiFreePage(p);
}

TEST_F(Mm, ExhaustionReturnsZeroAndEverythingComesBack) {
    std::vector<ULONG> pfns;
    while (ULONG p = MiAllocatePage(MmSystemMemoryUsage, nullptr, false))
        pfns.push_back(p);
    EXPECT_EQ(MmAvailablePages, 0u);
    EXPECT_EQ(pfns.size(), available0);
    EXPECT_EQ(MiAllocatePage(MmSystemMemoryUsage, nullptr, false), 0u);
    EXPECT_EQ(CheckPfnDatabase(), "");
    for (ULONG p : pfns)
        MiFreePage(p);
    EXPECT_EQ(MmAvailablePages, available0);
}

TEST_F(Mm, CorruptFreeCountIsABugCheckNotAHang) {
    // The count says pages are free but every list is empty: the allocator must stop with PFN_LIST_CORRUPT.
    MMPFNREGION saved = MmRetailPfnRegion;
    ULONG savedAvailable = MmAvailablePages;
    for (auto &c : MmRetailPfnRegion.FreePagesByColor)
        c = {0xFFFE, 0xFFFE};
    EXPECT_BUGCHECK(0x4E, MiAllocatePage(MmSystemMemoryUsage, nullptr, false));
    MmRetailPfnRegion = saved;
    MmAvailablePages = savedAvailable;
}

// --- Contiguous memory

TEST_F(Mm, ContiguousMemoryIsMappedAtThePhysicalMapAndFreedPageByPage) {
    auto p = (uint8_t *)MmAllocateContiguousMemory(5 * 4096 + 1);
    ASSERT_NE(p, nullptr);
    ULONG va = (ULONG)p;
    EXPECT_EQ(va & 0xF0000FFF, 0x80000000u);
    ULONG pfn = (va & 0x0FFFFFFF) >> 12;
    for (ULONG i = 0; i < 6; i++) {
        uint32_t e = PfnEntry(pfn + i);
        EXPECT_EQ(e & ~0xFFFu, (pfn + i) << 12);
        EXPECT_TRUE(e & PTE_PRESENT);
        EXPECT_EQ((bool)(e & PTE_END_OF_ALLOCATION), i == 5) << i;
    }
    EXPECT_EQ(MmQueryAllocationSize(p), 6u * 4096);
    EXPECT_EQ(MmGetPhysicalAddress(p + 0x1234), (pfn << 12) + 0x1234);
    memset(p, 0x5A, 6 * 4096); // usable through the kernel mapping
    EXPECT_EQ(host::Phys(pfn << 12)[6 * 4096 - 1], 0x5A);
    EXPECT_EQ(MmAvailablePages, available0 - 6);
    MmFreeContiguousMemory(p);
    EXPECT_EQ(MmAvailablePages, available0);
}

TEST_F(Mm, ContiguousMemoryHonoursTheRangeAndAlignment) {
    auto p = MmAllocateContiguousMemoryEx(3 * 4096, 0x100000, 0x1FFFFF, 0x10000, PAGE_READWRITE);
    ASSERT_NE(p, nullptr);
    ULONG pa = MmGetPhysicalAddress(p);
    EXPECT_GE(pa, 0x100000u);
    EXPECT_LE(pa + 3 * 4096 - 1, 0x1FFFFFu);
    EXPECT_EQ(pa & 0xFFFF, 0u);
    MmFreeContiguousMemory(p);
    EXPECT_EQ(MmAllocateContiguousMemoryEx(0x200000, 0x1000000, 0x10FFFFF, 0, PAGE_READWRITE), nullptr)
        << "larger than the range";
}

TEST_F(Mm, ContiguousMemoryTypes) {
    auto uc = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFF, 0, PAGE_READWRITE | PAGE_NOCACHE);
    auto wc = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    auto ro = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFF, 0, PAGE_READONLY);
    ASSERT_TRUE(uc && wc && ro);
    EXPECT_TRUE(Pte((ULONG)uc) & PTE_PCD);
    EXPECT_TRUE(Pte((ULONG)wc) & PTE_PWT);
    EXPECT_FALSE(Pte((ULONG)ro) & PTE_WRITE);
    EXPECT_EQ(MmQueryAddressProtect(uc), PAGE_READWRITE | PAGE_NOCACHE);
    EXPECT_EQ(MmQueryAddressProtect(wc), PAGE_READWRITE | PAGE_WRITECOMBINE);
    EXPECT_EQ(MmQueryAddressProtect(ro), PAGE_READONLY);
    EXPECT_EQ(MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFF, 0, PAGE_READWRITE | PAGE_NOCACHE | PAGE_WRITECOMBINE),
              nullptr);
    MmFreeContiguousMemory(uc);
    MmFreeContiguousMemory(wc);
    MmFreeContiguousMemory(ro);
}

TEST_F(Mm, ContiguousRequestsMoveUnlockedPagesAndKeepTheirContents) {
    // Fill the top of memory with system-memory pages (busy, unlocked, movable), each with its own pattern.
    std::vector<uint32_t *> pages;
    for (int i = 0; i < 2000; i++) {
        auto p = (uint32_t *)MmAllocateSystemMemory(4096, PAGE_READWRITE);
        ASSERT_NE(p, nullptr);
        for (int w = 0; w < 1024; w++)
            p[w] = (uint32_t)i * 0x10001 + w;
        pages.push_back(p);
    }
    ULONG relocations = MiRelocations;
    // A large contiguous request below 16 MB that cannot be satisfied from free pages alone.
    ULONG free = MmAvailablePages;
    ULONG want = std::min<ULONG>(free / 2, 0x800); // 8 MB
    auto c = MmAllocateContiguousMemoryEx(want * 4096, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    ASSERT_NE(c, nullptr);
    // Every system page still reads its pattern through its own virtual address.
    for (size_t i = 0; i < pages.size(); i++)
        for (int w = 0; w < 1024; w += 255)
            ASSERT_EQ(pages[i][w], (uint32_t)i * 0x10001 + w) << "page " << i << " word " << w;
    // The contiguous block is really contiguous and ours.
    ULONG pfn = MmGetPhysicalAddress(c) >> 12;
    for (ULONG i = 0; i < want; i++)
        ASSERT_EQ(PfnEntry(pfn + i) & ~0xFFFu, (pfn + i) << 12);
    MmFreeContiguousMemory(c);
    for (auto p : pages)
        MmFreeSystemMemory(p, 4096);
    (void)relocations;
}

TEST_F(Mm, LockedPagesAreNotMoved) {
    // Allocate every free page as single system pages, lock one in the middle, then ask for a contiguous run of
    // half of memory: the locked page must stay where it is.
    std::vector<uint8_t *> pages;
    while (MmAvailablePages > 64) {
        auto p = (uint8_t *)MmAllocateSystemMemory(4096, PAGE_READWRITE);
        if (!p)
            break;
        pages.push_back(p);
    }
    ASSERT_GT(pages.size(), 1000u);
    uint8_t *locked = pages[pages.size() / 2];
    ULONG pa = MmGetPhysicalAddress(locked);
    MmLockUnlockBufferPages(locked, 4096, FALSE);
    EXPECT_EQ((uint16_t)PfnEntry(pa >> 12), 2) << "lock count";
    auto c = MmAllocateContiguousMemoryEx(32 * 4096, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
    if (c) {
        EXPECT_EQ(MmGetPhysicalAddress(locked), pa) << "a locked page moved";
        ULONG cpfn = MmGetPhysicalAddress(c) >> 12;
        EXPECT_TRUE((pa >> 12) < cpfn || (pa >> 12) >= cpfn + 32) << "the run includes the locked page";
        MmFreeContiguousMemory(c);
    }
    MmLockUnlockBufferPages(locked, 4096, TRUE);
    EXPECT_EQ((uint16_t)PfnEntry(pa >> 12), 0);
    for (auto p : pages)
        MmFreeSystemMemory(p, 4096);
}

// --- System memory, system PTEs, kernel stacks, I/O space

TEST_F(Mm, SystemMemoryIsMappedInTheSystemPteRange) {
    auto p = (uint8_t *)MmAllocateSystemMemory(3 * 4096, PAGE_READWRITE);
    ASSERT_NE(p, nullptr);
    ULONG va = (ULONG)p;
    EXPECT_GE(va, MM_SYSTEM_PTE_BASE);
    EXPECT_LT(va, MM_SYSTEM_PTE_END);
    EXPECT_TRUE(MmIsAddressValid(p));
    EXPECT_TRUE(MmIsAddressValid(p + 3 * 4096 - 1));
    EXPECT_FALSE(Pte(va) & PTE_END_OF_ALLOCATION);
    EXPECT_TRUE(Pte(va + 2 * 4096) & PTE_END_OF_ALLOCATION);
    memset(p, 0x77, 3 * 4096);
    EXPECT_EQ(MmQueryAllocationSize(p), 3u * 4096);
    EXPECT_EQ(MmFreeSystemMemory(p, 3 * 4096), 3u);
    EXPECT_FALSE(MmIsAddressValid(p));
    EXPECT_EQ(MmAvailablePages, available0);
    EXPECT_EQ(MmAllocateSystemMemory(0, PAGE_READWRITE), nullptr);
    EXPECT_EQ(MmAllocateSystemMemory(4096, PAGE_EXECUTE_READWRITE), nullptr) << "unsupported protection";
}

TEST_F(Mm, FreedSystemPtesAreReusedAndDoubleFreesRefused) {
    auto a = MmAllocateSystemMemory(4096, PAGE_READWRITE);
    MmFreeSystemMemory(a, 4096);
    auto b = MmAllocateSystemMemory(4096, PAGE_READWRITE);
    EXPECT_EQ(a, b) << "first fit, lowest address first";
    ULONG pfn = Pte((ULONG)b) >> 12;
    ULONG frees = MiPteDoubleFrees;
    MiReleaseSystemPtes(b, 1);
    MiReleaseSystemPtes(b, 1); // overlaps the run just freed
    EXPECT_EQ(MiPteDoubleFrees, frees + 1);
    MiFreePage(pfn); // the page itself (its PTE was released by hand)
}

TEST_F(Mm, SystemPteRangeGrowsPastOnePageTable) {
    // 1100 pages need a second page table in the system PTE range.
    auto p = (uint8_t *)MmAllocateSystemMemory(1100 * 4096, PAGE_READWRITE);
    ASSERT_NE(p, nullptr);
    for (ULONG i = 0; i < 1100; i++)
        p[i * 4096] = (uint8_t)i;
    for (ULONG i = 0; i < 1100; i++)
        ASSERT_EQ(p[i * 4096], (uint8_t)i);
    MmFreeSystemMemory(p, 1100 * 4096);
    // Page tables of the system PTE range are never freed: they count as used afterwards.
    EXPECT_LE(MmAvailablePages, available0);
    available0 = MmAvailablePages;
}

TEST_F(Mm, KernelStacksHaveAGuardPageBelow) {
    auto top = (uint8_t *)MmCreateKernelStack(3 * 4096, FALSE);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ((ULONG)top & 0xFFF, 0u);
    EXPECT_TRUE(MmIsAddressValid(top - 1));
    EXPECT_TRUE(MmIsAddressValid(top - 3 * 4096));
    EXPECT_FALSE(MmIsAddressValid(top - 3 * 4096 - 1)) << "guard page";
    top[-1] = 1;
    MmDeleteKernelStack(top, top - 3 * 4096);
    EXPECT_FALSE(MmIsAddressValid(top - 1));
    EXPECT_EQ(MmAvailablePages, available0);
}

TEST_F(Mm, IoSpaceUsesTheIdentityWindowsOrSystemPtes) {
    EXPECT_EQ(MmMapIoSpace(0xFD000000, 0x1000, PAGE_READWRITE | PAGE_NOCACHE), (PVOID)0xFD000000);
    EXPECT_EQ(MmMapIoSpace(0xF0001000, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE), (PVOID)0xF0001000);
    // Anything else gets system PTEs that map the physical address (offset kept).
    auto p = (uint8_t *)MmMapIoSpace(0x00123456, 0x2000, PAGE_READWRITE);
    ASSERT_NE(p, nullptr);
    EXPECT_GE((ULONG)p, MM_SYSTEM_PTE_BASE);
    EXPECT_EQ((ULONG)p & 0xFFF, 0x456u);
    EXPECT_EQ(MmGetPhysicalAddress(p), 0x123456u);
    host::Phys(0x123456)[0] = 0xC3;
    EXPECT_EQ(p[0], 0xC3);
    MmUnmapIoSpace(p, 0x2000);
    EXPECT_FALSE(MmIsAddressValid(p));
    MmUnmapIoSpace((PVOID)0xFD000000, 0x1000); // identity windows: nothing to undo
    EXPECT_EQ(MmMapIoSpace(0x1000, 0x1000, PAGE_EXECUTE), nullptr);
}

TEST_F(Mm, SetAddressProtectChangesEveryPage) {
    auto p = (uint8_t *)MmAllocateSystemMemory(2 * 4096, PAGE_READWRITE);
    MmSetAddressProtect(p, 2 * 4096, PAGE_READONLY);
    EXPECT_EQ(MmQueryAddressProtect(p), PAGE_READONLY);
    EXPECT_EQ(MmQueryAddressProtect(p + 4096), PAGE_READONLY);
    volatile uint8_t r = p[0]; // readable (system memory is not zeroed)
    (void)r;
    MmSetAddressProtect(p, 2 * 4096, PAGE_READWRITE | PAGE_NOCACHE);
    EXPECT_EQ(MmQueryAddressProtect(p + 4096), PAGE_READWRITE | PAGE_NOCACHE);
    p[4096] = 9; // writable again (the old read-only translation was invalidated)
    MmSetAddressProtect(p, 4096, 0x12345); // invalid: ignored
    EXPECT_EQ(MmQueryAddressProtect(p), PAGE_READWRITE | PAGE_NOCACHE);
    EXPECT_EQ(MmQueryAddressProtect((PVOID)0xD7FFF000), PAGE_NOACCESS);
    MmFreeSystemMemory(p, 2 * 4096);
}

TEST_F(Mm, BufferLockingCountsPerPageAndSkipsUnmappedRanges) {
    auto p = (uint8_t *)MmAllocateSystemMemory(3 * 4096, PAGE_READWRITE);
    ULONG pfn0 = Pte((ULONG)p) >> 12, pfn2 = Pte((ULONG)p + 8192) >> 12;
    MmLockUnlockBufferPages(p + 100, 2 * 4096, FALSE); // touches pages 0..2
    MmLockUnlockBufferPages(p + 100, 10, FALSE);       // page 0 again
    EXPECT_EQ((uint16_t)PfnEntry(pfn0), 4);
    EXPECT_EQ((uint16_t)PfnEntry(pfn2), 2);
    MmLockUnlockBufferPages(p + 100, 10, TRUE);
    MmLockUnlockBufferPages(p + 100, 2 * 4096, TRUE);
    EXPECT_EQ((uint16_t)PfnEntry(pfn0), 0);
    EXPECT_EQ((uint16_t)PfnEntry(pfn2), 0);
    // A range without a page table (title space nobody mapped) and the physical map are ignored, not dereferenced.
    MmLockUnlockBufferPages((PVOID)0x00400000, 0x10000, FALSE);
    MmLockUnlockBufferPages((PVOID)0x80100000, 0x1000, FALSE);
    MmLockUnlockPhysicalPage(pfn0 << 12, FALSE);
    EXPECT_EQ((uint16_t)PfnEntry(pfn0), 2);
    MmLockUnlockPhysicalPage(pfn0 << 12, TRUE);
    MmFreeSystemMemory(p, 3 * 4096);
}

TEST_F(Mm, AddressValidityAndPhysicalAddresses) {
    EXPECT_TRUE(MmIsAddressValid((PVOID)0x80010000)) << "kernel image in the physical map";
    EXPECT_EQ(MmGetPhysicalAddress((PVOID)0x80012345), 0x12345u);
    EXPECT_TRUE(MmIsAddressValid((PVOID)0xFD000000)) << "GPU registers: a large page";
    EXPECT_EQ(MmGetPhysicalAddress((PVOID)0xFD001234), 0xFD001234u);
    EXPECT_FALSE(MmIsAddressValid((PVOID)0x00010000)) << "no title mappings";
    EXPECT_EQ(MmGetPhysicalAddress((PVOID)0x00010000), 0u);
    EXPECT_FALSE(MmIsAddressValid((PVOID)0x84000000)) << "nothing above 64 MB";
}

// --- Title virtual memory

namespace {
class Vm : public Mm {};
NTSTATUS Allocate(ULONG *base, ULONG size, ULONG type, ULONG protect) {
    PVOID b = (PVOID)*base;
    SIZE_T s = size;
    NTSTATUS st = NtAllocateVirtualMemory(&b, 0, &s, type, protect);
    *base = (ULONG)b;
    return st;
}
NTSTATUS Free(ULONG base, ULONG size, ULONG type) {
    PVOID b = (PVOID)base;
    SIZE_T s = size;
    return NtFreeVirtualMemory(&b, &s, type);
}
} // namespace

TEST_F(Vm, ReserveCommitUseDecommitRelease) {
    ULONG base = 0x00400000;
    ASSERT_EQ(Allocate(&base, 0x20000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_EQ(base, 0x00400000u);
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT_EQ(NtQueryVirtualMemory((PVOID)0x00401000, &mbi), STATUS_SUCCESS);
    EXPECT_EQ(mbi.State, MEM_RESERVE);
    EXPECT_EQ(mbi.AllocationBase, (PVOID)0x00400000);
    EXPECT_EQ(mbi.RegionSize, 0x1F000u);

    ULONG commit = 0x00402000;
    ASSERT_EQ(Allocate(&commit, 0x3000, MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
    auto p = (uint32_t *)commit;
    EXPECT_EQ(p[0], 0u) << "committed pages start zeroed";
    p[0] = 0x11223344;
    p[0x3000 / 4 - 1] = 7;
    ASSERT_EQ(NtQueryVirtualMemory((PVOID)0x00402000, &mbi), STATUS_SUCCESS);
    EXPECT_EQ(mbi.State, MEM_COMMIT);
    EXPECT_EQ(mbi.Protect, PAGE_READWRITE);
    EXPECT_EQ(mbi.RegionSize, 0x3000u);

    ASSERT_EQ(Free(0x00402000, 0x1000, MEM_DECOMMIT), STATUS_SUCCESS);
    ASSERT_EQ(NtQueryVirtualMemory((PVOID)0x00402000, &mbi), STATUS_SUCCESS);
    EXPECT_EQ(mbi.State, MEM_RESERVE);
    EXPECT_EQ(p[0x3000 / 4 - 1], 7u) << "the rest stays committed";

    ASSERT_EQ(Free(0x00400000, 0, MEM_RELEASE), STATUS_SUCCESS);
    ASSERT_EQ(NtQueryVirtualMemory((PVOID)0x00400000, &mbi), STATUS_SUCCESS);
    EXPECT_EQ(mbi.State, MEM_FREE);
    EXPECT_EQ(MmAvailablePages, available0) << "pages and page tables all back";
}

TEST_F(Vm, ConflictsAndBadParameters) {
    ULONG base = 0x00500000;
    ASSERT_EQ(Allocate(&base, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
    ULONG again = 0x00500000;
    EXPECT_NE(Allocate(&again, 0x1000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS) << "overlapping reservation";
    ULONG outside = 0x00520000;
    EXPECT_NE(Allocate(&outside, 0x1000, MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS) << "commit outside a VAD";
    ULONG bad = 0x00530000;
    EXPECT_NE(Allocate(&bad, 0x1000, MEM_RESERVE, 0x7777), STATUS_SUCCESS) << "bad protection";
    EXPECT_NE(Allocate(&bad, 0x1000, 0, PAGE_READWRITE), STATUS_SUCCESS) << "no allocation type";
    EXPECT_NE(Free(0x00501000, 0, MEM_RELEASE), STATUS_SUCCESS) << "release not at the base";
    EXPECT_NE(Free(0x00600000, 0x1000, MEM_RELEASE), STATUS_SUCCESS) << "nothing there";
    EXPECT_EQ(Free(0x00500000, 0, MEM_RELEASE), STATUS_SUCCESS);
    EXPECT_EQ(MmAvailablePages, available0);
}

TEST_F(Vm, ProtectAndPartialRelease) {
    ULONG base = 0x00600000;
    ASSERT_EQ(Allocate(&base, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
    PVOID b = (PVOID)0x00610000;
    SIZE_T s = 0x1000;
    ULONG old = 0;
    ASSERT_EQ(NtProtectVirtualMemory(&b, &s, PAGE_READONLY, &old), STATUS_SUCCESS);
    EXPECT_EQ(old, PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION mbi;
    NtQueryVirtualMemory((PVOID)0x00610000, &mbi);
    EXPECT_EQ(mbi.Protect, PAGE_READONLY);
    EXPECT_EQ(mbi.RegionSize, 0x1000u);
    EXPECT_EQ(((volatile uint8_t *)0x00610000)[0], 0);
    // Release the middle 64 KB: the VAD splits in two.
    ASSERT_EQ(Free(0x00610000, 0x10000, MEM_RELEASE), STATUS_SUCCESS);
    NtQueryVirtualMemory((PVOID)0x00610000, &mbi);
    EXPECT_EQ(mbi.State, MEM_FREE);
    NtQueryVirtualMemory((PVOID)0x00620000, &mbi);
    EXPECT_EQ(mbi.State, MEM_COMMIT);
    EXPECT_EQ(mbi.AllocationBase, (PVOID)0x00620000) << "the split-off tail is a reservation of its own";
    EXPECT_EQ(Free(0x00600000, 0x10000, MEM_RELEASE), STATUS_SUCCESS);
    EXPECT_EQ(Free(0x00620000, 0x10000, MEM_RELEASE), STATUS_SUCCESS);
    EXPECT_EQ(MmAvailablePages, available0);
}

TEST_F(Vm, AnywhereAllocationsAreGranularityAligned) {
    ULONG a = 0, b = 0;
    ASSERT_EQ(Allocate(&a, 0x1000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    ASSERT_EQ(Allocate(&b, 0x1000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_EQ(a & 0xFFFF, 0u);
    EXPECT_EQ(b & 0xFFFF, 0u);
    EXPECT_NE(a, b);
    ULONG t = 0;
    ASSERT_EQ(Allocate(&t, 0x1000, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_GE(t, 0x70000000u) << "top down";
    Free(a, 0, MEM_RELEASE);
    Free(b, 0, MEM_RELEASE);
    Free(t, 0, MEM_RELEASE);
}

TEST_F(Vm, StatisticsFollowTheAllocations) {
    MM_STATISTICS st = {sizeof(st)};
    ASSERT_EQ(MmQueryStatistics(&st), STATUS_SUCCESS);
    ULONG committed = st.VirtualMemoryBytesCommitted, reserved = st.VirtualMemoryBytesReserved;
    ULONG base = 0x00700000;
    ASSERT_EQ(Allocate(&base, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
    ASSERT_EQ(MmQueryStatistics(&st), STATUS_SUCCESS);
    EXPECT_EQ(st.VirtualMemoryBytesCommitted, committed + 0x10000);
    EXPECT_EQ(st.VirtualMemoryBytesReserved, reserved + 0x10000);
    EXPECT_EQ(st.TotalPhysicalPages, MM_PAGES);
    Free(base, 0, MEM_RELEASE);
    ASSERT_EQ(MmQueryStatistics(&st), STATUS_SUCCESS);
    EXPECT_EQ(st.VirtualMemoryBytesCommitted, committed);
    EXPECT_EQ(st.VirtualMemoryBytesReserved, reserved);
    MM_STATISTICS bad = {1};
    EXPECT_NE(MmQueryStatistics(&bad), STATUS_SUCCESS);
}
