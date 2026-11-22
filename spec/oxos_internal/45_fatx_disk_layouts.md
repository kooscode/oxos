# 45. FATX disk layouts by observation: the partitions a kernel reports for a disk

OXOS research method and result, 2026-11-20. Disks larger than the retail 8 GB disk carry extra FATX partitions (F:, G:, more), either described by a
`****PARTINFO****` table or laid out by the kernel itself when there is none. Consoles that ran Cerbios have such disks, and they must keep working when the
console runs OXOS. So the layout Cerbios gives each disk was observed from outside: a probe title asked the running kernel where each partition lies, on
disks of chosen sizes and with chosen tables, and the same was asked of OXOS. Nothing was taken from Cerbios's image: no code, no constants, no bytes, and
none of it was disassembled. The rules of §5 are the observed answers; OXOS's disk layout code (`kernel/src/storage/disk_layout.cpp`) gives the same answers
in every case run.

Earlier work this builds on: 48-bit LBA (spec 04), exFAT disks and how a disk is recognised as FATX or exFAT (spec 05), the retail partitions (stock spec
04), and the PARTINFO table format of Paul Bartholomew's (oz_paulb) LBA48 patch (2003, GPL; EvoX spec 03 §3), which XBPartitioner (public domain) and
XBlast OS (GPL-2.0) also read and write.

## 1. What was asked
`tests/partition_probe_title/` (nxdk) opens `\Device\Harddisk0\Partition0` to `Partition14` and asks each for its start and length
(IOCTL_DISK_GET_PARTITION_INFO), and Partition0 also for the disk's geometry (IOCTL_DISK_GET_DRIVE_GEOMETRY). It keeps the answers in a block that starts
with `OXOSPRB1`, then waits. It writes nothing. These are the answers every dashboard, formatter and title gets from the kernel, so equal answers mean a disk
is used the same way.

## 2. What was run
| Item | What |
|---|---|
| Emulator | xemu, 64 MB, MCPX 1.0 boot ROM, `tools/run.py --gdb`; the probe on a disc image in the DVD drive |
| Observed BIOS | Cerbios Hybrid 3.1.0 Beta, default settings |
| Control BIOS | "Complex 4627" (Microsoft's retail 4627 kernel with its image checks removed) |
| OXOS | the release build of 2026-11-20 |
| Disks | sparse qcow2 overlays of the Microsoft dashboard disk (retail C: and E:), each with its virtual size set to an exact sector count; tables written into the overlay with `qemu-io` |

**Capture:** gdb on xemu's GDB server stops at the probe's first instruction, lets it run 15 seconds, stops it again and dumps its data sections (their full
memory size); the answers are found by the `OXOSPRB1` mark. One run takes about a minute; 87 runs in all (29 disks, three kernels).

## 3. Disks without a table (20 sizes)
The sizes sit on each edge where an answer could change. RETAIL = 0xEE8AB0 (the retail partitions' end), LBA28 = 0x0FFFFFFF.

| Disk (sectors) | Cerbios: F: (Partition6) | Cerbios: G: (Partition7) | Complex 4627 | OXOS |
|---|---|---|---|---|
| 0xEE8AB0 (retail) | none | none | none | = Cerbios |
| RETAIL + 0x1FFFFF, RETAIL + 0x200000 | none | none | F: to the end | = Cerbios |
| RETAIL + 0x200001 | 0xEE8AB0, 0x200001 sectors | none | F: to the end | = Cerbios |
| 10 GiB, 100 GB | 0xEE8AB0 to the disk's end | none | the same | = Cerbios |
| 0x0FFFFFFF, 0x10000000 | 0xEE8AB0, 0xF11754F sectors (to 0x0FFFFFFF) | none | the same (sees 0x0FFFFFFF sectors) | = Cerbios |
| LBA28 + 0x1FFFFF, LBA28 + 0x200000 | the same | none | the same | = Cerbios |
| LBA28 + 0x200001 | the same | 0x0FFFFFFF, 0x200001 sectors | no G: | = Cerbios |
| 500 GB, 1 TB, LBA28 + 0x74000000 | the same | 0x0FFFFFFF to the disk's end | no G: | = Cerbios |
| LBA28 + 0x74000001, LBA28 + 0x74200000, 2 TB, 0xFFFFFFFF, 0x100200000, 4 TB | the same | 0x0FFFFFFF, **0x74000000 sectors** | no G: | = Cerbios |

- Partitions 1–5 are the retail ones in every case. No partition beyond G: appears without a table.
- Partition0 reports the disk's full size in every case (beyond 32 bits too: 0x100200000 and 4 TB), except under the control, which sees at most 0x0FFFFFFF
  sectors.

## 4. Disks with a table (9 cases)
500 GB disks (one 2 TB disk) with a PARTINFO table (magic, then from +0x30 up to 14 entries of 0x20 bytes: name, flags with bit 31 "in use", first sector,
sector count) at chosen sectors. Each table gives F: a length of its own (A = 0x1000000, B = 0x2000000, C = 0x3000000), so the answer shows which table
was used. N = the disk's sector count.

| Case | Table(s) | Cerbios | Complex 4627 | OXOS |
|---|---|---|---|---|
| sector 0 | A at 0 | F: = A | F: to 0x0FFFFFFF (no table read) | = Cerbios |
| end − 8 | B at N − 8 | F: = B | the same | = Cerbios |
| last sector | C at N − 1 | F: = C | the same | = Cerbios |
| sector 0 and end − 8 | A at 0, B at N − 8 | F: = A | the same | = Cerbios |
| sector 0 and last | A at 0, C at N − 1 | F: = A | the same | = Cerbios |
| end − 8 and last | B at N − 8, C at N − 1 | F: = B | the same | = Cerbios |
| entries over the cap (2 TB disk) | F: 0x80000000 sectors, G: the rest (0x67E61A60) | exactly as written | the same | = Cerbios |
| partitions 6–14 in use | F:, G: and entries 8–14, 0x1000000 each | Partition6 to Partition14 exactly as written | the same | = Cerbios |
| F: not in use | F:'s entry without bit 31 | no F:, no G: | the same | = Cerbios |

## 5. The rules (as observed; OXOS implements them)
1. **Retail partitions:** Partition1–5 at their retail places (stock spec 04), in every case run (the tables run held the retail entries, §6).
2. **A table is looked for at sector 0, then at N − 8, then at N − 1**; the first sector that starts with `****PARTINFO****` is used and the others are
   ignored.
3. **With a table**, partitions 6–14 are its entries 6–14 that have bit 31 set, at the first sector and sector count written, with no cap; the automatic
   layout of rule 4 is not used, even where the table has no F:.
4. **Without a table:**
   - F: (Partition6) from 0xEE8AB0 up to the disk's end or to 0x0FFFFFFF, whichever comes first; it exists only when it would be longer than 0x200000
     sectors (1 GiB).
   - G: (Partition7) from 0x0FFFFFFF up to the disk's end, at most 0x74000000 sectors; it exists only when it would be longer than 0x200000 sectors.
   - Nothing beyond G:.

## 6. What this does not cover
- **Formatting:** which cluster size a formatter picks for each partition, and whether a volume formatted under one kernel mounts under the other, was not
  run here. The partitions' places and lengths are what decide that a volume is found where it was made. What FATXplorer writes is in §8,
  and how OXOS reads it in §9.
- **Disks over 2 TB on a console:** xemu answers for them; real disks and adapters were not tried.
- **Tables of other forms:** a table that is damaged, has entries out of order, overlapping or past the disk's end, or entries 1–5 that differ from the
  retail ones, was not run.

## 7. Reproducing
Build the probe (`make -C tests/partition_probe_title`, nxdk) and put its `default.xbe` on a disc image (`tests/mkxiso.py`). For each disk: `qemu-img create
-f qcow2 -F qcow2 -b DASHBOARD.qcow2 DISK.qcow2 BYTES`, a table written with `qemu-io -c "write -s TABLE.bin OFFSET 512" DISK.qcow2`, then `tools/run.py
BIOS --mcpx-rom MCPX --memory 64 --gdb PORT --iso PROBE.iso --hdd DISK.qcow2` and gdb with the stops of §2. The answers sit after the `OXOSPRB1` mark: a
status and the geometry, then fifteen entries of {open status, information status, starting offset in bytes, length in bytes}.

## 8. The disks FATXplorer writes (studied 2026-11-21, for interoperability)
FATXplorer (Eaton Works) is the Windows tool most original-Xbox hard disks larger than the retail one are formatted with. A 4 TB disk it formatted as F:
(64 KB clusters) and G: (256 KB clusters) showed F: but not G: under OXOS, so it was studied for interoperability: this section holds only what a reader of
its disks needs (where the partitions lie, the table, the volumes), not how the tool works. Only the original-Xbox hard-disk paths; nothing from it is
code. How OXOS reads these disks is §9.

### 8.1 Source
| File | sha256 |
|---|---|
| FATXplorerBeta64.zip (download) | 51a511981bb946fcb5c4ac1bbd102f2bb74b83a208d46f6680e65f54794b32b8 |
| FATXplorer/FATXplorer.dll (all logic; .NET, assembly version 3.0.0.27459) | 2ec5e9db473e71d09d41f7bc4302e70e71c1c9a8475befb09332308ee97a127a |

The assembly was read as data; nothing was executed. Not described: everything Xbox 360 (its big-endian FATX and its formats), and the original-Xbox
memory-unit formatter (not studied).

### 8.2 Without a table
N = the disk's length in sectors (512-byte sectors; FATXplorer refuses other logical sector sizes for an original-Xbox disk). X, Y, Z, C and E are at their
retail places (stock spec 04). Then:
| Layout | F: (Partition6) | G: (Partition7) |
|---|---|---|
| retail / LBA28 | 0xEE8AB0 to min(N, 0x0FFFFFFF) | none |
| LBA without a table (disks up to 0x9001FFFF sectors) | 0xEE8AB0 to 0x0FFFFFFF | 0x0FFFFFFF to min(N, 0xFFFFFFFF): up to **0x80020000 sectors**, not capped at 0x74000000 |
| F: over the whole disk (disks over 0xFFFFFFFF sectors) | 0xEE8AB0 to N, at most sector 0x7FFFFFFF8 | none |

- F: and G: are made from **24 sectors** up (§5 rule 4 requires more than 0x200000).
- Sectors 0 to 0x3FF are zeroed: a table at sector 0 is erased, a copy at the disk's end (§8.4) is not.
- When FATXplorer reads such a disk, it takes the third layout when N > 0xFFFFFFFF and a `FATX` volume is at F: but none at G:.

### 8.3 With a table
The partitions are the table's entries (§8.4). The sizes are free: entries can start and end past sector 0xFFFFFFFF, with F: alone over the rest of the disk
or F:, G: and partitions 8–14 one after another. X, Y, Z, C and E keep their retail places with the default sizes.

### 8.4 The partition table
Format (one 512-byte sector, little-endian):
| Offset | Size | Field |
|---|---|---|
| 0x000 | 16 | ASCII `****PARTINFO****` |
| 0x010 | 1 | format type, 0 to 3 (not needed for reading) |
| 0x011 | 31 | zero |
| 0x030 + 32·i | 32 | entry i (i = 0 to 13) for partition i + 1 |

Entry:
| Offset | Size | Field |
|---|---|---|
| +0x00 | 16 | name, zero-padded ("XBOX DATA E", "XBOX SHELL C", "XBOX CACHE X" … "XBOX EXTRA F", "XBOX EXTRA G", "XBOX EXTRA 8" … "XBOX EXTRA 14") |
| +0x10 | 4 | flags: bit 31 "in use" |
| +0x14 | 4 | first sector, bits 0–31 |
| +0x18 | 4 | sector count, bits 0–31 |
| +0x1C | 2 | first sector, bits 32–47 |
| +0x1E | 2 | sector count, bits 32–47 |

An absent partition's entry is all zeros. The high 16 bits of the first sector and the count are what a disk over 2 TB needs; the original LBA48 table
(EvoX spec 03 §3) leaves them zero.

Where it is written and read:
- Written to **sector 0**, and a copy to the **disk's last sector** (N − 1).
- FATXplorer reads only sector 0. A format without a table later zeroes sector 0 but leaves the copy at N − 1.

### 8.5 The FATX volumes
- **Cluster sizes:** 4, 8, 16, 32, 64 KB, and on disks over 2 TB also 128, 256, 512 KB, 1 and 2 MB (8 to 4096 sectors per cluster). E: always has 16 KB.
- **Geometry:** the retail arithmetic of stock spec 04 §6, unchanged for every cluster size: a 4096-byte superblock; entries = length ÷ cluster + 1, taken as a
  32-bit number; 2-byte FAT entries below 0xFFF0, else 4; the FAT rounded up to 4096 bytes; the data area right after it (4 KB-aligned, not
  cluster-aligned).
- **Most clusters:** a volume is read with at most 0x0FFFFFFF usable clusters; the cluster must be at least 4096 bytes.
- **Superblock:** `FATX`, a serial number, sectors per cluster, root directory cluster 1 (stock spec 04b); the rest of the 4096 bytes 0xFF.
- **A new volume:** FAT entry 0 = 0xFFF8 (0xFFFFFFF8), entry 1 (the root directory) end of chain, the rest zero; the root directory is one cluster of 0xFF.
- A volume does not record its own length: a reader must give it the length it was formatted with, or the FAT's size, and so where the data starts, come
  out wrong.

### 8.6 A 4 TB disk (N = 7 814 037 168 = 0x1D1C0BEB0)
| Layout | Partitions after E: | Clusters |
|---|---|---|
| LBA, table | only the first 0xFFFFFFFF sectors used: F: at 0xEE8AB0, 0x80020000 sectors; G: at 0x80F08AB0, 0x7F0F754F sectors | 64 KB |
| table, F: over the whole disk | F: at 0xEE8AB0, 0x1D0D23400 sectors (count: low 0xD0D23400, high 0x0001) | 256 KB or 512 KB |
| no table, F: over the whole disk | F: from 0xEE8AB0 to the disk's end | 512 KB |
| table, sizes chosen by the user | as written | up to 2 MB |

### 8.7 What stays unknown
- Whether FATXplorer refuses, when reading, a table entry that runs past the disk's end or entries 1–5 that differ from the retail places.
- The original-Xbox memory-unit format (not studied).

## 9. OXOS reads them: clusters over 64 KB, and the layout chosen from the volumes (2026-11-21)
OXOS reads every original-Xbox disk FATXplorer makes (§8). Two things differed: the cluster sizes a FATX volume may have, and the layouts
without a table, which rules 2 and 4 of §5 do not cover.

### 9.1 Cluster sizes (the FATX mount, `kernel/src/filesystems/hddfs/volume_fatx.cpp`)
| Rule | Retail (stock spec 04 §6) | OXOS |
|---|---|---|
| Sectors per cluster | 1, 2, 4 … 128 (at most 64 KB) | any power of two with a cluster of 4 KB to 32 MB (FATXplorer writes up to 2 MB, §8.5) |
| Usable clusters | — | at most 0x0FFFFFFF, as FATXplorer reads (§8.5); more: the volume is not recognised |
| Arithmetic | entries = length ÷ cluster + 1 (as 32 bits), 2-byte entries below 0xFFF0, the FAT rounded up to 4 KB, the data after it | unchanged; the FAT's size and the data's start in 64 bits |

Directories keep the retail limit where it is checked, when a directory grows by a cluster: with clusters up to 256 KB that is 4096 entries. A larger
cluster holds more than 4096 entries in a directory's first cluster, and OXOS uses them (the retail kernel cannot mount such a volume at all).

### 9.2 The layout (`kernel/src/storage/disk_layout.cpp`)
1. **A table at sector 0** is used as in §5 rule 3, with 48-bit entries (bits 32–47 of the first sector at +0x1C, of the count at +0x1E;
   §8.4). Formatters write their tables there.
2. **No table at sector 0, a cold boot:** the volumes decide where the layouts differ, by the check below (boot-only code). The result is kept across quick
   reboots, which apply it without checking again.
3. **The check of a volume** at a first sector and a length: a `FATX` superblock with sectors per cluster from 8 to 65536 (a power of two) and a root
   cluster ≥ 1; the geometry of stock spec 04 §6 for that length (the FAT's size, and so the data's start, follow from the length), at most 0x0FFFFFFF
   clusters; then the first sector of the root directory there holds entries with valid FATX names (1–42 characters from 0x20–0x7E, none of `"*/:<>?\|`,
   attributes within 0x37), deleted entries (0xE5), or the end marker 0xFF; 0x00 counts as an end marker only after an entry, as zeros are what a FAT's
   unused tail holds. Only the length a volume was formatted for finds its root directory.
4. **The choices**, each made only when its own volume checks out and §5 rule 4's does not (so a disk that works under rule 4 keeps it):
   | Choice | Layout | Made by FATXplorer |
   |---|---|---|
   | F: to the disk's end | F: from 0xEE8AB0 to the disk's end (at most sector 0x7FFFFFFF8), no G:, on a disk over 0xFFFFFFFF sectors | the layout "F: over the whole disk" without a table (§8.2) |
   | G: to the disk's end | G: from 0x0FFFFFFF to the disk's end (at most sector 0xFFFFFFFF), not capped at 0x74000000 sectors, from 24 sectors | the LBA layout without a table (§8.2): G: up to 0x80020000 sectors |
   | A small F: | F: of 24 sectors up to 0x200000 sectors (§5 rule 4 has none) | §8.2: F: and G: exist from 24 sectors |
5. **A table only at N − 8 or N − 1** (§5 rule 2): used, unless the layout without it (with the choices of step 4) has more partitions 6–14 whose volumes
   check out. FATXplorer leaves a copy of its table in the last sector, and a later format without a table keeps it (§8.4).
6. Without any volume to check (unformatted partitions), the layout is §5's, as before.

### 9.3 Checked (`make -C kernel test_fatx_layouts`, `tests/fatx_layout_test.py`)
Each disk is made as FATXplorer writes it (tables, volumes of §8.5 with an `oxos_probe.txt`) and booted with the partition probe title
(§1), which reports every partition's place and reads the file on F: and G:, then writes 640 KB there and reads it back; the test reads the written file
from the disk image too.
| Disk | Expected F: / G: |
|---|---|
| 4 TB, table (type 1) at sector 0 and the last sector: F: 1 TiB at 64 KB, G: the rest at 256 KB (48-bit count) | as in the table |
| 4 TB, table (type 3): F: over the rest at 1 MB | F: 0x1D0D23400 sectors, no G: |
| 4 TB, no table: F: to the disk's end at 512 KB, nothing at G: | F: 0x1D0D23400 sectors, no G: |
| 0x9001FFFF sectors, no table: G: to the disk's end (0x80020000 sectors) at 64 KB | G: 0x80020000 sectors |
| the same disk, G: formatted capped (0x74000000 sectors, §5 rule 4) | G: 0x74000000 sectors |
| the G:-to-the-end disk with a stale table (F: only) in the last sector | the layout without the table |
| the same size, a table in the last sector whose F: and G: are formatted | as in the table |
| xemu's 8 GiB dashboard disk (0x1000000 sectors), F: formatted in the 0x117550 sectors after E: | F: 0x117550 sectors |
| the same disk, nothing at F: | no F: (§5 rule 4) |
