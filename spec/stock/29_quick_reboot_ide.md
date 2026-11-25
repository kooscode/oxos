# Spec 29 — The IDE channel and the other bus masters around a quick reboot (xboxkrnl 1.0.5838.1; 4627 compared)

Clean-room behavioural spec. Sources: Ghidra 12.1.4 analysis (REA) of the shipped binaries:

| Binary | Version | sha256 |
|---|---|---|
| `5838-unpacked/xboxkrnl.exe` (primary) | 1.0.5838.1 | 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde |
| `xboxkrnl-4627.exe` (compared) | 1.0.4627.1 | 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f |

Addresses are 5838 unless marked; 4627 equivalents are in §8. Export addresses come from the PE export table (ordinal → RVA + 0x80010000). No code is
reproduced.

**What was searched.**
- Every `out`/`in` with the immediate ports 0x1F0–0x1F7, 0x3F6, 0xFF60–0xFF64 in both images (5838: 0x800241FE–0x80025340, 0x8002600E/0x80026029, INIT
  0x80055276–0x80055667); every use of the constant 0x3F6.
- Every caller of `HalReturnToFirmware` (ord 49), `HalRegisterShutdownNotification` (ord 47), `HalIsResetOrShutdownPending` (ord 358),
  `IoMarkIrpMustComplete` (ord 359), `HalInitiateShutdown` (ord 360), `PhyInitialize` / `PhyGetLinkState` (ord 253 / 252), and every reference to the
  shutdown-pending byte, the must-complete counter and the shutdown-notification list head.
- Every immediate reference to 0xFEF00000 (NIC), 0xFED00000 / 0xFED08000 (USB), 0xFEC00000 (AC'97) in both images.
- The quick-reboot routine and its page-directory helper, the IDE channel init, its quick-reboot branches, the IDE timer DPC, ISR, transfer start/finish,
  error and reset paths, the ATAPI command path.
- Not examined: the title side of a launch (`XLaunchNewImage` is in the title's statically linked XAPI library, not in the kernel; see §9).

## 1. From `HalReturnToFirmware(2)` to the restarted kernel
The kernel never quick-reboots on its own: its two calls of `HalReturnToFirmware` (0x8002EF92, 0x8002F74D, the XBE loader / dashboard-launch failure paths) pass
routine 4. Routine 2 comes only from a title (its XAPI `XLaunchNewImage`, after it has written `LaunchDataPage`). The kernel side, in order:

| # | Address | Step |
|---|---|---|
| 1 | 0x8001542D (`HalReturnToFirmware`) | routine ≠ 2 → §1.2; routine 2 continues here, **at the caller's IRQL** (no raise, no `cli`) |
| 2 | 0x8001542D (same function) | **Shutdown-notification list** (head 0x80036A40): repeatedly, at DISPATCH_LEVEL (`KeRaiseIrqlToDpcLevel` 0x80013D04), unlink the first entry, lower back (0x80013D24), then call that entry's routine with the entry's address; until the list is empty. Each routine runs once, at the caller's IRQL; the list is empty afterwards. |
| 3 | 0x8001B18C | interrupts off (`cli`); EBP = 0; ESP = 0x8003A5F0 (boot stack) |
| 4 | 0x8001ECFB (the "pre-reset hook"; 4627: 0x8001EABC) | **page-directory restore, no device access**: PTEs of pages 0x3FE0–0x3FEF ← frame \| 0x47B; PDE 0x000–0x1FF ← 0; PDE 0x340–0x3BF ← 0; PDE 0x200–0x23F ← 0xE3 + n × 4 MB (the 2BL's large pages at 0x80000000); PDE 0x3FF ← 0xFFC000E3 (flash); CR3 reload (0x80018FA8). (Spec 23 §2.9 describes the same.) |
| 5 | 0x8001B199 | copy initialized `.data` from flash (header 0x2C = 0x610 bytes from 0xFFFF5198 to 0x800368E0), zero the rest (header 0x28 = 0x4FDC bytes): the **whole `.data` section 0x800368E0–0x8003BECB** is reset |
| 6 | 0x8001B1BE | quick-reboot flag 0x8003B87C ← 1 |
| 7 | 0x8001C748 | 5838 only: integrity check of the flash `.data` copy (spec 23 §1 row 7; no hardware write; absent in 4627) |
| 8 | 0x8001C7E2, `lidt` 0x8001B1E0 | IDT fix-up and load (0x80036A50, limit 0x1FF) |
| 9 | 0x8001C792 | PCR/PRCB |
| 10 | 0x80018CD9 (argument 1) | debugger init, quick variant |
| 11 | 0x80013CF4 / 0x8001C815 | IRQL → HIGH_LEVEL, kernel init → executive init: HAL phase 0 quick = 0x80CF pulse (suppressed on quick reboot) + PIC re-init (all IRQs masked), MmInit quick, Ob/Ps init, phase 1 (spec 02 §2, spec 23 §2.1) |
| 12 | 0x8001B206–0x8001B218 | `sti`, IRQL → DISPATCH, then the idle loop (0x8001B21A) |

### 1.1 Shutdown notifications
- **Record** (passed to ord 47): {routine, signed priority, LIST_ENTRY}. Register (second argument ≠ 0): inserted before the first listed entry with a lower
  priority (higher priority runs first; equal priorities in registration order). Unregister (0): removed if present. Both at DISPATCH_LEVEL.
- **Registrations inside the kernel image** — only the INIT-resident graphics and sound libraries of the cold-boot animation:
  | Register / unregister site | Routine | What the routine does |
  |---|---|---|
  | 0x80046363 / 0x80046810 | 0x800457F0 | NV2A PMC interrupt enable (0xFD000140) ← 0, then the graphics device teardown 0x80046681 (§6) |
  | 0x80052496 / 0x800521C8 | 0x800523AE | sound teardown 0x80052181 (unregisters itself) |
  | 0x80052A59 / 0x8005287A | 0x8005263E | stop every voice list, then 0x800522E2 |
  These are removed by their own teardowns when the animation ends; INIT is freed before the title starts (spec 23 §2.8), so (inferred) the list is empty
  of kernel entries while a title runs.
- **No IDE, disk, DVD, file-system-cache, USB or NIC code in the kernel registers a shutdown notification.** Any entry present at a quick reboot was
  registered by the running title's libraries (not examined).

### 1.2 Every other routine (for comparison)
Routine 4 first writes SMC scratch 0x1B ← 2. Then (0x8001540C): if the IRQL is below DISPATCH_LEVEL, SMC 0x02 ← 1 (reset); port 0xCF9 ← 0x0E; spin. **No
notifications, no wait, no IDE command.** (Spec 12 §2 already states this.)

## 2. A transfer in flight when the quick reboot is requested
**The stock kernel does nothing about it.** Nothing on the path of §1 reads or writes an IDE port, waits for the channel, the device queue or the I/O count,
flushes the file-system cache or the drive's write cache, or stops the bus master.
| Mechanism | Used on a quick reboot? |
|---|---|
| Waiting for the IDE channel to be idle / the current command or 128 KB chunk to finish | no |
| Bus-master stop (0xFF60 ← 0) | no (only the ISR, the command timeout and nothing else write it, §4) |
| Channel reset (SRST) | no |
| Draining the I/O queue / the must-complete count | no |
| File-system cache (Fsc) flush | no — and there is nothing to flush: FATX writes metadata through at once (spec 16) |
| Drive FLUSH CACHE (0xE7) / STANDBY IMMEDIATE (0xE0) | no (only the SMC power-off path, §2.2) |
| `HalIsResetOrShutdownPending` | **false on a quick reboot**: `HalReturnToFirmware` never sets it (§2.1) |

What therefore happens to an in-flight command:
1. A title thread calls `HalReturnToFirmware(2)` at PASSIVE_LEVEL, so no IDE port sequence can be half-written: commands are programmed by StartIo / the DPC
   at device IRQL (`IdexChannelObject.InterruptIrql`, 0x8003BD38) and finish before the passive thread runs. The channel is either idle, between two 128 KB
   chunks (completion DPC queued), or **a command is executing in the drive with the bus master running** (0xFF60 = 0x09 read / 0x01 write).
2. `cli` (§1 step 3) and the `.data` reset (step 5) **forget the IRP, the channel state and the queue**: `IdexChannelObject` (0x8003BD20–0x8003BDE0:
   InterruptRoutine, FinishIoRoutine, CurrentIrp, the device queue, Timeout, the timer and DPCs), the must-complete count 0x80037838 and the shutdown state
   0x80037820 all lie inside `.data` and are zeroed. Pool (where the IRP lives) is rebuilt by MmInit. The drive and the MCPX bus master are not told.
3. The drive keeps going: an executing DMA command transfers the rest of its sectors into the physical pages its PRD table named (IDEXPRDT 0x800367C0 is not
   in `.data`; it is only rewritten by the next transfer), then raises INTRQ. While IRQ 14 is masked (PIC re-init) or before the ISR is reconnected, the
   interrupt is not taken. (Inferred from ATA/bus-master behaviour; the kernel holds no state about it.)
4. When IRQ 14 is reconnected (§3 step 6) and an interrupt is taken with no command outstanding, the ISR (0x80024D3B) finds InterruptRoutine = 0, **reads the
   status register 0x1F7 (which clears the drive's INTRQ) and claims the interrupt**; it writes nothing (0xFF60 / 0xFF62 stay as they are).
5. The first command after the restart (§3.1) first waits for the drive: master BSY clear and DRDY set, up to 1000 × 100 µs (**100 ms**; slave for ATAPI:
   BSY clear, 100 ms). If an orphaned command is still running past that, the command fails with STATUS_IO_TIMEOUT and goes through the timeout recovery of §4
   (SRST for the disk, ATAPI DEVICE RESET for the DVD, then retries).
6. The bus master is reprogrammed by that first command without first being stopped: PRD table address → 0xFF64, 0xFF62 ← 0x06 (clear the error and
   interrupt bits), task file, command, 0xFF60 ← 0x09 / 0x01. So a bus master left active by an orphaned command (start bit still 1) is overwritten in place.

### 2.1 `HalIsResetOrShutdownPending` (0x80014FBA) and who sets it
It returns "byte 0x80037820 ≠ 0". The byte holds the SMC register of a pending final write (0x02 or 0x0D), 0x8003780C its value. Writers, all SMC/shutdown
code: AV pack changed (0x80014937: 0x02, 1), eject while a title runs (0x80014A91: 0x02, 1), power button (0x80014B30: 0x0D, value from the event),
new disc while a title runs (0x80014C50: 0x0D, 8), power-off (0x80014CFA: 0x02, 0x80), `HalInitiateShutdown` (0x80014FDD: 0x02, 0x80). **Not set by
`HalReturnToFirmware` (any routine).**
Readers in the I/O path (all abort with **STATUS_REQUEST_ABORTED 0xC0000240** through 0x80024E48, which completes the current IRP and starts the next packet):
| Site | Effect when pending |
|---|---|
| disk StartIo 0x800247DD | the new disk IRP is aborted before any port access |
| CD-ROM StartIo 0x800267F9 | the new DVD IRP is aborted |
| disk chunk advance 0x80024490 | a transfer longer than 128 KB stops after the chunk that just finished (that chunk's data is in memory) |
| channel reset 0x80024216 / DVD reset 0x80025230 | the reset is not done; the IRP is aborted |
| `IofCompleteRequest` 0x8001664F | see §2.2 |

### 2.2 The real shutdown/reset path waits for disk I/O (for comparison; not used by a quick reboot)
- Every disk IRP that reaches the disk StartIo (read, write, flush, verify, pass-through) is first passed to `IoMarkIrpMustComplete` (0x800247F3 →
  0x800161AA): stack-location flag 0x02 is set once and the **must-complete count** 0x80037838 is incremented. DVD IRPs are not counted.
- `IofCompleteRequest` (0x8001660E): completing an IRP with that flag decrements the count; when it reaches 0 and a reset/shutdown is pending, it continues
  the shutdown (0x80014814).
- SMC shutdown sequence (0x800148F0): SMC 0x0B ← 1 → AV outputs off (Xcalibur 0xE0 reg 4 ← 0x0F; Focus 0xD4 reg 0xA0 ← 0x020F; Conexant: port 0x80D3 ← 5,
  0x8A reg 0xBA ← 0x3F) → **if the must-complete count is 0, continue, otherwise wait for the last must-complete IRP to complete** (no time limit of its own;
  each disk command is bounded by its 10 s timeout and the retries of §4) → for power-off (0x0D ← 1 or 0x02 ← 0x80) only: FLUSH CACHE 0xE7 then STANDBY
  IMMEDIATE 0xE0 to the master by polled command (0x80024862; select 0xA0, BSY clear ≤ 10000 × 100 µs before and after the command), then wait until
  `KeTickCount` has advanced 125 (ms) → the final SMC write, retried until it succeeds → `cli; hlt`. A reset (0x02 ← 1) skips the flush/standby. The
  sequence starts by jumping into the idle loop (0x8001B25C), so the rest runs from DPCs.

## 3. The quick-reboot path of the IDE channel init
`IdexChannelInit` (5838 0x80025146; 4627 0x80024ECC, confirmed) runs at phase 1 on every boot:
| # | Step | Ports |
|---|---|---|
| 1 | StartIo / StartNextPacket handlers (0x80024DC2 / 0x80024D5B) into the channel object; device queue 0x8003BD44 | — |
| 2 | timer DPC 0x8003BD54 (routine 0x800250A6); timer 0x8003BD8C; armed **1000 ms periodic** (0x80024BCE) | — |
| 3 | finish DPC 0x8003BD70 (routine 0x80024D52) | — |
| 4 | **0x3F6 ← 0x00** (nIEN = 0: device interrupts enabled; SRST = 0) | **the only port write** |
| 5 | `HalGetInterruptVector(14)`; interrupt object 0x8003BDB4 (ISR 0x80024D3B, latched); connect; failure → fatal 0x20731 | (PIC mask, ELCR by the HAL) |
| 6 | PRD table physical address (`MmGetPhysicalAddress(0x800367C0)`) → 0x8003BD50 | — |
| 7q | quick reboot (flag 0x8003B87C): disk 0x80024AB0, then DVD 0x80026919, return | below |
| 7c | cold boot: disk 0x8005539B, DVD 0x80055688 (spec 04 §2, §3) | |

- **Disk, quick (0x80024AB0): no port access, no command, no status check.** Creates the `\Device\Harddisk0` object and the whole-disk Partition0 device (flags
  \|= 0x44 \| 1, alignment 1, sector size 0x200, length = persisted sector count 0x8003C2F0 (STICKY) × 512), clears the initializing flag, stores it at
  0x8003BE40. Failures → fatal 0x30760 / 0x30767 / 0x30774.
- **DVD, quick (0x80026919):** creates `\Device\CdRom0` (sector size 0x800) and `\??\CdRom0:`; device pointer 0x8003BD00. Then, **only if the title-disc
  flag 0x8003C350 (STICKY) was set**: clear it and up to 5 times send MODE SENSE(10) page 0x3E, 28 bytes (0x8002611C: SCSI pass-through IOCTL 0x4D014,
  CDB 0x5A 0x00 0x3E …, length 0x1C; byte 11 must be 1); keep the flag as soon as one reply passes and has bytes 10 and 12 non-zero. This is **the only
  drive command issued by the quick-reboot path**; it goes through the normal ATAPI path (§3.1), i.e. with the 100 ms slave not-busy wait and the timeout /
  DEVICE RESET recovery of §4. No flag → no DVD command.
- Not done on a quick reboot (cold only): the 30 s ready polls, SET FEATURES, IDENTIFY, security unlock, INITIALIZE DEVICE PARAMETERS, the refurb count, the
  DVD check-verify settle loop, any SRST.
- PCI configuration of the IDE function (spec 23 §3.1 row 5, BAR4 0xFF61, timings) is **not repeated** (HAL phase 0 quick = PIC only).

### 3.1 Port order of the first commands after the restart (same as any command)
**Disk DMA read/write** (0x800244DB, at device IRQL): 0x1F6 ← 0xA0; poll 0x1F7 until BSY clear and DRDY set, 1000 × 100 µs (timeout → STATUS_IO_TIMEOUT, §4);
PRD entries (contiguous 0x80024BF1 or scatter/gather 0x80024CA0, 64 KB-boundary split, end bit 0x80000000), 0xFF64 ← table physical address, 0xFF62 ← 0x06;
0x1F6 ← 0xE0 \| LBA[27:24]; 0x1F3, 0x1F4, 0x1F5 ← LBA bytes; 0x1F2 ← length / 512; 0x1F7 ← 0xC8 (read) / 0xCA (write); 0xFF60 ← 0x09 / 0x01. Interrupt expected
(bus-master flag set), timeout 10 (seconds).
**ATAPI packet** (0x800253BD): 0x1F6 ← 0xB0; poll 0x1F7 until BSY clear, 1000 × 100 µs; if there is data: PRD as above (0xFF64, 0xFF62 ← 0x06); 0x1F1 ← 1 (DMA)
or 0; 0x1F7 ← 0xA0 (PACKET); poll until BSY clear and DRQ set, 1000 × 100 µs; 12-byte CDB as six words to 0x1F0; with data 0xFF60 ← 0x09 (from the drive)
/ 0x01 (to it). Timeout 12 (seconds).
**Disk completion** (ISR 0x800242E2): read 0xFF62; if a bus-master interrupt was expected and bit 2 is clear → not ours, return; else 0xFF60 ← 0, 0xFF62 ←
0x04. Read 0x1F7; 0xFF60 ← 0, 0xFF62 ← 0x04 again; status: ATA ERR or bus-master error bit 1 → STATUS_IO_DEVICE_ERROR (0xC0000185); bus-master active bit 0
still set → STATUS_DATA_OVERRUN (0xC000003C); else success; queue the finish DPC.

## 4. Every IDE software reset, and the other resets
There is **one SRST routine** in each kernel (5838 0x80024216, 4627 0x80023FA7) and the constant 0x3F6 appears nowhere else except the `0x3F6 ← 0` of the
channel init.
| Item | Value |
|---|---|
| Entered | at device IRQL, with a current IRP |
| Pending reset/shutdown | no pulse; IRP aborted (0xC0000240) |
| Pulse | 0x3F6 ← 0x00, stall 50 µs; **0x3F6 ← 0x04 (SRST)**, stall 100 µs; 0x3F6 ← 0x00, stall 50 µs (stalls: `KeStallExecutionProcessor`) |
| Then | lower to DISPATCH; channel timer re-armed **100 ms periodic**; PollResetCompleteRoutine ← 0x800241F5; Timeout ← **250** |
| Poll (each 100 ms tick, 0x80025045) | done when slave status (0x1F6 ← 0xB0, read 0x1F7) has BSY clear **and** master status (0x1F6 ← 0xA0, read 0x1F7) has BSY clear and DRDY set; the slave read also sets the "DVD was reset" flag 0x8003BCC8 |
| Poll success | timer back to 1000 ms; retries (0x8003BD3D) < maximum (0x8003BD3E = 4) → retries + 1 and the current IRP's StartIo runs again; otherwise complete with 0xC000016B |
| Poll timeout | 250 ticks (**25 s**) without success → timer back to 1000 ms, complete with 0xC000016B |

When it is used (callers):
| Condition | Site |
|---|---|
| A disk read / write / verify / flush ends with STATUS_IO_TIMEOUT (0xC00000B5): the 100 ms ready wait before the command failed, or no interrupt within the 10 s timeout (timer DPC 0x800250A6: InterruptRoutine cleared, **0xFF60 ← 0, 0xFF62 ← 0x04**, IRP status IO_TIMEOUT, FinishIo) | disk error handler 0x80024360 → 0x8002437A |
| Disk IDE pass-through (IOCTL 0x4D028): after selecting the drive, BSY does not clear within 10000 × 100 µs (1 s), or DRQ does not come within 1 s when the host sends data | pass-through 0x80024F0D, reset routine passed as its argument (0x80024835) |
| Any other disk error (device error, overrun) | **no reset, no retry**: completed with the error |

DVD equivalent (not SRST): **ATAPI DEVICE RESET** (0x80025230): pending → abort; else 0x1F6 ← 0xB0, 0x1F7 ← 0x08; timer 100 ms; poll 0x80025217 (slave BSY
clear; sets 0x8003BCC8), Timeout 250; same success/retry/timeout handling. Used on STATUS_IO_TIMEOUT of a DVD command (0x800257AD) and by the DVD
pass-through (0x80026895). After a reset, the first CHECK CONDITION with ASC 0x29 (power-on/reset occurred) while 0x8003BCC8 is set clears the flag and
re-issues the command (0x80025609).
Timer DPC detail (5838): when InterruptRoutine is the cold-boot init routine 0x800554F3 the countdown is skipped (those commands wait on an event for 1 s
instead, 0x80055511); 4627's DPC (0x80024E36) has no such exception.

## 5. NIC
**The stock kernel leaves the NIC alone, on a quick reboot and on a cold boot, apart from the PCI set-up at cold boot.**
| Where looked | NIC access |
|---|---|
| Quick-reboot path (§1: `HalReturnToFirmware`, 0x8001B18C, 0x8001ECFB, re-init) | none |
| HAL phase 0, quick (pulse + PIC) | none |
| HAL phase 0, cold, PCI (0x80062E24) | PCI 0:4 "full" read-modify-write of the 256-byte space: **command \|= 7 (I/O, memory, bus master)**, BAR0 ← 0xFEF00000, BAR1 ← 0xE001, interrupt line 4, pin 0 (spec 23 §3.1 row 6). The only PCI write to device 4; never on a quick reboot. All PCI access goes through `HalReadWritePCISpace` and no other kernel caller targets device 4. |
| MMIO 0xFEF00000 (every immediate reference) | only in `PhyInitialize` (0x8002DBB3) and `PhyGetLinkState` (0x8002DD5A): **MII control 0x190 and data 0x194 only** (MII busy 0x8000 waits in 50 µs steps, ≤ 0x2000 µs; PHY register 0x18 bit 8 cleared unless McpRevision = 0xA1). **The kernel never calls either**: they are exports for titles' network stacks. |
| NIC registers 0x000, 0x004 (IRQ status/mask), 0x084, 0x088 (transmit), 0x094, 0x098 (receive), 0x100, 0x104 (rings), 0x144, 0x188, 0x26C | **never referenced** |
| Shutdown notifications | none for the NIC in the kernel |
| IRQ 4 | never connected by the kernel |
So the kernel neither stops, waits for, nor resets the NIC: frames in flight (receive DMA into the title's ring buffers, transmit) continue across a quick
reboot unless the title's own network library stopped the adapter before calling `HalReturnToFirmware` (not examined). 4627: the same (PHY exports
0x8002D718 / 0x8002D8BF, only 0x190/0x194, no kernel caller).

## 6. Other bus-master devices on a quick reboot
The quick-reboot path writes no device register at all (no OHCI, NIC, AC'97, APU or NV2A access); HAL phase 0 quick re-initializes only the PICs (all IRQs
masked until each driver reconnects).
| Device | Kernel code that touches it | On a quick reboot |
|---|---|---|
| USB OHCI 0xFED00000 / 0xFED08000 | PCI BARs at cold boot only; **no OHCI register access anywhere in the kernel** | nothing |
| NIC 0xFEF00000 | §5 | nothing |
| AC'97 0xFEC00000, APU 0xFE800000 | boot sound only (INIT 0x8004D574–0x80052C3F: AC'97 0x12C, 0x130, 0x134, 0x17C; APU 0xFE801000 ← 0xFFFFFFFF on teardown, spec 23) | nothing (the boot sound and its notification belong to the cold boot) |
| NV2A | AV / display-mode code; boot-animation graphics device. Its teardown (0x80046681, also run by its shutdown notification): PMC interrupt enable 0x140 ← 0 (in the notification), wait for the graphics engine to idle, PFIFO 0x2044 ← 0, caches/pushers off (0x3220, 0x2500, 0x3000, 0x3050, 0x3200, 0x3250, 0x3210, 0x3270 ← 0), 0x2508 ← 0, PFIFO interrupt enable 0x2140 ← 0, then restores PFB 0x100200/0x100204, PMC 0x200 and PMC 0x140 to their values from device creation | nothing from the kernel; a title's graphics library would do the same from its own notification |

## 7. In short
1. `HalReturnToFirmware(2)`: drain the shutdown-notification list (each routine once, at the caller's IRQL, highest priority first) → `cli`, boot stack →
   page-directory restore (the "pre-reset hook" 0x8001ECFB / 4627 0x8001EABC: PTE/PDE rewrite and CR3 reload only) → `.data` re-copy and zero → flag → kernel
   re-init. The kernel registers notifications only for the boot animation's graphics and sound; IDE, file system cache, USB and NIC register none.
2. In-flight transfers are neither waited for, aborted nor stopped; the bus master is not touched; no queue drain, no cache flush; `HalIsResetOrShutdownPending`
   stays false. The channel state is zeroed with `.data`; the drive finishes on its own; the first command afterwards waits ≤ 100 ms for not-busy and
   otherwise times out into SRST + retries (≤ 4, ≤ 25 s per reset poll).
3. Quick path port accesses: **only 0x3F6 ← 0** (in the common channel init); no BSY/DRQ check, no SRST, no disk command; the DVD gets up to 5 MODE SENSE
   page 0x3E only if the title-disc flag was set.
4. One SRST routine (0x3F6: 0, 50 µs, 4, 100 µs, 0, 50 µs; then a 100 ms poll for slave not busy and master not busy + ready, ≤ 250 polls), used after disk
   command timeouts and failed pass-through busy/DRQ waits; the DVD uses ATAPI DEVICE RESET (0x08) the same way.
5. No other bus master is stopped by the kernel's quick-reboot path.
6. The NIC is configured only by the cold-boot PCI write (command \|= 7, BARs, IRQ 4); its MAC registers are never touched; only the MII registers, and only
   through two exports the kernel itself never calls.

## 8. 4627 against 5838
Identical behaviour except where noted.
| Item | 5838 | 4627 |
|---|---|---|
| `HalReturnToFirmware` / reset helper | 0x8001542D / 0x8001540C | 0x80015299 / 0x80015278 |
| `HalRegisterShutdownNotification`, list head | 0x80015385, 0x80036A40 | 0x800151F1, 0x80035A6C |
| quick reboot, boot stack, flag | 0x8001B18C, 0x8003A5F0, 0x8003B87C | 0x8001AF9C, 0x800395F0, 0x8003A87C |
| pre-reset hook (page directory) | 0x8001ECFB | 0x8001EABC (same values) |
| flash `.data` integrity check | 0x8001C748 | absent |
| `IdexChannelInit`; quick disk / DVD; MODE SENSE | 0x80025146; 0x80024AB0 / 0x80026919; 0x8002611C | 0x80024ECC; 0x80024844 / 0x80026457; 0x80025E57 |
| IdexChannelObject, PRD table | 0x8003BD20, 0x800367C0 | 0x8003AD20, 0x8003CA00 |
| SRST routine, poll, caller | 0x80024216, 0x800241F5, 0x8002437A | 0x80023FA7, 0x80023F86, 0x8002410B |
| ready wait before a disk command | 0x80024275 | 0x80024006 |
| timer DPC | 0x800250A6 (skips 0x800554F3) | 0x80024E36 (no exception) |
| `HalIsResetOrShutdownPending`, byte | 0x80014FBA, 0x80037820 | 0x80014E29, 0x80036820 |
| pending checks: completion, chunk, disk StartIo, CD StartIo, DVD reset | 0x8001664F, 0x80024490, 0x800247DD, 0x800267F9, 0x80025230 | 0x800164C8, 0x80024221, 0x8002456E, 0x80026337, 0x80024FAF |
| `IoMarkIrpMustComplete`, count | 0x800161AA (call 0x800247F3), 0x80037838 | 0x8001601A (call 0x80024584), 0x80036834 |
| shutdown start / drain / final; flush+standby | 0x800148F0 / 0x8001487F / 0x80014814; 0x80024862 | 0x80014867 / 0x80014846 / 0x800147DB; 0x800245F6 |
| AV off in the shutdown sequence | encoder outputs off (Xcalibur / Focus / Conexant) | port 0x80D3 ← 5 only |
| kernel's shutdown-notification registrations | 0x80046363, 0x80046810, 0x800521C8, 0x80052496, 0x8005287A, 0x80052A59 | 0x80046AC3, 0x80046F70, 0x80052928, 0x80052BF6, 0x80052FDA, 0x800531B9 |
| `PhyInitialize` / `PhyGetLinkState` | 0x8002DBB3 / 0x8002DD5A | 0x8002D718 / 0x8002D8BF |
Corrections to earlier specs (made there): spec 02's "pre-reset hook, purpose not established" is the page-directory restore. Spec 04 §5 "error → retry (≤ 4)": only a
timeout is retried, after a reset; spec 04 §5a pass-through "else reset the channel … STATUS_REQUEST_ABORTED": the reset is followed by retries and
0xC000016B on failure; STATUS_REQUEST_ABORTED is the pending-shutdown abort.

## 9. Unknown
- What the title's `XLaunchNewImage` (XAPI) does before `HalReturnToFirmware(2)`: whether it waits for outstanding I/O, flushes or dismounts volumes, and
  which notifications the title's libraries (graphics, sound, USB, network) register and what they stop. Needs a study of a title's library code.
- What the MCPX bus master and the 8259 do with an orphaned command's completion (an edge on IRQ 14 while masked; writing 0xFF64 with the start bit still
  set): hardware behaviour, to be observed, not in the kernel.
- The meaning of 0xC000016B as the reset-failure status (taken as a value only).
- Whether a drive still busy with an orphaned command for more than 100 ms is common in practice (the stock design relies on the timeout recovery).
