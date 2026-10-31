# 23 — Title-start call for game trainers

OXOS addition, implemented 2026-10-31. Code: `kernel/include/xk/title.h` (`XeCallTitleEntry`), used by
`kernel/src/xe/loader.cpp` (`XepTitleThread`). Guards: unit tests `tests/unit/test_trainer.cpp` (the bytes, the
dashboard's patch round trip, page 0) and the link-time check `tools/check_kernel_layout.py` (check 3: the image). Background: spec/stock/03 §6 (the stock kernel starts the title with
`call [0x00010128]`), spec/oxos-internal/18 §3 (no boot-only page is freed below 0x8003B000).

The kernel starts every title with exactly the 6-byte instruction **`call [0x00010128]`** (`FF 15 28 01 01 00`), the
XBE entry point that the loader decoded into the image header. There is one copy of it in the kernel image. It lies
between 0x80010000 and 0x8003AF90, outside the boot-only sections. Dashboards patch this instruction to run a game
trainer before the game. Checked on hardware (2026-10-31): a Halo 2 trainer loaded from XBMC4Gamers works on OXOS.

## 1. What XBMC4Gamers does (studied in its default.xbe)
**Install, when a game is launched with a trainer selected** (XBE 0x116F89):
1. Search 0x80010000 + [0, 0x2AF90) byte by byte for `FF 15 28 01 01 00` or `FF 15 10 00 00 80` (first match).
2. If `[0x8000000C]` points at an earlier trainer stub (it starts `60 BA`, `pusha; mov edx, imm`), free that stub.
3. `MmAllocateContiguousMemoryEx` (0x3C00 or 0x960 bytes, depending on the trainer format), then
   `MmPersistContiguousMemory(…, TRUE)`, so the block survives the quick reboot into the game.
4. Store the block's address at **physical 0xC** (`0x8000000C`).
5. Copy in the stub, the trainer and XBMC's helper routines. The helpers use physical 0x10 and 0x14 as their own
   slots.
6. With CR0.WP clear, overwrite the call's address field (instruction + 2) with 0x8000000C:
   `call [0x8000000C]`.

**Stub, run by the kernel's call in place of the game:**
1. `pusha`, then flash the LED (SMC commands 7/8 through the SMBus ports).
2. Compare the certificate's title ID (`[0x10118] + 8`) with the trainer's 3 title IDs.
3. On a match, call the trainer with CR0.WP clear, so it can patch the game.
4. `popa`, then `call [0x10128]` (the game) and `ret`.

**Remove, every time XBMC starts** (XBE 0x117352, called from 0x13A09):
1. Search the same range for `FF 15 0C 00 00 80`.
2. Write 0x00010128 back into the address field.

The hook would otherwise outlive the game: a quick reboot re-enters the same kernel image (it isn't reloaded from
flash), so the patched call stays for every later title until XBMC runs again. A cold boot always starts clean.

## 2. What OXOS needs for it
- **The call instruction.** It is written as inline assembly (`XeCallTitleEntry`), so the compiler can't turn it into
  `mov eax, [0x10128]; call eax`. Registers eax/ecx/edx are clobbered; the title returns only to
  `PsTerminateSystemThread`.
- **Exactly one match.** Dashboards take the first match. The build checks that the image has exactly one
  `FF 15 28 01 01 00` below 0x8003AF90, outside .xka/.xkb, and no `FF 15 10 00 00 80` or `FF 15 0C 00 00 80`.
  Otherwise the link fails. The check reads the image as loaded: the headers plus each section at its address.
- **The page stays mapped.** It is kernel .text (read-only; the dashboard clears CR0.WP to write it) and is never
  freed (spec 18 §3: nothing below 0x8003B000 is freed).
- **Page 0 is free for the dashboard.** Physical page 0 is reserved, mapped read-write at 0x80000000 and kept
  across quick reboots (MmInitSystem). OXOS writes nothing to physical 0xC, 0x10 or 0x14.
- **Persisted contiguous memory is re-adopted by the quick reboot** (MmPersistContiguousMemory, MmInitSystem: PTE
  bit 10).

## 3. Other kernel patches by XBMC4Gamers
- At XBE 0x116F00 it searches 0x80011000-0x80023FFF for the 25-byte start of a stock-kernel function (a setting
  query) and overwrites 0x46 bytes to force the region.
- It never matches OXOS. Its reads stay below 0x8003B000, so they are harmless. OXOS already starts titles of
  any region (spec/stock/05 §4 step 10).

## 4. Other BIOSes
- EvoX M8 and Cerbios 3.1.0 keep the stock call: EvoX at 0x80015A6A, Cerbios at 0x8001653F.
- Neither installs a trainer hook in its kernel image.
- Cerbios loads titles in its own extension, with its own title patches (spec/cerbios/06).
