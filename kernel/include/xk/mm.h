// Memory manager (spec/stock/09). 64 MB retail layout:
//   physical 0x00000-0x0FFFF reserved (page directory at 0xF000), kernel image from 0x10000,
//   free RAM up to 0x3FDFFFF, GPU instance memory 0x3FE0000-0x3FEFFFF (uncached),
//   page tables for the 0x80000000 mapping at 0x3FF0000-0x3FFFFFF.
// Virtual: 0x80000000 + PA maps all RAM with 4 KB PTEs (PTE array at 0x83FF0000); contiguous memory
// is returned at 0x80000000|PA. System memory, kernel stacks and I/O mappings use 0xD0000000-0xEFFFFFFF.
#pragma once
#include <xk/types.h>

constexpr ULONG PAGE_SIZE = 0x1000, PAGE_SHIFT = 12;
constexpr ULONG MM_PAGES = 0x4000;                 // 64 MB
constexpr ULONG MM_GPU_INSTANCE_FIRST_PFN = 0x3FE0, MM_PAGE_TABLES_FIRST_PFN = 0x3FF0;
#ifdef XK_EXPERIMENT_GPU_LAST_MB
// Experiment: nothing allocatable in the last MB of RAM (the NV2A's PRAMIN window), in case the GPU uses more of it
// than the claimed instance memory.
constexpr ULONG MM_HIGHEST_CONTIGUOUS_PFN = 0x3EFF;
#else
constexpr ULONG MM_HIGHEST_CONTIGUOUS_PFN = 0x3FDF;
#endif
constexpr ULONG MM_SYSTEM_PHYSICAL_MAP = 0x80000000;
constexpr ULONG MM_SYSTEM_PTE_BASE = 0xD0000000, MM_SYSTEM_PTE_END = 0xF0000000;

// Win32 protection flags accepted by the Mm APIs (others fail, as in the reference).
constexpr ULONG PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_NOCACHE = 0x200, PAGE_WRITECOMBINE = 0x400;

// PTE bits. Bit 9 marks the last page of an allocation; bit 10 marks persistent pages.
constexpr ULONG PTE_PRESENT = 0x01, PTE_WRITE = 0x02, PTE_PWT = 0x08, PTE_PCD = 0x10, PTE_ACCESSED = 0x20,
                PTE_DIRTY = 0x40, PTE_LARGE = 0x80, PTE_END_OF_ALLOCATION = 0x200, PTE_PERSIST = 0x400;

static inline ULONG *MiGetPteAddress(ULONG va) { return (ULONG *)(0xC0000000 + ((va >> 12) << 2)); }
static inline ULONG *MiGetPdeAddress(ULONG va) { return (ULONG *)(0xC0300000 + ((va >> 22) << 2)); }
static inline void MiInvalidatePage(ULONG va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }
static inline void MiFlushCaches() { __asm__ volatile("wbinvd" ::: "memory"); }

void MmInitSystem(bool QuickReboot = false);
void MiInitializeVirtualMemory();
bool MiProtectToPte(ULONG protect, ULONG *pteBits);

// Physical pages (spec/stock/09 §7). The PTE array at 0x83FF0000 is also the page database, in the reference's format:
// a present entry maps the page at 0x80000000|PA (kernel image, contiguous memory); a free page holds its colour
// list links; any other page in use holds a busy entry with its usage and lock count.
enum MMPFN_USAGE : ULONG {
    MmUnknownUsage, MmStackUsage, MmVirtualPageTableUsage, MmSystemPageTableUsage, MmPoolUsage,
    MmVirtualMemoryUsage, MmSystemMemoryUsage, MmImageUsage, MmFsCacheUsage, MmContiguousUsage, MmDebuggerUsage,
    MmMaximumUsage
};
extern ULONG MmAllocatedPagesByUsage[MmMaximumUsage];
extern ULONG MmAvailablePages, MmNumberOfPhysicalPages;
// A page for `usage`, mapped by *ownerPte (or, for a user page table, by a user PDE): zeroed unless asked not to.
// The caller writes the mapping. Returns the PFN, or 0 when no page is free.
ULONG MiAllocatePage(ULONG usage, ULONG *ownerPte, bool zero = true);
void MiFreePage(ULONG pfn);
// Contiguous run inside [lowPfn, highPfn]: the highest one that is free or can be freed by moving pages that are in
// use but not locked. The pages are mapped at 0x80000000|PA with pteBits, the last one marked; returns the first PFN.
bool MiAllocateContiguousPages(ULONG count, ULONG lowPfn, ULONG highPfn, ULONG alignPages, ULONG pteBits,
                               ULONG *firstPfn);
void MiFreeContiguousPage(ULONG pfn);

// System PTE range 0xD0000000-0xEFFFFFFF
PVOID MiReserveSystemPtes(ULONG count);
void MiReleaseSystemPtes(PVOID va, ULONG count);

XBAPI PVOID NTAPI MmAllocateContiguousMemory(ULONG NumberOfBytes);
XBAPI PVOID NTAPI MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG LowestAcceptableAddress,
                                               ULONG HighestAcceptableAddress, ULONG Alignment, ULONG Protect);
XBAPI void NTAPI MmFreeContiguousMemory(PVOID BaseAddress);
XBAPI void NTAPI MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist);
XBAPI ULONG NTAPI MmQueryAllocationSize(PVOID BaseAddress);
XBAPI PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress(PVOID BaseAddress);
XBAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress);
XBAPI PVOID NTAPI MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect);
XBAPI ULONG NTAPI MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes);
XBAPI PVOID NTAPI MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread);
XBAPI void NTAPI MmDeleteKernelStack(PVOID KernelStackBase, PVOID KernelStackLimit);
XBAPI PVOID NTAPI MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, ULONG NumberOfBytes, ULONG Protect);
XBAPI void NTAPI MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes);
XBAPI void NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect);
XBAPI ULONG NTAPI MmQueryAddressProtect(PVOID VirtualAddress);
XBAPI void NTAPI MmLockUnlockPhysicalPage(PHYSICAL_ADDRESS PhysicalAddress, BOOLEAN UnlockPage);
XBAPI void NTAPI MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages);
XBAPI PVOID NTAPI MmClaimGpuInstanceMemory(ULONG NumberOfBytes, ULONG *NumberOfPaddingBytes);

typedef enum _POOL_TYPE { NonPagedPool = 0, PagedPool = 1 } POOL_TYPE;
XBAPI PVOID NTAPI ExAllocatePool(SIZE_T NumberOfBytes);
XBAPI PVOID NTAPI ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag);
XBAPI void NTAPI ExFreePool(PVOID P);
XBAPI ULONG NTAPI ExQueryPoolBlockSize(PVOID PoolBlock);
void ExInitializePool();

// --- Title virtual memory (spec/stock/09 §6)
constexpr ULONG MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000,
                MEM_FREE = 0x10000, MEM_PRIVATE = 0x20000, MEM_RESET = 0x80000, MEM_TOP_DOWN = 0x100000,
                MEM_NOZERO = 0x800000;
constexpr ULONG PAGE_NOACCESS = 0x01, PAGE_WRITECOPY = 0x08, PAGE_EXECUTE = 0x10, PAGE_EXECUTE_READ = 0x20,
                PAGE_EXECUTE_READWRITE = 0x40, PAGE_EXECUTE_WRITECOPY = 0x80, PAGE_GUARD = 0x100;
constexpr ULONG MM_LOWEST_USER_ADDRESS = 0x00010000, MM_HIGHEST_USER_ADDRESS = 0x7FFDFFFF;
constexpr ULONG ALLOCATION_GRANULARITY = 0x10000;
constexpr ULONG PTE_COMMITTED_NOACCESS = 0x800; // software bit: committed page with PAGE_NOACCESS

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    ULONG AllocationProtect;
    ULONG RegionSize;
    ULONG State;
    ULONG Protect;
    ULONG Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _MM_STATISTICS {
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
} MM_STATISTICS, *PMM_STATISTICS;

XBAPI NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                             ULONG AllocationType, ULONG Protect);
XBAPI NTSTATUS NTAPI NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
XBAPI NTSTATUS NTAPI NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Info);
XBAPI NTSTATUS NTAPI NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect);
XBAPI NTSTATUS NTAPI MmQueryStatistics(PMM_STATISTICS Statistics);
