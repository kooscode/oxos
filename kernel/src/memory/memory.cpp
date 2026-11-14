// Exported memory-manager routines (spec/stock/09 §2-5).
#include <memory/memory.h>
#include <hardware/chipset/serial.h>
#include <arch/arch.h>
#include <core/core.h>
#include <runtime/runtime.h>

namespace xk::memory {

static ULONG *const PteArray = (ULONG *)0x83FF0000;
static ULONG GpuInstanceSize = 0x10000; // reserved below the page tables (reference initial value)

static ULONG BytesToPages(ULONG bytes) {
    return (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
}

// --- Contiguous memory: physical pages from the top down, returned at 0x80000000|PA.

XBAPI PVOID NTAPI MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG LowestAcceptableAddress,
                                               ULONG HighestAcceptableAddress, ULONG Alignment, ULONG Protect) {
    ULONG pteBits;
    if (!NumberOfBytes || !ProtectToPte(Protect, &pteBits))
        return nullptr;
    ULONG highPfn = HighestAcceptableAddress >> PAGE_SHIFT;
    if (highPfn > MM_HIGHEST_CONTIGUOUS_PFN)
        highPfn = MM_HIGHEST_CONTIGUOUS_PFN;
    ULONG lowPfn = LowestAcceptableAddress >> PAGE_SHIFT;
    if (lowPfn > highPfn)
        lowPfn = highPfn;
    ULONG alignPages = Alignment >> PAGE_SHIFT;
    if (!alignPages)
        alignPages = 1;
    ULONG pages = BytesToPages(NumberOfBytes), first;
    if (!AllocateContiguousPages(pages, lowPfn, highPfn, alignPages, pteBits, &first))
        return nullptr;
    if (Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
        FlushCaches();
    return (PVOID)(MM_SYSTEM_PHYSICAL_MAP | (first << PAGE_SHIFT));
}

XBAPI PVOID NTAPI MmAllocateContiguousMemory(ULONG NumberOfBytes) {
    return MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

XBAPI void NTAPI MmFreeContiguousMemory(PVOID BaseAddress) {
    ULONG pfn = ((ULONG)BaseAddress & 0x7FFFFFFF) >> PAGE_SHIFT;
    for (;;) {
        ULONG pte = PteArray[pfn];
        if (!(pte & PTE_PRESENT))
            break;
        FreeContiguousPage(pfn);
        if (pte & PTE_END_OF_ALLOCATION)
            break;
        pfn++;
    }
}

XBAPI void NTAPI MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG *pte = PteAddress((ULONG)BaseAddress), *last = PteAddress((ULONG)BaseAddress + NumberOfBytes - 1);
    for (; pte <= last; pte++)
        *pte = (*pte & ~PTE_PERSIST) | (Persist ? PTE_PERSIST : 0);
    KfLowerIrql(old);
}

// As the reference (0x8001de2e): counts PTEs up to the one marked as the allocation's end, at DISPATCH_LEVEL so
// that the allocation cannot be freed or remapped meanwhile. Like the reference it trusts the caller to pass the
// base of a live system-memory or pool allocation.
XBAPI ULONG NTAPI MmQueryAllocationSize(PVOID BaseAddress) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG pages = 1;
    for (ULONG *pte = PteAddress((ULONG)BaseAddress); !(*pte & PTE_END_OF_ALLOCATION); pte++)
        pages++;
    KfLowerIrql(old);
    return pages << PAGE_SHIFT;
}

XBAPI PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress(PVOID BaseAddress) {
    ULONG va = (ULONG)BaseAddress;
    ULONG pde = *PdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return 0;
    if (pde & PTE_LARGE)
        return (pde & 0xFFC00000) | (va & 0x3FFFFF);
    ULONG pte = *PteAddress(va);
    if (!(pte & PTE_PRESENT))
        return 0;
    return (pte & 0xFFFFF000) | (va & 0xFFF);
}

XBAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress) {
    ULONG va = (ULONG)VirtualAddress;
    ULONG pde = *PdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return FALSE;
    return (pde & PTE_LARGE) || (*PteAddress(va) & PTE_PRESENT);
}

// --- System PTE range: system memory, kernel stacks, I/O mappings.

// Pages mapped in the system PTE range, each recorded with its usage (system memory, pool, stacks).
PVOID AllocateMappedPages(ULONG pages, ULONG pteBits, bool guardPage, ULONG usage) {
    ULONG slots = pages + (guardPage ? 1 : 0);
    if (pages > AvailablePages)
        return nullptr;
    auto base = (ULONG)ReserveSystemPtes(slots);
    if (!base)
        return nullptr;
    ULONG va = base + (guardPage ? PAGE_SIZE : 0);
    // Uncached / write-combined pages: no line of any free page may still sit in the caches when it is mapped with
    // another memory type (a memory-type conflict is a machine check on the P6), so flush before mapping, not after.
    if (pteBits & (PTE_PCD | PTE_PWT))
        FlushCaches();
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        ULONG pfn = AllocatePage(usage, PteAddress(a), false);
        if (!pfn) {
            for (ULONG j = 0; j < i; j++) { // undo
                ULONG b = va + (j << PAGE_SHIFT);
                FreePage(*PteAddress(b) >> PAGE_SHIFT);
                *PteAddress(b) = 0;
            }
            ReleaseSystemPtes((PVOID)base, slots);
            return nullptr;
        }
        *PteAddress(a) = (pfn << PAGE_SHIFT) | pteBits | (i == pages - 1 ? PTE_END_OF_ALLOCATION : 0);
        InvalidatePage(a);
    }
    if (pteBits & (PTE_PCD | PTE_PWT))
        FlushCaches();
    return (PVOID)va;
}

ULONG FreeMappedPages(ULONG va, ULONG pages) {
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        ULONG *pte = PteAddress(a);
        if (*pte & PTE_PRESENT) {
            ULONG pfn = *pte >> PAGE_SHIFT;
            *pte = 0;
            InvalidatePage(a);
            FreePage(pfn);
        }
    }
    return pages;
}

XBAPI PVOID NTAPI MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect) {
    ULONG pteBits;
    if (!NumberOfBytes || !ProtectToPte(Protect, &pteBits))
        return nullptr;
    return AllocateMappedPages(BytesToPages(NumberOfBytes), pteBits, false, MmSystemMemoryUsage);
}

XBAPI ULONG NTAPI MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes) {
    ULONG pages = BytesToPages(NumberOfBytes);
    FreeMappedPages((ULONG)BaseAddress, pages);
    ReleaseSystemPtes(BaseAddress, pages);
    return pages;
}

// Returns the stack top; a not-present guard page sits below the stack.
XBAPI PVOID NTAPI MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread) {
    ULONG pages = BytesToPages(NumberOfBytes);
    auto stack = (UCHAR *)AllocateMappedPages(pages, PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY, true,
                                              MmStackUsage);
    if (!stack)
        return nullptr;
    return stack + (pages << PAGE_SHIFT);
}

XBAPI void NTAPI MmDeleteKernelStack(PVOID KernelStackBase, PVOID KernelStackLimit) {
    ULONG pages = ((ULONG)KernelStackBase - (ULONG)KernelStackLimit) >> PAGE_SHIFT;
    FreeMappedPages((ULONG)KernelStackLimit, pages);
    ReleaseSystemPtes((UCHAR *)KernelStackLimit - PAGE_SIZE, pages + 1);
}

// Uncached MMIO in 0xF8000000-0xFFBFFFFF and write-combined AGP space in 0xF0000000-0xF7FFFFFF are
// already mapped 1:1 by large pages; anything else gets system PTEs.
XBAPI PVOID NTAPI MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, ULONG NumberOfBytes, ULONG Protect) {
    ULONG pteBits;
    if (!ProtectToPte(Protect, &pteBits))
        return nullptr;
    if (Protect == (PAGE_READWRITE | PAGE_NOCACHE) && PhysicalAddress >= 0xF8000000 &&
        PhysicalAddress + NumberOfBytes < 0xFFC00000)
        return (PVOID)PhysicalAddress;
    if (Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE) && PhysicalAddress >= 0xF0000000 &&
        PhysicalAddress + NumberOfBytes < 0xF8000000) {
        FlushCaches();
        return (PVOID)PhysicalAddress;
    }
    ULONG pages = BytesToPages((PhysicalAddress & 0xFFF) + NumberOfBytes);
    auto va = (ULONG)ReserveSystemPtes(pages);
    if (!va)
        return nullptr;
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        *PteAddress(a) = ((PhysicalAddress & ~0xFFFu) + (i << PAGE_SHIFT)) | pteBits;
        InvalidatePage(a);
    }
    if (pteBits & (PTE_PCD | PTE_PWT))
        FlushCaches();
    return (PVOID)(va + (PhysicalAddress & 0xFFF));
}

XBAPI void NTAPI MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes) {
    ULONG va = (ULONG)BaseAddress;
    if (va < MM_SYSTEM_PTE_BASE || va >= MM_SYSTEM_PTE_END)
        return; // identity-mapped window
    ReleaseSystemPtes((PVOID)(va & ~0xFFFu), BytesToPages((va & 0xFFF) + NumberOfBytes));
}

// --- Protection

static ULONG PteToProtect(ULONG pte) {
    if (!(pte & PTE_PRESENT))
        return 0x01; // PAGE_NOACCESS
    ULONG protect = (pte & PTE_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    if (pte & PTE_PCD)
        protect |= PAGE_NOCACHE;
    else if (pte & PTE_PWT)
        protect |= PAGE_WRITECOMBINE;
    return protect;
}

XBAPI ULONG NTAPI MmQueryAddressProtect(PVOID VirtualAddress) {
    ULONG va = (ULONG)VirtualAddress;
    ULONG pde = *PdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return 0x01;
    return PteToProtect((pde & PTE_LARGE) ? pde : *PteAddress(va));
}

XBAPI void NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect) {
    ULONG pteBits;
    if (!ProtectToPte(NewProtect, &pteBits))
        return;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG va = (ULONG)BaseAddress & ~0xFFFu, end = (ULONG)BaseAddress + NumberOfBytes;
    const ULONG attrMask = PTE_PRESENT | PTE_WRITE | PTE_PWT | PTE_PCD | PTE_ACCESSED | PTE_DIRTY;
    // A change of memory type: flush the caches first (nothing at DISPATCH_LEVEL touches these pages in between),
    // then the new type, then flush again for anything prefetched meanwhile. Flushing only afterwards left cached
    // lines of a page already mapped uncached.
    bool typeChange = (NewProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) != 0;
    for (ULONG a = va; a < end && !typeChange; a += PAGE_SIZE) {
        ULONG *pte = PteAddress(a);
        typeChange = (*pte & PTE_PRESENT) && ((*pte ^ pteBits) & (PTE_PCD | PTE_PWT));
    }
    if (typeChange)
        FlushCaches();
    for (; va < end; va += PAGE_SIZE) {
        ULONG *pte = PteAddress(va);
        if (*pte & PTE_PRESENT) {
            *pte = (*pte & ~attrMask) | pteBits;
            InvalidatePage(va);
        }
    }
    if (typeChange)
        FlushCaches();
    KfLowerIrql(old);
}

// --- Page locking, as the reference: a lock count in the low word of a page's busy entry, in steps of 2. Locked
// pages are not moved by contiguous allocations. Mapped pages (contiguous memory, the kernel) never move and keep
// no count.

XBAPI void NTAPI MmLockUnlockPhysicalPage(PHYSICAL_ADDRESS PhysicalAddress, BOOLEAN UnlockPage) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG pfn = PhysicalAddress >> PAGE_SHIFT;
    if (pfn <= HighestPhysicalPage && !(PteArray[pfn] & PTE_PRESENT))
        ((USHORT *)&PteArray[pfn])[0] += UnlockPage ? -2 : 2;
    KfLowerIrql(old);
}

// Every page of the buffer, outside the 0x80000000 mapping and large pages.
XBAPI void NTAPI MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages) {
    ULONG va = (ULONG)BaseAddress;
    if (!NumberOfBytes || (va >= MM_SYSTEM_PHYSICAL_MAP && va < MM_SYSTEM_PHYSICAL_MAP + 0x10000000) ||
        (*PdeAddress(va) & PTE_LARGE))
        return;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    for (ULONG a = va & ~(PAGE_SIZE - 1); a < va + NumberOfBytes; a += PAGE_SIZE) {
        if (!(*PdeAddress(a) & PTE_PRESENT) || (*PdeAddress(a) & PTE_LARGE))
            continue; // no page table here (nothing to lock)
        ULONG *pte = PteAddress(a);
        ULONG pfn = *pte >> PAGE_SHIFT;
        if ((*pte & PTE_PRESENT) && pfn <= HighestPhysicalPage && !(PteArray[pfn] & PTE_PRESENT))
            ((USHORT *)&PteArray[pfn])[0] += UnlockPages ? -2 : 2;
    }
    KfLowerIrql(old);
}

// --- GPU instance memory: the region below the top 64 KB of the low 64 MB, and with 128 MB its copy below the top of
// RAM (spec/stock/09 §10); shrinking it returns pages of both to the pool. The address returned is the top of RAM
// less the padding: the GPU uses the top of the memory it has.

XBAPI PVOID NTAPI MmClaimGpuInstanceMemory(ULONG NumberOfBytes, ULONG *NumberOfPaddingBytes) {
    *NumberOfPaddingBytes = 0x10000;
    if (NumberOfBytes != 0xFFFFFFFF) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        ULONG oldFirst = MM_PAGE_TABLES_FIRST_PFN - BytesToPages(GpuInstanceSize);
        ULONG newFirst = MM_PAGE_TABLES_FIRST_PFN - BytesToPages(NumberOfBytes);
        for (ULONG pfn = oldFirst; pfn < newFirst; pfn++) {
            FreeContiguousPage(pfn);
            if (HighestPhysicalPage >= MM_LOW_PAGES)
                FreeContiguousPage(pfn + MM_LOW_PAGES);
        }
        GpuInstanceSize = (NumberOfBytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        KfLowerIrql(old);
    }
    return (PVOID)(MM_SYSTEM_PHYSICAL_MAP + ((HighestPhysicalPage + 1) << PAGE_SHIFT) - *NumberOfPaddingBytes);
}

} // namespace xk::memory
