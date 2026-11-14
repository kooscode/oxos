// Executive pool (spec/stock/09 §5), the reference's allocator (ExAllocatePoolWithTag 0x80012e11, ExFreePool
// 0x80012fc8, initialisation 0x80012dc2). Requests above 0xFD8 bytes get whole pages; smaller ones get a block with
// an 8-byte header inside a pool page, sized in 32-byte units (header included), so ExQueryPoolBlockSize is
// units * 32 - 8.
//
// - Sizes of 1-8 units first use a per-size lookaside list (an S-list holding at most 2 freed blocks).
// - Free blocks sit on doubly linked lists by size: list i holds blocks of i + 1 units. An allocation of n units
//   takes the first block from list n upwards (so a block larger than n), or else a new page, which starts as one
//   free 128-unit block. The block is split: at a page start the allocation takes the front, elsewhere the end.
// - Freeing merges the block with free neighbours (PreviousSize finds the one in front) and returns the page to
//   the memory manager once the whole page is free.
// - Everything except the lookaside lists runs at DISPATCH_LEVEL, page allocation included.
#include <xk/memory.h>
#include <xk/core.h>
#include <xk/runtime.h>

namespace xk::memory {

namespace {
struct POOL_HEADER {
    UCHAR PreviousSize; // units of the block in front in the same page; 0 at a page start
    UCHAR PoolIndex;    // POOL_IN_USE while handed out
    UCHAR PoolType;     // 1 = in use (or on a lookaside list), 0 = on a free list
    UCHAR BlockSize;    // units, header included
    ULONG PoolTag;
};
static_assert(sizeof(POOL_HEADER) == 8);

// A free block: the header, then its free-list links.
struct POOL_FREE_BLOCK {
    POOL_HEADER Header;
    LIST_ENTRY Links;
};

// The reference's 0x18-byte lookaside descriptor (table at 0x80036328, indexed by units). The S-list header is
// 8-byte aligned, as one must be: the push/pop is a locked cmpxchg8b, and a misaligned one is a split lock when it
// crosses a cache line (very slow on a console; a modern Linux host throttles the unit tests to a crawl on it).
struct POOL_LOOKASIDE {
    SLIST_HEADER List;
    USHORT MaximumDepth;
    USHORT Spare;
    ULONG TotalAllocates;
    ULONG AllocateHits;
    ULONG Spare2;
};
static_assert(sizeof(POOL_LOOKASIDE) == 0x18 && alignof(POOL_LOOKASIDE) == 8);

constexpr ULONG POOL_SMALL_LIMIT = 0xFD8, POOL_UNIT = 32, POOL_UNITS_PER_PAGE = PAGE_SIZE / POOL_UNIT;
constexpr ULONG POOL_LOOKASIDE_UNITS = 8, POOL_LOOKASIDE_DEPTH = 2;
constexpr UCHAR POOL_IN_USE = 0x80;
} // namespace

static LIST_ENTRY PoolListHeads[POOL_UNITS_PER_PAGE];          // [i]: free blocks of i + 1 units
static POOL_LOOKASIDE PoolLookaside[POOL_LOOKASIDE_UNITS + 1]; // [units]: freed small blocks kept for reuse
// Statistics as the reference keeps them (0x80036400..0x8003640C), plus the number of blocks handed out.
ULONG PoolAllocateCalls, PoolFreeCalls;
volatile ULONG PoolPages, PoolLargePages, PoolBlocks;

static inline bool IsPageAligned(const void *p) {
    return ((ULONG)p & (PAGE_SIZE - 1)) == 0;
}

static inline POOL_HEADER *PoolBlockAt(POOL_HEADER *h, LONG units) {
    return (POOL_HEADER *)((UCHAR *)h + units * (LONG)POOL_UNIT);
}

static inline POOL_HEADER *PoolHeaderOf(PLIST_ENTRY links) {
    return &CONTAINING_RECORD(links, POOL_FREE_BLOCK, Links)->Header;
}

static inline PLIST_ENTRY PoolLinksOf(POOL_HEADER *h) {
    return &((POOL_FREE_BLOCK *)h)->Links;
}

void InitPool() {
    PoolAllocateCalls = PoolFreeCalls = 0;
    PoolPages = 0;
    PoolLargePages = 0;
    for (auto &l : PoolListHeads)
        InitializeListHead(&l);
    for (auto &l : PoolLookaside) {
        l.List = {};
        l.MaximumDepth = POOL_LOOKASIDE_DEPTH;
        l.TotalAllocates = l.AllocateHits = 0;
    }
}

static PVOID AllocatePoolPages(ULONG bytes) {
    return AllocateMappedPages((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT,
                               PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY, false, MmPoolUsage);
}

// Returns the number of pages freed.
static ULONG FreePoolPages(PVOID va) {
    return MmFreeSystemMemory(va, MmQueryAllocationSize(va));
}

XBAPI PVOID NTAPI ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag) {
    if (NumberOfBytes > POOL_SMALL_LIMIT) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        PoolAllocateCalls++;
        PVOID p = AllocatePoolPages(NumberOfBytes);
        if (p)
            PoolLargePages = PoolLargePages + ((NumberOfBytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
        KfLowerIrql(old);
        return p;
    }

    ULONG units = (NumberOfBytes + sizeof(POOL_HEADER) + POOL_UNIT - 1) / POOL_UNIT;
    if (units <= POOL_LOOKASIDE_UNITS) {
        POOL_LOOKASIDE &l = PoolLookaside[units];
        l.TotalAllocates++;
        if (auto body = (ULONG *)InterlockedPopEntrySList(&l.List)) {
            l.AllocateHits++;
            POOL_HEADER *h = (POOL_HEADER *)body - 1;
            h->PoolType = 1;
            h->PoolIndex = POOL_IN_USE;
            h->PoolTag = Tag;
            body[0] = 0; // the S-list link
            __atomic_add_fetch(&PoolBlocks, 1, __ATOMIC_RELAXED);
            return body;
        }
    }

    KIRQL old = KeRaiseIrqlToDpcLevel();
    PoolAllocateCalls++;
    PLIST_ENTRY head = &PoolListHeads[units]; // blocks of units + 1 and more
    while (IsListEmpty(head)) {
        if (++head == &PoolListHeads[POOL_UNITS_PER_PAGE]) {
            auto page = (POOL_HEADER *)AllocatePoolPages(PAGE_SIZE);
            if (!page) {
                KfLowerIrql(old);
                return nullptr;
            }
            PoolPages = PoolPages + 1;
            page->PreviousSize = 0;
            page->PoolIndex = 0;
            page->PoolType = 0;
            page->BlockSize = (UCHAR)POOL_UNITS_PER_PAGE;
            head = &PoolListHeads[POOL_UNITS_PER_PAGE - 1];
            InsertHeadList(head, PoolLinksOf(page));
        }
    }
    PLIST_ENTRY links = head->Flink;
    RemoveEntryList(links);
    POOL_HEADER *found = PoolHeaderOf(links), *h = found;
    if (found->BlockSize != units) {
        ULONG rest = found->BlockSize - units;
        POOL_HEADER *freeBlock, *after;
        if (found->PreviousSize == 0) {
            // At a page start: the allocation takes the front, the rest follows it.
            freeBlock = PoolBlockAt(found, units);
            freeBlock->BlockSize = (UCHAR)rest;
            freeBlock->PreviousSize = (UCHAR)units;
            freeBlock->PoolIndex = 0;
            after = PoolBlockAt(freeBlock, rest);
            if (!IsPageAligned(after))
                after->PreviousSize = (UCHAR)rest;
        } else {
            // Elsewhere the allocation takes the end and the free block stays where it was.
            freeBlock = found;
            freeBlock->BlockSize = (UCHAR)rest;
            h = PoolBlockAt(found, rest);
            h->PreviousSize = (UCHAR)rest;
            after = PoolBlockAt(h, units);
            if (!IsPageAligned(after))
                after->PreviousSize = (UCHAR)units;
        }
        h->BlockSize = (UCHAR)units;
        freeBlock->PoolType = 0;
        InsertTailList(&PoolListHeads[rest - 1], PoolLinksOf(freeBlock));
    }
    h->PoolType = 1;
    h->PoolIndex = POOL_IN_USE;
    h->PoolTag = Tag;
    auto body = (ULONG *)(h + 1);
    body[0] = body[1] = 0; // the free-list links
    __atomic_add_fetch(&PoolBlocks, 1, __ATOMIC_RELAXED);
    KfLowerIrql(old);
    return body;
}

XBAPI PVOID NTAPI ExAllocatePool(SIZE_T NumberOfBytes) {
    return ExAllocatePoolWithTag(NumberOfBytes, 'enoN');
}

XBAPI void NTAPI ExFreePool(PVOID P) {
    if (IsPageAligned(P)) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        PoolFreeCalls++;
        PoolLargePages = PoolLargePages - FreePoolPages(P);
        KfLowerIrql(old);
        return;
    }
    POOL_HEADER *h = (POOL_HEADER *)P - 1;
    if (h->PoolIndex != POOL_IN_USE)
        KeBugCheckEx(0xC2, 7, 0x376, (ULONG)h, (ULONG)P); // BAD_POOL_CALLER: not a live block (double free)
    h->PoolIndex = 0;
    __atomic_sub_fetch(&PoolBlocks, 1, __ATOMIC_RELAXED);
    if (h->BlockSize <= POOL_LOOKASIDE_UNITS) {
        POOL_LOOKASIDE &l = PoolLookaside[h->BlockSize];
        if (l.List.Depth < l.MaximumDepth) {
            InterlockedPushEntrySList(&l.List, (PSINGLE_LIST_ENTRY)P); // stays PoolType 1: never merged while there
            return;
        }
    }

    KIRQL old = KeRaiseIrqlToDpcLevel();
    PoolFreeCalls++;
    bool merged = false;
    POOL_HEADER *next = PoolBlockAt(h, h->BlockSize);
    if (!IsPageAligned(next) && next->PoolType == 0) {
        RemoveEntryList(PoolLinksOf(next));
        h->BlockSize = (UCHAR)(h->BlockSize + next->BlockSize);
        merged = true;
    }
    if (h->PreviousSize) {
        POOL_HEADER *prev = PoolBlockAt(h, -(LONG)h->PreviousSize);
        if (prev->PoolType == 0) {
            RemoveEntryList(PoolLinksOf(prev));
            prev->BlockSize = (UCHAR)(prev->BlockSize + h->BlockSize);
            h = prev;
            merged = true;
        }
    }
    if (IsPageAligned(h) && IsPageAligned(PoolBlockAt(h, h->BlockSize))) {
        // The whole page is free: back to the memory manager.
        FreePoolPages(h);
        PoolPages = PoolPages - 1;
    } else {
        h->PoolType = 0;
        if (merged) {
            next = PoolBlockAt(h, h->BlockSize);
            if (!IsPageAligned(next))
                next->PreviousSize = h->BlockSize;
            InsertTailList(&PoolListHeads[h->BlockSize - 1], PoolLinksOf(h));
        } else {
            InsertHeadList(&PoolListHeads[h->BlockSize - 1], PoolLinksOf(h));
        }
    }
    KfLowerIrql(old);
}

XBAPI ULONG NTAPI ExQueryPoolBlockSize(PVOID PoolBlock) {
    if (IsPageAligned(PoolBlock))
        return MmQueryAllocationSize(PoolBlock);
    return ((POOL_HEADER *)PoolBlock - 1)->BlockSize * POOL_UNIT - sizeof(POOL_HEADER);
}

} // namespace xk::memory
