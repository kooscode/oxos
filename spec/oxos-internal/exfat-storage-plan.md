# exFAT storage for OXOS: plan

Design: `exfat-storage-spec.md`. Idea: one PC-friendly exFAT volume, Xbox partitions as folders, FATX kept for
disks shared with other BIOSes; exFAT memory units last.

## Why
- Format and fill the disk on any PC over USB (Windows, macOS and Linux all read and write exFAT), no FATXplorer, no
  FTP, and the same for xemu images.
- No 2 TB volume or 4 GB file limits (FAT32 has both; FATX has the second).
- Simpler large-disk support: no partition table of our own, one volume, as big as the disk.

## What the trace showed (Halo, 2026-10-18)
Everything a title does to the disk goes through kernel calls we control. The only raw (non-file) accesses are:
- the cache record: 512 bytes read and written at byte 0x800 of `\Device\Harddisk0\partition0`;
- formatting its cache partition (Z: = Partition5 for Halo): open the raw partition, query geometry and partition
  info, write a FATX header at offset 0 plus an empty allocation table and root directory, dismount, reopen.
Both are easy to emulate: a small file for the reserved sectors, and "empty the folder" when a FATX header is
written to a virtual partition.

## Phases
1. **GPT and exFAT, read-only.** Read and check the GPT (primary, then backup) or an MBR; mount the exFAT
   volume; open, read and list files; volume queries.
   Written from the published exFAT specification. Test: a host tool builds an exFAT qcow2 (Python writer in
   `tests/`, like `tests/fatx.py`), then `make test` style checks of directory listings and file contents.
2. **Read-write.** Create, write, extend, truncate, rename, delete; allocation bitmap; up-case table; entry-set
   checksums. Test: the existing FATX test cases run against the exFAT image; a PC (Linux `fsck.exfat`) checks the
   image afterwards.
3. **Virtual partitions.** `\Device\Harddisk0\PartitionN` on folders; FATX name rules; bounded sizes and free space
   (spec §6); `Partition0`'s reserved area in disk sectors 1024–2047. Test: the dashboard boots from `/C`, saves
   land in `/E/UDATA`.
4. **Cache-format emulation** (spec §5). Test: Halo and Halo 2 on a fresh exFAT disk: the trace shows the same
   sequence, the format empties `/Z` (or `/X`, `/Y`), the cache files appear there, a second boot reuses them.
5. **Disk recognition and coexistence.** FATX disks unchanged; exFAT disks detected; memory units still FATX. Test: boot
   tests on both disk kinds.
6. **Compatibility pass.** The titles we test (Halo, Halo 2, Half-Life 2, Enter the Matrix, Prince of Persia, Brute
   Force, demo disc) plus the stock dashboard, each run once on exFAT with the tracer; any new raw access goes into
   the spec.
7. **Memory units (last).** Trace an xemu memory-unit image (a title saving, the dashboard's memory-unit screen)
   to confirm spec §9, then exFAT recognition on memory-unit devices, name and format emulation, the format
   setting. Test: saves written under OXOS read back on a PC from an exFAT stick image, and the reverse.
8. **Tools and docs.** A host script that lays out the folders on a freshly formatted exFAT disk (or the kernel does
   it at first boot), README instructions for preparing a disk on a PC, and an xemu disk image ready to use.

## Order with other work
After large-disk support (Cerbios spec 03) or instead of most of it: an exFAT disk needs 48-bit LBA for disks over
128 GiB, but no extended partition tables. 128 MB RAM is independent and can come first.

## Risks
- **Titles with unusual disk use:** raw reads of E:/C:, their own cache handling. The tracer shows these quickly;
  the fallback is to log and fail the raw access, as the spec says.
- **Free-space arithmetic** in titles and the stock dashboard on E:/C: (32-bit wrap): covered by the 2 TB cap; the
  compatibility pass checks it, and the cap can be lowered if a title needs it.
- **Performance of directory walks** for usage accounting on large folders: cache the totals in
  `/OXOS/partitions.ini` and update them incrementally.
- **Power loss:** exFAT has no journal. Keep writes ordered (data, then bitmap, then directory entry) and mark the
  volume dirty while mounted, as the exFAT spec describes, so a PC's check can repair it.

## Decided
- **GPT and MBR** (spec §2): GPT is the documented layout that our tools create; MBR is read too, so disks
  formatted with a PC's defaults work as they are.
- **Free-space reporting** (spec §6): E: and C: report real free space capped at 2 TB (a setting); X/Y/Z their
  retail 750 MB; F: the real free space, no cap.
- **F: is the only extra drive** (spec §2.1): the `/F` folder, with the whole disk's free space; no G: and up.

## Decisions to make
- The default format for new memory units (FATX proposed) and the size cap for USB sticks (1 GB proposed).
- Whether `/E` should be the volume root instead of a folder (shorter paths on the PC, but mixes Xbox and PC files).
