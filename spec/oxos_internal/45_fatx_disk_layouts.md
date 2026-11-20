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
  run. The partitions' places and lengths are what decide that a volume is found where it was made.
- **Disks over 2 TB on a console:** xemu answers for them; real disks and adapters were not tried.
- **Tables of other forms:** a table that is damaged, has entries out of order, overlapping or past the disk's end, or entries 1–5 that differ from the
  retail ones, was not run.

## 7. Reproducing
Build the probe (`make -C tests/partition_probe_title`, nxdk) and put its `default.xbe` on a disc image (`tests/mkxiso.py`). For each disk: `qemu-img create
-f qcow2 -F qcow2 -b DASHBOARD.qcow2 DISK.qcow2 BYTES`, a table written with `qemu-io -c "write -s TABLE.bin OFFSET 512" DISK.qcow2`, then `tools/run.py
BIOS --mcpx-rom MCPX --memory 64 --gdb PORT --iso PROBE.iso --hdd DISK.qcow2` and gdb with the stops of §2. The answers sit after the `OXOSPRB1` mark: a
status and the geometry, then fifteen entries of {open status, information status, starting offset in bytes, length in bytes}.
