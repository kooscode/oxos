# 43. Kernel modules: launch-time kernel code kept out of resident memory

OXOS addition, implemented 2026-11-17 (user: "kernel modules that we load from a single oxos_kernel.cache, with a universal kernel module loader and
unloader, as the extension loader does today; offload the title patcher, launch redirects, the dashboard resolver and the kernel settings"). Code:
`kernel/src/kernel_modules/` (the loader, resident), `kernel/modules/` (the modules), `tools/link_kernel_modules.py`, `tools/mkbios.py --kernel-modules`.

## 1. What a kernel module is
Kernel code that runs only while a title is being launched (before its first instruction) and is not needed while it runs. It is part of the kernel (built
with it, always there, no listing in an ini file, not switched off by safe boot unless the kernel skips it), but not of the resident kernel: titles do not pay
for it. The kernel extension loader (spec 34 §8.1) was the first such code (2026-11-13); since 2026-11-17 it is one of the modules.

| Module (`kernel/modules/`) | What it does | Spec |
|---|---|---|
| `extension_loader.cpp`, `extension_parser.cpp` | loads the extensions `oxos_extensions.ini` lists (`extensions::LoadListedExtensions`) | 34 §8 |
| `kernel_settings.cpp` | reads OXOS's own options from `oxos_settings.ini` at the cold boot: the boot video's switches and whether toasts are off (`ReadColdBootSwitches`) | 38 §2, 36 §2 |
| `sound_recovery.cpp` | at every quick reboot, a frozen sound codec found by a silent DMA test: the launch saved, the console powered off and on, the launch made again at the cold boot (`CheckSoundAtQuickReboot`, `RestoreSoundRecoveryLaunch`) | 46 §3 |
| `launch_redirect.cpp` | the target `oxos_launch_redirect.ini` gives a launched XBE (`FindLaunchRedirect`) | 33 |
| `dashboard_resolver.cpp` | loads the first dashboard `oxos_settings.ini`'s `[dashboard]` lists that loads (`LoadListedDashboard`) | 19 |
| `title_patcher.cpp` | builds a title's patch records from `oxos_title_patches.ini` (`BuildTitlePatches`) | 25 §3 |
| `kernel_modules.cpp` | the image's entry point `XkKernelModulesEntry`: the table of the functions above (`XK_KERNEL_MODULES`) | |

With them go the shared ini reader and parsers (`shared/ini_file/`), which nothing resident uses any more.

**What stays resident** is what runs while a title runs, or is called by the kernel without the modules: the module loader itself (about 0.5 KB), the
extension runtime (spec 34 §8.1), writing the title's patch records into sections a title loads later through `XeLoadSection` (`ApplyPatches`, spec 25 §3),
the XBE loader, `C:\xboxdash.xbe` as the fallback dashboard, and the error screens.

## 2. The image
- **One image for all modules**, linked by `tools/link_kernel_modules.py` at the fixed address 0xB0000000 (`XK_KERNEL_MODULES_BASE`, a page directory entry
  nothing else uses) against this kernel build: each kernel symbol the modules use becomes an absolute symbol at the kernel's address, so they call the kernel
  directly. A symbol the kernel does not have, or one in its boot-only sections, stops the build. At most 128 KB (`XK_KERNEL_MODULES_MAX_BYTES`); 11.6 KB on
  2026-11-17.
- **In the kernel image:** `tools/mkbios.py --kernel-modules` appends it after the boot-only sections, behind a 20-byte header (`XK_KERNEL_MODULES_HEADER`:
  magic `XOKM`, the kernel's PE stamp, base, size, entry), named in the kernel's DOS header (offset 0x38). Its pages are boot-only: freed before the first
  title.
- **The cache:** at every cold boot the kernel writes the image to `C:\OXOS\cache\oxos_kernel.cache` (`kernel_modules::WriteKernelModulesCache`, boot-only,
  right after it makes `C:\OXOS\cache`), overwriting the last one, and deletes the extension loader's old cache, `oxos_extension_loader.cache`.

## 3. Loading and unloading (`kernel_modules/kernel_module_loader.cpp`)
- **At every launch** (`titles::TitleThread`): `kernel_modules::LoadKernelModules` before the title is chosen, `UnloadKernelModules` once the title is loaded
  and its loaded event has run, before it starts.
- **Loading:** at a cold boot from the copy in the kernel image (its boot-only pages are still there, so no disk read); after a quick reboot from the cache
  file. The header is checked (magic, this kernel's stamp, base, sizes, entry), the image copied into contiguous pages, mapped at 0xB0000000 through a
  temporary page table, and its entry point called for the table (`kernel_modules::Modules`).
- **Unloading:** the page directory entry cleared, the pages and the page table freed; `Modules` is null again. The title gets every page back.
- **Without the modules** (no cache: the disk full or not writable; or a cache from another kernel, which a cold boot replaces): `Modules` stays null and every
  caller has a way without them: no extensions, no launch redirects, no title patches, the boot video off, and the dashboard is `C:\xboxdash.xbe`. The serial
  log says so.

## 4. Memory (2026-11-17)
- The resident kernel lost 3 996 bytes (`.text` 0x285D4 to 0x27638): the title patcher (1.7 KB), launch redirects (0.8 KB), the ini reader (0.5 KB), the
  settings and dashboard code, less the loader.
- test_memory: 15 960 free pages again (15 959 with the dashboard order resident, spec 19 §2). The kept video encoder tables (spec 18) now start at 0x8003ECA0;
  about 450 more bytes out of the resident kernel would let the Conexant tables fit one page lower and give titles one more page.
- While a title is being launched, the image and its page table hold about 16 KB, freed before the title starts.

## 5. Adding a module
A function that runs only during a launch goes into `kernel/modules/NAME.cpp` (namespace `xk::modules`, declared in `modules/modules.h`), gets a field in
`XK_KERNEL_MODULES` (`kernel_modules.h`, with what it does) and its entry in `kernel_modules.cpp`'s table; the kernel calls it through
`kernel_modules::Modules`, with a way for when the modules are not there. It may call any resident kernel function or variable that is not static (the link
needs the kernel map's public symbols), never boot-only code.

## 6. Tests
Every phase that launches a title runs through the modules: `make test_extensions` (the extension loader), `test_relaunch` (launch redirects), `test_dash`
(the dashboard resolver), `test_boot_video` (the kernel settings), the title patches through the self-test kernel's title load (`make test`); `test_memory`
checks the free pages.
