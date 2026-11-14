# 18. Boot-only memory and the kernel's size

OXOS addition, implemented 2026-10-27. Titles get the memory the kernel does not use, and the kernel image stays
resident, so its size comes straight out of what a title has (spec/stock/09 §7). The stock 4627 kernel keeps 45 image
pages once it has freed its INIT section. OXOS had grown to 69 (the encoder tables, exFAT, UDF, virtual discs, the
splash's PNG decoder (now its vector renderer), the jingle), and Half-Life 2 started 25 pages behind the stock kernel.
Code: `kernel/Makefile`, `kernel/src/memory/init.cpp` (`memory::FreeBootPages`), `tools/check_kernel_layout.py`.

## 1. Built for size
The kernel is compiled with `-Oz`. `-O2` stays where speed shows: the hashes and ciphers (`src/crypto/crypto.cpp`: XBE
sections, saves), `memcpy`/`memset` (`src/runtime/memory.cpp`), the virtual-disc decompressor
(`src/storage/virtual_disc.cpp`, every sector a title reads from a CCI) and the boot-only files of §2 (the splash and
the jingle keep up with the clock, and their pages are freed anyway). Debug builds (`DEBUG=1`) keep `-Og`.

## 2. Boot-only sections
Code and data used only during a cold boot go in two sections of their own, which the kernel frees before the first
title starts:
- `.xkb`, code: the splash (`src/boot_splash/splash.cpp`, except `boot_splash::EndSplash`, which every title start
  calls), its vector renderer (`src/graphics/vector_renderer.cpp`), the jingle's synthesizer and player
  (`src/boot_splash/jingle.cpp`, `src/boot_splash/bootsound.cpp`), the network stack (`src/network/`) and the upload
  server (`src/webserver/`, spec/oxos_internal/31), and the disk checks of a cold boot (recognising a blank disk, naming
  what a foreign one holds: `src/storage/disk_format.cpp`, and the countdown and checks in
  `src/storage/disk_layout.cpp`, spec/oxos_internal/30 §4), the cold boot's launch choice (`titles::LaunchAtColdBoot` in
  `src/titles/title_loader.cpp`: the pre-launch checks, the SMC boot flags, X for the File Manager and B for safe mode,
  2026-11-13), the netboot title's installation (`src/titles/title_netboot.cpp`) and the kernel extension loader's cache
  file (`extensions::WriteLoaderCache`, spec/oxos_internal/34 §8.1). The kernel extension loader itself is appended to
  the image after these sections, so its pages are boot-only too. The files, or those parts of them, say `#pragma
  code_seg(".xkb")`.
- Code that can also run after the pages are freed asks `memory::BootPagesPresent()` before it calls boot-only code: the
  layout detection runs on every boot, and on a quick reboot it skips the disk checks (the cold boot already made
  the disk usable).
- `.xka`, read-only data, in this order (COFF grouped sections, sorted by the name after `$`): `$c0`/`$c1` the
  Conexant's tables, `$f0`/`$f1` the Focus's, `$x0`/`$x1` the Xcalibur's, `$x9` the end marker
  (`hardware/video/video.cpp`; `tools/gen_av_tables.py` tags each table with `AVP_CX`, `AVP_FO` or `AVP_XC`), then `$y`,
  the boot data: the CPU microcode updates (`XK_BOOT_DATA`) and the constant tables of the boot-only files (`#pragma
  const_seg(".xka$y")`).

The link marks both sections discardable (`/SECTION:.xka,D /SECTION:.xkb,D`), so the linker places them after every
other section, `.xka` first. The kept image ends in the page where the Conexant's tables begin, so on a Conexant
console (and in xemu) those tables cost one page more.

Variables stay in the ordinary data sections, where the boot tests read them after the title has started.

## 3. Freeing them
`boot_splash::EndSplash` (each title start) calls `memory::FreeBootPages` with the range of the tables in use
(`hardware::VideoTablesInUse`: the Focus's or the Xcalibur's when that encoder was found, else the Conexant's, which the
display-mode code also falls back to when no encoder answers). It runs once: it frees every page from the first page
that holds only discardable sections to the end of the image, except the pages overlapping that range. It never frees a
page below 0x8003B000: titles read the kernel image up to a fixed address (XBMC4Gamers searches 0x80010000-0x8003AF90
byte by byte for an instruction of the stock kernel, which OXOS does not have), and a freed page there is an access
violation in the title. On a Focus console a kernel whose boot-only data starts just below 0x8003A000 lost the page
between the image and the Focus's tables, and XBMC4Gamers stopped right after starting (2026-10-31). Each goes to the
tail of its free list, as a freed contiguous page (`memory::FreeContiguousPage`), so the page database no longer maps
it, and a bit in `memory::BootPagesFreed` (`.persist`) records it.

Before that point the splash thread and the jingle thread have ended: `boot_splash::EndSplash` waits for both thread
objects, and the self-tests that set display modes use only the kept tables.

## 4. Quick reboots
A quick reboot re-enters the same image in memory (spec/stock/02 §1), so the freed pages hold whatever titles left
there. `memory::InitMemoryManager` counts the pages `memory::BootPagesFreed` names as free memory, re-adopting a
persisted allocation there as anywhere else. Nothing that runs on a quick reboot uses them: the splash and the jingle
start only on a cold boot, and the microcode is loaded only on a cold boot (a quick reboot does not reset the processor,
so the update stays loaded). The kept encoder's tables are still in place: a console's encoder does not change.

## 5. The layout check
`tools/check_kernel_layout.py` runs after every link, and the build fails when either check fails:
- The linker keeps one copy of an inline function or a shared constant that several objects define, and that copy can be
  the one a boot-only file put in `.xkb`. So no other object may define or use a global symbol that the map places in a
  boot-only section, except the boot-only files themselves (they call one another before the pages go) and the entries
  kept code calls on a cold boot: `boot_splash::StartSplash` (start-up), `webserver::Start` and `webserver::Stop` (the
  dashboard launch with no dashboard: spec/oxos_internal/31). Ending the splash early (`boot_splash::StopSplash`: the
  format countdown, the upload screen) keeps the pages; the title start (`boot_splash::EndSplash`) frees them.
- The initialised `.data` must fit in the quick-reboot copy (`KI_DATA_COPY_SIZE`, `src/arch/entry.S`).

## 6. Smaller items
- Double-fault and NMI stacks: 1 KB each (their handlers only record the fault in CMOS).
- The quick-reboot copy of `.data`: 0x600 bytes (`.data` holds 0x500).
- Start-up marks in release builds: each log point leaves the address of a one-byte mark instead of its format
  string, which only serial builds print (about 5.5 KB of strings). The map names a mark after the function that
  holds it.
- The Xcalibur's write list: 4 bytes an entry instead of 6 (bit fields).

## 7. Result
Kernel pages that stay resident (xemu, release build):

| Encoder | Kept image | Its tables | Resident |
|---|---|---|---|
| Conexant | 41 | 2 | 43 |
| Focus | 41 | 2 | 43 |
| Xcalibur | 41 | 4 | 45 |

The stock 4627 kernel keeps 45 (it has no Xcalibur support; 1.6 consoles run a later kernel, not measured here).
The boot-only sections are 11 pages, 9 of them freed on a Conexant console.

Free pages, the same runs on both kernels in xemu (64 MB, Conexant):

| | Stock 4627 | OXOS before | OXOS now |
|---|---|---|---|
| Memory test title, at its start | 15 917 | 15 893 | 15 919 |
| Half-Life 2, lowest while loading | 5 935 | 5 912 | 5 938 |
| Half-Life 2, at its menu | 7 203 | 7 178 | 7 204 |

At Half-Life 2's menu the usage counts match the stock kernel's within a page each (pool +1, image +1, virtual memory
−1, contiguous −2).

## 8. Tests
- `make test_dash`: at least 5 boot-only pages freed, the splash code's page no longer mapped in the page database,
  the Conexant's tables still mapped.
- `make test_relaunch`: quick reboots after the pages were freed.
- `make test_encoders`: each encoder, release and self-test builds, with the other two encoders' tables freed.
- `make test_memory`: the free pages a title starts with.
