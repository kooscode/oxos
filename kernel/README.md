# Open Xbox kernel

A clean-room C++ reimplementation of the original Xbox kernel (`xboxkrnl.exe`), ABI-compatible with
retail 1.0.4627.1. It is built from the behavioural specs in `../spec/`; no decompiled code is used.
Targets console revisions 1.0–1.5 (1.6 comes later). It builds on Linux with clang + lld-link.

## Build and test
```
make            # build/xboxkrnl.exe + build/xboxkrnl.map
make bios       # build/bios.bin: kernel packed into the base BIOS (MCPX 1.0) with its stock 2BL
make test       # boot bios.bin in xemu; check every milestone's boot self-tests over QMP
make SELFTEST=0 # build without the boot self-tests
```
Inputs: `BIOS_BASE` (default `../xbox-files/bios/Complex_4627.bin`) and `MCPX_ROM`
(`../xbox-files/mcpx/mcpx_1.0.bin`). The xemu binary is found on PATH or at `~/.local/bin/xemu`.

## Layout
| path | contents |
|---|---|
| `src/arch/` | entry from the 2BL, exception/IRQ stubs (GNU as) |
| `src/ke/` | processor bring-up (GDT/TSS/IDT/PCR), start-up, clock, IRQL, exported data |
| `src/hal/` | PIC, PIT, RTC, PCI chipset set-up, ACPI PM, hardware detection |
| `src/mm/` | early boot allocator (memory manager comes in 2c) |
| `src/rtl/` | runtime library |
| `exports.lst` | implemented exports; `../tools/gen_def.py` assigns the reference ordinals |
| `../tools/mkbios.py` | flattens the PE, LZX-packs (uncompressed blocks), encrypts, fixes the 2BL digest |
| `../tests/` | `boot_test.py` (xemu/QMP milestone checks), `test_lzx_frames.py` (packer round-trip) |

## Milestones
1. **Boot to idle** (specs 00–02): CPU tables, PCR at FS, PIC/PIT/RTC/PCI/ACPI, 1 ms clock, idle loop. ✅ passes in xemu.
2. Kernel core (spec 06):
   - 2a: IRQL and PIC masking, software interrupts, KINTERRUPT, DPCs, timers, events. ✅ passes in xemu.
   - 2b: threads, scheduler, context switch, lazy FPU, waits, semaphores, mutants (spec 07). ✅ passes in xemu.
   - 2b+: APCs, alerts, suspend/resume, queues, device queues, wake-up boosts (spec 08). ✅ passes in xemu.
   - PsCreateSystemThreadEx and the Nt* wrappers wait for object-manager handles (milestone 3).
   - 2c: memory manager (paging, PFN database, pools, contiguous/system memory, virtual memory).
3. Object manager, I/O manager, IDE + DVD + partitions (stock LBA28), FATX (spec 04).
4. XBE loader and title launch (spec 05): boot the stock dashboard.
5. Remaining exports, then real-hardware validation (SMC, video encoder, microcode).

## Known deviations (tracked)
- `XboxHardwareInfo.GpuRevision` is read from PCI config rather than NV2A PMC_BOOT_0, and the strap flags are pending (needs MMIO mapping).
- `KeQueryPerformance*` / `KeStallExecutionProcessor` use the ACPI PM timer; the reference behaviour isn't confirmed yet.
- CPU microcode update (spec 01 §2.3) is not applied (no redistributable blob); emulators ignore it.
- IRQL masks the PIC eagerly instead of the reference's lazy scheme (equivalent for titles, spec/06 §1). KINTERRUPT.DispatchCode isn't used: the IDT keeps fixed per-IRQ stubs.
- Thread objects and kernel stacks come from a boot-time bump allocator (0x80100000-0x80400000) until the memory manager exists, and are never freed.
- The image uses LZX *uncompressed* blocks, so it is larger than a compressed one would be. Fine at this size; a real compressor may be needed as the kernel grows.
