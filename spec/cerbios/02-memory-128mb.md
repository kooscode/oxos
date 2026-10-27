# Cerbios spec 02: 128 MB memory

Studied in Cerbios 3.1.0 Beta (kernel memory manager, extension's title-launch code) and measured in xemu
with the memory test title (`tests/memory-test-title`, `make test-memory`). Behaviour and formats only.

## 1. RAM size
- At memory-manager start-up the kernel reads **one byte from PCI bus 0, device 0, function 0, register 0x87**
  (the host bridge; `HalReadWritePCISpace`). With that value v, the highest physical page is **(v << 12) | 0xFFF**,
  i.e. RAM = (v + 1) × 16 MB: v = 3 on a 64 MB console (highest page 0x3FFF), v = 7 on 128 MB (0x7FFF).
  (The stock 4627 kernel reads the same register: this resolves the open question in stock spec 02 §4,
  `../stock/02-executive-init.md`.)
- Everything below scales with that highest page; with 64 MB the behaviour is the retail one.

## 2. Mappings and page database
- RAM stays mapped at 0x80000000 + PA for **all** of it (0x80000000–0x87FFFFFF on 128 MB).
- The page database (one 32-bit entry per page, retail format, stock spec 09 §8) is at **VA 0x83FF0000** and grows
  with RAM: (highest page + 1) × 4 bytes (128 KB on 128 MB, so it reaches 0x84010000). At boot it is cleared for
  every page.
- The page tables for 0x84000000–0x87FFFFFF are the first pages above 64 MB (**PA 0x4000000–0x400FFFF**, measured).
- The GPU instance memory sits at the **top of RAM**: 0x7FE0000–0x7FEFFFF on 128 MB (VA 0x87FE0000), measured;
  the retail 64 MB position is 0x3FE0000.

## 3. Two page regions
- Free pages live in **regions**, each with the retail layout: 32 colour list heads (page colour = PFN & 31; lists
  linked through the page-database entries, 0xFFFE = end) followed by a free-page count.
  - region 0 ("retail", the one `MmGlobalData` points to): pages below 64 MB
  - region 1: pages from 64 MB up, stored **right after region 0** in the kernel's data
- A page's region is found from a small table indexed by **PFN >> 14** (one entry per 64 MB): [region 0, region 1,
  region 1].
- **Boot fill** (which pages start free): PFN 0x00–0x0F; from the end of the kernel image to PFN 0x3FE0 (below the
  64 MB GPU block, which stays reserved); and, **only when the highest page is above 0x4000**, from PFN 0x3FF0 + the
  number of pages the page database occupies up to the highest page.
- **Allocation order:** take from region 0 while it has free pages, then from region 1; within a region the colour
  rotates round-robin (as retail).

## 4. Who may use the upper 64 MB
- The kernel-wide **AvailablePages** counter (what `MmQueryStatistics` and every "is there memory" check use) counts
  **only region 0** at first: freeing or allocating a region-1 page does not change it. TotalPhysicalPages still
  reports all pages (32 768 on 128 MB).
- **At each title launch**, after the XBE header is loaded at 0x10000, the upper memory is **unlocked** when the
  XBE's init flags (header offset 0x124) **do not have bit 0x4 ("limit to 64 MB")**, or when the configuration's
  DISABLELIMITMEM is set: with the dispatcher locked, region 1's free count is added to AvailablePages and a flag
  makes region-1 pages count from then on. Every title starts with a fresh kernel boot, so the next title starts
  locked again.
- Effect (measured, §5): with the flag, a title gets 62 MB (available 15 904 pages); without it 125 MB (32 242),
  with ordinary title memory coming from both regions.
- **Contiguous memory stays below 64 MB** even when unlocked: a contiguous request limited to 0x4000000–0x7FFFFFF
  is refused (measured), consistent with the retail cap on contiguous allocations (stock spec 09 §8: highest PFN
  0x3FDF).
- Safe mode forces DISABLELIMITMEM off (spec 01 §4).

## 5. Measurements (xemu, 2026-10-18)
Cerbios 3.1.0 with the MCPX 1.0 boot ROM, `mem_limit` 128, the xemu-dashboard disk (built-in settings), observed
through QMP (`info mem`, `info tlb`, memory reads of MmGlobalData and the export table):
- at the dashboard: AvailablePages 29 020 + allocated 3 748 = 32 768 pages; region 0 had 12 682 free pages, region 1
  16 338 (its colour heads pointing at PFNs from 0x7FC0 down); three pointers after region 1 list the regions
  (region 0, region 1, region 1). The dashboard's XBE (init flags 0x5) had all 1 213 of its pages below 64 MB.
- besides the page tables at PA 0x4000000, one more page table sat at PA 0x4021000 (for VA 0xB0000000) and 13
  pages at PA 0x4042000 were mapped at VA 0xB0020000 (purpose not traced).

The memory test title (`tests/memory-test-title`) asks MmQueryStatistics, asks for one contiguous page in
0x4000000–0x7FFFFFF, then commits 1 MB blocks until refused:

| BIOS, memory | XBE init flags | TotalPhysicalPages | Available at start | Title got | Pages above 64 MB |
|---|---|---|---|---|---|
| stock 4627, 64 MB | 0x5 or 0x1 | 16 384 | 15 917 | 62 MB | 0 |
| OXOS, 64 or 128 MB | 0x5 or 0x1 | 16 384 | 15 919 | 62 MB | 0 |
| Cerbios, 128 MB | 0x5 (limit to 64 MB) | 32 768 | 15 904 | 62 MB | 0 |
| Cerbios, 128 MB | 0x1 | 32 768 | 32 242 | 125 MB | 16 111 (highest PA 0x7F2D000) |

The contiguous request above 64 MB was refused in both Cerbios runs.

## 6. For OXOS (implementation notes, not Cerbios behaviour)
- Read PCI 0:0 0x87 at boot; size the page database, mappings and boot fill from it.
- Keep region 0 exactly as today (so 64 MB titles see the same memory as on 64 MB), add region 1 for PFN ≥ 0x4000.
- Unlock at title launch from the XBE init flag (and an optional override setting).
- The trace-full build keeps its call record at PA 0x4000000 today: it must move when the kernel uses that memory.

## Open
- How the system-PTE and page-table allocators choose regions on 128 MB (two pointers set only on 64 MB: the
  "available pages" pointer and an allocator routine).
- Whether kernel-internal allocations (pool, stacks, file cache) ever take region-1 pages before a title unlocks it.
