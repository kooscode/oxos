# Spec 09 — Memory manager (xboxkrnl 1.0.4627.1, 64 MB retail)

Clean-room behavioural spec. Evidence: batch_decompile `ev_1928c381…` (MmInitSystem physical init,
MmAllocateContiguousMemoryEx, MmGetPhysicalAddress, MmCreateKernelStack), `ev_ef3f9b35…` (system-PTE allocator,
MmAllocateSystemMemory, MmMapIoSpace, MmFreeContiguousMemory, free-list helpers), `ev_dc8155bd…` (protect→PTE,
MmPersistContiguousMemory, MmQueryAllocationSize, MmLockUnlockPhysicalPage, MmClaimGpuInstanceMemory,
ExQueryPoolBlockSize), objdump of ExAllocatePoolWithTag, the system-PTE descriptor at 0x800360F0, MmGlobalData,
and a live dump of the 2BL page directory. **Status: physical/system memory and pool implemented (2c part 1);
virtual memory (Nt*VirtualMemory, MmQueryStatistics, MmGlobalData) is part 2.**

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

## Implementation notes (internal, ABI-neutral)
- Free physical pages are tracked in a bitmap, not the reference's per-colour free lists encoded in not-present PTEs.
  Single pages are allocated bottom-up and contiguous runs top-down.
- Small pool blocks use exact-size free lists without coalescing.
- Thread stacks are freed by the next thread to run after a thread terminates. Thread objects are kept until Ob
  reference counting exists (milestone 3).
