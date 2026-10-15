# Spec 03 — Phase-1 system thread and title launch (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile `ev_333970bd…` (0x8001576F) and kernel strings.
Names marked (inferred) are not symbols in the binary. Disk/FATX behaviour stays **exactly stock** until
the open kernel is complete (project decision).

Started by PsInitSystem (spec 02 §2.5) as a system thread. Runs these steps in order:

## 1. System time from the CMOS RTC (every boot)
- Read the RTC into TIME_FIELDS (0x80015468, probably HalQueryRealTimeClock).
- If the read fails, the **year is outside 2000–2100**, or `RtlTimeFieldsToTime` fails, use a fixed
  default time (high dword 0x20000001, low 0).
- Convert local time to system time (0x80019f83) and store it as the boot/system time (0x8003A8E8);
  a related 64-bit value (0x8003A8E0) is zeroed.

## 2. Cold-only device and boot-UI init
Runs only when the quick-reboot flag is 0, with the 0x80CF pulse between steps:
- 0x800626e0, 0x8005510c, 0x80054ee1: INIT-section routines, **not yet decompiled**.
- **Boot flags byte at 0x8003B138** (origin not yet traced; candidates are the 2BL or the SMC
  scratch register). Observed uses:
  - bit 0x20 → SMC write (0x80014e0a with 0xE1), then 0x80055523(0x15)
  - bit 0x01 clear → run 0x80055fba(arg = bit 4). **Likely the boot animation** (inferred: the
    INIT section is ~443 KB, far more than init code needs, which fits an embedded logo animation
    and its assets. Unverified).
  - bit 0x04 / 0x08 / 0x40 / 0x80 → alternative paths in §4.
  - bit 0x01 also makes the 0x80CF pulse a no-op (spec 02 §3).

## 3. I/O system init (every boot)
- 0x80024ecc: in the IDE driver code region. Inferred to be I/O init: create the IDE channel, disk and
  DVD devices, and the partition devices (spec in notes: `IdexDiskCreatePartitionDevice`, partitions
  1–5 = E, C, X, Y, Z, stock LBA28 behaviour). **Next decompile target for spec 04 (IDE + FATX).**

## 4. Choose and load the title (cold boot)
Only when: cold boot, boot-flags bit 0x04 clear, and bit 31 of 0x8003B158 clear (0x8003B158 is right after
`XboxHardwareInfo`; meaning to be established):
- run a check (0x800551ef). If it returns non-zero, launch with that value (below).
- if it returns 0:
  - boot-flags bit 0x80 → 0x8002f291(5, 0)
  - else bit 0x40 → 0x80055e75
  - else bit 0x08 → 0x8002f236
  - else launch with 0.
Otherwise (quick reboot, or either flag set): launch with 0.
**Launch** = 0x8002f2f4(value), in the XBE-loader code (near `XeLoadSection`). Default images
named in the kernel:
- `\Device\CdRom0\default.xbe` (disc)
- `\Device\Harddisk0\Partition2\xboxdash.xbe` (dashboard on C:)
The decision logic (disc vs dashboard, LaunchDataPage on quick reboot) is spec 05.

## 5. Finish boot (cold only)
- **SMC write: SMBus 0x20, command 0x1A, value 1** (HalWriteSMBusValue). Likely tells the SMC the
  kernel booted. **Required on real hardware**; xemu does not need it. (Inferred: without it the SMC
  may time out and flash the error LED. Verify on hardware.)
- 0x80055ee4; if bit 31 of 0x8003B158 is set → 0x8005526d.
- 0x8001e972: probably discards (frees) the INIT section's memory.

## 6. Enter the title (every boot)
- 0x8001e9f5 (pre-launch set-up, to decompile).
- **Call the XBE entry point read from VA 0x00010128** (the XBE image header is mapped at 0x00010000;
  offset 0x128 holds the entry point, already decoded by the loader). **The title runs on this
  system thread.**
- If the title returns: `PsTerminateSystemThread(0)`, then `int 3`.

## Open questions → next specs
- Spec 04: 0x80024ecc and the IDE/DVD/partition/FATX stack (stock behaviour).
- Spec 05: the title chooser and XBE loader (0x800551ef, 0x8002f2f4, 0x8002f291, 0x8002f236,
  0x80055e75, 0x8001e9f5), LaunchDataPage, and XBE header decoding (entry and kernel-thunk XOR keys).
- Where boot-flags 0x8003B138 and 0x8003B158 come from; INIT routines 0x800626e0, 0x8005510c, 0x80054ee1, 0x80055fba.
