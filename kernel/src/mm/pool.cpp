// Executive pool (spec/09 §6). Same block format as the reference: an 8-byte header in front of each
// small block, with the block size in 32-byte units (header included) at byte 3, so callers see
// ExQueryPoolBlockSize == units * 32 - 8. Requests above 0xFD8 bytes get whole pages.
#include <xk/ke.h>
#include <xk/mm.h>

PVOID MiAllocateMappedPages(ULONG pages, ULONG pteBits, bool guardPage, ULONG usage);
ULONG MiFreeMappedPages(ULONG va, ULONG pages);

// Pool pages: system PTE range pages recorded as pool (MmQueryStatistics.PoolPagesCommitted).
static PVOID ExpAllocatePages(ULONG bytes) {
    return MiAllocateMappedPages((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT,
                                 PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY, false, MmPoolUsage);
}

struct POOL_HEADER {
    UCHAR PreviousSize;
    UCHAR PoolIndex;
    UCHAR PoolType; // 1 = in use
    UCHAR BlockSize; // 32-byte units
    ULONG PoolTag;
};
static_assert(sizeof(POOL_HEADER) == 8);

constexpr ULONG POOL_SMALL_LIMIT = 0xFD8, POOL_UNIT = 32, POOL_UNITS_PER_PAGE = PAGE_SIZE / POOL_UNIT;

struct FreeBlock {
    POOL_HEADER Header;
    FreeBlock *Next;
};

static FreeBlock *ExpFreeLists[POOL_UNITS_PER_PAGE]; // by size in units
static UCHAR *ExpCurrentPage;
static ULONG ExpCurrentUsed = POOL_UNITS_PER_PAGE;
volatile ULONG ExpPoolPages, ExpPoolBlocks;

void ExInitializePool() {
    for (auto &l : ExpFreeLists)
        l = nullptr;
    ExpCurrentPage = nullptr;
    ExpCurrentUsed = POOL_UNITS_PER_PAGE;
}

XBAPI PVOID NTAPI ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag) {
    if (NumberOfBytes > POOL_SMALL_LIMIT) {
        return ExpAllocatePages(NumberOfBytes);
    }

    ULONG units = (NumberOfBytes + sizeof(POOL_HEADER) + POOL_UNIT - 1) / POOL_UNIT;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    POOL_HEADER *h;
    FreeBlock *larger = nullptr;
    if (ExpCurrentUsed + units > POOL_UNITS_PER_PAGE && !ExpFreeLists[units])
        for (ULONG u = units + 1; u < POOL_UNITS_PER_PAGE && !larger; u++)
            larger = ExpFreeLists[u];
    if (FreeBlock *b = ExpFreeLists[units]) {
        ExpFreeLists[units] = b->Next;
        h = &b->Header;
    } else if (larger) {
        // Split the smallest larger free block rather than start a new page; the rest goes back by its size.
        ULONG have = larger->Header.BlockSize;
        ExpFreeLists[have] = larger->Next;
        auto rest = (FreeBlock *)((UCHAR *)larger + units * POOL_UNIT);
        rest->Header.PoolType = 0;
        rest->Header.BlockSize = (UCHAR)(have - units);
        rest->Next = ExpFreeLists[have - units];
        ExpFreeLists[have - units] = rest;
        h = &larger->Header;
    } else {
        if (ExpCurrentUsed + units > POOL_UNITS_PER_PAGE) {
            // Give the page's tail to the free list of its exact size, then start a new page.
            if (ExpCurrentPage && ExpCurrentUsed < POOL_UNITS_PER_PAGE) {
                auto tail = (FreeBlock *)(ExpCurrentPage + ExpCurrentUsed * POOL_UNIT);
                ULONG tailUnits = POOL_UNITS_PER_PAGE - ExpCurrentUsed;
                tail->Header.BlockSize = (UCHAR)tailUnits;
                tail->Next = ExpFreeLists[tailUnits];
                ExpFreeLists[tailUnits] = tail;
            }
            KfLowerIrql(old);
            auto page = (UCHAR *)ExpAllocatePages(PAGE_SIZE);
            if (!page)
                return nullptr;
            old = KeRaiseIrqlToDpcLevel();
            ExpCurrentPage = page;
            ExpCurrentUsed = 0;
            ExpPoolPages = ExpPoolPages + 1;
        }
        h = (POOL_HEADER *)(ExpCurrentPage + ExpCurrentUsed * POOL_UNIT);
        ExpCurrentUsed += units;
    }
    h->PreviousSize = 0;
    h->PoolIndex = 0;
    h->PoolType = 1;
    h->BlockSize = (UCHAR)units;
    h->PoolTag = Tag;
    ExpPoolBlocks = ExpPoolBlocks + 1;
    KfLowerIrql(old);
    return h + 1;
}

XBAPI PVOID NTAPI ExAllocatePool(SIZE_T NumberOfBytes) { return ExAllocatePoolWithTag(NumberOfBytes, 'enoN'); }

XBAPI void NTAPI ExFreePool(PVOID P) {
    if (((ULONG)P & (PAGE_SIZE - 1)) == 0) {
        MmFreeSystemMemory(P, MmQueryAllocationSize(P));
        return;
    }
    auto b = (FreeBlock *)((POOL_HEADER *)P - 1);
    if (b->Header.PoolType != 1)
        KeBugCheckEx(0xC2, 7, (ULONG)P, b->Header.PoolTag, 0); // BAD_POOL_CALLER: double free
    KIRQL old = KeRaiseIrqlToDpcLevel();
    b->Header.PoolType = 0;
    b->Next = ExpFreeLists[b->Header.BlockSize];
    ExpFreeLists[b->Header.BlockSize] = b;
    ExpPoolBlocks = ExpPoolBlocks - 1;
    KfLowerIrql(old);
}

XBAPI ULONG NTAPI ExQueryPoolBlockSize(PVOID PoolBlock) {
    if (((ULONG)PoolBlock & (PAGE_SIZE - 1)) == 0)
        return MmQueryAllocationSize(PoolBlock);
    return ((POOL_HEADER *)PoolBlock - 1)->BlockSize * POOL_UNIT - sizeof(POOL_HEADER);
}
