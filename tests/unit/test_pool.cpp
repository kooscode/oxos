// Executive pool: block format, sizes, reuse, splitting, page allocations, and the page hand-over race.
#include "kernel_test.h"

#include <map>
#include <random>
#include <vector>

extern volatile ULONG ExpPoolPages, ExpPoolBlocks;

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
        EXPECT_EQ(h->PoolTag, (ULONG)'tseT');
        EXPECT_EQ(ExQueryPoolBlockSize(p), units * 32 - 8) << size;
        EXPECT_GE(ExQueryPoolBlockSize(p), size);
        memset(p, 0xEE, size);
        ExFreePool(p);
        EXPECT_EQ(h->PoolType, 0) << "free";
    }
}

TEST(Pool, LargeRequestsGetWholePages) {
    auto p = (uint8_t *)ExAllocatePool(0xFD9);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ((ULONG)p & 0xFFF, 0u);
    EXPECT_EQ(ExQueryPoolBlockSize(p), 0x1000u);
    auto q = (uint8_t *)ExAllocatePool(3 * 4096 + 5);
    EXPECT_EQ(ExQueryPoolBlockSize(q), 4u * 4096);
    memset(q, 1, 4 * 4096);
    ULONG available = MmAvailablePages;
    ExFreePool(q);
    EXPECT_EQ(MmAvailablePages, available + 4);
    ExFreePool(p);
}

TEST(Pool, FreedBlocksAreReusedBySize) {
    auto a = ExAllocatePool(100);
    ExFreePool(a);
    auto b = ExAllocatePool(100);
    EXPECT_EQ(a, b);
    ExFreePool(b);
}

TEST(Pool, LargerFreeBlocksAreSplitBeforeANewPageIsStarted) {
    // A 1000-byte block (32 units) freed, then two 400-byte blocks (13 units): when the current page cannot hold
    // them, they come out of the freed block.
    std::vector<void *> fill;
    ULONG pages = ExpPoolPages;
    // Start a fresh page and fill it so that nothing small fits any more.
    while (ExpPoolPages == pages)
        fill.push_back(ExAllocatePool(0xFD8));
    void *big = ExAllocatePool(1000);
    for (int i = 0; i < 4; i++)
        fill.push_back(ExAllocatePool(0xFD8)); // each on a page of its own: the current page is full
    ExFreePool(big);
    pages = ExpPoolPages;
    auto a = (uint8_t *)ExAllocatePool(0x3C0); // 31 units: needs a new page or the freed 32-unit block
    EXPECT_EQ(ExpPoolPages, pages) << "split the free block instead of starting a page";
    EXPECT_EQ((void *)a, big);
    ExFreePool(a);
    for (void *p : fill)
        ExFreePool(p);
}

TEST(Pool, DoubleFreeIsABugCheck) {
    auto p = ExAllocatePool(64);
    ExFreePool(p);
    EXPECT_BUGCHECK(0xC2, ExFreePool(p));
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

// --- The page hand-over race (fixed 2026-10-23): while ExAllocatePoolWithTag drops IRQL to map a new page, a DPC
// allocates from the pool. It must not see the old, full page as current and hand its tail out a second time.

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

    // 127-unit blocks take a page each; the page's 1-unit tail goes to the free list when the next one starts.
    void *first = ExAllocatePool(0xFD8);
    host::RaiseIrq(5); // delivered when ExAllocatePoolWithTag first lowers IRQL: inside the hand-over
    void *second = ExAllocatePool(0xFD8);
    EXPECT_EQ(RaceDpcRuns, 1) << "the DPC did not run inside the hand-over";
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
