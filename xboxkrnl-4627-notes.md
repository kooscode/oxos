# xboxkrnl.exe 1.0.4627.1 — RE notes

- SHA-256: 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f
- Image base 0x80010000; every section has raw offset == RVA, so **file offset = VA - 0x80010000**.
- Exports are by ordinal only (base 1, 366 entries). XboxKrnlVersion (ord 324) @ 0x80010a10 = 1.0.4627.1.
- Names below are inferred from behavior and are not symbols in the binary.

## IdexDiskCreatePartitionDevice? @ 0x80024634 (arg = partition number)

| Partition | Start LBA (instr VA) | Length in bytes (stored at [ebp-0x20]/[ebp-0x1c]) |
|---|---|---|
| 1 (E) | 0x55F400 (`mov eax` @ 0x80024682) | 0x1_312D6000 (lo @ 0x80024687, hi=edi=1) |
| 2 (C) | 0x465400 (@ 0x80024670) | 0x1F400000 (@ 0x80024675) |
| 3-5 (X/Y/Z) | 0x400 + (n-3)*0x177000 (@ 0x8002465c) | 0x2EE00000 (@ 0x80024667) |
| >=6 | `cmp ebx,6 / jb` @ 0x8002464a -> 0xC0000034 STATUS_OBJECT_NAME_NOT_FOUND | |

Adding F/G partitions: extend the `cmp ebx,6` gate and add a case that computes start=0xEE8AB0
and length=(IdexDiskLengthInSectors - 0xEE8AB0)*512 (or the size from your own partition table).
The function is too small for this in place, so the extra code needs a code cave and a jump.

## Disk size
- IdexDiskInit (INIT) @ 0x80055be2: reads IDENTIFY words 60-61 (LBA28 user-addressable sectors),
  `cmp eax,0xEE8AB0 / jae` (0x80055be8). A smaller disk causes fatal error 9.
  Stores the result in **IdexDiskLengthInSectors @ 0x8003b2d8** (32-bit, LBA28 only).
- Readers: 0x80024484 (IOCTL_DISK_GET_DRIVE_GEOMETRY-style handler), 0x800248ee (whole-disk
  partition0 length = sectors*512).
- For more than 137 GB: read IDENTIFY words 100-103 when word 83 bit 10 (LBA48) is set.

## ATA command issue (LBA28) @ 0x800242c5
- 0x1F6 = 0xE0 | (lba>>24 & 0xF); 0x1F3/0x1F4/0x1F5 = LBA bytes [esi+0xc..0xe]; 0x1F2 = len>>9 (8-bit).
- READ DMA 0xC8 @ 0x800242fb, WRITE DMA 0xCA @ 0x80024302; bus-master start via port 0xFF60.
- LBA48 needs: two writes to each of 0x1F2-0x1F5 (high bytes then low bytes), 0x1F6 = 0x40,
  commands 0x25 (READ DMA EXT) / 0x35 (WRITE DMA EXT). Also check the PIO paths (0x1F0 users near
  0x80024c6b/0x80024d59) and the bounds check @ 0x80024985.

## FATX mount (FatxProcessBootSector?) @ ~0x80026bf0
- `cmp [edi],'FATX'` @ 0x80026c49.
- Allowed SectorsPerCluster (+8) = 1,2,4,8,16,32,64,128 (up to 64 KB clusters) @ 0x80026c55-0x80026c85.
- ClusterSize = spc << [vcb+0x20] (sector shift) @ 0x80026c8b.
- FAT16/32 choice: `cmp clusters+1, 0xFFF0` @ 0x80026cd9 (below means FATX16 with 2-byte entries, otherwise FATX32 with 4-byte entries).
- Fails with 0xC000014F (STATUS_UNRECOGNIZED_VOLUME) when a check does not pass.
- 32-bit cluster indexes with 64 KB clusters can address far more than 2 TB, so the cluster math is
  probably not the first limit. The 32-bit sector math in the IDE driver is. (Inference, not yet verified.)
