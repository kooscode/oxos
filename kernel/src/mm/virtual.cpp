// Title virtual memory: Nt{Allocate,Free,Query,Protect}VirtualMemory and MmQueryStatistics (spec/stock/09 §6).
// One address space (0x00010000-0x7FFDFFFF); reservations are VADs kept in an address-sorted list.
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

constexpr NTSTATUS STATUS_INVALID_PARAMETER = (NTSTATUS)0xC000000D, STATUS_NO_MEMORY = (NTSTATUS)0xC0000017,
                   STATUS_CONFLICTING_ADDRESSES = (NTSTATUS)0xC0000018, STATUS_UNABLE_TO_FREE_VM = (NTSTATUS)0xC000001A,
                   STATUS_INVALID_PAGE_PROTECTION = (NTSTATUS)0xC0000045,
                   STATUS_INSUFFICIENT_RESOURCES = (NTSTATUS)0xC000009A,
                   STATUS_FREE_VM_NOT_AT_BASE = (NTSTATUS)0xC000009F, STATUS_MEMORY_NOT_ALLOCATED = (NTSTATUS)0xC00000A0;

struct MMVAD {
    ULONG StartVpn, EndVpn; // inclusive page numbers
    ULONG AllocationProtect;
    LIST_ENTRY Links;
};

static LIST_ENTRY MiVadList;
struct RTL_CRITICAL_SECTION_ {
    UCHAR Event[16];
    LONG LockCount, RecursionCount;
    PVOID OwningThread;
};
XBAPI void NTAPI RtlInitializeCriticalSection(RTL_CRITICAL_SECTION_ *CriticalSection);
XBAPI void NTAPI RtlEnterCriticalSection(RTL_CRITICAL_SECTION_ *CriticalSection);
XBAPI void NTAPI RtlLeaveCriticalSection(RTL_CRITICAL_SECTION_ *CriticalSection);
static RTL_CRITICAL_SECTION_ MiAddressSpaceLock; // the reference's type (MmGlobalData->AddressSpaceLock)
// Committed PTEs of a title page table, kept in bits 17-27 of the page table's page database entry (unused for that
// usage): no resident table for it.
static ULONG &MiPageTableEntry(ULONG va) { return ((ULONG *)0x83FF0000)[*MiGetPdeAddress(va) >> PAGE_SHIFT]; }
static ULONG MiPageTableUse(ULONG va) { return (MiPageTableEntry(va) >> 17) & 0x7FF; }
static void MiAdjustPageTableUse(ULONG va, LONG delta) { MiPageTableEntry(va) += (ULONG)delta << 17; }
ULONG MmVirtualMemoryBytesReserved;
volatile ULONG MiCommitFailures, MiCommitFailNeeded, MiCommitFailAvailable; // diagnostics (tests, title bring-up)

static void Lock() { RtlEnterCriticalSection(&MiAddressSpaceLock); }
static void Unlock() { RtlLeaveCriticalSection(&MiAddressSpaceLock); }

void MiInitializeVirtualMemory() {
    InitializeListHead(&MiVadList);
    RtlInitializeCriticalSection(&MiAddressSpaceLock);
}

// Protection for user pages. One base protection; GUARD is accepted but not enforced.
static bool MiVmProtectToPte(ULONG protect, ULONG *bits) {
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

static ULONG MiPteToVmProtect(ULONG pte) {
    if (pte & PTE_COMMITTED_NOACCESS)
        return PAGE_NOACCESS;
    ULONG p = (pte & PTE_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    if (pte & PTE_PCD)
        p |= PAGE_NOCACHE;
    else if (pte & PTE_PWT)
        p |= PAGE_WRITECOMBINE;
    return p;
}

static MMVAD *MiFindVad(ULONG vpn) {
    for (PLIST_ENTRY e = MiVadList.Flink; e != &MiVadList; e = e->Flink) {
        auto v = CONTAINING_RECORD(e, MMVAD, Links);
        if (vpn < v->StartVpn)
            return nullptr;
        if (vpn <= v->EndVpn)
            return v;
    }
    return nullptr;
}

static bool MiRangeFree(ULONG startVpn, ULONG endVpn) {
    for (PLIST_ENTRY e = MiVadList.Flink; e != &MiVadList; e = e->Flink) {
        auto v = CONTAINING_RECORD(e, MMVAD, Links);
        if (v->StartVpn <= endVpn && startVpn <= v->EndVpn)
            return false;
    }
    return true;
}

static void MiInsertVad(MMVAD *vad) {
    PLIST_ENTRY e = MiVadList.Flink;
    while (e != &MiVadList && CONTAINING_RECORD(e, MMVAD, Links)->StartVpn < vad->StartVpn)
        e = e->Flink;
    InsertTailList(e, &vad->Links);
}

// Find a free, 64 KB-aligned range of `size` bytes below `highest` (bottom-up or top-down).
static ULONG MiFindFreeRange(ULONG size, ULONG highest, bool topDown) {
    if (topDown) {
        ULONG candidate = (highest + 1 - size) & ~(ALLOCATION_GRANULARITY - 1);
        while (candidate >= MM_LOWEST_USER_ADDRESS && candidate + size - 1 <= highest) {
            ULONG s = candidate >> PAGE_SHIFT, e = (candidate + size - 1) >> PAGE_SHIFT;
            if (MiRangeFree(s, e))
                return candidate;
            candidate -= ALLOCATION_GRANULARITY;
        }
        return 0;
    }
    ULONG candidate = MM_LOWEST_USER_ADDRESS;
    for (PLIST_ENTRY e = MiVadList.Flink;; e = e->Flink) {
        ULONG limit = e == &MiVadList ? highest + 1 : CONTAINING_RECORD(e, MMVAD, Links)->StartVpn << PAGE_SHIFT;
        if (candidate + size <= limit && candidate + size - 1 <= highest)
            return candidate;
        if (e == &MiVadList)
            return 0;
        ULONG end = (CONTAINING_RECORD(e, MMVAD, Links)->EndVpn + 1) << PAGE_SHIFT;
        candidate = (end + ALLOCATION_GRANULARITY - 1) & ~(ALLOCATION_GRANULARITY - 1);
        if (candidate < end)
            return 0;
    }
}

static bool MiEnsureUserPageTable(ULONG va) {
    ULONG *pde = MiGetPdeAddress(va);
    if (*pde & PTE_PRESENT)
        return true;
    ULONG pfn = MiAllocatePage(MmVirtualPageTableUsage, pde); // zeroed: all PTEs not present
    if (!pfn)
        return false;
    *pde = (pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | 0x04 /*US*/ | PTE_ACCESSED | PTE_DIRTY;
    MiInvalidatePage((ULONG)MiGetPteAddress(va) & ~(PAGE_SIZE - 1));
    return true;
}

static void MiDecommitRange(ULONG start, ULONG end) { // [start, end] page-aligned VAs, inclusive end page
    for (ULONG va = start; va <= end; va += PAGE_SIZE) {
        ULONG *pde = MiGetPdeAddress(va);
        if (!(*pde & PTE_PRESENT)) {
            va = (va | 0x3FFFFF) & ~(PAGE_SIZE - 1); // skip the rest of this 4 MB
            continue;
        }
        ULONG *pte = MiGetPteAddress(va);
        if (!*pte)
            continue;
        ULONG pfn = *pte >> PAGE_SHIFT;
        *pte = 0;
        MiInvalidatePage(va);
        MiFreePage(pfn);
        MiAdjustPageTableUse(va, -1);
        if (MiPageTableUse(va) == 0) {
            ULONG ptPfn = *pde >> PAGE_SHIFT;
            *pde = 0;
            MiInvalidatePage((ULONG)MiGetPteAddress(va) & ~(PAGE_SIZE - 1));
            MiFreePage(ptPfn);
        }
    }
}

static NTSTATUS MiAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                        ULONG AllocationType, ULONG Protect, ULONG Usage);

volatile ULONG MiVmFailures, MiVmLastFailure[5]; // diagnostics: status, base, size, type, protect of the last one

XBAPI NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                             ULONG AllocationType, ULONG Protect) {
    ULONG base = (ULONG)*BaseAddress, size = *RegionSize;
    NTSTATUS status =
        MiAllocateVirtualMemory(BaseAddress, ZeroBits, RegionSize, AllocationType, Protect, MmVirtualMemoryUsage);
    if (!NT_SUCCESS(status)) {
        MiVmFailures = MiVmFailures + 1;
        MiVmLastFailure[0] = (ULONG)status;
        MiVmLastFailure[1] = base;
        MiVmLastFailure[2] = size;
        MiVmLastFailure[3] = AllocationType;
        MiVmLastFailure[4] = Protect;
    }
    return status;
}

// The XBE loader commits the title image with this (counted as image pages, as the reference).
NTSTATUS MiCommitImageMemory(PVOID *BaseAddress, PSIZE_T RegionSize) {
    return MiAllocateVirtualMemory(BaseAddress, 0, RegionSize, MEM_COMMIT, PAGE_READWRITE, MmImageUsage);
}

static NTSTATUS MiAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                        ULONG AllocationType, ULONG Protect, ULONG Usage) {
    if (ZeroBits > 21 || (AllocationType & ~(MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_NOZERO | 0x400000)) ||
        !(AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) ||
        ((AllocationType & MEM_RESET) && AllocationType != MEM_RESET))
        return STATUS_INVALID_PARAMETER;
    ULONG pteBits;
    if (!MiVmProtectToPte(Protect, &pteBits))
        return STATUS_INVALID_PAGE_PROTECTION;
    ULONG base = (ULONG)*BaseAddress, size = *RegionSize;
    if (base > MM_HIGHEST_USER_ADDRESS || size == 0 || size > MM_HIGHEST_USER_ADDRESS + 1 - base)
        return STATUS_INVALID_PARAMETER;

    Lock();
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
            start = MiFindFreeRange(size, highest, AllocationType & MEM_TOP_DOWN);
            if (!start) {
                Unlock();
                return STATUS_NO_MEMORY;
            }
            end = start + size - 1;
        } else {
            start = base & ~(ALLOCATION_GRANULARITY - 1);
            end = (base + size - 1) | (PAGE_SIZE - 1);
            if (!MiRangeFree(start >> PAGE_SHIFT, end >> PAGE_SHIFT)) {
                Unlock();
                return STATUS_CONFLICTING_ADDRESSES;
            }
        }
        newVad = (MMVAD *)ExAllocatePoolWithTag(sizeof(MMVAD), 'SdaV');
        if (!newVad) {
            Unlock();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        newVad->StartVpn = start >> PAGE_SHIFT;
        newVad->EndVpn = end >> PAGE_SHIFT;
        newVad->AllocationProtect = Protect;
        MiInsertVad(newVad);
        MmVirtualMemoryBytesReserved += end - start + 1;
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
    MMVAD *vad = MiFindVad(start >> PAGE_SHIFT);
    if (!vad || (end >> PAGE_SHIFT) > vad->EndVpn) {
        status = STATUS_CONFLICTING_ADDRESSES;
    } else if (AllocationType == MEM_RESET) {
        status = STATUS_SUCCESS;
    } else {
        // Pages to allocate: every uncommitted page plus each page table that has to be created. The reference
        // fails only when that exceeds the free pages (no reserve kept back: Halo 2 commits down to the last few).
        ULONG needed = 0;
        for (ULONG va = start; va <= end; va += PAGE_SIZE) {
            ULONG *pde = MiGetPdeAddress(va);
            if (!(*pde & PTE_PRESENT)) {
                if (va == start || !(va & 0x3FFFFF))
                    needed++; // the page table
                needed++;
            } else if (!*MiGetPteAddress(va)) {
                needed++;
            }
        }
        if (needed > MmAvailablePages) {
            MiCommitFailures = MiCommitFailures + 1;
            MiCommitFailNeeded = needed;
            MiCommitFailAvailable = MmAvailablePages;
            status = STATUS_NO_MEMORY;
        } else {
            for (ULONG va = start; va <= end && NT_SUCCESS(status); va += PAGE_SIZE) {
                if (!MiEnsureUserPageTable(va)) {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                ULONG *pte = MiGetPteAddress(va);
                if (*pte) {
                    *pte = (*pte & ~0xFFFu & ~0u) | pteBits; // recommit: apply the new protection
                    MiInvalidatePage(va);
                    continue;
                }
                ULONG pfn = MiAllocatePage(Usage, pte, !(AllocationType & MEM_NOZERO));
                if (!pfn) {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                *pte = (pfn << PAGE_SHIFT) | pteBits;
                MiInvalidatePage(va);
                MiAdjustPageTableUse(va, 1);
            }
            if (Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
                MiFlushCaches();
        }
        if (NT_SUCCESS(status)) {
            base = start;
            size = end - start + 1;
        }
    }
    if (!NT_SUCCESS(status) && newVad) {
        RemoveEntryList(&newVad->Links);
        MmVirtualMemoryBytesReserved -= (newVad->EndVpn - newVad->StartVpn + 1) << PAGE_SHIFT;
        ExFreePool(newVad);
    } else if (NT_SUCCESS(status)) {
        *BaseAddress = (PVOID)base;
        *RegionSize = size;
    }
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
    MMVAD *vad = MiFindVad(start >> PAGE_SHIFT);
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
        MiDecommitRange(start, end & ~(PAGE_SIZE - 1));
        KfLowerIrql(old);
        if (FreeType == MEM_RELEASE) {
            ULONG s = start >> PAGE_SHIFT, e = end >> PAGE_SHIFT;
            MmVirtualMemoryBytesReserved -= (e - s + 1) << PAGE_SHIFT;
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
                MiInsertVad(tail);
            }
        }
        *BaseAddress = (PVOID)start;
        *RegionSize = end - start + 1;
    }
    Unlock();
    return status;
}

static ULONG MiQueryPte(ULONG va) {
    ULONG *pde = MiGetPdeAddress(va);
    return (*pde & PTE_PRESENT) ? *MiGetPteAddress(va) : 0;
}

XBAPI NTSTATUS NTAPI NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Info) {
    ULONG va = (ULONG)BaseAddress & ~(PAGE_SIZE - 1);
    if (va > MM_HIGHEST_USER_ADDRESS)
        return STATUS_INVALID_PARAMETER;
    Lock();
    MMVAD *vad = MiFindVad(va >> PAGE_SHIFT);
    if (!vad) { // free region up to the next VAD
        ULONG next = MM_HIGHEST_USER_ADDRESS + 1;
        for (PLIST_ENTRY e = MiVadList.Flink; e != &MiVadList; e = e->Flink) {
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
        ULONG first = MiQueryPte(va);
        ULONG end = va;
        while ((end >> PAGE_SHIFT) < vad->EndVpn) {
            ULONG next = MiQueryPte(end + PAGE_SIZE);
            if ((next != 0) != (first != 0) || (first && MiPteToVmProtect(next) != MiPteToVmProtect(first)))
                break;
            end += PAGE_SIZE;
        }
        Info->BaseAddress = (PVOID)va;
        Info->AllocationBase = (PVOID)(vad->StartVpn << PAGE_SHIFT);
        Info->AllocationProtect = vad->AllocationProtect;
        Info->RegionSize = end + PAGE_SIZE - va;
        Info->State = first ? MEM_COMMIT : MEM_RESERVE;
        Info->Protect = first ? MiPteToVmProtect(first) : 0;
        Info->Type = MEM_PRIVATE;
    }
    Unlock();
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect,
                                            PULONG OldProtect) {
    ULONG pteBits;
    if (!MiVmProtectToPte(NewProtect, &pteBits))
        return STATUS_INVALID_PAGE_PROTECTION;
    ULONG start = (ULONG)*BaseAddress & ~(PAGE_SIZE - 1);
    ULONG end = ((ULONG)*BaseAddress + *RegionSize - 1) | (PAGE_SIZE - 1);
    Lock();
    MMVAD *vad = MiFindVad(start >> PAGE_SHIFT);
    NTSTATUS status = STATUS_SUCCESS;
    if (!vad || (end >> PAGE_SHIFT) > vad->EndVpn) {
        status = STATUS_CONFLICTING_ADDRESSES;
    } else {
        for (ULONG va = start; va <= end; va += PAGE_SIZE)
            if (!MiQueryPte(va)) {
                status = (NTSTATUS)0xC000002D; // STATUS_NOT_COMMITTED
                break;
            }
    }
    if (NT_SUCCESS(status)) {
        *OldProtect = MiPteToVmProtect(MiQueryPte(start));
        KIRQL old = KeRaiseIrqlToDpcLevel();
        for (ULONG va = start; va <= end; va += PAGE_SIZE) {
            ULONG *pte = MiGetPteAddress(va);
            *pte = (*pte & ~0xFFFu) | pteBits;
            MiInvalidatePage(va);
        }
        if (NewProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE))
            MiFlushCaches();
        KfLowerIrql(old);
        *BaseAddress = (PVOID)start;
        *RegionSize = end - start + 1;
    }
    Unlock();
    return status;
}

XBAPI NTSTATUS NTAPI MmQueryStatistics(PMM_STATISTICS Statistics) {
    if (Statistics->Length != sizeof(MM_STATISTICS))
        return STATUS_INVALID_PARAMETER;
    Statistics->TotalPhysicalPages = MmNumberOfPhysicalPages;
    Statistics->AvailablePages = MmAvailablePages;
    Statistics->VirtualMemoryBytesCommitted =
        (MmAllocatedPagesByUsage[MmVirtualMemoryUsage] + MmAllocatedPagesByUsage[MmImageUsage]) << PAGE_SHIFT;
    Statistics->VirtualMemoryBytesReserved = MmVirtualMemoryBytesReserved;
    Statistics->CachePagesCommitted = MmAllocatedPagesByUsage[MmFsCacheUsage];
    Statistics->PoolPagesCommitted = MmAllocatedPagesByUsage[MmPoolUsage];
    Statistics->StackPagesCommitted = MmAllocatedPagesByUsage[MmStackUsage];
    Statistics->ImagePagesCommitted = MmAllocatedPagesByUsage[MmImageUsage];
    return STATUS_SUCCESS;
}

// Exported pointer table (ordinal 102), read by debug tools (spec/stock/09 §9). The first five entries point at structures
// in the reference's formats; our VADs are a list, not the reference's address-node tree, so the three VAD entries
// point at an empty tree.
struct MMPFNREGION;
struct MMPTERANGE;
extern MMPFNREGION MmRetailPfnRegion;
extern MMPTERANGE MiSystemPteRange;
struct MM_GLOBAL_DATA {
    PVOID RetailPfnRegion, SystemPteRange, AvailablePages, AllocatedPagesByUsage, AddressSpaceLock,
        VadRoot, VadHint, VadFreeHint;
};
static PVOID MiNoVadTree;
extern "C" MM_GLOBAL_DATA MmGlobalData;
MM_GLOBAL_DATA MmGlobalData = {&MmRetailPfnRegion, &MiSystemPteRange, &MmAvailablePages, MmAllocatedPagesByUsage,
                               &MiAddressSpaceLock, &MiNoVadTree, &MiNoVadTree, &MiNoVadTree};
