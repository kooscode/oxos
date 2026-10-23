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
#include <xk/ke.h>
#include <xk/mm.h>

PVOID MiAllocateMappedPages(ULONG pages, ULONG pteBits, bool guardPage, ULONG usage);

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

// S-list header (rtl/runtime.cpp): {Next, Depth, Sequence}.
struct POOL_SLIST {
    PVOID Next;
    USHORT Depth, Sequence;
};

// The reference's 0x18-byte lookaside descriptor (table at 0x80036328, indexed by units).
struct POOL_LOOKASIDE {
    POOL_SLIST List;
    USHORT MaximumDepth;
    USHORT Spare;
    ULONG TotalAllocates;
    ULONG AllocateHits;
    ULONG Spare2;
};
static_assert(sizeof(POOL_LOOKASIDE) == 0x18);

constexpr ULONG POOL_SMALL_LIMIT = 0xFD8, POOL_UNIT = 32, POOL_UNITS_PER_PAGE = PAGE_SIZE / POOL_UNIT;
constexpr ULONG POOL_LOOKASIDE_UNITS = 8, POOL_LOOKASIDE_DEPTH = 2;
constexpr UCHAR POOL_IN_USE = 0x80;
} // namespace

XBAPI PVOID FASTCALL InterlockedPushEntrySList(PVOID ListHead, PVOID ListEntry);
XBAPI PVOID FASTCALL InterlockedPopEntrySList(PVOID ListHead);

static LIST_ENTRY ExpPoolListHeads[POOL_UNITS_PER_PAGE]; // [i]: free blocks of i + 1 units
static POOL_LOOKASIDE ExpPoolLookaside[POOL_LOOKASIDE_UNITS + 1];
// Statistics as the reference keeps them (0x80036400..0x8003640C), plus the number of blocks handed out.
ULONG ExpPoolAllocateCalls, ExpPoolFreeCalls;
volatile ULONG ExpPoolPages, ExpPoolLargePages, ExpPoolBlocks;

static inline bool ExpPageAligned(const void *p) { return ((ULONG)p & (PAGE_SIZE - 1)) == 0; }
static inline POOL_HEADER *ExpBlockAt(POOL_HEADER *h, LONG units) {
    return (POOL_HEADER *)((UCHAR *)h + units * (LONG)POOL_UNIT);
}
static inline POOL_HEADER *ExpHeaderOf(PLIST_ENTRY links) { return &CONTAINING_RECORD(links, POOL_FREE_BLOCK, Links)->Header; }
static inline PLIST_ENTRY ExpLinksOf(POOL_HEADER *h) { return &((POOL_FREE_BLOCK *)h)->Links; }

void ExInitializePool() {
    ExpPoolAllocateCalls = ExpPoolFreeCalls = 0;
    ExpPoolPages = 0;
    ExpPoolLargePages = 0;
    for (auto &l : ExpPoolListHeads)
        InitializeListHead(&l);
    for (auto &l : ExpPoolLookaside) {
        l.List = {};
        l.MaximumDepth = POOL_LOOKASIDE_DEPTH;
        l.TotalAllocates = l.AllocateHits = 0;
    }
}

static PVOID ExpAllocatePages(ULONG bytes) {
    return MiAllocateMappedPages((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT,
                                 PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY, false, MmPoolUsage);
}

// Returns the number of pages freed.
static ULONG ExpFreePages(PVOID va) { return MmFreeSystemMemory(va, MmQueryAllocationSize(va)); }

XBAPI PVOID NTAPI ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag) {
    if (NumberOfBytes > POOL_SMALL_LIMIT) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        ExpPoolAllocateCalls++;
        PVOID p = ExpAllocatePages(NumberOfBytes);
        if (p)
            ExpPoolLargePages = ExpPoolLargePages + ((NumberOfBytes + PAGE_SIZE - 1) >> PAGE_SHIFT);
        KfLowerIrql(old);
        return p;
    }

    ULONG units = (NumberOfBytes + sizeof(POOL_HEADER) + POOL_UNIT - 1) / POOL_UNIT;
    if (units <= POOL_LOOKASIDE_UNITS) {
        POOL_LOOKASIDE &l = ExpPoolLookaside[units];
        l.TotalAllocates++;
        if (auto body = (ULONG *)InterlockedPopEntrySList(&l.List)) {
            l.AllocateHits++;
            POOL_HEADER *h = (POOL_HEADER *)body - 1;
            h->PoolType = 1;
            h->PoolIndex = POOL_IN_USE;
            h->PoolTag = Tag;
            body[0] = 0; // the S-list link
            __atomic_add_fetch(&ExpPoolBlocks, 1, __ATOMIC_RELAXED);
            return body;
        }
    }

    KIRQL old = KeRaiseIrqlToDpcLevel();
    ExpPoolAllocateCalls++;
    PLIST_ENTRY head = &ExpPoolListHeads[units]; // blocks of units + 1 and more
    while (IsListEmpty(head)) {
        if (++head == &ExpPoolListHeads[POOL_UNITS_PER_PAGE]) {
            auto page = (POOL_HEADER *)ExpAllocatePages(PAGE_SIZE);
            if (!page) {
                KfLowerIrql(old);
                return nullptr;
            }
            ExpPoolPages = ExpPoolPages + 1;
            page->PreviousSize = 0;
            page->PoolIndex = 0;
            page->PoolType = 0;
            page->BlockSize = (UCHAR)POOL_UNITS_PER_PAGE;
            head = &ExpPoolListHeads[POOL_UNITS_PER_PAGE - 1];
            InsertHeadList(head, ExpLinksOf(page));
        }
    }
    PLIST_ENTRY links = head->Flink;
    RemoveEntryList(links);
    POOL_HEADER *found = ExpHeaderOf(links), *h = found;
    if (found->BlockSize != units) {
        ULONG rest = found->BlockSize - units;
        POOL_HEADER *freeBlock, *after;
        if (found->PreviousSize == 0) {
            // At a page start: the allocation takes the front, the rest follows it.
            freeBlock = ExpBlockAt(found, units);
            freeBlock->BlockSize = (UCHAR)rest;
            freeBlock->PreviousSize = (UCHAR)units;
            freeBlock->PoolIndex = 0;
            after = ExpBlockAt(freeBlock, rest);
            if (!ExpPageAligned(after))
                after->PreviousSize = (UCHAR)rest;
        } else {
            // Elsewhere the allocation takes the end and the free block stays where it was.
            freeBlock = found;
            freeBlock->BlockSize = (UCHAR)rest;
            h = ExpBlockAt(found, rest);
            h->PreviousSize = (UCHAR)rest;
            after = ExpBlockAt(h, units);
            if (!ExpPageAligned(after))
                after->PreviousSize = (UCHAR)units;
        }
        h->BlockSize = (UCHAR)units;
        freeBlock->PoolType = 0;
        InsertTailList(&ExpPoolListHeads[rest - 1], ExpLinksOf(freeBlock));
    }
    h->PoolType = 1;
    h->PoolIndex = POOL_IN_USE;
    h->PoolTag = Tag;
    auto body = (ULONG *)(h + 1);
    body[0] = body[1] = 0; // the free-list links
    __atomic_add_fetch(&ExpPoolBlocks, 1, __ATOMIC_RELAXED);
    KfLowerIrql(old);
    return body;
}

XBAPI PVOID NTAPI ExAllocatePool(SIZE_T NumberOfBytes) { return ExAllocatePoolWithTag(NumberOfBytes, 'enoN'); }

XBAPI void NTAPI ExFreePool(PVOID P) {
    if (ExpPageAligned(P)) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        ExpPoolFreeCalls++;
        ExpPoolLargePages = ExpPoolLargePages - ExpFreePages(P);
        KfLowerIrql(old);
        return;
    }
    POOL_HEADER *h = (POOL_HEADER *)P - 1;
    if (h->PoolIndex != POOL_IN_USE)
        KeBugCheckEx(0xC2, 7, 0x376, (ULONG)h, (ULONG)P); // BAD_POOL_CALLER: not a live block (double free)
    h->PoolIndex = 0;
    __atomic_sub_fetch(&ExpPoolBlocks, 1, __ATOMIC_RELAXED);
    if (h->BlockSize <= POOL_LOOKASIDE_UNITS) {
        POOL_LOOKASIDE &l = ExpPoolLookaside[h->BlockSize];
        if (l.List.Depth < l.MaximumDepth) {
            InterlockedPushEntrySList(&l.List, P); // stays PoolType 1: never merged while there
            return;
        }
    }

    KIRQL old = KeRaiseIrqlToDpcLevel();
    ExpPoolFreeCalls++;
    bool merged = false;
    POOL_HEADER *next = ExpBlockAt(h, h->BlockSize);
    if (!ExpPageAligned(next) && next->PoolType == 0) {
        RemoveEntryList(ExpLinksOf(next));
        h->BlockSize = (UCHAR)(h->BlockSize + next->BlockSize);
        merged = true;
    }
    if (h->PreviousSize) {
        POOL_HEADER *prev = ExpBlockAt(h, -(LONG)h->PreviousSize);
        if (prev->PoolType == 0) {
            RemoveEntryList(ExpLinksOf(prev));
            prev->BlockSize = (UCHAR)(prev->BlockSize + h->BlockSize);
            h = prev;
            merged = true;
        }
    }
    if (ExpPageAligned(h) && ExpPageAligned(ExpBlockAt(h, h->BlockSize))) {
        // The whole page is free: back to the memory manager.
        ExpFreePages(h);
        ExpPoolPages = ExpPoolPages - 1;
    } else {
        h->PoolType = 0;
        if (merged) {
            next = ExpBlockAt(h, h->BlockSize);
            if (!ExpPageAligned(next))
                next->PreviousSize = h->BlockSize;
            InsertTailList(&ExpPoolListHeads[h->BlockSize - 1], ExpLinksOf(h));
        } else {
            InsertHeadList(&ExpPoolListHeads[h->BlockSize - 1], ExpLinksOf(h));
        }
    }
    KfLowerIrql(old);
}

XBAPI ULONG NTAPI ExQueryPoolBlockSize(PVOID PoolBlock) {
    if (ExpPageAligned(PoolBlock))
        return MmQueryAllocationSize(PoolBlock);
    return ((POOL_HEADER *)PoolBlock - 1)->BlockSize * POOL_UNIT - sizeof(POOL_HEADER);
}
