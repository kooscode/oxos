# exFAT storage for OXOS: design spec

Status: design, not implemented. Companion plan: `exfat-storage-plan.md`. The title behaviour below was traced with
OXOS's kernel call tracer (Halo, 2026-10-18, §8).

## 1. Goal
A hard disk that any PC can format, read and write over USB: one **exFAT** volume holding everything, with the Xbox's
fixed partitions presented to titles as **folders**. No FATX tools, no FTP, no 2 TB / 4 GB limits. Titles keep using
the same kernel calls and see the same devices as on a retail disk.

Memory units come last (§9). Out of scope: changing what titles do.

## 2. Disk layout
- **Partition table: GPT and MBR** (decided). GPT is the layout OXOS documents and its tools create: 64-bit sector
  numbers, so no partition-table size limit, and a backup table at the end of the disk. MBR is read as well, so a
  disk or USB stick formatted with a PC's default settings works without repartitioning (MBR is limited to 2 TiB
  with 512-byte sectors). Reading MBR adds one sector check and four 16-byte entries.
- **The OXOS volume:** the first GPT entry with the "basic data" type GUID (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7)
  whose boot sector says exFAT (`EXFAT   ` at byte 3). With MBR: the first entry of type 0x07 holding exFAT.
- **Reading GPT** (once, at boot):
  1. sector 0 must hold a protective MBR (an entry of type 0xEE) and the 0x55AA signature;
  2. sector 1 holds the header: signature `EFI PART`, revision, header size, header CRC32, its own sector number,
     the backup header's sector number, first and last usable sectors, disk GUID, the entry array's first sector,
     entry count and entry size (128), entry-array CRC32;
  3. check the header CRC32 (computed with its CRC field zeroed) and the entry-array CRC32 (usually 128 entries of
     128 bytes = 32 sectors); entries hold the type GUID, a unique GUID, the first and last sector (64-bit),
     attributes and a UTF-16 name;
  4. if the primary header or its array fails a check, use the backup header (normally the last sector of the
     disk) and its array; if both fail, the disk is not used as an OXOS disk.
  OXOS never rewrites the table; partitioning is done on a PC or by our host tool.
- **Cost on the Xbox:** the table is read and checked once at boot (about 33 sectors and two CRC32 passes over
  16 KB). After that a partition is only a 64-bit starting sector added to each request; the kernel already carries
  64-bit offsets (NtReadFile/NtWriteFile byte offsets, file sizes) and sector sizes are powers of two, so there are
  shifts, not divisions. Disks above 128 GiB need 48-bit LBA commands (Cerbios spec 03 §1), whatever the layout.
- **Recognition:** at boot the IDE driver reads sector 0. Order:
  1. a retail or Cerbios-style FATX disk (stock spec 04 table, or `****PARTINFO****` in sector 0 / near the end,
     Cerbios spec 03 §2) → today's FATX path, unchanged;
  2. GPT (protective MBR) or MBR with an exFAT OXOS volume → the exFAT layout below;
  3. anything else → as today (fatal error for an unusable disk).
- **Folders** at the root of the exFAT volume, created at boot when missing:

  | Folder | Xbox device | Retail letter | Notes |
  |---|---|---|---|
  | `/E` | `\Device\Harddisk0\Partition1` | E: | saves (UDATA), title data (TDATA), apps |
  | `/C` | `\Device\Harddisk0\Partition2` | C: | dashboard |
  | `/X`, `/Y`, `/Z` | Partition3, 4, 5 | X:, Y:, Z: | title cache partitions (§5) |
  | `/F` | Partition6 | F: | games and apps: the rest of the disk (§2.1) |
  | `/OXOS` | none | none | kernel files: `partition0.bin` (§4), settings |

  Folder names are matched case-insensitively. A user can copy a game folder to `/F/Games/Halo` from a PC and
  launch it from a dashboard as `F:\Games\Halo\default.xbe`.

### 2.1 F: is the only extra drive (decided)
On FATX disks the extended partitions (F:, G:, and Cerbios's J:–N:) exist because each partition has a size limit.
On one exFAT volume every folder shares the same free space, so a single F: already offers the whole disk:
- **F:** (Partition6) is the `/F` folder and reports the volume's full free space (§6). It is a folder, not the
  volume root, so dashboards don't see the system folders (`/E`, `/C`, `/X`–`/Z`, `/OXOS`) as part of F:.
- **G: and up** (Partition7+) are not created on an exFAT disk; opening them fails as for a missing partition.
  Dashboards that offer G: simply don't list it.

## 3. Devices titles see
- `\Device\Harddisk0\Partition0` (the whole disk) and `\Device\Harddisk0\Partition1…N` exist as on a retail disk.
- Each `PartitionN` (N ≥ 1) is a **virtual partition**: a device whose file system is the exFAT driver rooted at that
  partition's folder. Opening `\Device\Harddisk0\Partition1\TDATA\4d530004` opens `/E/TDATA/4d530004`.
- Symbolic links (`\??\T:`, `\??\U:`, `\??\Z:`, `\??\D:` …) work unchanged: they point at those device paths.
- **File-name rules stay FATX's** so titles see the errors they expect: at most 42 characters, the FATX character
  set, case-insensitive, case-preserving. ANSI names are stored as UTF-16 (Latin-1 mapping). Files exFAT allows but
  FATX does not (longer names, other characters, written from a PC) are still listed and opened: names longer than
  42 characters are listed but cannot be created by titles.
- **Timestamps:** exFAT keeps 10 ms resolution and a UTC offset; the driver converts to and from the FATX/NT times
  the kernel APIs return.
- **File sizes:** up to the exFAT limit; the APIs already carry 64-bit sizes and offsets.

## 4. The whole-disk device (Partition0)
Titles open `\Device\Harddisk0\partition0` raw to keep the cache database (traced: one 512-byte read and one write at
byte offset 0x800, sector 4, §8). On an exFAT disk the real sector 4 belongs to the partition table, so:
- byte offsets **0 to 0x7FFFF** (the retail reserved area before X: at sector 0x400) are backed by
  `/OXOS/partition0.bin` (512 KB, created zero-filled);
- other raw reads and writes of Partition0 fail with STATUS_INVALID_PARAMETER and are logged on the debug serial
  port (none were seen);
- geometry and partition-info queries describe a disk of the real size.
The kernel's own uses of the reserved area (for example sector 3) read the same file.

## 5. Cache partitions and title formatting
A title formats its cache partition itself, through kernel calls (traced, §8). On a virtual partition the kernel
recognises that sequence and turns it into "empty the folder":
1. **Raw open** of `\Device\Harddisk0\PartitionN` (no file name, exclusive share access) → succeeds; the handle is a
   raw-volume handle on the virtual partition.
2. **IOCTL 0x70000** (geometry) and **IOCTL 0x74004** (partition information) → describe the virtual partition at its
   retail size (X/Y/Z: 0x177000 sectors = 750 MB; others as reported in §6), 512-byte sectors.
3. **Raw write at offset 0** whose data starts with `FATX` → **format**: record the header (signature, serial number
   at +4, sectors per cluster at +8, root cluster at +12) as the partition's current header, and empty the folder
   (rename it to `/OXOS/trash/<n>`, create a new empty one, delete the old one in the background).
4. **Raw writes after it** inside the metadata area (the allocation table and the root directory: up to the end of
   the first root cluster, 0x1D000 bytes for a 750 MB partition with 16 KB clusters) → accepted and dropped.
5. **FSCTL 0x90020** (dismount) → succeeds; open file handles on the partition are invalidated as for a FATX dismount.
6. The next open of the partition mounts the (empty) folder.
- **Raw reads** of a virtual partition: offset 0 returns the recorded header (or a generated one with a fixed serial
  if none was written); the rest of the metadata area returns what a freshly formatted FATX volume holds; beyond it,
  zeros. (Not seen in the trace; defined so a title that verifies its format gets consistent data.)
- **Any other raw write** (outside the metadata area, or not starting with `FATX` at offset 0) fails with
  STATUS_ACCESS_DENIED and is logged.
- The recorded header and the partition's size live in `/OXOS/partitions.ini`, so they survive reboots.

## 6. Size and free-space reporting
Every virtual partition shares the one exFAT volume, so each reports its own size and free space (decided):

| Partitions | Reported free space | Reported total size |
|---|---|---|
| **E:, C:** (Partition1, 2: legacy, present on every retail disk) | the volume's real free space, **capped at 2 TB** | the folder's used space + the reported free space |
| **X:, Y:, Z:** (Partition3–5: legacy cache drives) | retail size minus the folder's used space | the **retail 750 MB** (0x177000 sectors), always |
| **F:** (Partition6: added by modders, never on retail disks; the only extra drive, §2.1) | the volume's real free space, **no cap** | the folder's used space + the real free space |

- **Why the cap on E: and C::** games and the stock dashboard look at these and were only ever tested with a few GB;
  free space kept in 32-bit variables wraps around. At 2 TB, counts in 16 KB blocks or clusters (128 M) still fit in
  31 bits; code that counts bytes in 32 bits wraps at 4 GB, which only a cap below 4 GB would avoid. The cap is a
  setting: the compatibility pass (plan phase 6) runs the stock dashboard and the test games with E: reporting large
  values, and the cap is lowered if any of them needs it.
- **Why X/Y/Z stay at retail size:** a title sizes the FATX allocation table it writes from the partition size it is
  given (§5); a 2 TB cache drive would mean about 512 MB of table writes, and titles expect a 750 MB cache.
- **Why no cap on F::** titles never use it; dashboards and homebrew that do already handle large disks.
- **Cluster size reported:** 16 KB (retail FATX), whatever the exFAT cluster size; sizes are rounded to it.
- Usage per folder is computed at mount (a directory walk) and kept up to date on writes.

## 7. What stays FATX
- **FATX hard disks**: retail and Cerbios-style disks keep the current driver, so a disk can move between OXOS and
  other BIOSes.
- **FATX memory units** keep working; exFAT ones are added in §9.

## 8. Trace evidence (Halo, OXOS trace build, fresh disk, 2026-10-18)
The calls of the title's start-up, in order (handles and buffers omitted):

| # | Call | Target / arguments | Result |
|---|---|---|---|
| 10 | NtOpenFile | `\Device\Harddisk0\partition1\` | ok; volume query |
| 13 | IoCreateSymbolicLink | `\??\D:` (the disc) | ok |
| 14–19 | NtCreateFile ×2, link | `…partition1\TDATA`, `…TDATA\4d530004`, then `\??\T:` | ok |
| 20–189 | NtCreateFile, writes | `…partition1\UDATA`, `…UDATA\4d530004`, TitleMeta.xbx, TitleImage.xbx, SaveImage.xbx; `\??\U:` | ok |
| 190–193 | NtOpenFile, NtReadFile, NtWriteFile | `\Device\Harddisk0\partition0`: read 0x200 at 0x800, write 0x200 at 0x800 | ok |
| 196 | NtOpenFile | `\Device\Harddisk0\Partition5`, no file name, share 0 | ok |
| 197–198 | NtDeviceIoControlFile | 0x70000 (geometry), 0x74004 (partition info) | ok |
| 200 | NtWriteFile | 0x1000 at 0, data begins `FATX` | ok |
| 201–224 | NtWriteFile ×24 | 0x1000 each at 0x1000–0x18000: allocation table (first entries 0xFFFFFFF8, then zeros) | ok |
| 225–228 | NtWriteFile ×4 | 0x1000 each at 0x19000–0x1C000, filled with 0xFF (empty root directory) | ok |
| 229 | NtFsControlFile | 0x90020 (dismount) | ok |
| 231–234 | NtOpenFile, volume query, link | `\Device\Harddisk0\Partition5\`, then `\??\Z:` | ok |
| 288+ | NtOpenFile / NtCreateFile | `z:\cache000.map` … `z:\cache019.map` | not found, then created |

Resulting data on the disk image: sector 4 of the disk holds the dwords 0x97315286, 2, 0x4D530004 (Halo's title ID),
2, 1, then zeros (before Halo ran, its first dword was 0x00000202); Partition5 starts with `FATX`, serial
0xB4D06030, 32 sectors per cluster (16 KB), root cluster 1. The record's field meanings (beyond the title ID) are
not needed: the kernel only has to store the bytes.

## 9. Memory units (last phase; not traced yet)
Goal: a memory unit, or a USB stick used as one through a controller-port adapter, formatted exFAT on a PC so saves
can be copied directly.

What is expected, to confirm with the tracer before this phase (an xemu memory-unit image, a title saving to it,
and the dashboard's memory-unit screen):
- The USB stack and the memory-unit driver are XDK library code linked into each title, not kernel code: they
  find the unit, create its device and do the block I/O, then the kernel mounts a file system on that device. The
  file system is therefore the kernel's choice.
- The dashboard (and possibly titles) format a memory unit by writing FATX structures raw, like the cache
  partitions in §5.
- The unit's name (set in the dashboard) is kept in the FATX header area and read and written raw.

Design, once confirmed:
- **Recognition at mount:** FATX → today's driver; exFAT (no partition table, or MBR/GPT with one exFAT partition,
  as USB sticks come from a PC) → the exFAT driver, rooted at the volume (or at a `/UDATA` folder when present, so a
  stick can carry other files).
- **Names and sizes:** FATX name rules as on the hard disk (§3); size and free space reported in the units titles
  expect (16 KB blocks), capped (an official unit is 8 MB; a proposed cap of 1 GB for sticks).
- **Raw access:** the name area backed by a small file on the volume (`/OXOS/muname.bin`); a FATX format write
  turns into "empty the volume's save data", as in §5; other raw writes are refused and logged.
- **Formatting new units:** a setting chooses FATX (default, so units still work on other consoles) or exFAT.
- **Compatibility:** an exFAT unit only works under OXOS; units move between consoles more often than hard disks,
  which is why FATX stays the default.

## Open
- Other titles' sequences (Halo 2, a title that uses X: or Y:, one that uses two cache partitions): expected to be
  the same runtime code, to confirm with the tracer.
- The dashboard's formatting and "clear cache" paths, and homebrew dashboards that format partitions.
- Whether any title reads raw sectors of E: or C:.
- Memory units: the trace of §9 (device creation, raw name and format accesses, where the mount happens).
- exFAT driver details are the driver's own spec, written from the published exFAT specification. Two points
  already fixed by the console's 64 MB: the allocation bitmap (one bit per cluster: 8 MB for 8 TB at 128 KB
  clusters) is read and cached in pieces, never held whole; and files stored contiguously (the "no FAT chain"
  flag) need no allocation-table lookups, so the driver allocates contiguously where it can.
