# Open Xbox kernel

A clean-room C++ reimplementation of the original Xbox kernel (`xboxkrnl.exe`), ABI-compatible with
retail 1.0.4627.1. It is built from the behavioural specs in `../spec/`; no decompiled code is used.
Targets console revisions 1.0–1.5 (1.6 comes later). It builds on Linux with clang + lld-link.

## Build and test
```
make            # build/xboxkrnl.exe + build/xboxkrnl.map
make bios       # build/bios.bin: kernel packed into the base BIOS (MCPX 1.0) with its stock 2BL
make test-m1    # boot bios.bin in xemu and check milestone 1 over QMP
```
Inputs: `BIOS_BASE` (default `../xbox-files/bios/Complex_4627.bin`) and `MCPX_ROM`
(`../xbox-files/mcpx/mcpx_1.0.bin`). The xemu binary is found on PATH or at `~/.local/bin/xemu`.

## Layout
| path | contents |
|---|---|
| `src/arch/` | entry from the 2BL, exception/IRQ stubs (GNU as) |
| `src/ke/` | processor bring-up (GDT/TSS/IDT/PCR), start-up, clock, IRQL, exported data |
| `src/hal/` | PIC, PIT, RTC, PCI chipset set-up, ACPI PM, hardware detection |
| `src/rtl/` | runtime library |
| `exports.lst` | implemented exports; `../tools/gen_def.py` assigns the reference ordinals |
| `../tools/mkbios.py` | flattens the PE, LZX-packs (uncompressed blocks), encrypts, fixes the 2BL digest |
| `../tests/` | `m1_boot.py` (xemu/QMP), `test_lzx_frames.py` (packer round-trip) |

## Milestones
1. **Boot to idle** (specs 00–02): CPU tables, PCR at FS, PIC/PIT/RTC/PCI/ACPI, 1 ms clock, idle loop. ✅ passes in xemu.
2. Scheduler, DPCs/APCs, dispatcher objects, IRQL-driven PIC masking; memory manager (paging, pools).
3. Object manager, I/O manager, IDE + DVD + partitions (stock LBA28), FATX (spec 04).
4. XBE loader and title launch (spec 05): boot the stock dashboard.
5. Remaining exports, then real-hardware validation (SMC, video encoder, microcode).

## Known deviations (tracked)
- `XboxHardwareInfo.GpuRevision` is read from PCI config rather than NV2A PMC_BOOT_0, and the strap flags are pending (needs MMIO mapping).
- `KeQueryPerformance*` / `KeStallExecutionProcessor` use the ACPI PM timer; the reference behaviour isn't confirmed yet.
- CPU microcode update (spec 01 §2.3) is not applied (no redistributable blob); emulators ignore it.
- IRQL is bookkeeping only; nothing is masked by IRQL yet.
- The image uses LZX *uncompressed* blocks, so it is larger than a compressed one would be. Fine at this size; a real compressor may be needed as the kernel grows.
