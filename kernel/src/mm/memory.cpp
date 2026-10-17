// Exported memory-manager routines (spec/09 §2-5).
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

static ULONG *const MiPteArray = (ULONG *)0x83FF0000;
static ULONG MiGpuInstanceSize = 0x10000; // reserved below the page tables (reference initial value)

static ULONG BytesToPages(ULONG bytes) { return (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT; }

// --- Contiguous memory: physical pages from the top down, returned at 0x80000000|PA.

XBAPI PVOID NTAPI MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG LowestAcceptableAddress,
                                               ULONG HighestAcceptableAddress, ULONG Alignment, ULONG Protect) {
    ULONG pteBits;
    if (!NumberOfBytes || !MiProtectToPte(Protect, &pteBits))
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
    if (!MiAllocateContiguousPages(pages, lowPfn, highPfn, alignPages, pteBits, &first))
        return nullptr;
    if (Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
        MiFlushCaches();
    return (PVOID)(MM_SYSTEM_PHYSICAL_MAP | (first << PAGE_SHIFT));
}

XBAPI PVOID NTAPI MmAllocateContiguousMemory(ULONG NumberOfBytes) {
    return MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

XBAPI void NTAPI MmFreeContiguousMemory(PVOID BaseAddress) {
    ULONG pfn = ((ULONG)BaseAddress & 0x7FFFFFFF) >> PAGE_SHIFT;
    for (;;) {
        ULONG pte = MiPteArray[pfn];
        if (!(pte & PTE_PRESENT))
            break;
        MiFreeContiguousPage(pfn);
        if (pte & PTE_END_OF_ALLOCATION)
            break;
        pfn++;
    }
}

XBAPI void NTAPI MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG *pte = MiGetPteAddress((ULONG)BaseAddress), *last = MiGetPteAddress((ULONG)BaseAddress + NumberOfBytes - 1);
    for (; pte <= last; pte++)
        *pte = (*pte & ~PTE_PERSIST) | (Persist ? PTE_PERSIST : 0);
    KfLowerIrql(old);
}

XBAPI ULONG NTAPI MmQueryAllocationSize(PVOID BaseAddress) {
    ULONG pages = 1;
    for (ULONG *pte = MiGetPteAddress((ULONG)BaseAddress); !(*pte & PTE_END_OF_ALLOCATION); pte++)
        pages++;
    return pages << PAGE_SHIFT;
}

XBAPI PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress(PVOID BaseAddress) {
    ULONG va = (ULONG)BaseAddress;
    ULONG pde = *MiGetPdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return 0;
    if (pde & PTE_LARGE)
        return (pde & 0xFFC00000) | (va & 0x3FFFFF);
    ULONG pte = *MiGetPteAddress(va);
    if (!(pte & PTE_PRESENT))
        return 0;
    return (pte & 0xFFFFF000) | (va & 0xFFF);
}

XBAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress) {
    ULONG va = (ULONG)VirtualAddress;
    ULONG pde = *MiGetPdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return FALSE;
    return (pde & PTE_LARGE) || (*MiGetPteAddress(va) & PTE_PRESENT);
}

// --- System PTE range: system memory, kernel stacks, I/O mappings.

// Pages mapped in the system PTE range, each recorded with its usage (system memory, pool, stacks).
PVOID MiAllocateMappedPages(ULONG pages, ULONG pteBits, bool guardPage, ULONG usage) {
    ULONG slots = pages + (guardPage ? 1 : 0);
    if (pages > MmAvailablePages)
        return nullptr;
    auto base = (ULONG)MiReserveSystemPtes(slots);
    if (!base)
        return nullptr;
    ULONG va = base + (guardPage ? PAGE_SIZE : 0);
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        ULONG pfn = MiAllocatePage(usage, MiGetPteAddress(a), false);
        if (!pfn) {
            for (ULONG j = 0; j < i; j++) { // undo
                ULONG b = va + (j << PAGE_SHIFT);
                MiFreePage(*MiGetPteAddress(b) >> PAGE_SHIFT);
                *MiGetPteAddress(b) = 0;
            }
            MiReleaseSystemPtes((PVOID)base, slots);
            return nullptr;
        }
        *MiGetPteAddress(a) = (pfn << PAGE_SHIFT) | pteBits | (i == pages - 1 ? PTE_END_OF_ALLOCATION : 0);
        MiInvalidatePage(a);
    }
    if (pteBits & (PTE_PCD | PTE_PWT))
        MiFlushCaches();
    return (PVOID)va;
}

ULONG MiFreeMappedPages(ULONG va, ULONG pages) {
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        ULONG *pte = MiGetPteAddress(a);
        if (*pte & PTE_PRESENT) {
            ULONG pfn = *pte >> PAGE_SHIFT;
            *pte = 0;
            MiInvalidatePage(a);
            MiFreePage(pfn);
        }
    }
    return pages;
}

XBAPI PVOID NTAPI MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect) {
    ULONG pteBits;
    if (!NumberOfBytes || !MiProtectToPte(Protect, &pteBits))
        return nullptr;
    return MiAllocateMappedPages(BytesToPages(NumberOfBytes), pteBits, false, MmSystemMemoryUsage);
}

XBAPI ULONG NTAPI MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes) {
    ULONG pages = BytesToPages(NumberOfBytes);
    MiFreeMappedPages((ULONG)BaseAddress, pages);
    MiReleaseSystemPtes(BaseAddress, pages);
    return pages;
}

// Returns the stack top; a not-present guard page sits below the stack.
XBAPI PVOID NTAPI MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread) {
    ULONG pages = BytesToPages(NumberOfBytes);
    auto stack = (UCHAR *)MiAllocateMappedPages(pages, PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY, true,
                                                MmStackUsage);
    if (!stack)
        return nullptr;
    return stack + (pages << PAGE_SHIFT);
}

XBAPI void NTAPI MmDeleteKernelStack(PVOID KernelStackBase, PVOID KernelStackLimit) {
    ULONG pages = ((ULONG)KernelStackBase - (ULONG)KernelStackLimit) >> PAGE_SHIFT;
    MiFreeMappedPages((ULONG)KernelStackLimit, pages);
    MiReleaseSystemPtes((UCHAR *)KernelStackLimit - PAGE_SIZE, pages + 1);
}

// Uncached MMIO in 0xF8000000-0xFFBFFFFF and write-combined AGP space in 0xF0000000-0xF7FFFFFF are
// already mapped 1:1 by large pages; anything else gets system PTEs.
XBAPI PVOID NTAPI MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, ULONG NumberOfBytes, ULONG Protect) {
    ULONG pteBits;
    if (!MiProtectToPte(Protect, &pteBits))
        return nullptr;
    if (Protect == (PAGE_READWRITE | PAGE_NOCACHE) && PhysicalAddress >= 0xF8000000 &&
        PhysicalAddress + NumberOfBytes < 0xFFC00000)
        return (PVOID)PhysicalAddress;
    if (Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE) && PhysicalAddress >= 0xF0000000 &&
        PhysicalAddress + NumberOfBytes < 0xF8000000) {
        MiFlushCaches();
        return (PVOID)PhysicalAddress;
    }
    ULONG pages = BytesToPages((PhysicalAddress & 0xFFF) + NumberOfBytes);
    auto va = (ULONG)MiReserveSystemPtes(pages);
    if (!va)
        return nullptr;
    for (ULONG i = 0; i < pages; i++) {
        ULONG a = va + (i << PAGE_SHIFT);
        *MiGetPteAddress(a) = ((PhysicalAddress & ~0xFFFu) + (i << PAGE_SHIFT)) | pteBits;
        MiInvalidatePage(a);
    }
    if (pteBits & (PTE_PCD | PTE_PWT))
        MiFlushCaches();
    return (PVOID)(va + (PhysicalAddress & 0xFFF));
}

XBAPI void NTAPI MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes) {
    ULONG va = (ULONG)BaseAddress;
    if (va < MM_SYSTEM_PTE_BASE || va >= MM_SYSTEM_PTE_END)
        return; // identity-mapped window
    MiReleaseSystemPtes((PVOID)(va & ~0xFFFu), BytesToPages((va & 0xFFF) + NumberOfBytes));
}

// --- Protection

static ULONG MiPteToProtect(ULONG pte) {
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
    ULONG pde = *MiGetPdeAddress(va);
    if (!(pde & PTE_PRESENT))
        return 0x01;
    return MiPteToProtect((pde & PTE_LARGE) ? pde : *MiGetPteAddress(va));
}

XBAPI void NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect) {
    ULONG pteBits;
    if (!MiProtectToPte(NewProtect, &pteBits))
        return;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG va = (ULONG)BaseAddress & ~0xFFFu, end = (ULONG)BaseAddress + NumberOfBytes;
    const ULONG attrMask = PTE_PRESENT | PTE_WRITE | PTE_PWT | PTE_PCD | PTE_ACCESSED | PTE_DIRTY;
    for (; va < end; va += PAGE_SIZE) {
        ULONG *pte = MiGetPteAddress(va);
        if (*pte & PTE_PRESENT) {
            *pte = (*pte & ~attrMask) | pteBits;
            MiInvalidatePage(va);
        }
    }
    if (NewProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
        MiFlushCaches();
    KfLowerIrql(old);
}

// --- Page locking, as the reference: a lock count in the low word of a page's busy entry, in steps of 2. Locked
// pages are not moved by contiguous allocations. Mapped pages (contiguous memory, the kernel) never move and keep
// no count.

XBAPI void NTAPI MmLockUnlockPhysicalPage(PHYSICAL_ADDRESS PhysicalAddress, BOOLEAN UnlockPage) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG pfn = PhysicalAddress >> PAGE_SHIFT;
    if (pfn < MM_PAGES && !(MiPteArray[pfn] & PTE_PRESENT))
        ((USHORT *)&MiPteArray[pfn])[0] += UnlockPage ? -2 : 2;
    KfLowerIrql(old);
}

// Every page of the buffer, outside the 0x80000000 mapping and large pages.
XBAPI void NTAPI MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages) {
    ULONG va = (ULONG)BaseAddress;
    if (!NumberOfBytes || (va >= MM_SYSTEM_PHYSICAL_MAP && va < MM_SYSTEM_PHYSICAL_MAP + 0x10000000) ||
        (*MiGetPdeAddress(va) & PTE_LARGE))
        return;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    for (ULONG *pte = MiGetPteAddress(va), *last = MiGetPteAddress(va + NumberOfBytes - 1); pte <= last; pte++) {
        ULONG pfn = *pte >> PAGE_SHIFT;
        if ((*pte & PTE_PRESENT) && pfn < MM_PAGES && !(MiPteArray[pfn] & PTE_PRESENT))
            ((USHORT *)&MiPteArray[pfn])[0] += UnlockPages ? -2 : 2;
    }
    KfLowerIrql(old);
}

// --- GPU instance memory: the region below the page tables; shrinking it returns pages to the pool.

XBAPI PVOID NTAPI MmClaimGpuInstanceMemory(ULONG NumberOfBytes, ULONG *NumberOfPaddingBytes) {
    *NumberOfPaddingBytes = 0x10000;
    if (NumberOfBytes != 0xFFFFFFFF) {
        KIRQL old = KeRaiseIrqlToDpcLevel();
        ULONG oldFirst = MM_PAGE_TABLES_FIRST_PFN - BytesToPages(MiGpuInstanceSize);
        ULONG newFirst = MM_PAGE_TABLES_FIRST_PFN - BytesToPages(NumberOfBytes);
        for (ULONG pfn = oldFirst; pfn < newFirst; pfn++)
            MiFreeContiguousPage(pfn);
        MiGpuInstanceSize = (NumberOfBytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        KfLowerIrql(old);
    }
    return (PVOID)(0x84000000 - *NumberOfPaddingBytes);
}
