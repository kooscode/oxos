# Spec 04 — IDE channel, disk, DVD, partitions, FATX mount (stock behaviour)

Clean-room behavioural spec (no decompiled code). **Status: IDE channel + disk + partitions implemented (milestone 3c-1).** **Project rule: reproduce this exactly**
(LBA28, fixed partition table) so stock drives and stock-formatted disks work. Extensions come later.
Evidence: batch_decompile `ev_542cc685…` (0x80024ecc), `ev_9e7fc395…` (disk/DVD init),
`ev_7012e098…` (FATX mount, transfer start, StartIo); objdump notes in `xboxkrnl-4627-notes.md`.
Names marked (inferred) are not symbols in the binary.

## 1. IDE channel init (`IdexChannelInit`, inferred, 0x80024ecc; runs on every boot)
- One channel: legacy primary ports 0x1F0–0x1F7 and control port 0x3F6; bus-master registers at **I/O 0xFF60**
  (set in spec 02 PCI config).
- Initialize the device queue, a completion DPC, a **synchronization timer** with a timeout DPC, and
  arm the timer (1000 ms).
- `out 0x3F6, 0`: enable device interrupts.
- Interrupt: **IRQ 14**, using `HalGetInterruptVector(14)`, latched mode; connect the ISR. On failure the kernel halts.
- The **PRD (DMA descriptor) table** lives in the dedicated **`IDEXPRDT` section** (VA 0x8003CA00, 0x108 bytes).
  Its physical address is taken once with `MmGetPhysicalAddress`.
- Cold boot → disk init (§2), then DVD init (§3). Quick reboot → only recreate the device objects
  (§2.4 and §3.4).
- The channel object is exported as `IdexChannelObject` (ord 357, 0x8003AD20). Its fields include the
  StartIo/timeout handler pointers, current IRP, IRQL, the PRD physical address, and the timeout
  counter (default 10). These layouts are ABI if any title touches them. Unverified, so low priority.

## 2. Hard disk (device 0, master), cold boot (0x80055b01)
1. Unless boot-flags bit 0 is set: poll the drive until ready, **up to 600 × 50 ms (30 s)**.
   Timeout → **fatal error 7**.
2. At device IRQL: **SET FEATURES / transfer mode** to PIO mode 4 (value 0x0C), then **UDMA mode 2**
   (0x42). Failure → **error 9** path.
3. **IDENTIFY DEVICE (0xEC)**. Failure → **fatal error 8**.
4. Copy the IDENTIFY **model** (40 bytes) into `HalDiskModelNumber` (ord 41) and the **serial**
   (20 bytes) into `HalDiskSerialNumber` (ord 42), as ANSI strings with trailing space trimming (helper 0x80055947).
5. **Capacity** = IDENTIFY words 60–61 (LBA28 user-addressable sectors). Must be **≥ 0xEE8AB0
   (8 GB)**, otherwise **fatal error 9**. Store it in `IdexDiskLengthInSectors` (0x8003B2D8).
6. **Partition 0 / whole disk**: create the directory `\Device\Harddisk0`, then the disk device
   (sector size 512, alignment 1, length = sectors × 512). It goes in slot 0 of the partition device table
   (0x8003AE40[6]).
7. **ATA security** (IDENTIFY word 128):
   - security enabled and **not locked** → set the "disk unlocked/OK" flag (0x8003B2DC = 1)
   - security enabled and **locked** → password = **HMAC-SHA1(XboxHDKey, model, serial)** via `XcHMAC`
     (20 bytes, zero-padded to 32) → SECURITY UNLOCK (0x800559f8)
   - not enabled → flag stays 0. The XBE loader then refuses titles that don't allow "non-secure hard disk"
     with **fatal error 5** (spec 05).
   Unlock details (0x800559f8, confirmed): a 512-byte block, word 0 = 0 (user password), the 32-byte password at
   +2; sent as IDE pass-through (IOCTL 0x4D028) to Partition0 with command **0xF2**, HostSendsData = 1. Error (IRP
   failure or status ERR) → **fatal error 6**, unless the console is a manufacturing unit (region bit 31). Success →
   the disk-OK flag. Then, at the channel IRQL: IDENTIFY again, and **INITIALIZE DEVICE PARAMETERS (0x91)** with
   drive/head 0xAF and count 0x3F (16 heads, 63 sectors); failure → **fatal error 8**.
8. Final step 0x80055ac6: unless boot-flags 0x11 or a manufacturing unit, read the refurb record
   (`ExReadWriteRefurbInfo`, 16 bytes), increment its **power-cycle count** (dword 1) and write it back.
Fatal errors 5/7/8/9 go through the reporter at 0x80055933 (spec 05 §6).

## 3. DVD drive (device 1, slave), cold boot (0x80055d55)
1. Unless boot-flags bit 0 is set: select the slave (`0x1F6 = 0xB0`) and poll until BSY clears, **up to 600 × 50 ms**.
   Timeout → **fatal error 10**.
2. SET FEATURES PIO 4 (0x0C) then UDMA 2 (0x42). Failure → **error 12**.
3. **IDENTIFY PACKET DEVICE (0xA1)**. Failure → **error 11**.
4. Create **`\Device\CdRom0`** (sector size 0x800) and the symbolic link **`\??\CdRom0:`**. The device
   pointer is kept at 0x8003AD00.
   On quick reboot, if the "title disc" flag (0x8003B338) was set, re-check the media up to 5 times and keep the flag only
   if the check passes.
5. Cold boot, boot-flags bit 0x40 clear: wait for the drive to settle by sending **IOCTL 0x24800
   (CDROM check-verify)** to CdRom0 every 200 ms:
   - `STATUS_DEVICE_NOT_READY` (0xC00000A3) → keep waiting
   - `STATUS_IO_DEVICE_ERROR` (0xC0000185) → retry, at most 5 times
   - anything else → done.

## 4. Partition devices (`IdexDiskCreatePartitionDevice`, inferred, 0x80024634)
| # | name | start LBA | length (bytes) |
|---|---|---|---|
| 0 | whole disk | 0 | IdexDiskLengthInSectors × 512 |
| 1 | E: (Partition1) | 0x55F400 | 0x1_312D6000 |
| 2 | C: (Partition2) | 0x465400 | 0x1F400000 |
| 3 | X: (Partition3) | 0x400 | 0x2EE00000 |
| 4 | Y: (Partition4) | 0x177400 | 0x2EE00000 |
| 5 | Z: (Partition5) | 0x2EE400 | 0x2EE00000 |
| ≥6 | → STATUS_OBJECT_NAME_NOT_FOUND (0xC0000034) | | |
Created on demand (name parse of `\Device\Harddisk0\PartitionN`). Device: sector size 512,
flags |= 0x44, cleared the "initializing" flag. If the table slot is already filled, the new device is
deleted and the existing one is used.

## 5. Disk transfers (start routine 0x8002426c, StartIo 0x80024b56)
- StartIo runs at DISPATCH_LEVEL and queues on the channel device queue. If the queue is idle it records
  the current IRP and calls the device's start routine.
- **Per command at most 0x20000 bytes (128 KB = 256 sectors)**. Bigger requests are split. After each
  chunk: length -= 128 KB, buffer += 128 KB, LBA += 0x100, then the next chunk is started. A pending
  reset/shutdown (`HalIsResetOrShutdownPending`) aborts the chain.
- DMA set-up: if the device is scatter/gather capable (device flags bit 2) and the IRP isn't flagged
  0x80 → build PRD entries from the IRP's MDL/page list; otherwise from a contiguous buffer.
- Registers: `0x1F6 = 0xE0 | LBA[27:24]`; `0x1F3/4/5 = LBA[7:0]/[15:8]/[23:16]`; `0x1F2 = bytes >> 9`
  (0 means 256); command **0xC8 READ DMA** for IRP major 2 (read), otherwise **0xCA WRITE DMA**; then
  **bus-master command 0xFF60 = 0x09 (read) / 0x01 (write)**. Set the ISR/timeout handlers, set the
  timeout counter to 10, and lower IRQL back to DISPATCH.
- Note: **Xbox IRP major codes differ from NT** (read = 2 here).

## 5a. Driver details (added for milestone 3c-1)
Evidence: `ev_7550c775…` (channel init, partition create, channel StartPacket, transfer start/finish),
`ev_2dbaa749…` (Harddisk0 parse), the disk driver object at 0x80012348, and the READ/WRITE and DEVICE_CONTROL dispatch.
- **IdexChannelObject layout** (4-byte packed; matches the reference globals at 0x8003AD20):
  InterruptRoutine, FinishIoRoutine, PollResetCompleteRoutine, TimeoutExpiredRoutine, StartPacketRoutine,
  StartNextPacketRoutine, InterruptIrql +0x18, ExpectingBusMasterInterrupt, StartPacketBusy, StartPacketRequested,
  Timeout +0x1C (10 s per command), IoRetries, MaximumIoRetries (4), CurrentIrp +0x20, DeviceQueue +0x24,
  PRD table physical address +0x30, TimerDpc +0x34, FinishDpc +0x50, Timer +0x6C (1 s periodic), Interrupt +0x94.
- **Channel serialisation:** both drives' StartIo go through the channel's StartPacket. If the channel device queue is idle,
  record CurrentIrp, reset the retries, set StartPacketBusy and call the device driver's StartIo. A StartNextPacket requested
  meanwhile runs afterwards.
- **`\Device\Harddisk0`** is not a directory. It is an object with its own parse procedure:
  - empty name → STATUS_ACCESS_DENIED
  - the first element must be `Partition<decimal>` (case-insensitive), number < 20
  - the device is created on demand (table above), and the rest of the name goes to the I/O parse of that partition device
  - unknown partition → NAME_NOT_FOUND, or PATH_NOT_FOUND when more path follows
- **Partition device:** type DISK (mountable), extension 0x24 = {DeviceObject, PARTITION_INFORMATION
  {StartingOffset, PartitionLength, HiddenSectors = start LBA, PartitionNumber, …}}, Flags |= 0x44, SectorSize 512,
  AlignmentRequirement 1.
- **Disk driver dispatch:**
  - CREATE/CLOSE succeed
  - READ/WRITE: unless the stack flag 0x80 (cache request) is set, the buffer must be 2-byte aligned and the length and
    offset 512-aligned, inside the partition. Otherwise STATUS_INVALID_PARAMETER.
    - length 0 → success
    - otherwise Information = length; the stack is rewritten to {absolute LBA, buffer address}; pending
  - FLUSH_BUFFERS: flush the drive cache
  - DEVICE_CONTROL: GET_DRIVE_GEOMETRY 0x70000, VERIFY 0x70014, GET_PARTITION_INFO 0x74004, IDE pass-through 0x4D028
    (queued to the channel); others → STATUS_INVALID_DEVICE_REQUEST
- **IDE pass-through** (StartIo 0x80024c9d, ISR 0x80024c06): buffer `ATA_PASS_THROUGH` {IDEREGS (Features, Count,
  Number, CylLow, CylHigh, DriveHead, Command, HostSendsData), DataBufferSize, DataBuffer} = 16 bytes; the IRP's
  UserBuffer must *be* the input buffer and both lengths ≥ 16, else STATUS_INVALID_PARAMETER. Information = output
  length. Select drive 0xA0 (DriveHead from the caller is not used), wait not-busy (else reset the channel: SRST
  pulse on 0x3F6, then poll; STATUS_REQUEST_ABORTED), write 0x1F1–0x1F5 and the command, then PIO data out if
  HostSendsData (wait for DRQ). On the interrupt: the error, count, LBA bytes and **status** (in the Command byte)
  are copied back; PIO data in when !HostSendsData. Timeout 10 s. The IRP status is success even if the command
  failed.
  - everything else → IoInvalidDeviceRequest
- **Transfer completion:**
  - error → retry (≤ 4) or fail
  - more than 128 KB left → advance (length −= 128 KB, buffer += 128 KB, LBA += 0x100) and restart
  - otherwise success, IofCompleteRequest(boost 1), start the next channel packet
- Our implementation polls IDENTIFY/SET FEATURES at init; the whole-disk Partition0 is flagged RAW_MOUNT_ONLY.
  Milestone 6a: ATA security unlock and the pass-through IOCTL are implemented (ours polls the pass-through at the
  channel IRQL). Our disk init runs on the boot thread, so the unlock calls the pass-through code directly and the
  refurb power-cycle count is done by the title-launcher thread. xemu emulates a locked drive: word 128 goes
  0x0007 → 0x0003, proving the password derivation.

## 6. FATX volume mount (boot-sector processing, 0x80026be3)
Input: partition device (sector size and sector shift from the device) and partition length.
1. **Reserved area** R = max(4096, round_up(sector size, 4096)). If the partition length ≤ R → fail.
2. Read the boot sector at offset 0 (through the FS cache):
   - +0x00 signature must be **'FATX'** (0x58544146)
   - +0x04 volume serial number → stored in the volume
   - +0x08 **sectors per cluster ∈ {1,2,4,8,16,32,64,128}**
   - +0x0C root directory first cluster
3. Bytes per cluster = SPC << sector shift; store its log2. It must be ≥ R (round_up(sector size, 4 KB)).
4. Cluster count N = (partition length >> cluster shift) + 1.
   **N < 0xFFF0 → FATX16 (2-byte FAT entries)**, otherwise **FATX32 (4-byte entries)**.
5. FAT size = round_up(N × entry size, round_up(sector size, 4 KB)). It must be < partition length − R.
6. Layout: **FAT at byte offset R (normally 0x1000)**; **data area at R + FAT size**;
   usable clusters = (length − R − FAT size) >> cluster shift.
7. Open the root directory (0x80026e55). Any failure → **STATUS_UNRECOGNIZED_VOLUME (0xC000014F)**.
The FATX directory entry format, allocation, the FS cache (`Fsc*`), and the rest of the file system are
**not yet specified** (spec 04b).

## Open questions
- 0x80055ac6 (end of disk init), 0x800559f8 (unlock command details), 0x80023f86 (ready test).
- ISR / DPC completion / timeout handling, PIO paths, error retry policy.
- FATX directory entries, FAT chain and allocation, Fsc cache semantics (spec 04b).
