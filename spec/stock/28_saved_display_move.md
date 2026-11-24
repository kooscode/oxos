# Spec 28 — Moving the LaunchDataPage and the saved display block before a title starts (xboxkrnl 1.0.4627.1; 5838 compared)

Clean-room behavioural spec. Sources: Ghidra 12.1.4 analysis (REA) of the shipped binaries:

| Binary | Version | sha256 |
|---|---|---|
| `xboxkrnl-4627.exe` (primary) | 1.0.4627.1 | 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f |
| `5838-unpacked/xboxkrnl.exe` | 1.0.5838.1 | 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde |

Addresses are 4627 unless marked. No code is reproduced.

What was searched: the title-entry helper 0x8001E9F5 and its only caller (the phase-1 system thread 0x8001576F); the move helper 0x8002FD1B; every
reference to the target-address variable 0x8003B2D0 (writer 0x8001E972; the reference from 0x800557BF is the GDT byte fix-up loop's end bound, not a use);
every reference to the saved-data variable 0x8003B350 (`AvGetSavedDataAddress` 0x8002FD09, `AvSetSavedDataAddress` 0x8002FD0F, the move helper); every
caller of `AvGetSavedDataAddress` / `AvSetSavedDataAddress` (all others are in INIT, the boot animation's graphics code); every caller of
`MmPersistContiguousMemory` (5838); the quick-reboot page re-adoption 0x8001FA6C; the store to PCRTC 0x600800 (5838: one in `.text`, 0x8002FFB6, inside the
move helper). The Mm exports used are identified by their export ordinals in the image.

| Name (ordinal) | 4627 | 5838 |
|---|---|---|
| title-entry helper (this spec) | 0x8001E9F5 | 0x8001EC34 |
| move-the-saved-display helper | 0x8002FD1B | 0x8002FF6F |
| INIT discard (sets the target address) | 0x8001E972 | 0x8001EB79 |
| phase-1 system thread (caller of both) | 0x8001576F | 0x800158FF |
| target physical address variable (STICKY) | 0x8003B2D0 | 0x8003C2E4 |
| saved-data pointer (STICKY) | 0x8003B350 | 0x8003C368 |
| `LaunchDataPage` (164, STICKY) | 0x8003B348 | 0x8003C360 |
| `AvGetSavedDataAddress` (1) / `AvSetSavedDataAddress` (4) | 0x8002FD09 / 0x8002FD0F | 0x8002FF5D / 0x8002FF63 |
| `MmGetPhysicalAddress` (173) | 0x8001DC1E | 0x8001DDD4 |
| `MmAllocateContiguousMemoryEx` (166) | 0x8001DC6C | 0x8001DE22 |
| `MmQueryAllocationSize` (180) | 0x8001DE2E | 0x8001DFE4 |
| `MmPersistContiguousMemory` (178) | 0x8001DE6B | 0x8001E021 |
| `MmFreeContiguousMemory` (171) | 0x8001E331 | 0x8001E4E7 |
| quick-reboot flag | 0x8003A87C | 0x8003B87C |

## 1. Where it runs
End of the phase-1 system thread (0x8001576F), after the title has been chosen and the LaunchDataPage prepared (spec 05 §2–§4):
1. **Cold boot only** (quick-reboot flag clear): 0x80CF pulse, boot-animation wait / shutdown and the other cold-boot steps of spec 05, then the INIT discard
   0x8001E972 (§2).
2. **Every boot** (cold and quick): the title-entry helper 0x8001E9F5: first the LaunchDataPage move (§3), then the saved display move (§4).
3. Then the XBE entry point (the dword at 0x00010128, the XBE header's entry point) is called. Nothing else touching physical placement lies between §4
   and the title's entry.

## 2. The target address: the start of the discarded INIT section (0x8001E972, cold boot only)
| # | Step |
|---|---|
| 1 | Read the kernel's own PE headers (image base 0x80010000); take the **last section** (INIT): its VirtualAddress and SizeOfRawData. |
| 2 | Target = `MmGetPhysicalAddress((0x80010000 + INIT.VirtualAddress + 0xFFF) & ~0xFFF)`: the first whole page of INIT, rounded **up**. Stored in 0x8003B2D0 (STICKY section, so it survives quick reboots; it is not recomputed on a quick reboot). |
| 3 | At raised IRQL, every page from that page up to (but not including) the page of `(0x80010FFF + INIT.VirtualAddress + INIT.SizeOfRawData)` is returned to the free lists; the in-use page count (0x8003AB64) drops by one per page. |
| 4 | TLB / cache flush helper (0x80018E38). |
| 5 | 5838 only: the GDT's code descriptor (selector 0x08, image at 0x8003C2A0) is rewritten with base 0, G = 1, D = 1, type 0x9B and a limit ending at the page that holds INIT's start (the page number of `0x80010000 + INIT.VirtualAddress`, i.e. 0x8003C): code in discarded INIT can no longer execute. 4627 has no such step. |

Values (identity map, kernel at physical 0x10000):
| | INIT VirtualAddress | SizeOfRawData | Target (0x8003B2D0) | Pages freed |
|---|---|---|---|---|
| 4627 | 0x2CB20 | 0x6E9E0 | **0x0003D000** | 0x3D–0xAA (0x3D000–0xAAFFF, 110 pages) |
| 5838 | 0x2C3C0 | 0x6C8E0 | **0x0003D000** | 0x3D–0xA8 (0x3D000–0xA8FFF, 108 pages) |

The partial page holding the start of INIT (0x3C000) stays with the resident image. spec 05 §5's "expected physical address (0x8003B2D0)" is the address
of this variable, not the address itself.

## 3. LaunchDataPage move (0x8001E9F5, first part)
Done whenever `LaunchDataPage` (0x8003B348) is non-null; there is **no test of where it currently is**.
| # | Step |
|---|---|
| 1 | Allocate `MmAllocateContiguousMemoryEx(0x1000, 0, MmGetPhysicalAddress(0x80010000) − 1, 0, 4)`: size one page, lowest 0, highest **0x0000FFFF** (one below the kernel image's physical address, 0x10000), alignment 0 (treated as one page), protect 4 = PAGE_READWRITE (cached). The allocator works top-down, so this is the highest free page below 64 KB. |
| 2 | Failure (null): nothing else; the old page stays as it is. |
| 3 | Success: copy the 4096 bytes from the old page; `MmPersistContiguousMemory(new, 0x1000, TRUE)`; `LaunchDataPage` ← new; `MmFreeContiguousMemory(old)` (the old page's persist bit is not cleared first). |

## 4. Saved display block move (0x8001E9F5, second part, and 0x8002FD1B)
### 4.1 The block
The saved display block is what `AvSetSavedDataAddress` was given: one contiguous allocation holding a 0x40-byte surface header followed by the pixels. The
boot animation makes it at its end (5838 0x8003ECB0 → 0x80040D30): `MmAllocateContiguousMemoryEx(frame size + 0x40, 0, 0x3FFB000, 0x40, 0x404)` (top-down,
so near the top of memory), header dword 0 = 0x81050001, **dword 1 (offset 4) = physical address of the pixels** (block + 0x40, masked with 0x3FFFFFF), dword
2 = 0, dword 3 = format, dword 4 = size word, dword 5 = 0; the last frame is copied in, the display is pointed at dword 1, then
`MmPersistContiguousMemory(block, MmQueryAllocationSize(block), TRUE)` and `AvSetSavedDataAddress(block)`. A title's graphics library keeps the same layout
(640 × 480 × 4 + 0x40 = 0x12C040 for a 32-bpp 640 × 480 frame). The kernel itself never reads anything of the header but dword 1.

### 4.2 Size
The kernel does not know the frame size: it uses `MmQueryAllocationSize(block)`, which counts the block's pages up to and including the one marked "last
page of an allocation" (PTE bit 9) and returns pages × 0x1000. For a 0x12C040-byte block: 0x12D pages = **0x12D000**.

### 4.3 Decision and placement (0x8001E9F5)
Let `block` = `AvGetSavedDataAddress()`, `phys` = `MmGetPhysicalAddress(block)`, `T` = the target (0x8003B2D0, 0x3D000), `size` = §4.2.
| # | Condition | Action |
|---|---|---|
| 1 | `block` = 0 | nothing (no saved display). |
| 2 | `phys` = `T` | nothing (already in place). |
| 3 | otherwise | `size` ← `MmQueryAllocationSize(block)`; continue. |
| 4 | `phys` < `T + size` (the block overlaps the target range; nothing usable lies below T) | **intermediate move:** `MmAllocateContiguousMemoryEx(size, T, 0xFFFFFFFF, 0, 0x404)` — lowest T, highest unlimited (the allocator caps it at page 0x3FDF), alignment one page, protect 0x404 = PAGE_READWRITE \| PAGE_WRITECOMBINE; top-down, so it lands at the highest free run below 0x3FE0000. If it succeeds, move there (§4.4). If it fails, go on with step 5 anyway. |
| 5 | always (after 3/4) | **final move:** `MmAllocateContiguousMemoryEx(size, T, T + size − 1, 0, 0x404)`: the range is exactly the block's size, so the only possible result is **physical T = 0x3D000** (0x3D000–0x169FFF for a 0x12D000 block). If it succeeds, move there (§4.4). If it fails, the block stays where it is (its original place, or the intermediate one of step 4). |

Notes:
- The second allocation is always tried, also after the intermediate move (which then is the source of the copy).
- The allocator may take pages that are in use but unlocked and move their contents away (spec 09 §4 / §8), so after a quick reboot (which has freed every
  non-persisted page) the final allocation normally succeeds; only persisted or locked pages in 0x3D000–T+size block it.
- Protect 0x404 makes the allocator flush the TLB and caches after mapping (its "caching type changed" path), and the new block is write-combined.

### 4.4 The move itself (0x8002FD1B, arguments: new block, size)
| # | Step | Value |
|---|---|---|
| 1 | Copy `size` bytes from the current block (0x8003B350) to the new one (dwords, then the remaining bytes). | whole block, header included |
| 2 | New header dword 1 ← old dword 1 − `MmGetPhysicalAddress(old)` + `MmGetPhysicalAddress(new)`: the pixel address keeps its offset (0x40) inside the block. | |
| 3 | NV2A **PCRTC_START 0xFD600800 ← new dword 1** (the scan-out start: the picture keeps showing, now from the new copy). | physical address |
| 4 | NV2A **PCRTC_INTR_0 0xFD600100 ← 1** (clear a pending vertical-blank flag). | 1 |
| 5 | `MmPersistContiguousMemory(new, size, TRUE)`. | |
| 6 | Saved-data pointer 0x8003B350 ← new (the same variable `AvSetSavedDataAddress` writes; the export itself is not called). | |
| 7 | Spin until PCRTC_INTR_0 bit 0 is set: wait for the next vertical blank, so the CRTC has latched the new start address before the old pixels go. No time-out. | |
| 8 | `MmFreeContiguousMemory(old)`. The old block's persist bit is **not** cleared first; the pages are simply freed (up to the bit-9 page, with `invlpg`). | |

No other display register, encoder or AV call is made. With both moves (§4.3 steps 4 and 5) there are two copies and two vertical-blank waits.

## 5. Order of everything at that point
1. (Cold only) INIT pages freed, target recorded (§2).
2. LaunchDataPage → highest free page below 0x10000 (§3).
3. Saved display block → physical 0x3D000, possibly through a top-of-memory intermediate copy (§4).
4. Title entry point called.

Consequence for a title: after these moves the only persisted memory is below 0x10000 (LaunchDataPage) and at 0x3D000–~0x16A000 (saved display), so the top
of memory up to 0x3FE0000 is free. The title's graphics library's 32 MB contiguous request, `MmAllocateContiguousMemoryEx(0x2000000, 0, 0x7FFFFFF, 0x1000,
0x404)`, has its highest page capped at 0x3FDF, is placed top-down, and gets 0x1FE0000–0x3FDFFFF, which is 64 KB-aligned (0x1FE0000 = 0x1FE × 0x10000).
A saved display left anywhere between 0x1FE0000 and 0x3FE0000 pushes that block down to a page-aligned but generally not 64 KB-aligned address.

## 6. Other places that move or free persisted memory
| Where | When | What |
|---|---|---|
| Quick-reboot page re-adoption 0x8001FA6C (5838 0x8001FCAB) | early on a quick reboot | walks the page database (0x83FF0000, one dword per page up to 64 MB); keeps pages that are in use **and** persisted (bit 10), and kernel-owned entries; frees every other page. Nothing is moved. |
| Boot animation, first presented frame (5838 0x8003DDF0, INIT) | cold boot | if a saved display exists: `MmPersistContiguousMemory(block, size, FALSE)`, `MmFreeContiguousMemory`, `AvSetSavedDataAddress(0)` (spec 22 §8.1). |
| Boot animation, start (5838 0x8003ECB0, INIT) | cold boot | frees any existing saved display (`MmFreeContiguousMemory`, `AvSetSavedDataAddress(0)`) before making its own (§4.1). |
| Title launch code (spec 05 §2–§4, 0x8002F291 / 0x8002F2F4) | before §1 | creates, persists and fills the LaunchDataPage; no move. |

No other kernel code (outside INIT) calls `AvGetSavedDataAddress` / `AvSetSavedDataAddress`; titles' own graphics libraries release the saved display at
their first presented frame (same three calls as the boot animation).

## 7. 4627 vs 5838
| Item | 4627 | 5838 |
|---|---|---|
| Title-entry helper logic (§3, §4.3) | as described | identical (same arguments, same order, same conditions) |
| Move helper (§4.4) | as described | identical (same register writes, same wait, same calls) |
| Target address | 0x3D000 | 0x3D000 |
| Pages freed from INIT | 0x3D–0xAA | 0x3D–0xA8 |
| CS limit narrowed after the INIT discard | no | yes (§2 step 5) |
| Contiguous allocator's highest-page cap | 0x3FDF | 0x3FDF |

## 8. Unknown / not studied
- Whether `MmFreeContiguousMemory` (through the page-free helper 0x8001F899) clears the freed pages' persist bit, or whether that only stops mattering
  because the pages are no longer "in use" for §6's re-adoption.
- Behaviour on a 128 MB console: the allocator cap 0x3FDF is a constant in both images; no place that changes it was found, but the 128 MB path was not
  followed.
- What the allocator does if step 5's range holds locked or persisted pages (beyond returning null) was not traced past spec 09.

## 9. Ours
OXOS moves the LaunchDataPage as §3 (`PrepareToEnterTitle`, `kernel/src/titles/title_loader.cpp`), and the saved display block before it
(`hardware::video::MoveSavedDisplay`): not to 0x3D000, which in OXOS holds the title's own image by then (spec 09 §8), but below 16 MB
(`OXOS_KEPT_MEMORY_HIGHEST`, spec/oxos_internal/34 §8.2), top-down, when any page of it lies above. The move is §4.4's: copy, rebase header dword 1, PCRTC_START,
PCRTC_INTR_0 cleared, persisted, the saved-data address set, the next vertical blank (at most 100 ms: every wait has a limit), the old block freed.

Why: a dashboard's persisted 0x12C040-byte frame buffer at 0x376F000 (64 MB console) stayed where it was across the quick reboots; the title's 32 MB
Direct3D block, taken from the top, went below it at 0x176F000 instead of 0x1FE0000, lost its 64 KB alignment and the GPU hung on a black screen (1.6
console, 2026-11-24). Moved to 0xED0000, the block lands at 0x1FE0000 and the title runs.
