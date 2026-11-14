// Title virtual memory: Nt{Allocate,Free,Query,Protect}VirtualMemory and MmQueryStatistics (spec/stock/09 §6).
// One address space (0x00010000-0x7FFDFFFF); reservations are VADs kept in an address-sorted list.
#include <memory/memory.h>
#include <arch/arch.h>
#include <hardware/chipset/serial.h>
#include <core/core.h>
#include <runtime/runtime.h>

namespace xk::memory {

constexpr NTSTATUS STATUS_INVALID_PARAMETER = (NTSTATUS)0xC000000D, STATUS_NO_MEMORY = (NTSTATUS)0xC0000017,
                   STATUS_CONFLICTING_ADDRESSES = (NTSTATUS)0xC0000018, STATUS_UNABLE_TO_FREE_VM = (NTSTATUS)0xC000001A,
                   STATUS_INVALID_PAGE_PROTECTION = (NTSTATUS)0xC0000045,
                   STATUS_INSUFFICIENT_RESOURCES = (NTSTATUS)0xC000009A,
                   STATUS_FREE_VM_NOT_AT_BASE = (NTSTATUS)0xC000009F,
                   STATUS_MEMORY_NOT_ALLOCATED = (NTSTATUS)0xC00000A0;

struct MMVAD {
    ULONG StartVpn, EndVpn; // inclusive page numbers
    ULONG AllocationProtect;
    LIST_ENTRY Links;
};

static LIST_ENTRY VadList;                    // the title's reserved regions, in address order
static RTL_CRITICAL_SECTION AddressSpaceLock; // the reference's type (MmGlobalData->AddressSpaceLock)
ULONG VirtualMemoryBytesReserved;
volatile ULONG CommitFailures, CommitFailNeeded, CommitFailAvailable; // diagnostics (tests, title bring-up)
volatile ULONG VmFailures, VmLastFailure[5]; // diagnostics: status, base, size, type, protect of the last one

// --- Page tables, the address-space lock, protections

// Committed PTEs of a title page table, kept in bits 17-27 of the page table's page database entry (unused for that
// usage): no resident table for it.
static ULONG &PageTableEntry(ULONG va) {
    return ((ULONG *)0x83FF0000)[*PdeAddress(va) >> PAGE_SHIFT];
}

static ULONG PageTableUse(ULONG va) {
    return (PageTableEntry(va) >> 17) & 0x7FF;
}

static void AdjustPageTableUse(ULONG va, LONG delta) {
    PageTableEntry(va) += (ULONG)delta << 17;
}

static void Lock() {
    RtlEnterCriticalSection(&AddressSpaceLock);
}

static void Unlock() {
    RtlLeaveCriticalSection(&AddressSpaceLock);
}

void InitializeVirtualMemory() {
    InitializeListHead(&VadList);
    RtlInitializeCriticalSection(&AddressSpaceLock);
}

// Protection for user pages. One base protection; GUARD is accepted but not enforced.
static bool VmProtectToPte(ULONG protect, ULONG *bits) {
    ULONG cache = protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE);
    ULONG base = protect & ~(PAGE_NOCACHE | PAGE_WRITECOMBINE | PAGE_GUARD);
    if (cache == (PAGE_NOCACHE | PAGE_WRITECOMBINE))
        return false;
    ULONG b;
    switch (base) {
    case PAGE_NOACCESS: *bits = PTE_COMMITTED_NOACCESS; return cache == 0;
    case PAGE_READONLY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ: b = PTE_PRESENT | PTE_ACCESSED; break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: b = PTE_PRESENT | PTE_WRITE | PTE_ACCESSED | PTE_DIRTY; break;
    default: return false;
    }
    if (cache == PAGE_NOCACHE)
        b |= PTE_PCD;
    else if (cache == PAGE_WRITECOMBINE)
        b |= PTE_PWT;
    *bits = b;
    return true;
}

static ULONG PteToVmProtect(ULONG pte) {
    if (pte & PTE_COMMITTED_NOACCESS)
        return PAGE_NOACCESS;
    ULONG p = (pte & PTE_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    if (pte & PTE_PCD)
        p |= PAGE_NOCACHE;
    else if (pte & PTE_PWT)
        p |= PAGE_WRITECOMBINE;
    return p;
}

// --- VADs: the title's reservations

// The VAD that holds page vpn, or null.
static MMVAD *FindVad(ULONG vpn) {
    for (PLIST_ENTRY e = VadList.Flink; e != &VadList; e = e->Flink) {
        auto v = CONTAINING_RECORD(e, MMVAD, Links);
        if (vpn < v->StartVpn)
            return nullptr;
        if (vpn <= v->EndVpn)
            return v;
    }
    return nullptr;
}

// No reservation overlaps pages startVpn-endVpn.
static bool RangeFree(ULONG startVpn, ULONG endVpn) {
    for (PLIST_ENTRY e = VadList.Flink; e != &VadList; e = e->Flink) {
        auto v = CONTAINING_RECORD(e, MMVAD, Links);
        if (v->StartVpn <= endVpn && startVpn <= v->EndVpn)
            return false;
    }
    return true;
}

// Into the list, in address order.
static void InsertVad(MMVAD *vad) {
    PLIST_ENTRY e = VadList.Flink;
    while (e != &VadList && CONTAINING_RECORD(e, MMVAD, Links)->StartVpn < vad->StartVpn)
        e = e->Flink;
    InsertTailList(e, &vad->Links);
}

// Find a free, 64 KB-aligned range of `size` bytes below `highest` (bottom-up or top-down).
static ULONG FindFreeRange(ULONG size, ULONG highest, bool topDown) {
    if (topDown) {
        ULONG candidate = (highest + 1 - size) & ~(ALLOCATION_GRANULARITY - 1);
        while (candidate >= MM_LOWEST_USER_ADDRESS && candidate + size - 1 <= highest) {
            ULONG s = candidate >> PAGE_SHIFT, e = (candidate + size - 1) >> PAGE_SHIFT;
            if (RangeFree(s, e))
                return candidate;
            candidate -= ALLOCATION_GRANULARITY;
        }
        return 0;
    }
    ULONG candidate = MM_LOWEST_USER_ADDRESS;
    for (PLIST_ENTRY e = VadList.Flink;; e = e->Flink) {
        ULONG limit = e == &VadList ? highest + 1 : CONTAINING_RECORD(e, MMVAD, Links)->StartVpn << PAGE_SHIFT;
        if (candidate + size <= limit && candidate + size - 1 <= highest)
            return candidate;
        if (e == &VadList)
            return 0;
        ULONG end = (CONTAINING_RECORD(e, MMVAD, Links)->EndVpn + 1) << PAGE_SHIFT;
        candidate = (end + ALLOCATION_GRANULARITY - 1) & ~(ALLOCATION_GRANULARITY - 1);
        if (candidate < end)
            return 0;
    }
}

static bool EnsureUserPageTable(ULONG va) {
    ULONG *pde = PdeAddress(va);
    if (*pde & PTE_PRESENT)
        return true;
    ULONG pfn = AllocatePage(MmVirtualPageTableUsage, pde); // zeroed: all PTEs not present
    if (!pfn)
        return false;
    *pde = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | 0x04 /*US*/ | PTE_ACCESSED | PTE_DIRTY;
    InvalidatePage((ULONG)PteAddress(va) & ~(PAGE_SIZE - 1));
    return true;
}

static void DecommitRange(ULONG start, ULONG end) { // [start, end] page-aligned VAs, inclusive end page
    for (ULONG va = start; va <= end; va += PAGE_SIZE) {
        ULONG *pde = PdeAddress(va);
        if (!(*pde & PTE_PRESENT)) {
            va = (va | 0x3FFFFF) & ~(PAGE_SIZE - 1); // skip the rest of this 4 MB
            continue;
        }
        ULONG *pte = PteAddress(va);
        if (!*pte)
            continue;
        ULONG pfn = *pte >> PAGE_SHIFT;
        *pte = 0;
        InvalidatePage(va);
        FreePage(pfn);
        AdjustPageTableUse(va, -1);
        if (PageTableUse(va) == 0) {
            ULONG ptPfn = *pde >> PAGE_SHIFT;
            *pde = 0;
            InvalidatePage((ULONG)PteAddress(va) & ~(PAGE_SIZE - 1));
            FreePage(ptPfn);
        }
    }
}

// --- Allocate, free, query, protect

static NTSTATUS AllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                      ULONG AllocationType, ULONG Protect, ULONG Usage);

XBAPI NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                             ULONG AllocationType, ULONG Protect) {
    ULONG base = (ULONG)*BaseAddress, size = *RegionSize;
    NTSTATUS status =
        AllocateVirtualMemory(BaseAddress, ZeroBits, RegionSize, AllocationType, Protect, MmVirtualMemoryUsage);
    if (!NT_SUCCESS(status)) {
        VmFailures = VmFailures + 1;
        VmLastFailure[0] = (ULONG)status;
        VmLastFailure[1] = base;
        VmLastFailure[2] = size;
        VmLastFailure[3] = AllocationType;
        VmLastFailure[4] = Protect;
    }
    return status;
}

// The XBE loader commits the title image with this (counted as image pages, as the reference).
NTSTATUS CommitImageMemory(PVOID *BaseAddress, PSIZE_T RegionSize) {
    return AllocateVirtualMemory(BaseAddress, 0, RegionSize, MEM_COMMIT, PAGE_READWRITE, MmImageUsage);
}

static NTSTATUS AllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                      ULONG AllocationType, ULONG Protect, ULONG Usage) {
    constexpr ULONG ALLOCATION_TYPES = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_NOZERO | 0x400000;
    if (ZeroBits > 21 || (AllocationType & ~ALLOCATION_TYPES) ||
        !(AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) ||
        ((AllocationType & MEM_RESET) && AllocationType != MEM_RESET))
        return STATUS_INVALID_PARAMETER;
    ULONG pteBits;
    if (!VmProtectToPte(Protect, &pteBits))
        return STATUS_INVALID_PAGE_PROTECTION;
    ULONG base = (ULONG)*BaseAddress, size = *RegionSize;
    if (base > MM_HIGHEST_USER_ADDRESS || size == 0 || size > MM_HIGHEST_USER_ADDRESS + 1 - base)
        return STATUS_INVALID_PARAMETER;

    XK_MARK("mark: mm: allocate VM, taking the lock");
    Lock();
    XK_MARK("mark: mm: allocate VM, lock taken");
    NTSTATUS status = STATUS_SUCCESS;
    MMVAD *newVad = nullptr;
    if (base == 0 || (AllocationType & MEM_RESERVE)) {
        ULONG highest = ZeroBits ? (0xFFFFFFFFu >> ZeroBits) : MM_HIGHEST_USER_ADDRESS;
        if (highest > MM_HIGHEST_USER_ADDRESS) {
            Unlock();
            return STATUS_INVALID_PARAMETER;
        }
        ULONG start, end;
        if (base == 0) {
            size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            start = FindFreeRange(size, highest, AllocationType & MEM_TOP_DOWN);
            if (!start) {
                Unlock();
                return STATUS_NO_MEMORY;
            }
            end = start + size - 1;
        } else {
            start = base & ~(ALLOCATION_GRANULARITY - 1);
            end = (base + size - 1) | (PAGE_SIZE - 1);
            if (!RangeFree(start >> PAGE_SHIFT, end >> PAGE_SHIFT)) {
                Unlock();
                return STATUS_CONFLICTING_ADDRESSES;
            }
        }
        newVad = (MMVAD *)ExAllocatePoolWithTag(sizeof(MMVAD), 'SdaV');
        XK_MARK("mark: mm: VAD allocated");
        if (!newVad) {
            Unlock();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        newVad->StartVpn = start >> PAGE_SHIFT;
        newVad->EndVpn = end >> PAGE_SHIFT;
        newVad->AllocationProtect = Protect;
        InsertVad(newVad);
        VirtualMemoryBytesReserved += end - start + 1;
        base = start;
        size = end - start + 1;
        if (!(AllocationType & MEM_COMMIT)) {
            *BaseAddress = (PVOID)base;
            *RegionSize = size;
            Unlock();
            return STATUS_SUCCESS;
        }
    }

    // Commit [base, base+size) inside one VAD.
    ULONG start = base & ~(PAGE_SIZE - 1), end = (base + size - 1) | (PAGE_SIZE - 1);
    MMVAD *vad = FindVad(start >> PAGE_SHIFT);
    if (!vad || (end >> PAGE_SHIFT) > vad->EndVpn) {
        status = STATUS_CONFLICTING_ADDRESSES;
    } else if (AllocationType == MEM_RESET) {
        status = STATUS_SUCCESS;
    } else {
        // Pages to allocate: every uncommitted page plus each page table that has to be created. The reference
        // fails only when that exceeds the free pages (no reserve kept back: Halo 2 commits down to the last few).
        ULONG needed = 0;
        for (ULONG va = start; va <= end; va += PAGE_SIZE) {
            ULONG *pde = PdeAddress(va);
            if (!(*pde & PTE_PRESENT)) {
                if (va == start || !(va & 0x3FFFFF))
                    needed++; // the page table
                needed++;
            } else if (!*PteAddress(va)) {
                needed++;
            }
        }
        XK_MARK("mark: mm: commit counted");
        if (needed > AvailablePages) {
            CommitFailures = CommitFailures + 1;
            CommitFailNeeded = needed;
            CommitFailAvailable = AvailablePages;
            status = STATUS_NO_MEMORY;
        } else {
            for (ULONG va = start; va <= end && NT_SUCCESS(status); va += PAGE_SIZE) {
                XK_MARK("mark: mm: commit, page table");
                if (!EnsureUserPageTable(va)) {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                ULONG *pte = PteAddress(va);
                if (*pte) {
                    if ((*pte & PTE_PRESENT) && ((*pte ^ pteBits) & (PTE_PCD | PTE_PWT)))
                        FlushCaches();                       // memory type changes: flush first
                    *pte = (*pte & ~0xFFFu & ~0u) | pteBits; // recommit: apply the new protection
                    InvalidatePage(va);
                    continue;
                }
                XK_MARK("mark: mm: commit, page");
                ULONG pfn = AllocatePage(Usage, pte, !(AllocationType & MEM_NOZERO));
                XK_MARK("mark: mm: commit, page allocated");
                if (!pfn) {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                if (pteBits & (PTE_PCD | PTE_PWT))
                    FlushCaches(); // the zeroing went through a cached alias: out of the caches before the new type
                *pte = (pfn << PAGE_SHIFT) | pteBits;
                InvalidatePage(va);
                AdjustPageTableUse(va, 1);
            }
            if (Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
                FlushCaches();
        }
        if (NT_SUCCESS(status)) {
            base = start;
            size = end - start + 1;
        }
    }
    if (!NT_SUCCESS(status) && newVad) {
        RemoveEntryList(&newVad->Links);
        VirtualMemoryBytesReserved -= (newVad->EndVpn - newVad->StartVpn + 1) << PAGE_SHIFT;
        ExFreePool(newVad);
    } else if (NT_SUCCESS(status)) {
        *BaseAddress = (PVOID)base;
        *RegionSize = size;
    }
    XK_MARK("mark: mm: allocate VM done");
    Unlock();
    return status;
}

XBAPI NTSTATUS NTAPI NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType) {
    if (FreeType != MEM_DECOMMIT && FreeType != MEM_RELEASE)
        return STATUS_INVALID_PARAMETER;
    ULONG base = (ULONG)*BaseAddress, size = *RegionSize;
    if (base >= 0x7FFF0000 || size > 0x7FFEFFFF - base)
        return STATUS_INVALID_PARAMETER;

    Lock();
    ULONG start = base & ~(PAGE_SIZE - 1), end = (base + size - 1) | (PAGE_SIZE - 1);
    MMVAD *vad = FindVad(start >> PAGE_SHIFT);
    NTSTATUS status = STATUS_SUCCESS;
    if (!vad) {
        status = STATUS_MEMORY_NOT_ALLOCATED;
    } else if (size && (end >> PAGE_SHIFT) > vad->EndVpn) {
        status = STATUS_UNABLE_TO_FREE_VM;
    } else {
        if (size == 0) {
            if (FreeType == MEM_RELEASE && (base >> PAGE_SHIFT) != vad->StartVpn) {
                Unlock();
                return STATUS_FREE_VM_NOT_AT_BASE;
            }
            start = vad->StartVpn << PAGE_SHIFT;
            end = (vad->EndVpn << PAGE_SHIFT) | (PAGE_SIZE - 1);
        }
        KIRQL old = KeRaiseIrqlToDpcLevel();
        DecommitRange(start, end & ~(PAGE_SIZE - 1));
        KfLowerIrql(old);
        if (FreeType == MEM_RELEASE) {
            ULONG s = start >> PAGE_SHIFT, e = end >> PAGE_SHIFT;
            VirtualMemoryBytesReserved -= (e - s + 1) << PAGE_SHIFT;
            if (s == vad->StartVpn && e == vad->EndVpn) {
                RemoveEntryList(&vad->Links);
                ExFreePool(vad);
            } else if (s == vad->StartVpn) {
                vad->StartVpn = e + 1;
            } else if (e == vad->EndVpn) {
                vad->EndVpn = s - 1;
            } else { // split
                auto tail = (MMVAD *)ExAllocatePoolWithTag(sizeof(MMVAD), 'SdaV');
                if (!tail) {
                    Unlock();
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                *tail = *vad;
                tail->StartVpn = e + 1;
                vad->EndVpn = s - 1;
                InsertVad(tail);
            }
        }
        *BaseAddress = (PVOID)start;
        *RegionSize = end - start + 1;
    }
    Unlock();
    return status;
}

static ULONG QueryPte(ULONG va) {
    ULONG *pde = PdeAddress(va);
    return (*pde & PTE_PRESENT) ? *PteAddress(va) : 0;
}

XBAPI NTSTATUS NTAPI NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Info) {
    ULONG va = (ULONG)BaseAddress & ~(PAGE_SIZE - 1);
    if (va > MM_HIGHEST_USER_ADDRESS)
        return STATUS_INVALID_PARAMETER;
    Lock();
    MMVAD *vad = FindVad(va >> PAGE_SHIFT);
    if (!vad) { // free region up to the next VAD
        ULONG next = MM_HIGHEST_USER_ADDRESS + 1;
        for (PLIST_ENTRY e = VadList.Flink; e != &VadList; e = e->Flink) {
            ULONG s = CONTAINING_RECORD(e, MMVAD, Links)->StartVpn << PAGE_SHIFT;
            if (s > va) {
                next = s;
                break;
            }
        }
        Info->BaseAddress = (PVOID)va;
        Info->AllocationBase = nullptr;
        Info->AllocationProtect = 0;
        Info->RegionSize = next - va;
        Info->State = MEM_FREE;
        Info->Protect = PAGE_NOACCESS;
        Info->Type = 0;
    } else { // run of pages with the same commit state and protection
        ULONG first = QueryPte(va);
        ULONG end = va;
        while ((end >> PAGE_SHIFT) < vad->EndVpn) {
            ULONG next = QueryPte(end + PAGE_SIZE);
            if ((next != 0) != (first != 0) || (first && PteToVmProtect(next) != PteToVmProtect(first)))
                break;
            end += PAGE_SIZE;
        }
        Info->BaseAddress = (PVOID)va;
        Info->AllocationBase = (PVOID)(vad->StartVpn << PAGE_SHIFT);
        Info->AllocationProtect = vad->AllocationProtect;
        Info->RegionSize = end + PAGE_SIZE - va;
        Info->State = first ? MEM_COMMIT : MEM_RESERVE;
        Info->Protect = first ? PteToVmProtect(first) : 0;
        Info->Type = MEM_PRIVATE;
    }
    Unlock();
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect,
                                            PULONG OldProtect) {
    ULONG pteBits;
    if (!VmProtectToPte(NewProtect, &pteBits))
        return STATUS_INVALID_PAGE_PROTECTION;
    ULONG start = (ULONG)*BaseAddress & ~(PAGE_SIZE - 1);
    ULONG end = ((ULONG)*BaseAddress + *RegionSize - 1) | (PAGE_SIZE - 1);
    Lock();
    MMVAD *vad = FindVad(start >> PAGE_SHIFT);
    NTSTATUS status = STATUS_SUCCESS;
    if (!vad || (end >> PAGE_SHIFT) > vad->EndVpn) {
        status = STATUS_CONFLICTING_ADDRESSES;
    } else {
        for (ULONG va = start; va <= end; va += PAGE_SIZE) {
            if (!QueryPte(va)) {
                status = (NTSTATUS)0xC000002D; // STATUS_NOT_COMMITTED
                break;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        *OldProtect = PteToVmProtect(QueryPte(start));
        KIRQL old = KeRaiseIrqlToDpcLevel();
        bool typeChange = (QueryPte(start) ^ pteBits) & (PTE_PCD | PTE_PWT);
        if (typeChange || (NewProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)))
            FlushCaches(); // before the new memory type (see MmSetAddressProtect)
        for (ULONG va = start; va <= end; va += PAGE_SIZE) {
            ULONG *pte = PteAddress(va);
            *pte = (*pte & ~0xFFFu) | pteBits;
            InvalidatePage(va);
        }
        if (NewProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
            FlushCaches();
        KfLowerIrql(old);
        *BaseAddress = (PVOID)start;
        *RegionSize = end - start + 1;
    }
    Unlock();
    return status;
}

// --- Statistics and the exported pointer table

XBAPI NTSTATUS NTAPI MmQueryStatistics(PMM_STATISTICS Statistics) {
    if (Statistics->Length != sizeof(MM_STATISTICS))
        return STATUS_INVALID_PARAMETER;
    Statistics->TotalPhysicalPages = NumberOfPhysicalPages;
    Statistics->AvailablePages = AvailablePages;
    Statistics->VirtualMemoryBytesCommitted =
        (AllocatedPagesByUsage[MmVirtualMemoryUsage] + AllocatedPagesByUsage[MmImageUsage]) << PAGE_SHIFT;
    Statistics->VirtualMemoryBytesReserved = VirtualMemoryBytesReserved;
    Statistics->CachePagesCommitted = AllocatedPagesByUsage[MmFsCacheUsage];
    Statistics->PoolPagesCommitted = AllocatedPagesByUsage[MmPoolUsage];
    Statistics->StackPagesCommitted = AllocatedPagesByUsage[MmStackUsage];
    Statistics->ImagePagesCommitted = AllocatedPagesByUsage[MmImageUsage];
    return STATUS_SUCCESS;
}

// Exported pointer table (ordinal 102), read by debug tools (spec/stock/09 §9). The first five entries point at
// structures in the reference's formats; our VADs are a list, not the reference's address-node tree, so the three VAD
// entries point at an empty tree.
struct MM_GLOBAL_DATA {
    PVOID RetailPfnRegion, SystemPteRange, AvailablePages, AllocatedPagesByUsage, AddressSpaceLock,
        VadRoot, VadHint, VadFreeHint;
};
static PVOID NoVadTree;
extern "C" MM_GLOBAL_DATA MmGlobalData;
XBAPI MM_GLOBAL_DATA MmGlobalData = {&RetailPfnRegion, &SystemPteRange, &AvailablePages, AllocatedPagesByUsage,
                               &AddressSpaceLock, &NoVadTree, &NoVadTree, &NoVadTree};

} // namespace xk::memory
