// Memory manager initialisation, physical pages and the system PTE range (spec/09 §1-3).
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

constexpr ULONG MM_LOW_RESERVED_PFN = 0xF; // the page directory; page 0 is reserved too
static ULONG *const MiPteArray = (ULONG *)0x83FF0000; // PTEs for 0x80000000-0x83FFFFFF
static ULONG MiFreeBitmap[MM_PAGES / 32];             // bit set = free physical page
ULONG MmAvailablePages;
static ULONG MiNextLowPfn;                            // single-page allocations go bottom-up

static bool MiIsFree(ULONG pfn) { return MiFreeBitmap[pfn >> 5] & (1u << (pfn & 31)); }
static void MiMarkFree(ULONG pfn) {
    MiFreeBitmap[pfn >> 5] |= 1u << (pfn & 31);
    MmAvailablePages++;
}
static void MiMarkUsed(ULONG pfn) {
    MiFreeBitmap[pfn >> 5] &= ~(1u << (pfn & 31));
    MmAvailablePages--;
}

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

// Single pages come from the bottom so top-down contiguous allocations stay unfragmented.
// The page stays mapped at 0x80000000|PA (callers use that alias) and is zeroed.
ULONG MiAllocatePhysicalPage(bool zero) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG pfn = 0;
    for (ULONG p = MiNextLowPfn; p <= MM_HIGHEST_CONTIGUOUS_PFN; p++)
        if (MiIsFree(p)) {
            pfn = p;
            break;
        }
    if (pfn) {
        MiMarkUsed(pfn);
        MiNextLowPfn = pfn + 1;
        MiPteArray[pfn] = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY | PTE_END_OF_ALLOCATION;
        MiInvalidatePage(MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT));
    }
    KfLowerIrql(old);
    if (pfn && zero)
        RtlZeroMemory((PVOID)(MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT)), PAGE_SIZE);
    return pfn;
}

void MiFreePhysicalPage(ULONG pfn) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    MiPteArray[pfn] = 0;
    MiInvalidatePage(MM_SYSTEM_PHYSICAL_MAP | (pfn << PAGE_SHIFT));
    MiMarkFree(pfn);
    if (pfn < MiNextLowPfn)
        MiNextLowPfn = pfn;
    KfLowerIrql(old);
}

// Highest run of `count` free pages within [lowPfn, highPfn] whose first page is aligned.
bool MiAllocatePhysicalRange(ULONG count, ULONG lowPfn, ULONG highPfn, ULONG alignPages, ULONG *firstPfn) {
    if (!count || highPfn < lowPfn || count > highPfn - lowPfn + 1)
        return false;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG start = (LONG)(((highPfn - count + 1)) & ~(alignPages - 1));
    bool found = false;
    while (start >= (LONG)lowPfn) {
        ULONG i = 0;
        while (i < count && MiIsFree(start + i))
            i++;
        if (i == count) {
            found = true;
            break;
        }
        // Skip below the used page and re-align.
        LONG next = (LONG)(start + i) - (LONG)count;
        start = next < 0 ? -1 : (LONG)((ULONG)next & ~(alignPages - 1));
    }
    if (found) {
        for (ULONG i = 0; i < count; i++)
            MiMarkUsed(start + i);
        *firstPfn = start;
    }
    KfLowerIrql(old);
    return found;
}

// --- System PTE range 0xD0000000-0xEFFFFFFF: slot bitmap + page tables allocated on demand.

constexpr ULONG SYSTEM_PTES = (MM_SYSTEM_PTE_END - MM_SYSTEM_PTE_BASE) >> PAGE_SHIFT; // 131072
static ULONG *MiSystemPteBitmap; // bit set = slot in use
static ULONG MiSystemPteHint;

static bool MiSlotUsed(ULONG i) { return MiSystemPteBitmap[i >> 5] & (1u << (i & 31)); }

static bool MiEnsurePageTable(ULONG va) {
    ULONG *pde = MiGetPdeAddress(va);
    if (*pde & PTE_PRESENT)
        return true;
    ULONG pfn = MiAllocatePhysicalPage(); // zeroed: all PTEs not present
    if (!pfn)
        return false;
    *pde = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
    MiInvalidatePage((ULONG)MiGetPteAddress(va) & ~(PAGE_SIZE - 1));
    return true;
}

PVOID MiReserveSystemPtes(ULONG count) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PVOID result = nullptr;
    for (int pass = 0; pass < 2 && !result; pass++) {
        ULONG i = pass == 0 ? MiSystemPteHint : 0;
        ULONG run = 0;
        for (; i < SYSTEM_PTES; i++) {
            run = MiSlotUsed(i) ? 0 : run + 1;
            if (run == count) {
                ULONG first = i - count + 1;
                ULONG va = MM_SYSTEM_PTE_BASE + (first << PAGE_SHIFT);
                bool ok = true;
                for (ULONG a = va & ~0x3FFFFFu; a < va + (count << PAGE_SHIFT) && ok; a += 0x400000)
                    ok = MiEnsurePageTable(a);
                if (!ok)
                    break;
                for (ULONG s = first; s <= i; s++)
                    MiSystemPteBitmap[s >> 5] |= 1u << (s & 31);
                MiSystemPteHint = i + 1;
                result = (PVOID)va;
                break;
            }
        }
    }
    KfLowerIrql(old);
    return result;
}

void MiReleaseSystemPtes(PVOID va, ULONG count) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG first = ((ULONG)va - MM_SYSTEM_PTE_BASE) >> PAGE_SHIFT;
    for (ULONG s = first; s < first + count; s++) {
        ULONG a = (ULONG)va + ((s - first) << PAGE_SHIFT);
        *MiGetPteAddress(a) = 0;
        MiInvalidatePage(a);
        MiSystemPteBitmap[s >> 5] &= ~(1u << (s & 31));
    }
    if (first < MiSystemPteHint)
        MiSystemPteHint = first;
    KfLowerIrql(old);
}

// --- Initialisation

static inline void MiReloadCr3() {
    ULONG cr3;
    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3)::"memory");
}

volatile ULONG MmPersistedPages; // pages re-adopted by the last quick reboot

void MmInitSystem(bool QuickReboot) {
    // Kernel image extent from our own PE header; .text (first section) becomes read-only.
    auto image = (UCHAR *)0x80010000;
    ULONG lfanew = *(ULONG *)(image + 0x3C);
    ULONG sizeOfImage = *(ULONG *)(image + lfanew + 24 + 56);
    USHORT optSize = *(USHORT *)(image + lfanew + 20);
    UCHAR *sections = image + lfanew + 24 + optSize;
    ULONG textEnd = *(ULONG *)(sections + 12) + *(ULONG *)(sections + 8); // VA + VirtualSize of .text
    ULONG imageEndPfn = (0x10000 + sizeOfImage + PAGE_SIZE - 1) >> PAGE_SHIFT;
    ULONG textEndPfn = (0x10000 + (textEnd & ~(PAGE_SIZE - 1))) >> PAGE_SHIFT;

    // 1. Build the PTE array for 0x80000000-0x83FFFFFF (still reachable through the 2BL large pages).
    const ULONG rw = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY;
    // Pages 1-14 (below the page directory at 0xF000) and those between the image and the GPU memory are
    // allocatable; a quick reboot re-adopts persisted allocations (PTE bit 10) there.
    auto allocatable = [&](ULONG pfn) {
        return (pfn >= 1 && pfn < MM_LOW_RESERVED_PFN) || (pfn >= imageEndPfn && pfn <= MM_HIGHEST_CONTIGUOUS_PFN);
    };
    ULONG persisted = 0;
    for (ULONG pfn = 0; pfn < MM_PAGES; pfn++) {
        bool keep = QuickReboot && allocatable(pfn) &&
                    (MiPteArray[pfn] & (PTE_PRESENT | PTE_PERSIST)) == (PTE_PRESENT | PTE_PERSIST);
        if (keep)
            persisted++;
        else
            MiPteArray[pfn] = 0;
    }
    MmPersistedPages = persisted;
    for (ULONG pfn = 0; pfn < imageEndPfn; pfn++)
        if (!allocatable(pfn))
            MiPteArray[pfn] = (pfn << PAGE_SHIFT) | rw | PTE_PERSIST;
    for (ULONG pfn = 0x10; pfn < textEndPfn; pfn++)
        MiPteArray[pfn] &= ~PTE_WRITE;
    for (ULONG pfn = MM_GPU_INSTANCE_FIRST_PFN; pfn < MM_PAGE_TABLES_FIRST_PFN; pfn++)
        MiPteArray[pfn] = (pfn << PAGE_SHIFT) | rw | PTE_PWT | PTE_PCD;
    for (ULONG pfn = MM_PAGE_TABLES_FIRST_PFN; pfn < MM_PAGES; pfn++)
        MiPteArray[pfn] = (pfn << PAGE_SHIFT) | rw | PTE_PERSIST;

    // 2. Free list: pages 1-14 and everything between the kernel image and the GPU instance memory.
    MmAvailablePages = 0;
    for (ULONG pfn = 1; pfn <= MM_HIGHEST_CONTIGUOUS_PFN; pfn++)
        if (allocatable(pfn) && !MiPteArray[pfn])
            MiMarkFree(pfn);
    MiNextLowPfn = imageEndPfn;

    // 3. Switch the page directory: 4 KB PTEs for RAM at 0x80000000, nothing above 64 MB, no user
    //    mappings, and the reference MMIO windows (spec/02 §4).
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
    // PDE 0x3FF (flash) is kept for the quick-reboot .data copy (the reference clears it; spec/09).
    MiReloadCr3();

    // 4. System PTE slot bitmap (16 KB) from physical pages.
    MiSystemPteBitmap = nullptr;
    ULONG first;
    if (!MiAllocatePhysicalRange(4, imageEndPfn, MM_HIGHEST_CONTIGUOUS_PFN, 1, &first))
        KeBugCheck(0x7D); // INSTALL_MORE_MEMORY
    for (ULONG i = 0; i < 4; i++)
        MiPteArray[first + i] = ((first + i) << PAGE_SHIFT) | rw | (i == 3 ? PTE_END_OF_ALLOCATION : 0);
    MiReloadCr3();
    MiSystemPteBitmap = (ULONG *)(MM_SYSTEM_PHYSICAL_MAP | (first << PAGE_SHIFT));
    RtlZeroMemory(MiSystemPteBitmap, 4 * PAGE_SIZE);

    ExInitializePool();
    MiInitializeVirtualMemory();
}
