# Open Xbox kernel

A clean-room C++ reimplementation of the original Xbox kernel (`xboxkrnl.exe`), ABI-compatible with
retail 1.0.4627.1. It is built from the behavioural specs in `../spec/stock/` (clean-room: no Microsoft source code,
machine code, disassembly or decompiled code is used; the video encoder and NV2A pad calibration tables are the one
data taken from Microsoft images, see [`../dmca-compliance.md`](../dmca-compliance.md)). Targets console revisions 1.0–1.5; on
1.6 the Xcalibur video encoder is driven, the rest of 1.6 is untested. It builds on Linux with clang + lld-link.

## Setting up
Tested on Ubuntu 24.04 with clang/lld 18, Python 3.12 and xemu 0.8.136; other Linux distributions work with the
same packages under their own names.

1. **Build tools:** `sudo apt install git make clang lld llvm` (`llvm` provides `llvm-objcopy`; the build scripts use
   the Python 3 that comes with Ubuntu).
2. **xemu:** download the AppImage from [xemu.app](https://xemu.app) and put it at `~/.local/bin/xemu` (or anywhere
   on `PATH` as `xemu`):
   ```
   mkdir -p ~/.local/bin
   mv ~/Downloads/xemu-*-x86_64.AppImage ~/.local/bin/xemu && chmod +x ~/.local/bin/xemu
   sudo apt install libfuse2t64     # AppImages need FUSE 2 on Ubuntu 24.04
   ```
3. **The repository:** `git clone https://github.com/kooscode/oxos.git`. Nothing else needs fetching by hand: on
   first use the build fetches into `../third_party/`, each at a pinned version (so the first build needs internet
   access): [XboxDev/cromwell](https://github.com/XboxDev/cromwell) by `make bios`, the
   [xemu-dashboard](https://github.com/xemu-project/xemu-dashboard) disk (checked by SHA-256) by `make test` and
   `make run`, and [nxdk](https://github.com/XboxDev/nxdk) by the targets that build console apps. Intel's microcode
   is in the repo (`../third_party/intel-ucode`).
4. **Optional, your own files** (git-ignored, never committed): images of your own discs (XISO) in `../xiso/`;
   `../xbox-files/` for your own copyrighted files, e.g. a disk with the stock Microsoft dashboard as
   `xbox-files/hdd/msdash.qcow2`, which `make run` then prefers over the default disk. No BIOS, MCPX boot ROM or
   hard-disk image is needed.
5. **Build and boot:** `make bios`, `make test`, `make run` (below).

Optional extras:
- **Controller test** (`make test-controller`): `sudo apt install cmake flex bison`, and write access to
  `/dev/uinput` (e.g. `sudo setfacl -m u:$USER:rw /dev/uinput` for the current session).
- **Debugging** (below): `sudo apt install gdb-multiarch binutils`; for nxdk apps also an nxdk checkout
  (`make nxdk` fetches one into `../third_party/nxdk`).
- **256 KB flash chips:** `make bios-256k` writes a 256 KB image (one bank, splash included); the first 256 KB of the
  1 MB image is the same image.
- **Your own boot splash:** `make bios SPLASH=my-picture.jpg` converts any picture with Pillow
  (`sudo apt install python3-pil`); the committed `splash/splash.png` needs nothing.
- **Comparing with the original:** your own BIOS dump in `xbox-files/bios/` and the MCPX boot ROM (`STOCK_MCPX`, or
  `MCPX_ROM=...` to boot OXOS through the real ROM as a console does) for `make run-stock` and `make test-mcpx`.

## Build and test
```
make            # build/selftest/xboxkrnl.exe + xboxkrnl.map (all build output: build/<kind>/, see the Makefile)
make bios       # build/selftest/bios.bin: the open BIOS image (our 2BL + reset code, Cromwell X-codes; spec 01 §5)
make test       # boot bios.bin in xemu; check every milestone's boot self-tests over QMP
make SELFTEST=0 # build without the boot self-tests
make run [ISO=game.iso]  # interactive xemu session (release build, HDD copy)
make run-stock [ISO=game.iso]  # the same session on the reference BIOS (A/B comparison); another BIOS:
                                # BIOS_BASE=its.bin BIOS_NAME=name (state xemu-state/<name>-<instance>)
                                # MEMORY=128 on any run target: a 128 MB console
                                # HD=1 (or HD=480p,720p): HDTV AV pack + EEPROM HD modes, so games use 480p/720p/1080i
                                # ENCODER=focus|xcalibur: the video encoder xemu presents (default conexant)
make run-trace-basic [ISO=game.iso]  # the same session with the debug serial port (spec/oxos-internal/03-debug-serial.md) printed in the terminal
make run-trace-full [ISO=game.iso] [TRACE=1|2]  # run-trace-basic plus a record of the title's last 32768 kernel calls
make print-trace-dump [LAST=N] [ONLY=Name,..] [ALL=1]  # while it runs or after a crash: print them (tools/trace.py)
make run-gdb [ISO=game.iso] [GDB_PORT=1234]  # the debug kernel (DEBUG=1, build/debug/) paused for gdb on xemu's
                                # GDB server; symbols in build/debug/xboxkrnl.elf. nxdk apps: ../tools/debug-app.py
make test-memory [MEMORY=128] [MEMORY_BIOS=... MCPX_ROM=...]  # memory a title is given, with and without the
                                # XBE's "limit to 64 MB" flag (tests/memory-test-title, nxdk)
make run-xbe XBE='F:\apps\demo\default.xbe' [STATE=...]  # run an XBE from the hard disk: a launcher disc
                                # (../tools/xbe-launcher) starts it as a dashboard would; the disk defaults to
                                # xemu-state/oxos-exfat/ (an exFAT copy of the play disk, made on first use)
make run-trace-xbe XBE=... [STATE=...]  # the same with the trace build: serial output here, call record on a crash
make test-exfat                 # the self-tests on an exFAT disk (xemu-state/oxos-xemu-exfat/): E: as a folder
make test-encoders              # the three video encoders (Conexant, Focus, Xcalibur) on xemu's models: probe, mode sets, options
make test-lba48                 # the self-tests on a 2 TB disk (xemu-state/oxos-xemu-lba48/): 48-bit addressing
make test-controller [CONTROLLER_BIOS=stock]  # controller test: a virtual gamepad (uinput) presses all 24 inputs;
                                # the tests/controller-test-title title (nxdk, fetched into ../third_party) checks each one
```
Inputs: none to provide. The default hard disk (`HDD_IMAGE`) is the free xemu-dashboard image, fetched on first use
into `../third_party/xemu-dashboard/` (`make hdd-image`); set `HDD_IMAGE=...` for another. Optional:
`MCPX_ROM=...` boots through a real boot ROM (`make test-mcpx`); `IMAGE=base BIOS_BASE=... MCPX_ROM=...` packs
the kernel into your own BIOS instead; `STOCK_MCPX` is used by `run-stock`. The xemu binary is found on PATH or at
`~/.local/bin/xemu`.

## Running in xemu
`make run` starts from `xbox-files/hdd/msdash.qcow2` when you have it (a hard disk with the stock Microsoft
dashboard, whose Settings set up video modes, clock and language), otherwise from the default xemu-dashboard disk;
pick another with `PLAY_HDD=...` (the tests use `HDD_IMAGE`, the xemu-dashboard disk unless set). It keeps its
hard-disk copy and EEPROM in `xemu-state/oxos-<instance>/`: `oxos-msdash`, `oxos-xemu` for the default disk, or
`INSTANCE=name` for any other. Saves and settings survive between runs (`FRESH=1` starts over from the image, which
is never written; `make run-stock` uses `xemu-state/stock-<instance>/`).

It starts with the EEPROM's widescreen flag set, in a 1920x1080 window at twice the render resolution; titles that
support widescreen show 16:9 and the others 4:3. Change that with `WIDE=0`, `ASPECT=16x9` (auto, native, 4x3,
16x9), `SIZE=1280x960`, `SCALE=3` or `FULL=1` (full screen). With HD modes enabled in the dashboard, titles run in
480p, 720p or 1080i (spec 17): 720p and 1080i always show at 16:9, while 480p follows the title's own widescreen
signal as on the original kernel, so a widescreen title that leaves it off in 480p (such as Halo 2) shows 4:3 under
`auto`; run it with `ASPECT=16x9`. Enter the Matrix in 720p or 1080i crashes xemu at render scale 2 or more (an xemu
limitation); use `SCALE=1` or turn those modes off in the dashboard.

## Disks and disc images
**An exFAT disk** (format it on a PC; the Xbox drives are folders `/C`, `/E`, `/F`, `/X`, `/Y`, `/Z` at its root,
created when first used; names are not case-sensitive). Either one exFAT partition (GPT or MBR) starting at 1 MiB or
later, or exFAT on the whole disk with its FAT at sector 2048 or later (`mkfs.exfat` does both); OXOS keeps the
Xbox's reserved sectors in the gap before the volume. Copy the dashboard's files into `/C` and saves (`TDATA`,
`UDATA`) into `/E`. In xemu, a small qcow2 overlay keeps a real disk untouched:
`qemu-img create -f qcow2 -F raw -b /dev/sdX overlay.qcow2`, then use `overlay.qcow2` as the hard disk.
`tests/exfat.py --from-fatx SRC.qcow2 OUT.qcow2 [--whole-disk]` builds such a disk from an Xbox disk image.
Design: [`../spec/oxos-internal/05-exfat-storage-spec.md`](../spec/oxos-internal/05-exfat-storage-spec.md).

**Disc images** of your own discs: XISO, full disc dumps and `.cci`, whole or split in two (`name.1.iso` +
`name.2.iso`; on FATX split below 4 GB); names of 42 characters or fewer.
1. Copy the image to the hard disk, e.g. `F:\titles\` (`/F/titles` on exFAT).
2. Build the ISO launcher (`make iso-launcher`, output `../tools/iso-launcher/bin/default.xbe`) and put a copy next to
   the image, or anywhere with an `iso.txt` naming the image (`F:\titles\Halo 2.iso`).
   [`../tools/oxos-isonator`](../tools/oxos-isonator) does this for you, one folder per game, with a launcher that
   carries the game's name, title ID and artwork.
3. Start the launcher from the dashboard: it mounts the image as the console's disc and the title starts. The image
   stays mounted through the title's own restarts until the console is switched off or reset (LT + RT + BACK +
   BLACK); an `iso.txt` of `none` unmounts it.

Details: [`../spec/oxos-internal/16-virtual-discs.md`](../spec/oxos-internal/16-virtual-discs.md).

## Debugging with gdb or VS Code
xemu's GDB server and a debug build of the kernel (`DEBUG=1`: DWARF debug info, `-Og`) let you debug at source
level: breakpoints, stepping, variables and the call stack, in your own [nxdk](https://github.com/XboxDev/nxdk)
apps and in the kernel, in one session (step from your app into a kernel call and back). You need `gdb-multiarch`
and GNU `objcopy` (binutils).

**The kernel in VS Code:** open the OXOS folder itself (its `.vscode/` is set up), set a breakpoint in kernel
source and press F5. It asks for a disc image (leave it empty for the dashboard), builds the debug kernel, starts
xemu paused and runs to your breakpoint. For example, a breakpoint in `XepLoadImage` (`src/xe/loader.cpp`) stops on
every title load; the watch `(char16_t*)(*(unsigned*)0x10118 + 12)` shows the title's name and `XeImageFileName`
its path.

![Kernel debugging in VS Code: stopped in XepLoadImage, with the loaded title's name in a watch](../docs/kernel-debugging.jpg)

**An nxdk app in VS Code** (C/C++ extension, `ms-vscode.cpptools`):
1. Copy `../tools/vscode/*.json` into your app's `.vscode/` folder and set `oxos.dir` and `oxos.nxdkDir` in
   `settings.json`.
2. Set breakpoints and press F5. VS Code builds the app with `make DEBUG=y`, then `tools/debug-app.py` builds the
   debug kernel (once), packs the app's `bin/` folder as a disc, and starts xemu paused with the kernel's serial log
   in the terminal. VS Code attaches, the kernel boots the disc, and the app stops at your first breakpoint. Stopping
   the debugger closes xemu. Kernel source is loaded too, so you can step from the app into kernel calls.
3. Add `.oxos/` (the disc image and gdb symbol file the launcher writes) to your app's `.gitignore`.

**From a terminal:** `make run-gdb` starts the debug kernel paused (add `ISO=...` for a disc); attach with
`gdb-multiarch build/debug/xboxkrnl.elf -ex 'target remote localhost:1234'`, set breakpoints and `continue`. For an
nxdk app, `python3 ../tools/debug-app.py path/to/app` does the app steps above; then attach with
`gdb-multiarch path/to/app/.oxos/main.elf -ex 'set confirm off' -ex 'add-symbol-file build/debug/xboxkrnl.elf'
-ex 'target remote localhost:1234'`.

Notes: gdb sees one CPU, not the app's threads; timers stop while you are paused. C++ apps should add
`CXXFLAGS += -mllvm -dwarf-linkage-names=Abstract` to their Makefile, so that gdb shows function names (it cannot
read the MSVC-style names nxdk's compiler uses). Debugging on a real Xbox is not implemented yet.

## About the BIOS image you build
`make bios` contains our code plus parts of XboxDev/cromwell (X-codes and the EEPROM key states), both GPL-2.0,
and Intel's CPU microcode (unmodified, under Intel's licence; [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)).
No Microsoft code. The one Microsoft-derived data in it is the video encoder and NV2A memory pad calibration tables,
functional hardware constants ([`../dmca-compliance.md`](../dmca-compliance.md) §5). There is no certificate key:
titles derive different save-signature and System Link keys than a stock console does, so saves made on a stock
console do not verify under OXOS and System Link with stock consoles does not work; saves made under OXOS work among
themselves.

`IMAGE=base` packs the kernel into your own Microsoft BIOS instead, for comparisons. Such an image contains
Microsoft's 2BL and keys: do not distribute it.

## How it was made
- **The kernel** (`src/`): the reference kernel was studied with Ghidra to write behavioural specifications
  (`../spec/stock/`), and the kernel is written from those specs only. No Microsoft source code, machine code,
  disassembly, decompiled code or keys are included.
- **Planned features** (`../spec/cerbios/`): the Cerbios BIOS's kernel and extension were studied the same way, and
  its features were written up as behavioural specs for OXOS.
- **The boot chain** (`boot/`, spec 01 §5): our own reset code and 2BL, with the hardware set-up table (X-codes) and
  EEPROM key handling of [XboxDev/cromwell](https://github.com/XboxDev/cromwell). On a console the MCPX boot ROM runs
  Cromwell's X-codes, whose "overflow trick" hands control to our 2BL without any Microsoft key; without a boot ROM
  (xemu), our reset code runs the same X-codes. The hard disk unlocks with Cromwell's EEPROM key states.

## Layout
| path | contents |
|---|---|
| `boot/` | the open image's X-code wrapper (Cromwell's table), reset code and 2BL (spec 01 §5) |
| `src/arch/` | entry from the 2BL, exception/IRQ stubs, context switch, SEH and quick-reboot code, the call tracer's stubs (GNU as) |
| `src/ke/` | processor bring-up (GDT/TSS/IDT/PCR), start-up, clock, IRQL, DPCs, APCs, threads, scheduler, waits, bug checks; the boot self-tests (`selftest*.cpp`, self-test build only) |
| `src/hal/` | PIC, PIT, RTC, PCI chipset set-up, ACPI PM, SMBus/SMC, AV (video modes), network PHY, boot splash, debug serial port (spec/oxos-internal/03-debug-serial.md) |
| `src/ob/`, `src/ex/`, `src/ps/` | object manager; executive objects (Nt*), EEPROM, fatal errors; system threads |
| `src/mm/` | page tables, physical pages, system PTEs, contiguous/system memory, pool, title virtual memory |
| `src/io/` | I/O manager: devices, IRPs, files, the file-system cache, the RAW file system |
| `src/idex/` | IDE channel, hard disk (28- and 48-bit addressing; retail or PARTINFO partitions, or an exFAT disk: GPT, MBR or whole disk) and DVD drive |
| `src/fatx/`, `src/xdvdfs/`, `src/udf/` | file systems: FATX and exFAT (hard disk; one request dispatcher, `exfat.cpp` for the exFAT volume and directory layer), XDVDFS (Xbox discs), UDF (DVD-video) |
| `src/xc/` | crypto: SHA-1, HMAC, RC4, DES block ciphers, RSA (modular exponentiation, PKCS signature checks) |
| `src/vd/` | virtual discs: disc images mounted as the console's DVD drive |
| `src/xe/` | the title (XBE) loader and launcher; the kernel call tracer (`trace.cpp`, trace builds only) |
| `src/rtl/` | runtime library |
| `exports.tsv` | the export table: every ordinal of the 4627 kernel with name, calling convention, argument bytes and kind (`func`, `data`, or `debug` for the two debugger-only exports, kept for a future debug build); `../tools/gen_def.py` turns it into the linker's `.def` |
| `splash/splash.png` | the boot splash picture (640x480, 256 colours) |
| `build/` | all build output, one folder per kind of build (`selftest`, `release`, `debug`, `trace-basic`, `trace-full`, `trace-full-64mb`, `netboot`; see the Makefile's header); `make clean` removes it |
| `../tools/` | host tools: BIOS packing, running and debugging in xemu, the call tracer's dump ([`../tools/README.md`](../tools/README.md)) |
| `../tests/` | the boot tests, the kernel test title and the controller test ([`../tests/README.md`](../tests/README.md)) |

## Milestones
Specs are in `../spec/stock/` unless named otherwise (`oxos/NN` = `../spec/oxos-internal/NN`). ✅ xemu = checked by
the boot tests; console = checked on the 1.4 console.

| # | What | Spec | Checked |
|---|---|---|---|
| 1 | Boot to idle: CPU tables, PCR, PIC/PIT/RTC/PCI/ACPI, 1 ms clock | 00–02 | ✅ xemu |
| 2 | Kernel core: IRQL, DPCs, timers, threads, scheduler, waits, APCs, queues; memory manager and title virtual memory | 06–09 | ✅ xemu |
| 3 | Object manager, I/O manager, IDE disk, FATX, DVD/XDVDFS, RAW | 04, 04b, 04c, 10, 11 | ✅ xemu |
| 4 | XBE loader and title launch; SMBus/SMC/EEPROM; AV options; the dashboard boots | 05, 12, 17 | ✅ xemu |
| 5 | Remaining exports: runtime, SEH, Xc crypto, dismount/cache control, quick reboot and launch data, launch policy, fatal errors | 02 §6, 05, 13–16 | ✅ xemu (`test-relaunch`, `test-fatal`) |
| 6 | Real-hardware readiness: ATA unlock, SMC events, `XboxHardwareInfo`, performance counter, scatter/gather I/O | 02 §5, 04, 06 §7, 12 | ✅ xemu |
| 8 | UDF (DVD-video discs) | 04d | ✅ xemu (`test-udf`) |
| – | Boot splash (`splash/splash.png`, in each boot bank's spare space too) | oxos/00 | ✅ xemu, console |
| 9 | Retail discs: DVD authentication, media/region policy, memory layout titles assume | 04 §3a | ✅ xemu: Halo loads |
| 10 | Volume dismount for utility-drive formatting; kernel call tracer | oxos/01 | ✅ xemu: Halo's menu |
| 11 | XDVDFS tail reads, one shared file cache, release builds without test code | 04c | ✅ xemu: Halo 2's title screen |
| 12 | Fully open BIOS image: our reset code and 2BL, Cromwell's X-codes and key states | 01 §4–5 | ✅ xemu, also through a real MCPX 1.0 ROM (`test-mcpx`) |
| 13 | Disc self-checks of later XDKs (emulated security sector), one doubled path separator | 04 §3a, 04b | ✅ xemu: Brute Force's menu |
| 14 | Memory and quick-reboot parity: reference page database, shared cache, small launcher footprint | 09 | ✅ xemu (`test-memory`) |
| 15 | HD display modes: 480p, 720p, 1080i, VGA; widescreen signal | oxos/02 | ✅ xemu |
| 16 | Debugging: serial port, call tracer, gdb / VS Code | oxos/01, oxos/03 | ✅ xemu |
| 17 | Developer environment: fetched default disk, reproducible builds, header dependencies, controller test | – | ✅ xemu (`test-controller`) |
| 18 | LBA48 for disks over 128 GiB | oxos/04 | ✅ xemu (`test-lba48`, 2 TB) |
| 19 | exFAT disks: Xbox drives as folders, read-write, cache formats | oxos/05, oxos/07 | ✅ xemu (`test-exfat`): Halo, Halo 2; console |
| 20 | Video encoders: Conexant, Focus, Xcalibur from their register maps | 18–20, oxos/02 | ✅ xemu (`test-encoders`); console: Focus |
| 21 | Real hardware: netboot, flash mapping, CPU set-up, microcode, pad calibration | oxos/09, oxos/13–15, 21 §3 | ✅ console: the dashboard boots |
| 22 | Virtual discs: XISO, full dumps, CCI, split images (`src/vd/`, ISO launcher) | oxos/16, cerbios/07 | ✅ xemu (`test-iso`); console: Halo 2 |
| 23 | Boot jingle | oxos/17 | ✅ xemu (`test-dash`), console |
| 24 | Boot-only memory freed before the first title (`-Oz` build) | oxos/18 | ✅ xemu (`test-memory`): more free memory than stock |
| 25 | Dashboard choice: EvoX when installed, else the Xbox dashboard | oxos/19 | ✅ xemu (`test-dash`) |
| 26 | DRAM refresh check before the kernel runs | oxos/20, oxos/21 | ✅ console: 0 bad boots in 56 (8 in 40 before) |
| 27 | In-game reset: LT + RT + BACK + BLACK | oxos/22 | ✅ xemu, console |
| 28 | Large FATX disks: F:/G: from a PARTINFO table, else Cerbios's layout | cerbios/03 | ✅ console: 2 TB XBPartitioner disk |
| 29 | exFAT on a whole disk (no partition table); main boot region repaired from the backup | oxos/05 §2a | ✅ xemu; console: 2 TB drive |

Exports: all 364 functions and variables the 4627 retail kernel exports are implemented (its ordinals 7 and 11,
`DbgLoadImageSymbols`/`DbgUnLoadImageSymbols`, are absent in the reference as well).

## Known deviations (tracked)
- IRQL masks the PIC eagerly instead of the reference's lazy scheme (equivalent for titles, spec/stock/06 §1). KINTERRUPT.DispatchCode isn't used: the IDT keeps fixed per-IRQ stubs.
- Only 64 MB RAM is supported, like the 4627 kernel.
- IRQ 8 (RTC) and IRQ 11 (SMBus), which the reference enables at start-up, stay masked: no RTC interrupts are used
  and the SMBus is polled.
- exFAT (milestone 19): a title's cache format empties the folder in place, synchronously, and the serial and cluster
  size it writes are kept across quick reboots but not a power cycle (`/OXOS/partitions.ini` is not written yet), so
  a title may format its cache again after a cold boot; exFAT names outside Latin-1 or longer than 42 characters, and
  files of 4 GB or more, are not shown to titles.
- No DVD drive (`../spec/oxos-internal/08-no-dvd-drive.md`): the console boots to the dashboard with `\Device\CdRom0` acting
  as an empty drive, where the reference stops with fatal error 10, 11 or 12.
- The LZX compressor (`tools/lzx.py`) is greedy/lazy with verbatim blocks only; the stock stream is somewhat smaller.
- Video (milestone 20; `../spec/oxos-internal/02-display-modes.md`): the NV2A timing is OXOS's own (derived from each
  encoder's row for SD, the standards for HD and VGA; 1080i per field), not the reference's NV2A/CRTC rows; no wait
  for the vertical blank phase before a mode change; Macrovision stays off; the Xcalibur is left alone on AV pack
  events. On hardware so far: the Focus encoder (1.4) in composite.
