# 18. Boot-only memory and the kernel's size

OXOS addition, implemented 2026-10-27. Titles get the memory the kernel does not use, and the kernel image stays resident, so its size comes straight out of
what a title has (spec/stock/09 §7). The stock 4627 kernel keeps 45 image pages once it has freed its INIT section. OXOS had grown to 69 (the encoder tables,
exFAT, UDF, virtual discs, the splash's PNG decoder (now its vector renderer), the jingle), and Half-Life 2 started 25 pages behind the stock kernel.
Code: `kernel/Makefile`, `kernel/src/memory/init.cpp` (`memory::FreeBootPages`), `tools/check_kernel_layout.py`.

## 1. Built for size
The kernel is compiled with `-Oz`. `-O2` stays where speed shows: the hashes and ciphers (`src/crypto/crypto.cpp`: XBE sections, saves), `memcpy`/`memset`
(`src/runtime/memory.cpp`), the virtual-disc decompressor (`src/storage/virtual_disc.cpp`, every sector a title reads from a CCI) and the boot-only files of §2
(the splash and the jingle keep up with the clock, and their pages are freed anyway). Debug builds (`DEBUG=1`) keep `-Og`.

## 2. Boot-only sections
Code and data used only during a cold boot go in two sections of their own, which the kernel frees before the first title starts:
- `.xkb`, code: the splash (`src/boot_splash/splash.cpp`, except `boot_splash::EndSplash`, which every title start calls), its vector renderer
  (`src/graphics/vector_renderer.cpp`), the jingle's synthesizer and player (`src/boot_splash/jingle.cpp`, `src/boot_splash/bootsound.cpp`), the network stack
  (`src/network/`) and the upload server (`src/webserver/`, spec/oxos_internal/31), and the disk checks of a cold boot (recognising a blank disk, naming what a
  foreign one holds: `src/storage/disk_format.cpp`, and the countdown and checks in `src/storage/disk_layout.cpp`, spec/oxos_internal/30 §4), the cold boot's
  launch choice (`titles::LaunchAtColdBoot` in `src/titles/title_loader.cpp`: the pre-launch checks, the SMC boot flags, X for the File Manager and B for safe
  mode, 2026-11-13), the netboot title's installation (`src/titles/title_netboot.cpp`) and the kernel extension loader's cache file
  (`extensions::WriteLoaderCache`, spec/oxos_internal/34 §8.1). The kernel extension loader itself is appended to the image after these sections, so its pages
  are boot-only too. The files, or those parts of them, say `#pragma code_seg(".xkb")`.
- Code that can also run after the pages are freed asks `memory::BootPagesPresent()` before it calls boot-only code: the layout detection runs on every boot,
  and on a quick reboot it skips the disk checks (the cold boot already made the disk usable).
- `.xka`, read-only data, in this order (COFF grouped sections, sorted by the name after `$`): `$c0`/`$c1` the Conexant's tables, `$f0`/`$f1` the Focus's,
  `$x0`/`$x1` the Xcalibur's, `$x9` the end marker (`hardware/video/video.cpp`; `tools/gen_av_tables.py` tags each table with `AVP_CX`, `AVP_FO` or `AVP_XC`),
  then `$y`, the boot data: the CPU microcode updates (`XK_BOOT_DATA`) and the constant tables of the boot-only files (`#pragma const_seg(".xka$y")`).

The link marks both sections discardable (`/SECTION:.xka,D /SECTION:.xkb,D`), so the linker places them after every other section, `.xka` first. The kept image
ends in the page where the Conexant's tables begin, so on a Conexant console (and in xemu) those tables cost one page more.

Variables stay in the ordinary data sections, where the boot tests read them after the title has started.

## 3. Freeing them
`boot_splash::EndSplash` (each title start) calls `memory::FreeBootPages` with the range of the tables in use (`hardware::VideoTablesInUse`: the Focus's or the
Xcalibur's when that encoder was found, else the Conexant's, which the display-mode code also falls back to when no encoder answers). It runs once: it frees
every page from the first page that holds only discardable sections to the end of the image, except the pages overlapping that range. It never frees a page
below 0x8003B000: titles read the kernel image up to a fixed address (XBMC4Gamers searches 0x80010000-0x8003AF90 byte by byte for an instruction of the stock
kernel, which OXOS does not have), and a freed page there is an access violation in the title. On a Focus console a kernel whose boot-only data starts just
below 0x8003A000 lost the page between the image and the Focus's tables, and XBMC4Gamers stopped right after starting (2026-10-31). Each goes to the tail of its
free list, as a freed contiguous page (`memory::FreeContiguousPage`), so the page database no longer maps it, and a bit in `memory::BootPagesFreed` (`.persist`)
records it.

Before that point the splash thread and the jingle thread have ended: `boot_splash::EndSplash` waits for both thread objects, and the self-tests that set
display modes use only the kept tables.

## 4. Quick reboots
A quick reboot re-enters the same image in memory (spec/stock/02 §1), so the freed pages hold whatever titles left there. `memory::InitMemoryManager` counts the
pages `memory::BootPagesFreed` names as free memory, re-adopting a persisted allocation there as anywhere else. Nothing that runs on a quick reboot uses them:
the splash and the jingle start only on a cold boot, and the microcode is loaded only on a cold boot (a quick reboot does not reset the processor, so the update
stays loaded). The kept encoder's tables are still in place: a console's encoder does not change.

## 5. The layout check
`tools/check_kernel_layout.py` runs after every link, and the build fails when either check fails:
- The linker keeps one copy of an inline function or a shared constant that several objects define, and that copy can be the one a boot-only file put in `.xkb`.
  So no other object may define or use a global symbol that the map places in a boot-only section, except the boot-only files themselves (they call one another
  before the pages go) and the entries kept code calls on a cold boot: `boot_splash::StartSplash` (start-up), `webserver::Start` and `webserver::Stop` (the
  dashboard launch with no dashboard: spec/oxos_internal/31). Ending the splash early (`boot_splash::StopSplash`: the format countdown, the upload screen) keeps
  the pages; the title start (`boot_splash::EndSplash`) frees them.
- The initialised `.data` must fit in the quick-reboot copy (`KI_DATA_COPY_SIZE`, `src/arch/entry.S`).

## 6. Smaller items
- Double-fault and NMI stacks: 1 KB each (their handlers only record the fault in CMOS).
- The quick-reboot copy of `.data`: 0x600 bytes (`.data` holds 0x500).
- Start-up marks in release builds: each log point leaves the address of a one-byte mark instead of its format string, which only serial builds print (about 5.5
  KB of strings). The map names a mark after the function that holds it.
- The Xcalibur's write list: 4 bytes an entry instead of 6 (bit fields).

## 7. Result
Kernel pages that stay resident (xemu, release build):

| Encoder | Kept image | Its tables | Resident |
|---|---|---|---|
| Conexant | 41 | 2 | 43 |
| Focus | 41 | 2 | 43 |
| Xcalibur | 41 | 4 | 45 |

The stock 4627 kernel keeps 45 (it has no Xcalibur support; 1.6 consoles run a later kernel, not measured here). The boot-only sections are 11 pages, 9 of them
freed on a Conexant console.

Free pages, the same runs on both kernels in xemu (64 MB, Conexant):

| | Stock 4627 | OXOS before | OXOS now |
|---|---|---|---|
| Memory test title, at its start | 15 917 | 15 893 | 15 919 |
| Half-Life 2, lowest while loading | 5 935 | 5 912 | 5 938 |
| Half-Life 2, at its menu | 7 203 | 7 178 | 7 204 |

At Half-Life 2's menu the usage counts match the stock kernel's within a page each (pool +1, image +1, virtual memory −1, contiguous −2).

**Measured again 2026-11-15 (v0.4.8):** OXOS is now behind the stock kernel on every title measured, by 4 to 36 KB: at most 0.055 % of the console's
64 MB. The features added since (exFAT, the File Manager's resident parts, the error screens, 128 MB, launch redirects, kernel extensions, log files) each
kept the memory test title at the same amount when they were added, but that amount was already below stock. Same runs on both kernels in xemu (64 MB,
Conexant, a fresh copy of the xemu-dashboard disk; stock = the Complex 4627 BIOS through the MCPX 1.0 ROM); free memory read from `MmGlobalData`'s
available-pages counter (4 KB pages) every 0.5 s:

| Free memory | Stock 4627 | OXOS v0.4.8 | OXOS − stock (share of the 64 MB) |
|---|---|---|---|
| Memory test title, at its start | 63 856 KB | 63 840 KB | −16 KB (0.024 %) |
| Half-Life 2, lowest while loading | 23 740 KB | 23 728 KB | −12 KB (0.018 %) |
| Half-Life 2, settled (120 s) | 27 788 KB | 27 768 KB | −20 KB (0.031 %) |
| Halo 2, settled (120 s; also its lowest) | 812 KB | 784 KB | −28 KB (0.043 %) |
| Halo, lowest while loading | 780 KB | 744 KB | −36 KB (0.055 %) |
| Halo, settled (90 s) | 1 100 KB | 1 076 KB | −24 KB (0.037 %) |
| xemu dashboard | 50 776 KB | 50 772 KB | −4 KB (0.006 %) |

**What would win it back (estimate, 2026-11-15, from the release build's resident sections per object):**

| Step | Resident kernel saved |
|---|---|
| Title patches and the netbooted title as extensions (spec 34 §10, decided): `title_patcher_ini` 1.7 KB, about 0.5 KB of the loader, `title_netboot` 1.0 KB | about 3 KB (0.005 %) |
| The exFAT formatter only while formatting (`format_exfat` 4.0 KB, `disk_format` 1.9 KB resident): boot-only plus loaded for the FTP server's format | about 6 KB (0.009 %) |
| The one kernel stack more than stock (12 KB, seen 2026-11-08; per-thread stacks are the reference's 12 KB, so it is one extra thread of the kernel's) | 12 KB (0.018 %) |
| All three | about 21 KB (0.032 %) |

Memory is handed out in 4 KB pages and the resident kernel ends about 0.3 KB below a page boundary today, so the first page is won after about 3.8 KB:
the extension moves alone most likely win nothing. All three would put OXOS ahead of stock on the dashboard (+17 KB, +0.026 %) and the memory test title
(+5 KB, +0.008 %), but still behind in Halo (−15 KB): the gap grows with the title (4 KB on the dashboard, 36 KB in Halo), so a per-title cost (per open
file, per thread or in the pool, to measure with `MmQueryStatistics` per use) has to be found as well.

The memory test title's amount (15 960 pages, 63 840 KB) is what `make test_memory` and `make regression` hold OXOS to, so that no change loses more; it is
not the stock kernel's (63 856 KB).

## 8. Tests
- `make test_dash`: at least 5 boot-only pages freed, the splash code's page no longer mapped in the page database, the Conexant's tables still mapped.
- `make test_relaunch`: quick reboots after the pages were freed.
- `make test_encoders`: each encoder, release and self-test builds, with the other two encoders' tables freed.
- `make test_memory`: the free pages a title starts with.
