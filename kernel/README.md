# Open Xbox kernel

A clean-room C++ reimplementation of the original Xbox kernel (`xboxkrnl.exe`), ABI-compatible with
retail 1.0.4627.1. It is built from the behavioural specs in `../spec/stock/` (clean-room: no Microsoft source code,
machine code, disassembly or decompiled code is used; the video encoders' register tables are the one data taken
from Microsoft images, see [`../dmca-compliance.md`](../dmca-compliance.md)). Targets console revisions 1.0–1.5; on
1.6 the Xcalibur video encoder is driven, the rest of 1.6 is untested. It builds on Linux with clang + lld-link.

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
| `src/idex/` | IDE channel, hard disk (28- and 48-bit addressing; retail partition table or a GPT/MBR exFAT disk) and DVD drive |
| `src/fatx/`, `src/xdvdfs/`, `src/udf/` | file systems: FATX and exFAT (hard disk; one request dispatcher, `exfat.cpp` for the exFAT volume and directory layer), XDVDFS (Xbox discs), UDF (DVD-video) |
| `src/xc/` | crypto: SHA-1, HMAC, RC4, DES block ciphers, RSA (modular exponentiation, PKCS signature checks) |
| `src/xe/` | the title (XBE) loader and launcher; the kernel call tracer (`trace.cpp`, trace builds only) |
| `src/rtl/` | runtime library |
| `exports.tsv` | the export table: every ordinal of the 4627 kernel with name, calling convention, argument bytes and kind (`func`, `data`, or `debug` for the two debugger-only exports, kept for a future debug build); `../tools/gen_def.py` turns it into the linker's `.def` |
| `splash/splash.png` | the boot splash picture (640x480, 256 colours) |
| `build/` | all build output, one folder per kind of build (`selftest`, `release`, `debug`, `trace-basic`, `trace-full`, `trace-full-64mb`; see the Makefile's header); `make clean` removes it |
| `../tools/` | host tools: BIOS packing, running and debugging in xemu, the call tracer's dump ([`../tools/README.md`](../tools/README.md)) |
| `../tests/` | the boot tests, the kernel test title and the controller test ([`../tests/README.md`](../tests/README.md)) |

## Milestones
1. **Boot to idle** (specs 00–02): CPU tables, PCR at FS, PIC/PIT/RTC/PCI/ACPI, 1 ms clock, idle loop. ✅ passes in xemu.
2. Kernel core (spec 06):
   - 2a: IRQL and PIC masking, software interrupts, KINTERRUPT, DPCs, timers, events. ✅ passes in xemu.
   - 2b: threads, scheduler, context switch, lazy FPU, waits, semaphores, mutants (spec 07). ✅ passes in xemu.
   - 2b+: APCs, alerts, suspend/resume, queues, device queues, wake-up boosts (spec 08). ✅ passes in xemu.
   - 2c: memory manager part 1 — page tables, physical pages, contiguous/system memory, kernel stacks, I/O mapping, pool (spec 09). ✅ passes in xemu.
   - 2c part 2: title virtual memory (Nt*VirtualMemory), MmQueryStatistics, MmGlobalData. ✅ passes in xemu.
3. Kernel services:
   - 3a: object manager, handles, namespace, symbolic links, Nt event/mutant/semaphore/timer objects, Ps threads (spec 10). ✅ passes in xemu.
   - 3b: I/O manager — devices, IRPs, StartIo, file objects, Nt*File services, completion ports (spec 11). ✅ passes in xemu.
   - 3c-1: IDE channel, hard disk DMA driver, partition devices (stock LBA28 table) (spec 04). ✅ passes in xemu with a copy of the HDD image. Later: 48-bit addressing for disks over 128 GiB (spec/oxos-internal/04-lba48.md), `make test-lba48` on a 2 TB disk.
   - 3c-2: FATX file system — create/read/write/rename/delete, directory and volume queries (spec 04b). ✅ passes in xemu; written files verified on the disk copy by an independent reader.
   - 3c-3: DVD (ATAPI), XDVDFS, RAW file system (spec 04c). ✅ passes in xemu with a generated XISO.
4. XBE loader and title launch (spec 05):
   - 4a: XBE loader, kernel thunks, section load/digests, media/region checks, title keys, path symlinks, kernel test title from disc (`tests/kernel-test-title`). ✅ passes in xemu.
   - 4b-1: SMBus, SMC tray/reset, EEPROM settings + security section (HD key, region), HalReturnToFirmware (spec 12). ✅ passes in xemu.
   - 4b-2: AV options/capabilities + basic display mode; the nxdk dashboard on C: boots and draws (`make test-dash`, `tools/fbshot.py` for a framebuffer PNG). ✅ passes in xemu.
5. Remaining exports:
   - 5a: runtime exports (Rtl/Interlocked/Ex locks/sprintf/Dbg/HAL extras/RTC time) (spec 13). ✅ passes in xemu.
   - 5b: structured exception handling: trap dispatch, raise, unwind, stack walks (spec 14). ✅ passes in xemu.
   - 5c: Xc public-key (BSAFE RSA, PKCS#1, modexp) and DES/3DES crypto, replaceable crypto vector (spec 15). ✅ passes in xemu.
   - 5d-1: volume dismount/remount (FATX, XDVDFS, RAW), Fsc cache control, Ethernet PHY (spec 16). ✅ passes in xemu.
   - 5d-2: quick reboot (`.persist` section, device quiesce, persisted-memory re-adoption) + LaunchDataPage title-to-title launches (spec 02 §6, 05 §9; `make test-relaunch`). ✅ passes in xemu.
   - 5e: launch policy — first-boot reasons (CMOS clock-set signature, settings), tray/error → dashboard LDP reasons, dashboard failure → LED + fatal error, LDP moved below the kernel (spec 05 §10). ✅ passes in xemu.
   - 5f: fatal-error reporter — LED, EEPROM error history, code drawn on screen (spec 05 §6; `make test-fatal`). ✅ passes in xemu.
6. Real-hardware readiness:
   - 6a: ATA security unlock (HMAC-SHA1 drive password), IDE pass-through IOCTL, refurb power-cycle count (spec 04 §2/§5); LZX compression in `tools/lzx.py` so the kernel fits the 2BL's 256 KB bank. ✅ passes in xemu (its locked drive is unlocked).
   - 6b: SMC events (power button, eject, tray, AV pack) on the ACPI SCI, boot flags from the SMC scratch register, the shutdown sequence, `HalReturnToFirmware`/`HalInitiateShutdown` as in the reference (spec 12 §4). ✅ passes in xemu (tray events and SCI delivery; xemu raises button events only from its UI).
   - 6c: `XboxHardwareInfo` exactly as the reference (flash marker, debug port, NV2A straps and boot ID) (spec 02 §5). ✅ passes in xemu.
   - 6d: performance counter as the reference: 3.375 MHz, 32-bit PM timer extended through the SCI (spec 06 §7). ✅ passes in xemu.
   - 6e: FATX scatter/gather I/O (`NtReadFileScatter`/`NtWriteFileGather`) (spec 04b). ✅ passes in xemu.
   - 8 (out of order): UDF 1.0x read-only file system for DVD-video discs, tried before XDVDFS (spec 04d; `make test-udf`). ✅ passes in xemu.
   - Boot splash: a picture fading in on cold boot (`src/hal/splash.cpp`): `splash/splash.png` (already reduced to 256 colours, so the build needs no Pillow), stored by `mkbios.py` in flash bank 1, decoded by the kernel's own PNG/inflate decoder; without it (256 KB flash) a drawn emblem. The reference's INIT-section 3D animation and its assets are not reproduced (`../spec/oxos-internal/00-boot-splash.md`).
   - 9: retail discs: Xbox DVD authentication (spec 04 §3a; xemu does not emulate it), modded-BIOS media/region policy by default (`RETAIL_MEDIA_POLICY=1` for strict), top-down physical page allocation and a low splash framebuffer so titles' fixed-address memory assumptions hold. Halo (XISO) now loads and runs its own code in xemu.
   - 10: FSCTL_DISMOUNT_VOLUME for FATX/RAW/XDVDFS/UDF (utility-drive formatting); import tracer (`../spec/oxos-internal/01-kernel-call-tracer.md`; `make BUILD=build-trace SELFTEST=0 TRACE_IMPORTS=1`: every title kernel call with arguments, result and file names in a ring for the host tools; today `make run-trace-full`). **Halo reaches its main menu in xemu.** Then: progressive CRTC (CR39 = 0xFF; xemu showed Halo at half height before) and the encoder query options D3D uses (spec 17); `make test-pad` (today `make test-controller`), a controller test (nxdk title + virtual uinput gamepad, all 24 inputs) that passes on ours and the reference.
   - 11: **Halo 2 reaches its title screen in xemu.** Its "disc dirty or damaged" error had two causes: non-cached XDVDFS reads with a length that is not whole sectors (a map file's tail) were rejected, and the kernel used more memory than the reference (per-volume FATX caches instead of one shared FscSetCacheSize cache, an 8-page commit reserve, self-test/tracer code and a page-lock table in release builds). `make BUILD=build-trace2 SELFTEST=0 TRACE_IMPORTS=2` traced with the ring above 64 MB (xemu `mem_limit = '128'`), so the title's memory matches a release build (today `make run-trace-full`).
   - 12: **fully open BIOS image** (spec 01 §4–5).
     - Our reset code (top 512 bytes) and 2BL, XboxDev/cromwell's X-codes with its overflow trick, and its
       EEPROM key states, so stock locked drives unlock.
     - No Microsoft files and no MCPX dump are needed. xemu boots it without a boot ROM, and with the real MCPX 1.0
       ROM (`make test-mcpx`).
     - The dashboard, Halo and Halo 2 run on it, and `make test` uses it by default.
   - 13: **Brute Force reaches its main menu in xemu.** Three fixes: titles from later XDKs check the disc themselves
     (security page and an RSA-signed security sector; `XePublicKeyData` is now an open exponent-1 key, and on a drive
     without Xbox security, such as xemu's, the CD-ROM driver emulates it, spec 04 §3a); XDVDFS reads test end of file
     before the non-cached alignment rule; FATX accepts one doubled path separator, as the reference (spec 04b).
   - 14: **memory and quick-reboot parity with the reference.**
     - The page database uses the reference's format (colour lists, bottom-up pages, relocation).
     - One file-system cache shared by FATX, XDVDFS and UDF.
     - At title entry the splash is freed, the launcher stack is 3 pages and there is no SMC thread, so titles get at
       least the reference's memory (`make test-memory`, `tests/memory-test-title`, measures it with and without
       the XBE's 64 MB flag).
     - Quick reboots keep `XboxHardwareInfo` (Half-Life 2's controller input), the boot AV pack, the SMC armed flag
       and the PHY state.
     - A disc loaded at the dashboard is recognised: the reference's mount order and drive retries.
   - 15: **HD display modes** (`../spec/oxos-internal/02-display-modes.md`): standard timings for 480p, 720p, 1080i and VGA sizes. The widescreen GPIO
     follows each mode change, so xemu's automatic aspect shows HD and widescreen games at 16:9.
   - 16: **debugging tools.**
     - Debug serial port on the SuperIO's COM1 (spec/oxos-internal/03-debug-serial.md): boot log, DbgPrint and crash reports (`make run-trace-basic`).
     - Kernel call tracer: the title's last 32768 kernel calls with arguments, results and file names, dumped on a
       crash and on exit (`make run-trace-full`, `make print-trace-dump`, `tools/trace.py`).
     - Source-level debugging of the kernel and nxdk titles with gdb or VS Code on xemu's GDB server (`make run-gdb`,
       `tools/vscode/`, `tools/debug-app.py`).
   - 17: **developer environment.**
     - Nothing to provide: the free xemu-dashboard disk is fetched as the default hard disk, and play state lives in
       `xemu-state/<bios>-<instance>/`.
     - Reproducible builds (`/Brepro`), with all output under `build/<kind>/`.
     - Each object rebuilds when any header it includes changes.
     - Boot tests run on any formatted disk.
     - `make test-controller`: an nxdk title and a virtual gamepad check all 24 inputs.
   - 18: **48-bit disk addressing (LBA48)** for disks over 128 GiB (`../spec/oxos-internal/04-lba48.md`). Disks up to 128 GiB
     keep the retail 28-bit commands. `make test-lba48` checks a 2 TB disk: every test pattern must land at the right
     sector of the image, and a forced 28-bit driver is checked to fail.
   - 19: **exFAT hard disks** (`../spec/oxos-internal/05-exfat-storage-spec.md`, `07-exfat-driver.md`).
     - A GPT (or MBR) disk with one exFAT volume. C:, E:, X:, Y: and Z: are the folders `/C`, `/E`, `/X`, `/Y`, `/Z`,
       and F: (`/F`) is the extra drive.
     - Titles see the retail partitions and sizes. E: and C: report at most 2 TB, and X/Y/Z report their retail
       750 MB less what their folder holds.
     - The retail reserved area (refurb record, cache database) is kept in sectors 1024–2047.
     - Read-write: files are stored contiguously where possible, with FAT chains otherwise, and the allocation bitmap
       tracks free space.
     - A title formatting its cache partition through the volume handle empties that folder.
     - `make test-exfat` runs the FATX self-tests on E: as a folder and checks the disk afterwards with an independent
       reader (`../tests/exfat.py`, which also builds exFAT disks from FATX ones).
     - **Halo plays on an exFAT disk in xemu**: it formats its cache in /Z, loads a level, saves its profile, and
       continues after a reboot. The dashboard boots from /C.
     - Memory units come last.
   - 20: **video encoders for all three console families** (`../spec/stock/18-conexant-encoder.md`, `19-focus-encoder.md`,
     `20-xcalibur-encoder.md`; OXOS's choices in `../spec/oxos-internal/02-display-modes.md`): Conexant (1.0–1.3), Focus
     (1.4–1.5) and Xcalibur (1.6) are programmed from their register maps (`../tools/gen_av_tables.py` writes
     `src/hal/av_modes.inc` from the specs), SD composite / S-video / SCART / component and 480p / 720p / 1080i, with
     the encoder options; the NV2A timing follows each encoder's row. `make test-encoders` passes on all three of
     xemu's encoder models; Halo, Halo 2 (480p) and Enter the Matrix (1080i) run on each. Not yet tried on hardware.

Exports: all 364 functions and variables the 4627 retail kernel exports are implemented (its ordinals 7 and 11,
`DbgLoadImageSymbols`/`DbgUnLoadImageSymbols`, are absent in the reference as well).

## Known deviations (tracked)
- CPU microcode update (spec 01 §2.3) is not applied (no redistributable blob); emulators ignore it.
- IRQL masks the PIC eagerly instead of the reference's lazy scheme (equivalent for titles, spec/stock/06 §1). KINTERRUPT.DispatchCode isn't used: the IDT keeps fixed per-IRQ stubs.
- Flash PDE 0x3FF stays mapped (the reference clears it); only 64 MB RAM is supported, like the 4627 kernel.
- exFAT (milestone 19): a title's cache format empties the folder in place, synchronously, and the serial it writes is
  kept only until reboot (`/OXOS/partitions.ini` is not written yet); exFAT names outside Latin-1 or longer than 42
  characters, and files of 4 GB or more, are not shown to titles.
- The LZX compressor (`tools/lzx.py`) is greedy/lazy with verbatim blocks only; the stock stream is somewhat smaller.
- Video (milestone 20; `../spec/oxos-internal/02-display-modes.md`): the NV2A timing is OXOS's own (derived from each
  encoder's row for SD, the standards for HD and VGA; 1080i per field), not the reference's NV2A/CRTC rows; no wait
  for the vertical blank phase before a mode change; Macrovision stays off; the Xcalibur is left alone on AV pack
  events. Nothing of it has been tried on a real console yet.
