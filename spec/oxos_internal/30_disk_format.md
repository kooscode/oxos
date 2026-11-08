# 30. Formatting the hard disk as exFAT; deleting all partitions (OXOS's own)

Status: implemented 2026-11-07. Code: `kernel/src/storage/disk_format.cpp` (GPT, partition delete, blank check),
`kernel/src/filesystems/hddfs/format_exfat.cpp` (the exFAT volume), `kernel/src/storage/disk_layout.cpp` (the exports
and the format at boot), `kernel/include/xk/disk_format.h`; the FTP server's `SITE FORMAT-EXFAT` and
`SITE DELETE-ALL-PARTITIONS` (`tools/ftp_server`).

A new or unwanted disk becomes an OXOS exFAT disk (spec/oxos_internal/05) without a PC: from the FTP server, or by
itself at boot, after a countdown, when the disk is blank or holds something other than FATX or exFAT.

## 1. The exports
Two exports of OXOS's own, at ordinals the reference kernel does not use (its table ends at 366; the export table
grows by 35 slots). Titles find them by ordinal in the kernel's export table at 0x80010000, so a title that uses them
still runs on another kernel (the FTP server reports "needs OXOS").

| Ordinal | Export | |
|---|---|---|
| 400 | `NTSTATUS NTAPI OXOSFormatDisk(PCSZ KeepPath)` | formats the hard disk as exFAT (§2); KeepPath (optional): a file on the hard disk, copied to the same place on the new disk |
| 401 | `NTSTATUS NTAPI OXOSDeleteAllPartitions(ULONG CountdownSeconds)` | deletes all partitions (§3); the next boot formats the blank disk after CountdownSeconds (§4) |

- Both run at PASSIVE_LEVEL in the caller's thread and hold the layout lock while they write.
- **KeepPath** is `\Device\Harddisk0\PartitionN\...` with N one of the drives an exFAT disk has (1 E:, 2 C:, 3–5 X:–Z:,
  6 F:). The file (at most 64 MB) is read into memory before anything is written, so a bad path or a read error
  fails the call with the disk untouched. Each part of its path: at most 60 printable ASCII characters.
- **After either call** the old partitions refuse writes (STATUS_MEDIA_WRITE_PROTECTED) until the next boot: their
  file systems' state describes the old layout. Only the whole-disk device writes. The caller restarts: the FTP server
  launches its kept XBE (a quick reboot reads the new layout), or reboots.

## 2. The format
- **GPT** (protective MBR, both headers, both 128-entry arrays with their CRCs): one basic-data partition named
  "OXOS" from sector 2048 up to the backup GPT, in whole MiB. Sectors 34–2047 are zeroed (the reserved-area copy at
  1024–2047, spec/oxos_internal/05 §2, starts empty). GUIDs come from a generator seeded by the volume serial (the
  system time).
- **exFAT** (as the exFAT specification lays it out): 32 KB clusters below 32 GB, 128 KB above; FAT at 2048 sectors
  into the partition, one FAT; the cluster heap aligned to a cluster. Clusters from 2: the allocation bitmap, the
  up-case table, the root, then one cluster per folder and the kept file, each chained in the FAT (the driver insists
  on a chained bitmap). The FAT is zeroed past the last used cluster.
- **Up-case table:** OXOS's own rules (Basic Latin, Latin-1, Latin Extended-A, Greek, Cyrillic, full-width Latin),
  stored compressed (0xFFFF n: n characters map to themselves), with its checksum.
- **Root:** volume label "OXOS", the bitmap and up-case entries, and the folders C, E, F, X, Y, Z, OXOS (the folders
  the kernel would otherwise create at boot) with the kept file's folders; timestamps from the system time.
- **Order:** up-case table, FAT and bitmap, directories, the kept file, the backup boot region, the main boot region,
  then the GPT (backup first): until the protective MBR is written last, the disk is not taken for exFAT.

## 3. Deleting all partitions
Zeroes everything a layout is recognised by, not the whole disk (a full wipe of a large disk would take hours):
the first 2 MiB (MBR or GPT, a whole-disk exFAT's boot regions, the reserved area, the start of a partition at 2048),
the last 2048 sectors (the backup GPT, PARTINFO tables), and the first 64 KB of each retail FATX partition (X: Y: Z:
C: E: and the automatic F:) and of the disk's own FATX partitions (a PARTINFO table's, the automatic G:). FATX disks
have no partition table: their partitions sit at fixed places and are found by their `FATX` headers, so those headers
must go too. Any PC disk (NTFS, ext4, ...) has its table in the first and last MiB.

Then it writes the **blank-disk marker** in sector 1023 (zeroed by the delete, not read by the blank check, zeroed
again by a format): `OXOS BLANK DISK\0` and the countdown in seconds (4 bytes, little-endian, at most 600).

## 4. Formatting at boot, after a countdown
Before the layout is read (the first partition open):
1. **Blank disk:** every sector a layout is recognised by reads back as zeros: 0, 1, 2048, the last and the one 8
   before it, and the first sectors of the retail C: and E:. The C: and E: checks matter: the first sectors of a
   retail FATX disk can be all zeros, and such a disk must never be taken for blank. A read that fails means not blank
   (a locked disk is never touched). Countdown: the marker's seconds (§3), else the default.
2. Otherwise the layout is read; a **foreign disk** is then a PC partition table without a usable exFAT volume
   (spec/oxos_internal/05 §2) or a PC file system's boot sector at sector 0 (no table), with no `FATX` header at the
   retail C: or E: (a FATX disk is never formatted by itself, whatever else it holds). Countdown: the default.
3. **The default countdown** is 30 s in release builds (made with `VERSION=`) and 5 s in development builds.
4. **The countdown screen** (the splash ends first): the seconds in large digits, "Blank Hard Disk Found" or
   "Unsupported Hard Disk Found", the disk's model and size, what it holds ("nothing (blank)", "MBR: NTFS",
   "GPT: EFI, NTFS", "FAT32, no partition table"; `IdexDiskDescribe`: the file systems of the first four partitions
   by their signatures, else by their partition type; an EFI system partition named as such), and "Turn the console
   off now to keep the disk." It counts down on the screen and the serial port; turning the console off keeps the
   disk as it is. Then the format (§2) and the layout is read again.
5. With netboot, the netboot title is then installed into `E:\` on the new disk as usual; without a dashboard on it
   the kernel stops with 13, "No Dashboard Found" (spec/oxos_internal/29).

## 5. The FTP server
- `SITE FORMAT-EXFAT YES`: OXOSFormatDisk with the server's own XBE (XeImageFileName, when on the hard disk), then a
  launch of that XBE: the server is back on the new disk.
- `SITE DELETE-ALL-PARTITIONS YES [SECONDS]`: OXOSDeleteAllPartitions(SECONDS, default 0: the next boot formats at
  once), then a cold reboot.
- Without `YES` each only says what it would do. Every SITE command, every refusal and failure, and what was done
  are shown on the screen (and the debug serial port).

## 6. Checks
- Unit tests (`tests/unit/test_format.cpp`): 8 GB and 2 TB images pass `fsck.exfat`; the folders and a kept file read
  back; both GPT headers and entry arrays check out; blank / formatted / partitions deleted; a FATX disk is not blank
  and the delete clears its headers; a failed read is not blank; bad paths refused.
- Unit tests also: the marker sets the countdown (0, 5, 30, capped at 600; a format clears it); `IdexDiskDescribe`
  names MBR + NTFS, MBR + ext4, an MBR type alone, FAT32 without a table, our own GPT + exFAT, a blank disk.
- `make test_format` (xemu, a development build): a new blank 16 GB disk (5 s countdown, the screen shown), a blank one
  with a 0 s marker (at once), and an MBR disk with NTFS (5 s, "Unsupported Hard Disk Found", "Holds: MBR: NTFS"); each
  formatted (GPT, partition at 2048, the seven folders, a clean volume for `tests/exfat.py`), then 13. `make test_dash`, `test_exfat`, `test_fatal` unchanged.
- Console (debug kit, Crucial M4 256 GB SSD, FATX, 2026-11-07): `SITE FORMAT-EXFAT YES` formatted it in 0.4 s and
  the server came back from `E:\ftp\ftp.xbe` on the new disk; a 3 MB file uploaded and read back. Then
  `SITE DELETE-ALL-PARTITIONS YES`: deleted in 0.1 s, reboot, "blank: formatting it as exFAT" (0.35 s), the netboot
  title installed and the server running again.
- Console, the countdown: `SITE DELETE-ALL-PARTITIONS YES 10` on the same kit: the next boot showed "Blank Hard Disk
  Found" counting down from 10 on the TV (and the serial port), formatted the disk (0.35 s), and the netboot FTP
  server came back.
- Cost: resident code 10.6 KB (release `.text` 0x247FC → 0x27160, three pages): the formatter 7.4 KB, the
  countdown screen and the disk description 3.2 KB.
