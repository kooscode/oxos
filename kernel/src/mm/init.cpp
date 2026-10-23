// Memory manager initialisation, the page database and the system PTE range (spec/stock/09 §1-3, §7).
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

constexpr ULONG MM_LOW_RESERVED_PFN = 0xF; // the page directory; page 0 is reserved too
static ULONG *const MiPteArray = (ULONG *)0x83FF0000; // PTEs for 0x80000000-0x83FFFFFF = the page database

bool MiProtectToPte(ULONG protect, ULONG *pteBits) {
    if (protect & ~(ULONG)(PAGE_READONLY | PAGE_READWRITE | PAGE_NOCACHE | PAGE_WRITECOMBINE))
        return false;
    ULONG bits;
    switch (protect & 6) {
    case PAGE_READONLY: bits = PTE_PRESENT | PTE_ACCESSED | PTE_DIRTY; break;
    case PAGE_READWRITE: bits = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY; break;
    default: return false;
    }
    switch (protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
    case 0: break;
    case PAGE_NOCACHE: bits |= PTE_PCD; break;
    case PAGE_WRITECOMBINE: bits |= PTE_PWT; break;
    default: return false;
    }
    *pteBits = bits;
    return true;
}

// --- Page database entries (not-present ones; a present entry is the page's own mapping).
// Free: low word = next, high word = previous page of the same colour, packed as (pfn >> 5) * 2 (MM_PACKED_NULL
// ends the list). Busy: bit 16, lock count in the low word (steps of 2, so bit 0 stays clear), bits 18-27 = index
// of the PTE that maps the page within its page table (a page table: its PDE index), bits 28-31 = usage.
constexpr USHORT MM_PACKED_NULL = 0xFFFE;
constexpr ULONG MM_PFN_BUSY = 0x10000, MM_COLORS = 32;
static inline USHORT MiPack(ULONG pfn) { return (USHORT)((pfn >> 4) & 0xFFFE); }
static inline ULONG MiUnpack(USHORT packed, ULONG color) { return ((ULONG)packed << 4) | color; }
static inline USHORT &MiFlink(ULONG pfn) { return ((USHORT *)&MiPteArray[pfn])[0]; }
static inline USHORT &MiBlink(ULONG pfn) { return ((USHORT *)&MiPteArray[pfn])[1]; }
static inline ULONG MiBusyUsage(ULONG e) { return e >> 28; }
static inline ULONG MiBusyIndex(ULONG e) { return (e >> 18) & 0x3FF; }

// The reference's free-page region (MmGlobalData->RetailPfnRegion): per-colour list heads, then the page count.
struct MMPFNFREE {
    USHORT PackedPfnFlink, PackedPfnBlink;
};
struct MMPFNREGION {
    MMPFNFREE FreePagesByColor[MM_COLORS];
    ULONG AvailablePages;
};
MMPFNREGION MmRetailPfnRegion;
ULONG MmAvailablePages, MmNumberOfPhysicalPages;
ULONG MmAllocatedPagesByUsage[MmMaximumUsage];
static ULONG MiNextColor; // single pages take the colours in turn

// Callers hold DISPATCH_LEVEL for all of these.
static void MiInsertFreePage(ULONG pfn, bool atHead) {
    ULONG color = pfn & (MM_COLORS - 1);
    MMPFNFREE &list = MmRetailPfnRegion.FreePagesByColor[color];
    USHORT me = MiPack(pfn);
    if (list.PackedPfnFlink == MM_PACKED_NULL) {
        MiFlink(pfn) = MiBlink(pfn) = MM_PACKED_NULL;
        list.PackedPfnFlink = list.PackedPfnBlink = me;
    } else if (atHead) {
        MiFlink(pfn) = list.PackedPfnFlink;
        MiBlink(pfn) = MM_PACKED_NULL;
        MiBlink(MiUnpack(list.PackedPfnFlink, color)) = me;
        list.PackedPfnFlink = me;
    } else {
        MiFlink(pfn) = MM_PACKED_NULL;
        MiBlink(pfn) = list.PackedPfnBlink;
        MiFlink(MiUnpack(list.PackedPfnBlink, color)) = me;
        list.PackedPfnBlink = me;
    }
    MmRetailPfnRegion.AvailablePages++;
    MmAvailablePages++;
}

static void MiUnlinkFreePage(ULONG pfn) {
    ULONG color = pfn & (MM_COLORS - 1);
    MMPFNFREE &list = MmRetailPfnRegion.FreePagesByColor[color];
    USHORT next = MiFlink(pfn), prev = MiBlink(pfn);
    if (prev == MM_PACKED_NULL)
        list.PackedPfnFlink = next;
    else
        MiFlink(MiUnpack(prev, color)) = next;
    if (next == MM_PACKED_NULL)
        list.PackedPfnBlink = prev;
    else
        MiBlink(MiUnpack(next, color)) = prev;
    MmRetailPfnRegion.AvailablePages--;
    MmAvailablePages--;
}

// The head of the next non-empty colour list, as the reference: colours in turn, so consecutive pages differ in
// colour, and each list in insertion order (boot-time pages ascending, freed system pages first).
static ULONG MiRemoveAnyPage() {
    ULONG color = MiNextColor;
    for (ULONG tried = 0; MmRetailPfnRegion.FreePagesByColor[color].PackedPfnFlink == MM_PACKED_NULL; tried++) {
        if (tried == MM_COLORS) // callers checked MmAvailablePages: the count and the lists disagree
            KeBugCheckEx(0x4E /*PFN_LIST_CORRUPT*/, 0x99, MmAvailablePages, MmRetailPfnRegion.AvailablePages,
                         (ULONG)__builtin_return_address(0));
        color = (color + 1) & (MM_COLORS - 1);
    }
    ULONG pfn = MiUnpack(MmRetailPfnRegion.FreePagesByColor[color].PackedPfnFlink, color);
    MiUnlinkFreePage(pfn);
    MiNextColor = (color + 1) & (MM_COLORS - 1);
    return pfn;
}

static ULONG MiBusyEntry(ULONG usage, ULONG *ownerPte) {
    return usage << 28 | (((ULONG)ownerPte & 0xFFC) << 16) | MM_PFN_BUSY;
}

ULONG MiAllocatePage(ULONG usage, ULONG *ownerPte, bool zero) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (!MmAvailablePages) {
        KfLowerIrql(old);
        return 0;
    }
    ULONG pfn = MiRemoveAnyPage();
    XK_MARK("mark: mm: page taken from the free list");
    ULONG alias = MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT);
    if (zero) { // through a temporary mapping at 0x80000000|PA, as the reference
        MiPteArray[pfn] = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
        RtlZeroMemory((PVOID)alias, PAGE_SIZE);
        XK_MARK("mark: mm: page zeroed");
    }
    MiPteArray[pfn] = MiBusyEntry(usage, usage == MmVirtualPageTableUsage ? nullptr : ownerPte);
    MiInvalidatePage(alias);
    MmAllocatedPagesByUsage[usage]++;
    KfLowerIrql(old);
    return pfn;
}

// Single pages go back to the head of their list (reused first).
void MiFreePage(ULONG pfn) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    MmAllocatedPagesByUsage[MiBusyUsage(MiPteArray[pfn])]--;
    MiInsertFreePage(pfn, true);
    KfLowerIrql(old);
}

// Contiguous pages go to the tail (reused last).
void MiFreeContiguousPage(ULONG pfn) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    MiInvalidatePage(MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT));
    MmAllocatedPagesByUsage[MmContiguousUsage]--;
    MiInsertFreePage(pfn, false);
    KfLowerIrql(old);
}

// --- Moving a page that is in use (for a contiguous request). Its mapping is found from the busy entry: page
// tables through the page directory, title pages through the title page tables, the rest through the system PTE
// range. The copy and the switch run with interrupts off, so the page may be the current stack.

static ULONG *MiFindOwnerPte(ULONG pfn, ULONG e) {
    ULONG *pd = (ULONG *)0xC0300000, index = MiBusyIndex(e);
    auto frame = [&](ULONG pte) { return pte >> PAGE_SHIFT == pfn; };
    switch (MiBusyUsage(e)) {
    case MmVirtualPageTableUsage:
        for (ULONG i = 0; i < 0x200; i++)
            if ((pd[i] & PTE_PRESENT) && frame(pd[i]))
                return &pd[i];
        return nullptr;
    case MmSystemPageTableUsage:
        return &pd[index];
    case MmFsCacheUsage: { // bits 17-27 = cache element
        ULONG *FscCachePte(ULONG elementIndex);
        ULONG *pte = FscCachePte((e >> 17) & 0x7FF);
        return pte && frame(*pte) ? pte : nullptr;
    }
    case MmVirtualMemoryUsage:
    case MmImageUsage:
        for (ULONG i = 0; i < 0x200; i++) {
            ULONG *pte = (ULONG *)(0xC0000000 + (i << PAGE_SHIFT)) + index;
            if ((pd[i] & PTE_PRESENT) && *pte && frame(*pte)) // committed no-access pages are not present
                return pte;
        }
        return nullptr;
    default:
        for (ULONG i = MM_SYSTEM_PTE_BASE >> 22; i < MM_SYSTEM_PTE_END >> 22; i++) {
            ULONG *pte = (ULONG *)(0xC0000000 + (i << PAGE_SHIFT)) + index;
            if ((pd[i] & PTE_PRESENT) && (*pte & PTE_PRESENT) && frame(*pte))
                return pte;
        }
        return nullptr;
    }
}

// Fixed registers: the compiler must not share one between the copy source (advanced by rep movsl) and the address
// to invalidate, although they start out equal.
static void MiCopyAndSwitch(ULONG src, ULONG dst, ULONG *pte, ULONG value, bool pageTable) {
    ULONG flags, n = PAGE_SIZE / 4, va = (ULONG)pte << 10;
    __asm__ volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags)::"memory");
    if (pageTable) // its self-map entries change too
        __asm__ volatile("rep movsl\n\tmovl %%edx, (%%ebx)\n\tmovl %%cr3, %%eax\n\tmovl %%eax, %%cr3"
                         : "+S"(src), "+D"(dst), "+c"(n), "+a"(va)
                         : "b"(pte), "d"(value)
                         : "memory");
    else
        __asm__ volatile("rep movsl\n\tmovl %%edx, (%%ebx)\n\tinvlpg (%%eax)"
                         : "+S"(src), "+D"(dst), "+c"(n)
                         : "b"(pte), "d"(value), "a"(va)
                         : "memory");
    __asm__ volatile("pushl %0\n\tpopfl" ::"r"(flags) : "memory", "cc");
}

volatile ULONG MiRelocations, MiLastRelocation[5]; // diagnostics: pages moved; the last one {from, to, pte, entry, va}

static bool MiRelocatePage(ULONG pfn) {
    ULONG e = MiPteArray[pfn];
    ULONG *pte = MiFindOwnerPte(pfn, e);
    MiRelocations = MiRelocations + 1;
    MiLastRelocation[0] = pfn;
    MiLastRelocation[2] = (ULONG)pte;
    MiLastRelocation[3] = e;
    if (!pte || !MmAvailablePages)
        return false;
    MiNextColor = pfn & (MM_COLORS - 1); // the replacement keeps the colour
    ULONG to = MiRemoveAnyPage();
    MiPteArray[to] = (to << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
    ULONG usage = MiBusyUsage(e);
    bool pageTable = usage == MmVirtualPageTableUsage || usage == MmSystemPageTableUsage;
    MiCopyAndSwitch((ULONG)pte << 10, MM_SYSTEM_PHYSICAL_MAP | (to << PAGE_SHIFT), pte,
                    (*pte & 0xFFF) | (to << PAGE_SHIFT), pageTable);
    MiPteArray[to] = e;
    MiInvalidatePage(MM_SYSTEM_PHYSICAL_MAP | (to << PAGE_SHIFT));
    MiLastRelocation[1] = to;
    MiLastRelocation[4] = (ULONG)pte << 10;
    return true;
}

// A page a contiguous request may take: free, or busy and unlocked (it is moved).
static inline bool MiCanTake(ULONG e) {
    return !(e & PTE_PRESENT) && (!(e & MM_PFN_BUSY) || !(USHORT)e);
}

bool MiAllocateContiguousPages(ULONG count, ULONG lowPfn, ULONG highPfn, ULONG alignPages, ULONG pteBits,
                               ULONG *firstPfn) {
    if (!count || highPfn < lowPfn || count > highPfn - lowPfn + 1)
        return false;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    bool found = false;
    LONG start = (LONG)((highPfn - count + 1) & ~(alignPages - 1));
    if (count <= MmAvailablePages)
        while (start >= (LONG)lowPfn) {
            ULONG i = count;
            while (i && MiCanTake(MiPteArray[start + i - 1]))
                i--;
            if (!i) {
                found = true;
                break;
            }
            // Below the page that is in the way, re-aligned.
            LONG next = (LONG)(start + i - 1) - (LONG)count;
            start = next < 0 ? -1 : (LONG)((ULONG)next & ~(alignPages - 1));
        }
    // Take the free pages first, so that a moved page cannot land inside the run, then move the rest.
    ULONG busy = 0;
    if (found) {
        for (ULONG i = 0; i < count; i++)
            if (MiPteArray[start + i] & MM_PFN_BUSY)
                busy++;
        found = busy <= MmAvailablePages - (count - busy);
    }
    if (found) {
        for (ULONG i = 0; i < count; i++) {
            ULONG pfn = start + i;
            if (!(MiPteArray[pfn] & MM_PFN_BUSY)) {
                MiUnlinkFreePage(pfn);
                MiPteArray[pfn] = MM_PFN_BUSY | 2; // held (locked) until mapped below
            }
        }
        for (ULONG i = 0; i < count && found; i++) {
            ULONG pfn = start + i;
            if (MiPteArray[pfn] != (MM_PFN_BUSY | 2))
                found = MiRelocatePage(pfn);
        }
        if (found && (pteBits & (PTE_PCD | PTE_PWT)))
            MiFlushCaches(); // the run (and pages just moved out of it) leaves the caches before its new memory type
        for (ULONG i = 0; i < count; i++) {
            ULONG pfn = start + i;
            if (found) {
                MiPteArray[pfn] = (pfn << PAGE_SHIFT) | pteBits | (i == count - 1 ? PTE_END_OF_ALLOCATION : 0);
                MmAllocatedPagesByUsage[MmContiguousUsage]++;
            } else if (MiPteArray[pfn] == (MM_PFN_BUSY | 2)) {
                MiInsertFreePage(pfn, false);
            }
            MiInvalidatePage(MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT));
        }
        *firstPfn = start;
    }
    KfLowerIrql(old);
    return found;
}

// --- System PTE range 0xD0000000-0xEFFFFFFF, in the reference's format (MmGlobalData->SystemPteRange): free runs
// are kept in their own not-present PTEs, in address order. The first PTE of a run holds the address of the next
// run's first PTE (0xFFFFFFFC ends the list) and bit 1 when the run is one PTE long; otherwise the second PTE holds
// the length << 2. Page tables are added at the end of the committed part as needed and never freed.

struct MMPTERANGE {
    ULONG Head; // as a run's first PTE: the first free run
    ULONG *FirstCommittedPte, *LastCommittedPte, *LastPte;
    ULONG *AvailablePages;
    ULONG (*AllocatePageTable)(ULONG usage, ULONG *ownerPte, bool zero);
};
constexpr ULONG MM_PTE_LIST_END = 0xFFFFFFFC;
MMPTERANGE MiSystemPteRange = {MM_PTE_LIST_END, MiGetPteAddress(MM_SYSTEM_PTE_BASE),
                               MiGetPteAddress(MM_SYSTEM_PTE_BASE), MiGetPteAddress(MM_SYSTEM_PTE_END),
                               &MmAvailablePages, MiAllocatePage};

static inline ULONG MiRunLength(ULONG *run) { return (run[0] & 2) ? 1 : run[1] >> 2; }
static inline ULONG *MiRunNext(ULONG link) { return (ULONG *)(link & ~3u); }
static inline void MiSetRun(ULONG *run, ULONG length, ULONG nextLink) {
    run[0] = (nextLink & ~3u) | (length == 1 ? 2 : 0);
    if (length > 1)
        run[1] = length << 2;
}
static inline void MiSetLink(ULONG *link, ULONG *to) { *link = (ULONG)to | (*link & 3); }

volatile ULONG MiPteDoubleFrees; // diagnostics: releases that overlapped free system PTEs (refused)

static void MiInsertPteRun(ULONG *start, ULONG count, PVOID caller = nullptr, PVOID caller2 = nullptr) {
    // Find the neighbours first: a range that overlaps a free run is being freed twice. Inserting it would make the
    // list overlap itself (a search that loops, or slots handed out twice and their live PTEs zeroed): refuse it.
    ULONG *link = &MiSystemPteRange.Head, *prev = nullptr;
    while ((*link & ~3u) != MM_PTE_LIST_END && MiRunNext(*link) < start) {
        prev = MiRunNext(*link);
        link = prev;
    }
    ULONG next0 = *link & ~3u;
    if ((prev && prev + MiRunLength(prev) > start) ||
        (next0 != MM_PTE_LIST_END && MiRunNext(next0) < start + count)) {
        MiPteDoubleFrees = MiPteDoubleFrees + 1;
        XK_SERIAL("*** mm: system PTEs %p (+%lu, va %08lx) freed while (partly) free; callers %p %p\n", start, count,
                  (ULONG)start << 10, caller, caller2);
        return;
    }
    RtlZeroMemory(start, count * 4);
    ULONG next = *link & ~3u;
    if (next != MM_PTE_LIST_END && start + count == MiRunNext(next)) { // join the following run
        ULONG *following = MiRunNext(next);
        count += MiRunLength(following);
        next = following[0] & ~3u;
    }
    if (prev && prev + MiRunLength(prev) == start) { // join the previous run
        MiSetRun(prev, MiRunLength(prev) + count, next);
        return;
    }
    MiSetRun(start, count, next);
    MiSetLink(link, start);
}

#if XK_NETLOG
extern "C" volatile ULONG KiSampleTitlePc;
#define MI_GROW_STEP(...) do { if (KiSampleTitlePc) XK_SERIAL(__VA_ARGS__); } while (0)
#else
#define MI_GROW_STEP(...) do { } while (0)
#endif

volatile ULONG MiPteListDamaged; // diagnostics: the system PTE free list failed its checks

// Report a damaged free list once: where the bad link was read, its value, the run before it, and the physical
// pages behind them (the page tables of the system PTE range).
static void MiPteListDamage(ULONG *link, ULONG *cand, ULONG *last) {
    ULONG n = MiPteListDamaged;
    MiPteListDamaged = n + 1;
    if (n >= 4)
        return;
    [[maybe_unused]] ULONG linkPa = MmGetPhysicalAddress(link);
    XK_SERIAL("*** mm: system PTE list damaged: link at %p (pa %08lx) = %08lx -> run %p; previous run %p (len %lu)\n",
              link, linkPa, *link, cand, last, last ? MiRunLength(last) : 0);
    [[maybe_unused]] auto w = (const ULONG *)((ULONG)link & ~0x1Fu);
    XK_SERIAL("    around it: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", w[0], w[1], w[2], w[3], w[4], w[5],
              w[6], w[7]);
}

PVOID MiReserveSystemPtes(ULONG count) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    MMPTERANGE &r = MiSystemPteRange;
    for (;;) {
        ULONG *link = &r.Head, *last = nullptr;
        while ((*link & ~3u) != MM_PTE_LIST_END) { // first fit, lowest address first
            // A damaged list (a stray write into these page tables) must not be followed: each run lies inside the
            // committed PTEs, after the previous one, with a sane length.
            ULONG *cand = MiRunNext(*link);
            if (cand < r.FirstCommittedPte || cand >= r.LastCommittedPte || (last && cand <= last) ||
                MiRunLength(cand) == 0 || cand + MiRunLength(cand) > r.LastCommittedPte) {
                MiPteListDamage(link, cand, last);
                KfLowerIrql(old);
                return nullptr;
            }
            ULONG *run = MiRunNext(*link), length = MiRunLength(run), next = run[0] & ~3u;
            if (length >= count) {
                if (length == count) {
                    *link = (*link & 3) | next;
                } else {
                    MiSetRun(run + count, length - count, next);
                    *link = (*link & 3) | (ULONG)(run + count);
                }
                RtlZeroMemory(run, count * 4);
                KfLowerIrql(old);
                return (PVOID)((ULONG)run << 10);
            }
            last = run;
            link = run;
        }
        // Commit page tables at the end; a free run ending there counts towards the request.
        ULONG needed = count;
        if (last && last + MiRunLength(last) == r.LastCommittedPte)
            needed -= MiRunLength(last);
        ULONG ptes = (needed + 0x3FF) & ~0x3FFu, tables = ptes >> 10;
        if ((ULONG)(r.LastPte - r.LastCommittedPte) < ptes || tables > *r.AvailablePages) {
            KfLowerIrql(old);
            return nullptr;
        }
        ULONG *first = r.LastCommittedPte;
        for (ULONG i = 0; i < tables; i++) {
            ULONG *pde = (ULONG *)(((ULONG)r.LastCommittedPte >> 10 & 0x3FFFFC) + 0xC0000000);
            MI_GROW_STEP("mm: grow: pde %p for ptes %p\n", pde, r.LastCommittedPte);
            ULONG pfn = r.AllocatePageTable(MmSystemPageTableUsage, pde, true);
            MI_GROW_STEP("mm: grow: table pfn %04lx\n", pfn);
            *pde = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
            MiInvalidatePage((ULONG)r.LastCommittedPte);
            MI_GROW_STEP("mm: grow: pde set\n");
            r.LastCommittedPte += 0x400;
        }
        MiInsertPteRun(first, ptes);
        MI_GROW_STEP("mm: grow: run inserted\n");
    }
}

void MiReleaseSystemPtes(PVOID va, ULONG count) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG *first = MiGetPteAddress((ULONG)va);
    for (ULONG i = 0; i < count; i++)
        MiInvalidatePage((ULONG)va + (i << PAGE_SHIFT));
    // The caller's caller through the frame-pointer chain (kernel built with frame pointers).
    auto frame = (ULONG *)__builtin_frame_address(0);
    PVOID caller2 = frame && frame[0] >= 0x80000000 ? (PVOID)((ULONG *)frame[0])[1] : nullptr;
    MiInsertPteRun(first, count, __builtin_return_address(0), caller2);
    KfLowerIrql(old);
}

// --- Initialisation

static inline void MiReloadCr3() {
    ULONG cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3)::"memory");
}

volatile ULONG MmPersistedPages; // pages re-adopted by the last quick reboot

extern "C" volatile ULONG MiRamProbe, MiGpuSizeBefore, MiGpuSizeAfter;
volatile ULONG MiGpuSizeBefore, MiGpuSizeAfter; // diagnostics: PFB 0x10020C before / after the 64 MB set-up
extern "C" volatile ULONG MiRamBadPages, MiRamBadPage[4];
extern "C" volatile ULONG MiMemtestErrors, MiMemtestPasses, MiMemtestError[4][2];
volatile ULONG MiMemtestErrors, MiMemtestPasses, MiMemtestError[4][2]; // XK_EXPERIMENT_MEMTEST: {address, bits wrong}
volatile ULONG MiRamBadPages, MiRamBadPage[4]; // diagnostics: pages whose tag read back wrong {pfn << 16 | seen pfn}
volatile ULONG MiRamProbe; // diagnostics: 1 = 64 MB (address bit 26 aliases), 2 = memory above 64 MB, 3 = unclear

// How much RAM answers (cold boot, before the page directory switch: the 2BL's large pages map 0-256 MB at 0):
// a marker written at 64 MB + x either appears at x (only 64 MB, the address wraps) or not.
static void MiProbeRamSize() {
    auto low = (volatile ULONG *)0x00000FF0, high = (volatile ULONG *)(0x04000000 + 0x0FF0);
    ULONG saved = *low;
    *low = 0x13579BDF;
    *high = 0x2468ACE0;
    __asm__ volatile("wbinvd" ::: "memory");
    ULONG l = *low, h = *high;
    MiRamProbe = l == 0x2468ACE0 ? 1 : (l == 0x13579BDF && h == 0x2468ACE0) ? 2 : 3;
    *low = saved;
}

void MmInitSystem(bool QuickReboot) {
    if (!QuickReboot) {
        MiProbeRamSize();
        // Every allocatable page answers as itself (no two addresses on the same cells): tag each page's first and
        // middle words with its address, then read them all back.
        ULONG imgEnd = (0x10000 + *(ULONG *)(0x80010000 + *(ULONG *)(0x80010000 + 0x3C) + 24 + 56) + PAGE_SIZE - 1) >>
                       PAGE_SHIFT;
        for (ULONG pfn = imgEnd; pfn <= MM_HIGHEST_CONTIGUOUS_PFN; pfn++) {
            auto w = (volatile ULONG *)(0x80000000 + (pfn << PAGE_SHIFT));
            w[0] = 0xA5000000 ^ pfn;
            w[512] = ~(0xA5000000 ^ pfn);
        }
        __asm__ volatile("wbinvd" ::: "memory");
        for (ULONG pfn = imgEnd; pfn <= MM_HIGHEST_CONTIGUOUS_PFN; pfn++) {
            auto w = (volatile ULONG *)(0x80000000 + (pfn << PAGE_SHIFT));
            if (w[0] != (0xA5000000 ^ pfn) || w[512] != ~(0xA5000000 ^ pfn)) {
                ULONG bad = MiRamBadPages;
                if (bad < 4)
                    MiRamBadPage[bad] = pfn << 16 | ((w[0] ^ 0xA5000000) & 0xFFFF);
                MiRamBadPages = bad + 1;
            }
        }
#ifdef XK_EXPERIMENT_MEMTEST
#ifdef XK_EXPERIMENT_MEMTEST_DECAY
#define XK_MEMTEST_PASSES 2
#else
#define XK_MEMTEST_PASSES 4
#endif
        // Experiment: a RAM test over every allocatable page (4 passes: patterns, their inverse, address-derived),
        // to see whether the memory interface returns what was written (no pad calibration is done).
        {
            static const ULONG patterns[4] = {0x55555555, 0xAAAAAAAA, 0, 1};
            for (ULONG pass = 0; pass < XK_MEMTEST_PASSES; pass++) {
                for (ULONG pfn = imgEnd; pfn <= MM_HIGHEST_CONTIGUOUS_PFN; pfn++) {
                    auto w = (volatile ULONG *)(0x80000000 + (pfn << PAGE_SHIFT));
                    for (ULONG i = 0; i < 1024; i++)
                        w[i] = pass < 2 ? patterns[pass] : (pass == 2 ? (pfn << 12 | i << 2) : ~(pfn << 12 | i << 2));
                }
                __asm__ volatile("wbinvd" ::: "memory");
#ifdef XK_EXPERIMENT_MEMTEST_DECAY
                // Leave the patterns alone for 10 s before checking them (DRAM refresh / retention).
                for (ULONG ms = 0; ms < 10000; ms++)
                    KeStallExecutionProcessor(1000);
#endif
                for (ULONG pfn = imgEnd; pfn <= MM_HIGHEST_CONTIGUOUS_PFN; pfn++) {
                    auto w = (volatile ULONG *)(0x80000000 + (pfn << PAGE_SHIFT));
                    for (ULONG i = 0; i < 1024; i++) {
                        ULONG want = pass < 2 ? patterns[pass] : (pass == 2 ? (pfn << 12 | i << 2) : ~(pfn << 12 | i << 2));
                        ULONG got = w[i];
                        if (got != want) {
                            ULONG n = MiMemtestErrors;
                            if (n < 4) {
                                MiMemtestError[n][0] = (pfn << 12) | (i << 2);
                                MiMemtestError[n][1] = got ^ want;
                            }
                            MiMemtestErrors = n + 1;
                        }
                    }
                }
                MiMemtestPasses = pass + 1;
            }
        }
#endif
        // 64 MB consoles (no RAM answers above 64 MB): the GPU's memory interface as Cromwell leaves it for 64 MB
        // (spec/stock/22 §11: PFB 0x100200 bit 8 clear, 0x03070003), not the 128 MB value our X-codes leave.
        // The GPU places its instance memory (channel contexts, hash table) at the top of the memory it believes
        // it has; at 128 MB that is memory a 64 MB console does not have, so a title's FIFO never starts.
        auto pfb = (volatile ULONG *)0xFD100200;
        MiGpuSizeBefore = pfb[3];
        if (MiRamProbe != 2 && (pfb[0] & 0x100))
            pfb[0] = pfb[0] & ~0x100u;
        MiGpuSizeAfter = pfb[3];
        // And the host bridge's top of memory (bus 0 device 0 register 0x84): 64 MB - 1, not 128 MB - 1.
        if (MiRamProbe != 2) {
            outl(0xCF8, 0x80000084);
            if (inl(0xCFC) == 0x07FFFFFF)
                outl(0xCFC, 0x03FFFFFF);
        }
    }
    // Kernel image extent from our own PE header; .text (first section) becomes read-only.
    auto image = (UCHAR *)0x80010000;
    ULONG lfanew = *(ULONG *)(image + 0x3C);
    ULONG sizeOfImage = *(ULONG *)(image + lfanew + 24 + 56);
    USHORT optSize = *(USHORT *)(image + lfanew + 20);
    UCHAR *sections = image + lfanew + 24 + optSize;
    ULONG textEnd = *(ULONG *)(sections + 12) + *(ULONG *)(sections + 8); // VA + VirtualSize of .text
    ULONG imageEndPfn = (0x10000 + sizeOfImage + PAGE_SIZE - 1) >> PAGE_SHIFT;
    ULONG textEndPfn = (0x10000 + (textEnd & ~(PAGE_SIZE - 1))) >> PAGE_SHIFT;

    // 1. The page database (still reachable through the 2BL large pages). Pages 1-14 (below the page directory)
    //    and those between the image and the GPU memory are allocatable; a quick reboot re-adopts persisted
    //    allocations (PTE bit 10) there.
    const ULONG rw = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
    auto allocatable = [&](ULONG pfn) {
        return (pfn >= 1 && pfn < MM_LOW_RESERVED_PFN) || (pfn >= imageEndPfn && pfn <= MM_HIGHEST_CONTIGUOUS_PFN);
    };
    //    On a quick reboot this array is also the live page table for 0x80000000 (the kernel re-enters on its own
    //    page directory), so each entry is written once with its final value: clearing the kernel's own entries
    //    first, even briefly, unmaps the code and stack running this loop as soon as a cached translation is evicted.
    RtlZeroMemory(MmAllocatedPagesByUsage, sizeof(MmAllocatedPagesByUsage));
    ULONG persisted = 0;
    for (ULONG pfn = 0; pfn < MM_PAGES; pfn++) {
        ULONG pte = 0;
        if (pfn >= MM_PAGE_TABLES_FIRST_PFN) {
            pte = (pfn << PAGE_SHIFT) | rw | PTE_PERSIST;
        } else if (pfn >= MM_GPU_INSTANCE_FIRST_PFN) {
            pte = (pfn << PAGE_SHIFT) | rw | PTE_PWT | PTE_PCD;
        } else if (!allocatable(pfn)) {
#ifdef XK_EXPERIMENT_GPU_LAST_MB
            if (pfn > MM_HIGHEST_CONTIGUOUS_PFN) // held: never on the free lists
                pte = (pfn << PAGE_SHIFT) | rw;
#endif
            if (pfn < imageEndPfn) // the kernel image (.text read-only) and the reserved low pages
                pte = ((pfn << PAGE_SHIFT) | rw | PTE_PERSIST) & ((pfn >= 0x10 && pfn < textEndPfn) ? ~PTE_WRITE : ~0u);
        } else if (QuickReboot &&
                   (MiPteArray[pfn] & (PTE_PRESENT | PTE_PERSIST)) == (PTE_PRESENT | PTE_PERSIST)) {
            persisted++;
            continue; // a persisted allocation: re-adopted as it is
        }
        MiPteArray[pfn] = pte;
    }
    MmPersistedPages = persisted;
#ifdef XK_EXPERIMENT_POISON_RAM
    // Experiment: real RAM powers up holding garbage, xemu's holds zeros. Fill every allocatable page with
    // pseudo-random words (through the 2BL's large pages, still in place), so code that assumes fresh memory is zero
    // fails in xemu too.
    if (!QuickReboot) {
        ULONG x = 0x12345678;
        for (ULONG pfn = 0; pfn < MM_PAGES; pfn++)
            if (allocatable(pfn)) {
                auto w = (volatile ULONG *)(0x80000000 + (pfn << PAGE_SHIFT));
                for (ULONG i = 0; i < PAGE_SIZE / 4; i++) {
                    x = x * 1664525 + 1013904223;
                    w[i] = x;
                }
            }
    }
#endif

    // 2. Free lists: every allocatable page, ascending, at the tails (the reference's order). The mapped pages
    //    count as contiguous memory.
    for (ULONG c = 0; c < MM_COLORS; c++)
        MmRetailPfnRegion.FreePagesByColor[c] = {MM_PACKED_NULL, MM_PACKED_NULL};
    MmRetailPfnRegion.AvailablePages = MmAvailablePages = 0;
    MiNextColor = 0;
    for (ULONG pfn = 0; pfn < MM_PAGES; pfn++) {
        if (!MiPteArray[pfn])
            MiInsertFreePage(pfn, false);
        else
            MmAllocatedPagesByUsage[MmContiguousUsage]++;
    }
    MmNumberOfPhysicalPages = MM_PAGES;

    // 3. Switch the page directory: 4 KB PTEs for RAM at 0x80000000, nothing above 64 MB, no user
    //    mappings, and the reference MMIO windows (spec/stock/02 §4).
    ULONG *pd = (ULONG *)0xC0300000;
    for (ULONG i = 0; i < 16; i++)
        pd[0x200 + i] = ((MM_PAGE_TABLES_FIRST_PFN + i) << PAGE_SHIFT) | rw;
    for (ULONG i = 0x210; i < 0x240; i++)
        pd[i] = 0;
    for (ULONG i = 0; i < 0x200; i++) // no title mappings
        pd[i] = 0;
    for (ULONG i = 0x340; i < 0x3C0; i++) // system PTE window 0xD0000000-0xEFFFFFFF starts empty
        pd[i] = 0;
    for (ULONG i = 0x3C0; i < 0x3E0; i++) // 0xF0000000-0xF7FFFFFF: write-through large pages
        pd[i] = (i << 22) | rw | PTE_PWT | PTE_LARGE;
    for (ULONG i = 0x3E0; i < 0x3FE; i++) // 0xF8000000-0xFF7FFFFF: uncached large pages
        pd[i] = (i << 22) | rw | PTE_PWT | PTE_PCD | PTE_LARGE;
    // PDE 0x3FF (flash) is kept for the quick-reboot .data copy (the reference clears it; spec/stock/09).
    MiReloadCr3();

    MiSystemPteRange.Head = MM_PTE_LIST_END;
    MiSystemPteRange.LastCommittedPte = MiSystemPteRange.FirstCommittedPte;
    ExInitializePool();
    MiInitializeVirtualMemory();
}
