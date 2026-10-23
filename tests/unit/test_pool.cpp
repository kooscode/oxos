// Executive pool: block format, sizes, lookaside lists, splitting, merging, page release, and the page hand-over
// race.
#include "kernel_test.h"

#include <map>
#include <random>
#include <vector>

extern volatile ULONG ExpPoolPages, ExpPoolLargePages, ExpPoolBlocks;

namespace {
struct PoolHeader {
    UCHAR PreviousSize, PoolIndex, PoolType, BlockSize;
    ULONG PoolTag;
};
PoolHeader *Header(void *p) { return (PoolHeader *)p - 1; }
} // namespace

TEST(Pool, SmallBlocksCarryTheReferenceHeader) {
    for (ULONG size : {1u, 24u, 25u, 100u, 1000u, 0xFD8u}) {
        auto p = (uint8_t *)ExAllocatePoolWithTag(size, 'tseT');
        ASSERT_NE(p, nullptr) << size;
        EXPECT_EQ((ULONG)p & 7, 0u) << "8-byte aligned";
        PoolHeader *h = Header(p);
        ULONG units = (size + 8 + 31) / 32;
        EXPECT_EQ(h->BlockSize, units) << size;
        EXPECT_EQ(h->PoolType, 1) << "in use";
        EXPECT_EQ(h->PoolIndex, 0x80) << "in use";
        EXPECT_EQ(h->PoolTag, (ULONG)'tseT');
        EXPECT_EQ(*(uint32_t *)p, 0u) << "the first body dword is cleared";
        EXPECT_EQ(ExQueryPoolBlockSize(p), units * 32 - 8) << size;
        EXPECT_GE(ExQueryPoolBlockSize(p), size);
        memset(p, 0xEE, size);
        ExFreePool(p); // the header may be gone now: its page goes back once it is all free
    }
}

TEST(Pool, LargeRequestsGetWholePages) {
    ULONG large = ExpPoolLargePages;
    auto p = (uint8_t *)ExAllocatePool(0xFD9);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ((ULONG)p & 0xFFF, 0u);
    EXPECT_EQ(ExQueryPoolBlockSize(p), 0x1000u);
    auto q = (uint8_t *)ExAllocatePool(3 * 4096 + 5);
    EXPECT_EQ(ExQueryPoolBlockSize(q), 4u * 4096);
    EXPECT_EQ(ExpPoolLargePages, large + 5);
    memset(q, 1, 4 * 4096);
    ULONG available = MmAvailablePages;
    ExFreePool(q);
    EXPECT_EQ(MmAvailablePages, available + 4);
    ExFreePool(p);
    EXPECT_EQ(ExpPoolLargePages, large);
}

TEST(Pool, SmallSizesGoThroughALookasideListOfTwo) {
    // 100 bytes = 4 units. Three allocations empty the 4-unit lookaside list (it holds two at most).
    void *a = ExAllocatePool(100), *b = ExAllocatePool(100), *c = ExAllocatePool(100);
    ExFreePool(a);
    ExFreePool(b);
    EXPECT_EQ(Header(a)->PoolType, 1) << "on the lookaside list: never merged";
    EXPECT_EQ(Header(a)->PoolIndex, 0) << "no longer handed out";
    ExFreePool(c); // the list is full: to the free lists
    EXPECT_EQ(ExAllocatePool(100), b) << "last in, first out";
    EXPECT_EQ(ExAllocatePool(100), a);
    ExFreePool(a);
    ExFreePool(b);
}

namespace {
// Allocates blocks of `units` units into `keep` until one comes from a new pool page. The free lists then hold no
// block of more than `units` units (an allocation takes any such block before starting a page), except the new
// page's remainder. Returns the new page.
uint8_t *FreshPage(ULONG units, std::vector<void *> &keep) {
    for (;;) {
        ULONG pages = ExpPoolPages;
        auto p = (uint8_t *)ExAllocatePool(units * 32 - 8);
        keep.push_back(p);
        if (ExpPoolPages != pages) {
            EXPECT_EQ((ULONG)p & 0xFFF, 8u) << "a new page's first block is at its start";
            return p - 8;
        }
    }
}
} // namespace

TEST(Pool, BlocksSplitFromTheFrontAtAPageStartAndFromTheEndElsewhere) {
    std::vector<void *> keep;
    uint8_t *page = FreshPage(32, keep);
    ASSERT_EQ(keep.back(), page + 8);
    EXPECT_EQ(Header(page + 8)->PreviousSize, 0);
    auto rest = (PoolHeader *)(page + 0x400);
    EXPECT_EQ(rest->PoolType, 0) << "the remainder is free";
    EXPECT_EQ(rest->BlockSize, 96);
    EXPECT_EQ(rest->PreviousSize, 32);

    void *b = ExAllocatePool(32 * 32 - 8);
    EXPECT_EQ(b, page + 0xC08) << "not at a page start: the end of the free block";
    EXPECT_EQ(Header(b)->PreviousSize, 64);
    EXPECT_EQ(rest->BlockSize, 64);
    void *c = ExAllocatePool(32 * 32 - 8);
    EXPECT_EQ(c, page + 0x808);
    EXPECT_EQ(Header(b)->PreviousSize, 32) << "the block behind follows the split";
    EXPECT_EQ(rest->BlockSize, 32);

    // A free block of exactly the requested size is not taken: the search starts one size up.
    ULONG pages = ExpPoolPages;
    void *d = ExAllocatePool(32 * 32 - 8);
    EXPECT_EQ(ExpPoolPages, pages + 1);
    EXPECT_NE((uint8_t *)d - page, 0x408);
    EXPECT_EQ(rest->PoolType, 0);

    for (void *p : {b, c, d})
        ExFreePool(p);
    for (void *p : keep)
        ExFreePool(p);
}

TEST(Pool, FreedNeighboursMergeAndAnEmptyPageGoesBack) {
    std::vector<void *> keep;
    uint8_t *page = FreshPage(32, keep);
    void *a = keep.back();
    keep.pop_back();
    void *b = ExAllocatePool(32 * 32 - 8); // page + 0xC00
    void *c = ExAllocatePool(32 * 32 - 8); // page + 0x800; page + 0x400 stays free (32 units)
    ASSERT_EQ(b, page + 0xC08);
    ASSERT_EQ(c, page + 0x808);
    auto free400 = (PoolHeader *)(page + 0x400);

    ExFreePool(c); // merges with the free block in front
    EXPECT_EQ(free400->BlockSize, 64);
    EXPECT_EQ(free400->PoolType, 0);
    EXPECT_EQ(Header(b)->PreviousSize, 64) << "the block behind learns the merged size";
    ExFreePool(b); // merges in front again, up to the page end
    EXPECT_EQ(free400->BlockSize, 96);

    ULONG pages = ExpPoolPages, available = MmAvailablePages;
    ExFreePool(a); // merges with the block behind: the whole page is free
    EXPECT_EQ(ExpPoolPages, pages - 1);
    EXPECT_EQ(MmAvailablePages, available + 1) << "the page went back to the memory manager";
    EXPECT_FALSE(MmIsAddressValid(page));
    for (void *p : keep)
        ExFreePool(p);
}

TEST(Pool, FreeingEverythingGivesThePagesBack) {
    ULONG available = MmAvailablePages;
    std::vector<void *> blocks;
    for (int i = 0; i < 200; i++)
        blocks.push_back(ExAllocatePool(300 + (i % 7) * 100)); // 10-31 units: no lookaside list
    EXPECT_LT(MmAvailablePages, available);
    for (size_t i = 0; i < blocks.size(); i += 2)
        ExFreePool(blocks[i]);
    for (size_t i = 1; i < blocks.size(); i += 2)
        ExFreePool(blocks[i]);
    EXPECT_GE(MmAvailablePages, available) << "every page those blocks needed is back";
    EXPECT_EQ(CheckPfnDatabase(), "");
}

TEST(Pool, DoubleFreeIsABugCheck) {
    auto p = ExAllocatePool(64);
    ExFreePool(p);
    EXPECT_BUGCHECK(0xC2, ExFreePool(p));
    EXPECT_EQ(host::LastBugCheck[1], 7u);
    EXPECT_EQ(host::LastBugCheck[2], 0x376u);
    EXPECT_EQ(host::LastBugCheck[3], (uint32_t)(uintptr_t)p - 8);
    EXPECT_EQ(host::LastBugCheck[4], (uint32_t)(uintptr_t)p);
    auto q = ExAllocatePool(2000);
    ExFreePool(q);
    if (MmIsAddressValid(q))
        EXPECT_BUGCHECK(0xC2, ExFreePool(q)); // from the free lists as well
}

TEST(Pool, RandomTrafficNeverHandsOutOverlappingBlocks) {
    std::mt19937 rng(1234);
    std::map<uint8_t *, ULONG> live;
    for (int step = 0; step < 20000; step++) {
        if (live.empty() || (rng() % 3) != 0) {
            ULONG size = 1 + rng() % (rng() % 4 ? 300 : 5000);
            auto p = (uint8_t *)ExAllocatePool(size);
            ASSERT_NE(p, nullptr);
            // No overlap with any live block.
            auto next = live.lower_bound(p);
            if (next != live.end())
                ASSERT_LE(p + size, next->first) << "overlaps the next block";
            if (next != live.begin()) {
                auto prev = std::prev(next);
                ASSERT_LE(prev->first + prev->second, p) << "overlaps the previous block";
            }
            memset(p, (uint8_t)(uintptr_t)p, size);
            live[p] = size;
        } else {
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            uint8_t fill = (uint8_t)(uintptr_t)it->first;
            for (ULONG i = 0; i < it->second; i++)
                ASSERT_EQ(it->first[i], fill) << "block " << (void *)it->first << " overwritten at " << i;
            ExFreePool(it->first);
            live.erase(it);
        }
    }
    for (auto &[p, size] : live)
        ExFreePool(p);
}

// --- The page hand-over race (fixed 2026-10-23, and gone with the reference allocator, which maps a new page without
// dropping IRQL): a DPC that allocates from the pool around a new page must get memory of its own.

namespace {
KINTERRUPT RaceInterrupt;
KDPC RaceDpc;
void *RaceBlock;
volatile int RaceDpcRuns;

void NTAPI RaceDpcRoutine(PKDPC, PVOID, PVOID, PVOID) {
    RaceBlock = ExAllocatePool(0xFD8);
    RaceDpcRuns = RaceDpcRuns + 1;
}
BOOLEAN NTAPI RaceIsr(PKINTERRUPT, PVOID) {
    KeInsertQueueDpc(&RaceDpc, nullptr, nullptr);
    return TRUE;
}
} // namespace

TEST(Pool, AnAllocationInsideThePageHandOverGetsItsOwnMemory) {
    KIRQL irql;
    ULONG vector = HalGetInterruptVector(5, &irql);
    KeInitializeDpc(&RaceDpc, RaceDpcRoutine, nullptr);
    KeInitializeInterrupt(&RaceInterrupt, RaceIsr, nullptr, vector, irql, LevelSensitive, FALSE);
    ASSERT_TRUE(KeConnectInterrupt(&RaceInterrupt));

    // 127-unit blocks take a page each, leaving a 1-unit block free behind them.
    void *first = ExAllocatePool(0xFD8);
    host::RaiseIrq(5); // delivered when ExAllocatePoolWithTag lowers IRQL
    void *second = ExAllocatePool(0xFD8);
    EXPECT_EQ(RaceDpcRuns, 1) << "the DPC did not run";
    EXPECT_NE(RaceBlock, second);
    EXPECT_NE(RaceBlock, first);
    // Every 1-unit tail handed out now must be a block of its own.
    std::vector<uint8_t *> tails;
    for (int i = 0; i < 64; i++)
        tails.push_back((uint8_t *)ExAllocatePool(24));
    std::sort(tails.begin(), tails.end());
    for (size_t i = 1; i < tails.size(); i++)
        EXPECT_NE(tails[i], tails[i - 1]) << "the same block handed out twice";
    for (auto t : tails)
        ExFreePool(t);
    ExFreePool(first);
    ExFreePool(second);
    ExFreePool(RaceBlock);
    KeDisconnectInterrupt(&RaceInterrupt);
}
