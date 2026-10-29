# EvoX M8 spec 03: the hard disk

LBA48, extra partitions and the partition table (spec 00 for sources, status marks and where the code lives). The
code is the resident module's (oz_paulb's LBA48 support); its jump table at 0x8003C600 has six entries. The IDE
hooks reach it through the install marker (spec 00 §3); the disk set-up at start-up runs before the install and uses
the INIT copies 0x800B2770 / 0x800B2784 of entries 0 and 4 (hooked at INIT 0x80055459 / 0x800554DB). **[C]**

## 1. Disk size and LBA48
- **Disk size at set-up** (entry 0): from the IDENTIFY data, if command set word 83 bit 10 (48-bit address feature)
  is set, the 48-bit sector count (words 100–103; a non-zero high dword gives 0xFFFFFFFF), else words 60–61. The
  result is the kernel's disk sector count (0x8003C2F0). **[C]**
- **Commands** (entries 2 and 3, hooked into the retail DMA path 0x80024534 and the other read/write path
  0x800243AA): a request uses 48-bit addressing only when the disk count is ≥ 2^28 **and** the request's last sector
  is ≥ 2^28; then the task file is written twice per register (high-order byte first: sector count, LBA 24–31/0–7,
  32–39/8–15, 40–47/16–23), device byte 0x40, command **0x25 READ DMA EXT / 0x35 WRITE DMA EXT** (the other path
  likewise with its EXT command); otherwise the retail LBA28 sequence (device byte 0xE0 | LBA 24–27, 0xC8 / 0xCA).
  The bus-master start value (0x09 read / 0x01 write) follows, then the retail code continues. **[C]**

## 2. Extra partitions
- Option bytes read at disk set-up (0x8003C5C4–0x8003C5C5, INIT copy 0x800B2734–35): +0 "lay out the extra
  partitions", +1 "do not read PARTINFO from the disk" (values 2 and 0 in this build). **[I]**
- When the disk count exceeds **0xEE8AB0 sectors** (the stock 8 GB disk) and option byte +0 (mode) is non-zero, the
  space above it becomes extra partitions, written into the default PARTINFO table (at INIT 0x800B28B0, magic
  `****PARTINFO****`) by the start-up hook 0x800B2B30 before the retail disk set-up runs. **[C]** With N the disk's
  sector count:
  - **entry 5 (partition 6, F:)**: name `DRIVE F:` (space-padded), flags 0x80000000, first sector **0xEE8AB0**,
    sector count N − 0xEE8AB0; but when N > 0x0FFFFFFF and mode ≠ 1, the count is **0x0FFFFFFF − 0xEE8AB0** (F:
    ends at sector 0x0FFFFFFF, the 28-bit limit, about 128 GiB);
  - **entry 6 (partition 7, G:)**: only when N > 0x0FFFFFFF and mode is neither 1 nor 3: name `DRIVE P:`, flags
    0x80000000, first sector 0x0FFFFFFF (F:'s end), sector count **N − 0x0FFFFFFF** (the rest of the disk, no cap).
  - So mode 1 = F: is the whole rest of the disk; mode 3 = F: up to 0x0FFFFFFF and no G:; other non-zero values
    (2 in this build) = F: up to 0x0FFFFFFF and G: after it.
  - High 16 bits of start and count stay 0. A PARTINFO sector found on the disk (§3) replaces the whole table.
  - Compared with Cerbios (spec/cerbios/03 §3): F: starts at the same sector (E:'s end = 0xEE8AB0); Cerbios caps
    each automatic partition at 0x74000000 sectors, EvoX does not, so on disks above ~1.06 TB their G: sizes differ.
  The XBE loader area also holds partition 6 / "F:" sizing from 0xEE8AB0 sectors (spec 02 §3).

## 3. Partition table (PARTINFO)
- **Format** (same magic and entry layout as Cerbios, spec/cerbios/03 §2): 16-byte magic `****PARTINFO****`, then at
  +0x30 up to 14 entries of 0x20 bytes: name (16 bytes, e.g. `XBOX MUSIC`, `XBOX SYSTEM`, `XBOX GAME SWAP 1–3`),
  +0x10 flags (bit 31 = in use), +0x14 first sector, +0x18 sector count, +0x1C reserved. **[C]**
- **Source**: the module's default table (0x8003C740), replaced at disk set-up (entry 4) by **sector 0 of
  `\Device\Harddisk0\partition0`** when that sector begins with the same 16-byte magic (0x1F0 bytes copied). **[C]**
  (The stock disk's sector 0 holds the "BRFR" refurb block, not a PARTINFO table, on retail units.)
- **Partition geometry** (entry 1, hooked at 0x800248B1, replacing the retail partition-number switch that knew only
  1–5): partition n = 1…14 is taken from entry n; not in use or out of range → STATUS_OBJECT_NAME_NOT_FOUND
  (0xC0000034); the partition's offset is the entry's first sector, its length the sector count × 512. **[C]**

## 4. Device control
Entry 5 (hooked at 0x80024759) replaces the start of the disk's device-control handler: the retail request codes
(0x4D028, 0x70000, 0x70014, 0x74004) go to the retail code; the extra code **0xCAFEBABE** returns the module's
information (the block at 0x8003C5C0 — dword 3, dword 2, **[I]** version / feature words — and the partition table)
to the caller (dashboards read the table this way). Exact output layout **[O]**.

## Open questions
- The exact 0xCAFEBABE output (§4). (§2's layout rules: resolved 2026-10-29.)
