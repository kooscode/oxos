# Open Xbox kernel

A clean-room C++ reimplementation of the original Xbox kernel (`xboxkrnl.exe`), ABI-compatible with
retail 1.0.4627.1. It is built from the behavioural specs in `../spec/`; no decompiled code is used.
Targets console revisions 1.0–1.5 (1.6 comes later). It builds on Linux with clang + lld-link.

## Build and test
```
make            # build/xboxkrnl.exe + build/xboxkrnl.map
make bios       # build/bios.bin: the open BIOS image (our 2BL + reset code, Cromwell X-codes; spec 01 §5)
make test       # boot bios.bin in xemu; check every milestone's boot self-tests over QMP
make SELFTEST=0 # build without the boot self-tests
make run [ISO=game.iso]  # interactive xemu session (release build, HDD copy)
make run-stock [ISO=game.iso]  # the same session on the reference BIOS (A/B comparison)
make run-trace [ISO=game.iso]  # the same session with the CD-ROM request log shown before the dashboard
make test-pad [PAD_BIOS=stock]  # controller test: a virtual gamepad (uinput) presses all 24 inputs; the
                                # tests/padtest title (nxdk, fetched into ../third_party) checks each one
```
Inputs: only `HDD_IMAGE` (`../xbox-files/hdd/xbox_hdd.qcow2`, e.g. the copyright-free xemu HDD image). Optional:
`MCPX_ROM=...` boots through a real boot ROM (`make test-mcpx`); `IMAGE=base BIOS_BASE=... MCPX_ROM=...` packs
the kernel into your own BIOS instead; `STOCK_MCPX` is used by `run-stock`. The xemu binary is found on PATH or at
`~/.local/bin/xemu`.

## Layout
| path | contents |
|---|---|
| `boot/` | the open image's X-code wrapper (Cromwell's table), reset code and 2BL (spec 01 §5) |
| `src/arch/` | entry from the 2BL, exception/IRQ stubs (GNU as) |
| `src/ke/` | processor bring-up (GDT/TSS/IDT/PCR), start-up, clock, IRQL, exported data |
| `src/hal/` | PIC, PIT, RTC, PCI chipset set-up, ACPI PM, hardware detection |
| `src/ob/`, `src/ex/`, `src/ps/` | object manager, executive objects (Nt*), system threads |
| `src/mm/` | page tables, physical pages, system PTEs, contiguous/system memory, pool |
| `src/rtl/` | runtime library |
| `exports.lst` | implemented exports; `../tools/gen_def.py` assigns the reference ordinals |
| `../tools/mkbios.py`, `../tools/lzx.py` | flattens the PE, LZX-compresses it, encrypts, fixes the 2BL digest |
| `../tests/` | `boot_test.py` (xemu/QMP milestone checks), `test_lzx_frames.py` (packer round-trip) |

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
   - 3c-1: IDE channel, hard disk DMA driver, partition devices (stock LBA28 table) (spec 04). ✅ passes in xemu with a copy of the HDD image.
   - 3c-2: FATX file system — create/read/write/rename/delete, directory and volume queries (spec 04b). ✅ passes in xemu; written files verified on the disk copy by an independent reader.
   - 3c-3: DVD (ATAPI), XDVDFS, RAW file system (spec 04c). ✅ passes in xemu with a generated XISO.
4. XBE loader and title launch (spec 05):
   - 4a: XBE loader, kernel thunks, section load/digests, media/region checks, title keys, path symlinks, test title from disc. ✅ passes in xemu.
   - 4b-1: SMBus, SMC tray/reset, EEPROM settings + security section (HD key, region), HalReturnToFirmware (spec 12). ✅ passes in xemu.
   - 4b-2: AV options/capabilities + basic display mode; the nxdk dashboard on C: boots and draws (`make test-dash`, `tools/fbshot.py` for a framebuffer PNG). ✅ passes in xemu. Real-hardware encoder tables pending.
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
   - Boot splash: a picture fading in on cold boot (`src/hal/splash.cpp`): `splash/splash.png`, reduced to 256 colours and stored by `mkbios.py` in flash bank 1, decoded by the kernel's own PNG/inflate decoder; without it (256 KB flash) a drawn emblem. The reference's INIT-section 3D animation and its assets are not reproduced (spec 17 §7).
   - 9: retail discs: Xbox DVD authentication (spec 04 §3a; xemu does not emulate it), modded-BIOS media/region policy by default (`RETAIL_MEDIA_POLICY=1` for strict), top-down physical page allocation and a low splash framebuffer so titles' fixed-address memory assumptions hold. Halo (XISO) now loads and runs its own code in xemu.
   - 10: FSCTL_DISMOUNT_VOLUME for FATX/RAW/XDVDFS/UDF (utility-drive formatting); import tracer (`make BUILD=build-trace SELFTEST=0 TRACE_IMPORTS=1`: every title kernel call with arguments, result and file names in a ring for the host tools). **Halo reaches its main menu in xemu.** Then: progressive CRTC (CR39 = 0xFF; xemu showed Halo at half height before) and the encoder query options D3D uses (spec 17); `make test-pad`, a controller test (nxdk title + virtual uinput gamepad, all 24 inputs) that passes on ours and the reference.
   - 11: **Halo 2 reaches its title screen in xemu.** Its "disc dirty or damaged" error had two causes: non-cached XDVDFS reads with a length that is not whole sectors (a map file's tail) were rejected, and the kernel used more memory than the reference (per-volume FATX caches instead of one shared FscSetCacheSize cache, an 8-page commit reserve, self-test/tracer code and a page-lock table in release builds). `make BUILD=build-trace2 SELFTEST=0 TRACE_IMPORTS=2` traces with the ring above 64 MB (xemu `mem_limit = '128'`), so the title's memory matches a release build.
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
   - Next (7): video encoder mode tables for real TVs (spec 17: algorithm and table layout documented; values must be derived from the Conexant/Focus datasheets with the reference's row numbering), then real-hardware validation (encoder mode tables, SMC events, microcode).

Exports: all 364 functions and variables the 4627 retail kernel exports are implemented (its ordinals 7 and 11,
`DbgLoadImageSymbols`/`DbgUnLoadImageSymbols`, are absent in the reference as well).

## Known deviations (tracked)
- `KeStallExecutionProcessor` polls the PM timer instead of spinning a boot-calibrated loop (spec/06 §7).
- CPU microcode update (spec 01 §2.3) is not applied (no redistributable blob); emulators ignore it.
- IRQL masks the PIC eagerly instead of the reference's lazy scheme (equivalent for titles, spec/06 §1). KINTERRUPT.DispatchCode isn't used: the IDT keeps fixed per-IRQ stubs.
- Flash PDE 0x3FF stays mapped (the reference clears it); only 64 MB RAM is supported, like the 4627 kernel.
- The LZX compressor (`tools/lzx.py`) is greedy/lazy with verbatim blocks only; the stock stream is somewhat smaller.
