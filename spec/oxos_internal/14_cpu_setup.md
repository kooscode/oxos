# 14. CPU set-up as the reference does it

OXOS choices, implemented 2026-10-26/27, each matching the reference kernel.

| Item | Reference (spec) | OXOS now | OXOS before |
|---|---|---|---|
| Microcode | update for 0x686 / 0x68A at start-up (spec/stock/01 §2 step 3, spec/stock/23 §2) | the same updates, from Intel's published files (below) | none |
| Machine checks | off: CR4.MCE clear, no MCG_CTL / MCi_CTL write (spec/stock/23 §1) | off; the banks are read for the debug log at every boot, and their MCi_STATUS cleared (0) at a cold boot (below) | enabled, #MC halted |
| Code-segment limit | 0xBFFFFFFF (spec/stock/23 §6.4) | 0xBFFFFFFF; 4 GB in TRACE_IMPORTS builds, whose stubs run from pool | 4 GB |
| Interrupts during start-up | IF set by the PIC set-up (0x80013F98) and by every `HalReadWritePCISpace` (cli … sti); `KfLowerIrql(1)` (0x8001C838) before executive init; IRQ 0/8/11/12 enabled after the memory manager and hardware detection (0x80031876) | the same: IRQL APC_LEVEL before HAL phase 0, IF on from `hardware::InitPic`, the clock enabled after the scheduler set-up, before the object/I/O managers, EEPROM, AV and IDE; IRQ 12 by `hardware::StartSmcEvents` later; IRQ 8 and 11 not used (no RTC interrupts; our SMBus polls) | HIGH_LEVEL with IF off until the end of start-up; the clock only after IDE |
| 2BL code generation | (Microsoft compiler, P5-style code) | `-march=pentium` like the kernel | `-march=pentium3` |
| Idle loop | spins, no `hlt` (spec/stock/01 §3) | spins; host unit tests keep `hlt` (their fake clock advances there) | `sti; hlt` |

## Machine checks (user, 2026-11-17)
- **Read at every boot** (serial builds log them, `diagnostics::ReportMachineChecksAtBoot`): every bank whose MCi_STATUS has its valid bit (63), with
  MCi_ADDR when bit 58 says it holds one, then MCG_STATUS.
- **What the Modxo test console shows:** bank 1 holds an entry at every cold boot, with a different error code each time (0, ITLB 0x10 / 0x11, L1 0x125 /
  0x185, e.g. `f200000000000185`: valid, overflow, uncorrected, processor context corrupt, an L1 data snoop), the same on good and bad boots, already there
  when the kernel first looks; machine checks are off, so it stops nothing. It is taken for the CPU's power-on contents or what the boot ROM's start left
  (investigation skipped by the user, 2026-10-29).
- **Cleared at a cold boot** after the report: 0 written to each bank's MCi_STATUS (the only value it takes), so the entry no longer comes back at every quick
  reboot and a bank that holds something at a later boot is a new error since the cold boot. The reference writes none of these registers (spec/stock/23
  §6.2); with machine checks off the write changes nothing else.

## Microcode
- Source: Intel's Linux microcode repository at a pinned commit, files `intel-ucode/06-08-06` and `06-08-0a`, SHA-256 checked (kernel/Makefile).
  `tools/mkucode.py` keeps the updates for platform 4 (platform-flag bit 0x10): 0x686 revision 8 and 0x68A revision 1, byte for byte the same updates the
  reference carries.
- `core::LoadMicrocode` (core/processor.cpp), first in `core::InitializeProcessor`: an update whose signature equals cpuid(1).eax and whose platform flags
  include the CPU's platform (MSR 0x17 bits 50-52) goes to MSR 0x79 (address of the data after the 48-byte header, 16-byte aligned); the revision is then read
  back (MSR 0x8B ← 0, cpuid, MSR 0x8B high half) and logged as "cpu: signature …, microcode revision …".
- Verified: the no-DVD console (0x68A, platform 4) reports revision 1 after the load (factory revision 0).
- xemu ignores the MSR writes; make test_dash passes.

## Interrupts during start-up: results (no-DVD 1.4 console)
The serial-only build, which had hung at "load: first page read" of xboxdash.xbe on every boot (more than 20), loaded the dashboard on 7 of 7 boots after this
change; the netlog build 2/2; the release build (no debug output) reached the dashboard menu on 4 of 5 boots (one netbooted and showed no video; not diagnosed).
make test_dash and the 143 host unit tests pass.
