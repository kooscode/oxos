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
- Pages 1–14 (below the page directory) are free in the reference: the LaunchDataPage is moved there before a
  title starts (spec 05 §5). Ours matches since milestone 5e.
- PTE software bits: **bit 9 = last page of an allocation**, **bit 10 = persist across quick reboot**.

## 3. Protection → PTE (MiProtectToPte)
Only PAGE_READONLY (2), PAGE_READWRITE (4), PAGE_NOCACHE (0x200) and PAGE_WRITECOMBINE (0x400) are accepted. Anything
else makes the call fail (returns NULL / does nothing). RO → 0x61, RW → 0x63; NOCACHE → |0x10 (PCD);
WRITECOMBINE → |0x08 (PWT).

## 4. APIs
- `MmAllocateContiguousMemoryEx(size, low, high, align, protect)`:
  - high is capped at PFN 0x3FDF (below the GPU instance memory)
  - finds the **highest** run that starts on an `align` boundary and whose pages are free or in use but unlocked;
    pages in use are moved out of the way (§8)
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
    - pages needed = uncommitted pages in the range + page tables to be created; more than AvailablePages →
      STATUS_NO_MEMORY (no reserve is kept: titles such as Halo 2 commit down to the last free pages)
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

## 7. Memory budget at the title's entry point (milestone 13)
Evidence: `ev_e3d73cd7…` (0x800557f4, physical init), `ev_c2735117…` (MmAllocateContiguousMemoryEx,
MmQueryStatistics, MmFreeContiguousMemory), `ev_18eb456e…` (free-list routines), objdump of the stack creation
sites (0x8001c73d, 0x80021cb6, 0x80055fe5). Measurements: xemu 0.8.136, retail BIOS vs ours, stopped by a
hardware breakpoint on the `call [0x10128]` that enters the title (stock 0x800158da), PTE array and Mm counters
read through the gdb stub (scratch tool `mmstat.py`).

**Reference behaviour.**
- Physical pages the kernel keeps for itself (observed, 0x800557f4): page 0 (0x663), the page directory (page
  0xF, 0xF663), the kernel image from 0x10000 (pa|0x463, .text read-only; **45 pages** once INIT is freed), the GPU
  instance memory 0x3FE0000–0x3FEFFFF (16 pages, pa|0x7B) and the 16 page tables at 0x3FF0000–0x3FFFFFF (pa|0x463)
  that map 0x80000000–0x83FFFFFF. Those 16 pages are the **only** page bookkeeping: the PTE array at 0x83FF0000 is
  the page database (§8). Everything else is counted as available. TotalPhysicalPages = 16384 (every page is counted
  once, as free or as contiguous).
- At the entry point (observed): 2 kernel stacks of 3 pages (the DPC stack, 0x3000 at 0x8001c73d; the phase-1 thread
  that calls the title, `PsCreateSystemThread` default 0x3000 at 0x80021cb6; the boot-animation thread's 0x4000
  stack, 0x80055fe5, is gone by then), 5 pool pages, 2 page tables, 16 file-cache pages (§16), the title image, and
  no boot framebuffer (spec/17 §7).

| Title (at entry) | Stock available | Ours before | Ours after |
|---|---|---|---|
| Dashboard (MS) | 15876 | 15560 | 15871 |
| Halo | 15347 | 15042 | 15356 |
| Halo 2 | 14880 | 14575 | 14892 |
| Half-Life 2 | 16166 | 15861 | 16178 |

Stock at Halo's entry, by usage: Stack 6, VirtualPageTable 1, SystemPageTable 1, Pool 5, VirtualMemory 1, Image 928,
Cache 16, Contiguous 79. During play (QMP snapshots every 10 s, scratch tool `snaplog.py`) the titles run close to
the limit: Halo 2 keeps 209–217 pages free, Halo ~273, Half-Life 2 drops to 69 (it loads sections on demand:
Image 109 → 2470 pages, VirtualMemory and Contiguous rise and fall); the dashboard sets the file cache to 256 pages.

**Ours.** Before milestone 13 the difference (−305 pages for every title) was: the boot splash kept as the persisted
display (301 pages), a 16-page stack for the title launcher thread (the reference runs it on 3 pages), our SMC
thread (3 pages, no reference counterpart), the system PTE slot bitmap (4 pages), against 16 cache pages and ~3
pool pages fewer. Now the splash is freed before the title starts (spec/17 §7) and the launcher stack is 0x3000 ("after" column;
the dashboard is still 5 pages short: our SMC thread and a lingering terminated-thread stack).

## 8. Page database and allocation order (milestone 13)
Evidence: `ev_e3d73cd7…` (0x800557f4 physical init), `ev_c2735117…` (0x8001dc6c MmAllocateContiguousMemoryEx,
0x8001e331 MmFreeContiguousMemory), `ev_18eb456e…` (0x8001f93b unlink, 0x8001f899 insert, 0x8001fb3c relocate,
0x8001e5bd system-PTE page allocation), objdump of 0x8001f9af (page picker), 0x8001fa21 / 0x8001fa52 (allocate /
free a page by usage), 0x8001fabd (zeroed page), 0x8001fcbc (copy and switch), 0x8001f91a (boot-time insert),
`ev_d465a17f…` (0x8001f745 / 0x8001f664 system PTE runs, 0x8001e076 MmLockUnlockPhysicalPage, 0x8001e6a1 free
mapped pages), `ev_bf671c80…` (0x8001dfe7 MmLockUnlockBufferPages), `ev_c2735117…` (0x8001e547 MmQueryStatistics).

**Reference behaviour.**
- **One entry per physical page**, the PTE at 0x83FF0000 + 4·PFN (observed). Three forms:
  - *present* (bit 0): the page's own mapping at 0x80000000|PA: kernel image, page 0, page directory, GPU memory, the
    page tables, contiguous allocations (bit 9 = last page, bit 10 = persist);
  - *free* (bit 0 and bit 16 clear): low word = next, high word = previous free page **of the same colour**, packed as
    `(PFN >> 5) * 2` (0xFFFE = none); PFN = packed·16 + colour. The packing keeps bits 0 and 16 clear;
  - *busy* (bit 0 clear, bit 16 set): low word = lock count in steps of 2 (MmLockUnlockPhysicalPage/BufferPages add or
    subtract 2), bits 18–27 = index of the PTE that maps the page within its page table (for a system page table: its
    PDE index; for the file cache: bits 17–27 = cache element index), bits 28–31 = usage: 0 unknown, 1 stack,
    2 title page table, 3 system page table, 4 pool, 5 VM, 6 system memory, 7 image, 8 file cache, 9 contiguous,
    10 debugger. Per-usage page counts at 0x8003AB40[11], AvailablePages at 0x8003AB6C (inferred names from the
    MmQueryStatistics mapping and the counters each routine updates).
- **Colours (observed):** 32 lists (colour = PFN & 31), heads at 0x80036108 {USHORT head, tail} ×32, region count at
  +0x80. At boot 0x8001f91a inserts pages 0–15 and image end–0x3FDF **ascending at the tails**, then unlinks page 0
  and 0xF; the INIT section is inserted later (tail), so it is used last.
- **Single pages (observed, 0x8001f9af):** the head of the next non-empty list, starting at the colour after the
  previous allocation (0x8003AB70); so consecutive pages rotate through the colours and come out **bottom-up**.
  Why (inferred): rotating colours spreads consecutive allocations over the CPU cache's sets. A page in use is
  freed to the **head** of its list (reused next); contiguous pages and the boot-time pages go to the **tail**.
- **Allocating a page (0x8001fa21 / 0x8001fabd):** pick a page, write the busy entry (usage, owner PTE index), bump
  the usage count; zeroed pages are cleared through a temporary present entry (pa|0x63) at 0x80000000|PA. The caller
  maps the page (system PTE range, title PTE or PDE). Pages in use are **not** reachable at 0x80000000|PA.
- **Contiguous requests (0x8001dc6c):** high capped at 0x3FDF; from the top, the highest aligned run whose pages are
  free or busy with lock count 0 (present pages and locked pages stop the run). Free pages of the run are unlinked
  first, then each busy page is **moved** (0x8001fb3c): its mapping is found from the usage (page tables through
  the PDEs, VM/image pages through the title page tables at the recorded index, cache pages through the cache
  element, everything else through the system PTE range), a replacement of the same colour is taken, and
  0x8001fcbc copies the page and rewrites the PTE with interrupts off (CR3 reload for page tables, else `invlpg`).
  Why (inferred): titles claim fixed physical ranges (Halo 0x61000–0x19A5FFF, Halo 2 0x61000–0x3144FFF) after the
  kernel has already put its own pages there; moving them lets single pages be handed out bottom-up.
- **System PTE range (0x800360F0, observed):** {Head, FirstCommittedPte 0xC0340000, LastCommittedPte,
  LastPte 0xC03C0000, &AvailablePages, page-table allocator}. Free runs live in their own not-present PTEs, sorted by
  address: the first PTE holds the next run's first PTE address (0xFFFFFFFC = end) and bit 1 for a one-PTE run; a
  longer run keeps its length << 2 in its second PTE. Reserve = first fit (split from the front); release zeroes the
  PTEs and merges with both neighbours; when nothing fits, whole page tables (usage 3, PDE 0x67) are added at
  LastCommittedPte (the request minus a free run ending there, rounded to 1024 PTEs) and never removed. Kernel stacks
  take one more PTE, left zero, as the guard page.
- **MmQueryStatistics (0x8001e547):** Total = pages counted at boot (16384), Available, VM committed =
  (VM + image pages)·4096, reserved bytes (0x8003AB78), cache, pool, stack and image pages from the usage counts.
- Stock contiguous request sequence for the D3D start-up of Halo 2 (QMP/gdb log): 0x2000→0x3FDE000, 0x6000→
  0x3FD8000, 4→0x3FD7000, 0x8000 (align 0x8000)→0x3FC8000, 0x4020 (align 0x4000)→0x3FD0000, … — each the highest
  aligned run, holes reused.

**Ours.** The same database format, colour lists, picker, free order, relocation and system PTE runs
(`src/mm/init.cpp`); usage counts feed MmQueryStatistics; page locks are kept as the reference does. Differences:
- Measured layout at Halo's entry: the kernel (45 pages), page 0, PD, GPU memory and page tables as stock; the title
  image starts at PFN 0x3D (stock 0xAC: its INIT section, 0x3D–0xAB, is still allocated while the title loads and
  is freed afterwards). Our extra stacks (SMC thread, a terminated thread's stack waiting to be freed) sit in pages
  1–14 like the reference's early pages.
- System page tables use PDE flags 0x63 (the reference 0x67, user bit set); not visible to titles.
- MmLockUnlockBufferPages counts only present PTEs (the reference also adjusts the page a not-present PTE's frame
  bits happen to name).
- The same contiguous call sequence gives the same relative placement as stock; absolute addresses differ by what
  was allocated before (our splash buffers vs the reference's boot animation).

## 9. MmGlobalData (ordinal 102) (milestone 13)
Evidence: the 13 dwords at 0x80010d04 read from the image, and the data they point to (initial values in .data).
**Reference behaviour (observed):** 8 pointers: RetailPfnRegion 0x80036108 (§8 colour heads + count),
SystemPteRange 0x800360F0 (§8), AvailablePages 0x8003AB6C, AllocatedPagesByUsage 0x8003AB40 (ULONG[11]),
AddressSpaceLock 0x800360D4 (an RTL_CRITICAL_SECTION: synchronization event {1, 0, 4, 0}, LockCount −1,
RecursionCount 0, OwningThread 0), VadRoot 0x8003AB84, VadHint 0x8003AB80, VadFreeHint 0x8003AB7C (pointers to
the VAD tree's address nodes). Who reads it (inferred): the debug monitor and memory-dump tools on development
kits; no retail title we traced imports it.
**Ours, and why:** the first five entries point at structures **in the reference's format** (our page database and
system PTE range are now built that way for allocation-order parity, and the address-space lock is an
RTL_CRITICAL_SECTION), so tools that walk them see the same data. Our VADs are an address-sorted list, not the
reference's address-node tree; rather than hand tools a list they would misread as a tree, the three VAD entries
point at a NULL root (an empty tree). Making the VADs a tree would only serve those tools, so it is left undone.

## Implementation notes (internal, ABI-neutral)
- The boot splash framebuffer is allocated below 16 MB and freed before the title starts (§7, spec/17 §7).
- Small pool blocks use exact-size free lists without coalescing.
- VADs are kept in an address-sorted list (the reference uses a tree), protected by a mutant.
- Thread stacks are freed by the next thread to run after a thread terminates. Thread objects are kept until Ob
  reference counting exists (milestone 3).
