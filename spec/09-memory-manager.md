# Spec 09 — Memory manager (xboxkrnl 1.0.4627.1, 64 MB retail)

Clean-room behavioural spec. Evidence: batch_decompile `ev_1928c381…` (MmInitSystem physical init,
MmAllocateContiguousMemoryEx, MmGetPhysicalAddress, MmCreateKernelStack), `ev_ef3f9b35…` (system-PTE allocator,
MmAllocateSystemMemory, MmMapIoSpace, MmFreeContiguousMemory, free-list helpers), `ev_dc8155bd…` (protect→PTE,
MmPersistContiguousMemory, MmQueryAllocationSize, MmLockUnlockPhysicalPage, MmClaimGpuInstanceMemory,
ExQueryPoolBlockSize), objdump of ExAllocatePoolWithTag, the system-PTE descriptor at 0x800360F0, MmGlobalData,
and a live dump of the 2BL page directory. Virtual memory: batch_decompile `ev_e049cd4d…` (NtAllocateVirtualMemory, NtFreeVirtualMemory).
**Status: implemented (2c parts 1 and 2).**

## 1. Hand-off from the 2BL
- Page directory at physical 0xF000 (CR3), self-mapped at PDE 0x300 (0xC0300000). **Only 4 MB large pages**
  (0xE3): 0–256 MB identity at VA 0 and at 0x80000000; NV2A (0xFD000000) uncached; flash 0xFFC00000.

## 2. Physical layout and the 0x80000000 mapping
| Physical | Use | PTE at 0x83FF0000[pfn] |
|---|---|---|
| 0x00000–0x0FFFF | reserved; page 0 and the page directory (0xF000) | page 0 = 0x663, page 0xF = 0xF663 |
| 0x10000–end of image | kernel | pa\|0x463; .text pages read-only (RW bit cleared) |
| image end – 0x3FDFFFF | free (free-list encoded in the not-present PTEs) | — |
| 0x3FE0000–0x3FEFFFF | GPU instance memory (64 KB initial claim) | pa\|0x7B (uncached) |
| 0x3FF0000–0x3FFFFFF | page tables for VA 0x80000000–0x83FFFFFF | pa\|0x463 |
- VA **0x80000000 + PA** maps all 64 MB with **4 KB PTEs**. The PTE array (16 K entries) is itself at VA 0x83FF0000.
- PDE changes: 0x200–0x20F point at those page tables; 0x210–0x23F cleared (no RAM above 64 MB in this kernel);
  0x000–0x1FF cleared (user space); 0x3C0–0x3DF = 4 MB write-through (0xF0000000–0xF7FFFFFF, 0xEB);
  0x3E0–0x3FD = 4 MB uncached (0xF8000000–0xFF7FFFFF, 0xFB). The reference clears PDE 0x3FF (flash).
  **Ours keeps it mapped**: quick reboot reads the .data copy from flash. To revisit with the quick-reboot pre-reset hook.
- PTE software bits: **bit 9 = last page of an allocation**, **bit 10 = persist across quick reboot**.

## 3. Protection → PTE (MiProtectToPte)
Only PAGE_READONLY (2), PAGE_READWRITE (4), PAGE_NOCACHE (0x200) and PAGE_WRITECOMBINE (0x400) are accepted. Anything
else makes the call fail (returns NULL / does nothing). RO → 0x61, RW → 0x63; NOCACHE → |0x10 (PCD);
WRITECOMBINE → |0x08 (PWT).

## 4. APIs
- `MmAllocateContiguousMemoryEx(size, low, high, align, protect)`:
  - high is capped at PFN 0x3FDF (below the GPU instance memory)
  - finds the **highest** run of free pages that starts on an `align` boundary
  - maps them with the protection bits and sets bit 9 on the last page
  - flushes caches for NOCACHE/WRITECOMBINE
  - returns **0x80000000 | PA**
  - the reference also has a protect flag 0x800 that adds 0x40000 to the page frame (a physical alias above
    1 GB); not implemented
- `MmAllocateContiguousMemory(size)` = Ex(size, 0, ~0, 0, PAGE_READWRITE).
- `MmFreeContiguousMemory(va)`: free pages up to and including the bit-9 page, with invlpg.
- `MmQueryAllocationSize(va)`: count PTEs up to bit 9 → bytes. `MmPersistContiguousMemory(va, size, b)`: set/clear bit 10.
- `MmGetPhysicalAddress(va)`: PDE (large → 4 MB frame) / PTE walk; 0 if not present. `MmIsAddressValid`: same walk.
- **System PTE range 0xD0000000–0xEFFFFFFF** (descriptor at 0x800360F0: PTEs 0xC0340000–0xC03C0000). Page tables
  are created on demand.
  - `MmAllocateSystemMemory(size, protect)`: one physical page per PTE, last PTE bit 9.
    `MmFreeSystemMemory(va, size)` returns the page count.
  - `MmCreateKernelStack(size, debugger)`: size + 1 guard PTE (not present) below; returns base+0x1000+size
    (the stack top). `MmDeleteKernelStack(base, limit)`.
  - `MmMapIoSpace(pa, size, protect)`:
    - protect == RW|NOCACHE with pa in [0xF8000000, 0xFFC00000) → return pa (already large-page mapped)
    - protect == RW|WRITECOMBINE with pa in [0xF0000000, 0xF8000000) → flush caches, return pa
    - otherwise map system PTEs and return va + (pa & 0xFFF)
    `MmUnmapIoSpace` only unmaps addresses inside the system range.
- `MmLockUnlockPhysicalPage(pa, unlock)` / `MmLockUnlockBufferPages`: per-page lock counts. Pages never move,
  so the counts are bookkeeping only.
- `MmClaimGpuInstanceMemory(size, *padding)`: *padding = 0x10000. Unless size == ~0, frees the pages between the old
  and new claim; the region grows down from 0x3FF0000. Returns **0x83FF0000** (0x84000000 − padding).

## 5. Executive pool
- `ExAllocatePoolWithTag(size, tag)`: **size > 0xFD8 → whole pages** from system memory (page-aligned pointer).
  Smaller requests use small blocks: **8-byte header** {PreviousSize, PoolIndex, PoolType, **BlockSize (32-byte units,
  header included)**, Tag}, so returned pointers are 8-byte aligned (≡ 8 mod 32).
- `ExQueryPoolBlockSize(p)`: page-aligned → MmQueryAllocationSize; else BlockSize × 32 − 8.
- `ExAllocatePool(size)` uses tag 'None'. `ExFreePool`: page-aligned → MmFreeSystemMemory, else return the block.

## 6. Title virtual memory (user range 0x00010000–0x7FFDFFFF)
- `NtAllocateVirtualMemory(*Base, ZeroBits, *Size, Type, Protect)`:
  - **Validation:** ZeroBits ≤ 21. Type ⊆ {COMMIT 0x1000, RESERVE 0x2000, RESET 0x80000, TOP_DOWN 0x100000, 0x400000,
    **NOZERO 0x800000**} and must include COMMIT, RESERVE or RESET; RESET is only allowed on its own.
    Base > 0x7FFDFFFF, size 0, or overflow → STATUS_INVALID_PARAMETER. A bad protection →
    STATUS_INVALID_PAGE_PROTECTION.
  - **Reserve** (base 0 or MEM_RESERVE):
    - with base 0, the size is rounded to pages and placed on a **64 KB** boundary, bottom-up from 0x10000 or
      top-down (MEM_TOP_DOWN) below 0x7FFDFFFF (or the ZeroBits limit)
    - with an explicit base, the base is rounded down to 64 KB; an overlap → STATUS_CONFLICTING_ADDRESSES (0xC0000018)
    - a VAD records the range (pool tag 'VadS'), and `VirtualMemoryBytesReserved` grows
  - **Commit:**
    - the range must lie inside one VAD, otherwise STATUS_CONFLICTING_ADDRESSES; MEM_RESET is a no-op success
    - too few free pages → STATUS_NO_MEMORY
    - page tables are created on demand (PDE flags 0x67)
    - new pages are zeroed unless MEM_NOZERO is given
    - already-committed pages take the new protection
    - caches are flushed for NOCACHE/WRITECOMBINE
    Returns the page-aligned base and size.
- `NtFreeVirtualMemory(*Base, *Size, Type)`:
  - **Validation:** Type must be exactly DECOMMIT (0x4000) or RELEASE (0x8000). No VAD → STATUS_MEMORY_NOT_ALLOCATED
    (0xC00000A0); range past the VAD end → STATUS_UNABLE_TO_FREE_VM (0xC000001A).
  - **Size 0:** the whole VAD; RELEASE must then be given the VAD base (STATUS_FREE_VM_NOT_AT_BASE 0xC000009F).
  - Freeing decommits pages, and a page table is released when its last PTE goes. RELEASE removes, trims or
    splits the VAD.
- `NtQueryVirtualMemory(base, MEMORY_BASIC_INFORMATION*)`:
  - inside a VAD: the run of pages with the same state and protection (Type MEM_PRIVATE, AllocationBase =
    VAD start)
  - outside: MEM_FREE up to the next VAD
- `NtProtectVirtualMemory(*Base, *Size, New, *Old)`: every page must be committed and inside one VAD. Returns the
  first page's old protection.
- `MmQueryStatistics(MM_STATISTICS*)`: Length must equal 0x24. Fields: total and available pages, committed and
  reserved VM bytes, pool and stack pages committed.
- `MmGlobalData` (ordinal 102): a table of pointers into Mm internals for debug tools.
- VM protections: NOACCESS (committed but not present), READONLY/EXECUTE/EXECUTE_READ → RO,
  READWRITE/WRITECOPY/EXECUTE_READWRITE/EXECUTE_WRITECOPY → RW, plus NOCACHE/WRITECOMBINE. GUARD is accepted but not
  enforced (no guard-page exception yet).

## Implementation notes (internal, ABI-neutral)
- Free physical pages are tracked in a bitmap, not the reference's per-colour free lists encoded in not-present PTEs.
  Single pages are allocated bottom-up and contiguous runs top-down.
- Small pool blocks use exact-size free lists without coalescing.
- VADs are kept in an address-sorted list (the reference uses a tree), protected by a mutant.
- Thread stacks are freed by the next thread to run after a thread terminates. Thread objects are kept until Ob
  reference counting exists (milestone 3).
