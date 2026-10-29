# Cerbios spec 03: large disks, partition table, extra partitions

Studied in Cerbios 3.1.0 Beta (kernel IDE disk driver and device control, extension's partition set-up).
Behaviour and formats only.

## 1. 48-bit addressing (per drive)
- At drive start-up the kernel issues IDENTIFY DEVICE. If **word 83 bit 10** (48-bit address feature set) is set,
  the drive is marked for 48-bit commands (one flag per drive: bit 9 = master, bit 10 = slave, in a kernel flags
  dword) and its **size is taken from words 100–103** (64-bit sector count); otherwise from the 28-bit field, as
  retail.
- Commands on a 48-bit drive use the EXT forms: the sector count and each LBA register are written twice (high byte
  first, then low), the device register is 0xE0 | (drive << 4), and the command is the EXT variant (READ VERIFY EXT
  0x42 instead of 0x40; reads and writes likewise use the DMA EXT commands). 28-bit drives keep the retail sequence.
- The **master disk must have at least 0xEE8AB0 sectors** (15 633 072, the retail 8 GB disk); smaller → fatal
  error 9.

## 2. Partition table (one per disk, 512 bytes)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 16 | magic `****PARTINFO****` |
| 0x10 | 32 | reserved (zero) |
| 0x30 + 32 × i | 32 | entry i (i = 0..13), partition number i + 1 |

Entry: name (16 bytes, space-padded), flags (32-bit; 0x80000000 = in use), start sector (low 32 bits), size in
sectors (low 32 bits), start sector bits 32–47 (16-bit), size bits 32–47 (16-bit). (The last 4 bytes of the
entry carry the high parts, so sectors beyond 2^32 work.)

- **Built-in default table** (each disk), as retail: entry 0 "XBOX MUSIC" (E:, start 0x55F400, size 0x9896B0),
  1 "XBOX SYSTEM" (C:, 0x465400, 0xFA000), 2–4 "XBOX GAME SWAP 1–3" (X/Y/Z: 0x400, 0x177400, 0x2EE400; each
  0x177000), entries 5–13 empty.
- **Loading:** for each disk, the built-in table is set up (with the automatic partitions below), then the raw
  disk (`\Device\RealHD<n>\Partition0`) is read at three places in turn, stopping at the first whose first 16
  bytes equal the magic: **sector 0**, then the sector **8 sectors before the end** (byte offset size − 0x1000),
  then the **last sector** (size − 0x200). The matching sector **replaces the whole table** (and a "table from
  disk" flag is set). A table found on the disk therefore always wins.

## 3. Automatic extra partitions (when the disk has no table)
A partition-mode byte controls them. It is the variable **exported as ordinal 40 (HalDiskCachePartitionCount)**,
which holds **5** in the 3.1.0 image (the retail kernel's value is 3); it is not an ini key. It applies only for disks larger than
0xEE8AB0 sectors:
- **bit 0:** entry 5 (F:, name "XBOX F") starts at E:'s end (E start + E size) and runs to the end of the disk but
  **not beyond sector 0x0FFFFFFF** (the 28-bit limit, about 128 GiB);
- **bit 1:** entry 5 (F:) from E:'s end to the end of the disk;
- **bit 2:** entry 6 (G:, "XBOX G") from F:'s end to the end of the disk.
- Each automatic partition is capped at **0x74000000 sectors** (about 928 GiB) and is
  marked in use only if larger than **0x200000 sectors** (1 GiB). The size's high 16 bits are 0.
- Default 5 = F: covers the rest of the first 128 GiB, G: everything after it (each at most ~928 GiB).
- **Exact arithmetic** (extension 0x3163–0x33A8; N = the disk's sector count, 64-bit):
  - The rules run only when N > 0xEE8AB0.
  - F: (bit 0): start = E:'s entry start + E:'s entry size (both 48-bit, so 0xEE8AB0 with the built-in table);
    end = **min(N, 0x0FFFFFFF)** (N ≥ 0x0FFFFFFF gives 0x0FFFFFFF); size = end − start, then capped at 0x74000000.
    The entry's name (16 bytes) is copied from a constant ("XBOX F"), its start high 16 bits are set, its size high
    16 bits cleared.
  - F: (bit 1): the same with end = N.
  - G: (bit 2): start = F:'s start + F:'s size as just set; size = N − start, capped at 0x74000000; name "XBOX G".
  - An entry is marked in use (flags 0x80000000) only when its size is above 0x200000 sectors (or its 64-bit
    size has a non-zero high part before the cap).
  - So with the default 5 on a disk of N ≥ 0x0FFFFFFF sectors: F: = 0xEE8AB0 … 0x0FFFFFFF (0xF11754F sectors,
    ~120.7 GiB), G: = 0x0FFFFFFF … min(N, 0x0FFFFFFF + 0x74000000).
- The same F: is what EvoX lays out (spec/evox/03 §2); EvoX's G: has no cap, so the two differ on disks above
  0x0FFFFFFF + 0x74000000 sectors (~1.06 TB).
- Disk tools (XBPartitioner, FATXplorer) write a PARTINFO table to sector 0 instead, which wins (§2). Seen on a
  2 TB disk formatted by XBPartitioner: entries 0–4 the retail five, entry 5 (F:) 0xEE8AB0 + 0xF11754F, entry 6
  (G:) 0x0FFFFFFF + 0xD8E088B1 (to the end of the disk, larger than the automatic cap).

## 4. Drive letters (extension)
Partition numbers by letter: **C 2, E 1, F 6, G 7, X 3, Y 4, Z 5**; and per disk J–N = partitions 10–14 (strings
`0J10\ 0K11\ 0L12\ 0M13\ 0N14\` for disk 0 and `1J10\ … 1N14\` for disk 1). Paths may use `HDD0-`/`HDD1-` prefixes
(`HDD?-?:\`) and `DVD-ROM:\`; spec 05 (dual HDD) covers disk 1.

## 5. Partition-table query for applications (device control)
A disk device accepts a Cerbios-specific control code **0xCAFEBABE**. With an output buffer of at least 0x1C bytes it
returns: 0xCAFEBABE, 0xBABEFACE, version **4**, three zero dwords, and a **pointer to that disk's 512-byte partition
table** (in kernel memory). Shorter buffers →
STATUS_BUFFER_TOO_SMALL (0xC0000023). The other handled codes are the retail ones (geometry 0x70000, partition info
0x74004, 0x70014, and 0x4D028 pass-through).

## For OXOS (notes, not Cerbios behaviour)
- OXOS reads the PARTINFO table and lays out the automatic F:/G: as in §2–§3 (default 5) on FATX disks
  (`kernel/src/idex/disk.cpp`, `IdexDiskReadPartitionTable`, `IdexDiskDefaultPartitions`): partition N is entry
  N − 1's start and size. exFAT disks keep their own layout (spec/oxos-internal/05).

## Open
- The DMA EXT command bytes and the read/write path (expected 0x25/0x35); how partition devices are created for
  entries 5–13 (FATX mount of F/G and up); whether the table can be written back by the kernel.
