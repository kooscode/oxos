# EvoX M8 spec 01: the resident module

Clean-room behaviour description (see spec 00 for sources and status marks). The module is installed at title
launch into 0x8003C3C4–0x800443C0 (spec 00 §3) and stays for the life of the kernel. Its strings name its origin:
"Kernel expansion support" and "LBA48 support by Paul Bartholomew (oz_paulb), Copyright (c) 2003 – Released under
GNU Public License". Addresses below are run-time addresses; the same code exists in INIT at address + 0x76170 and
is what runs before the install (every hook checks the marker, spec 00 §3).

## 1. Layout
| Run-time range | Content |
|---|---|
| 0x8003C3C0 | marker 0xCAFEBABE (written last by the installer) |
| 0x8003C3D0–0x8003C40F | installer copy (unused after install) |
| 0x8003C410 | module init: calls the retail routine 0x8001EB79 (whose CS-limit step is now a no-op, spec 00 §5.1), then resumes phase 1 at 0x80015A65 **[C]** |
| 0x8003C4C0, 0x8003C6C0 | the two credit strings **[C]** |
| 0x8003C5C0 | an 8-byte block returned through the new device-control code (§4): dword 3, dword 2 (**[I]** version / feature words) |
| 0x8003C5C4–0x8003C5C5 (INIT copy 0x800B2734–35) | option bytes read at disk set-up: +0 "lay out the extra partitions", +1 "do not read PARTINFO from the disk" (value 2 and 0 in this build) **[I]** |
| 0x8003C600 | jump table, 6 entries (§2–§4) **[C]** |
| 0x8003C740 | default partition table, `PARTINFO` format (§3) **[C]** |
| 0x8003E640, 0x8003E641 | LED sequence byte and fan-speed byte for §6 (0x0F / 0x0A mean "leave alone") **[C]** |
| 0x8003E6C0 | fan/LED at title launch (§6) **[C]** |
| 0x8003E7C0 | dashboard launcher (§5) **[C]** |
| 0x8003ED44… | dashboard paths (§5) **[C]** |
| 0x8003EFC0–0x8003F1BF | in-game reset (§7), stored bit-inverted in the image **[C]** |

## 2. Disk size and LBA48 (oz_paulb)
- **Disk size at set-up** (table entry 0, from the IDE disk start-up in INIT): from the IDENTIFY data, if command
  set word 83 bit 10 (48-bit address feature) is set, the 48-bit sector count (words 100–103; a non-zero high dword
  gives 0xFFFFFFFF), else words 60–61. The result is the kernel's disk sector count (0x8003C2F0). **[C]**
- **Extra space**: when that count exceeds **0xEE8AB0 sectors** (the stock 8 GB disk) and option byte +0 is set, the
  space above it becomes extra partitions (sizes from the remainder; layout details **[O]**). **[C]**
- **Commands** (table entries 2 and 3, from the retail DMA and second read/write paths): a request uses 48-bit
  addressing only when the disk count is ≥ 2^28 **and** the request's last sector is ≥ 2^28; then the task file is
  written twice per register (high-order byte first: sector count, LBA 24–31/0–7, 32–39/8–15, 40–47/16–23), device
  byte 0x40, command **0x25 READ DMA EXT / 0x35 WRITE DMA EXT** (the other path likewise with its EXT command);
  otherwise the retail LBA28 sequence (device byte 0xE0 | LBA 24–27, 0xC8 / 0xCA). The bus-master start value
  (0x09 read / 0x01 write) follows, then the retail code continues. **[C]**

## 3. Partition table (PARTINFO)
- **Format** (same magic and entry layout as Cerbios, spec/cerbios/03 §2): 16-byte magic `****PARTINFO****`, then at
  +0x30 up to 14 entries of 0x20 bytes: name (16 bytes, e.g. `XBOX MUSIC`, `XBOX SYSTEM`, `XBOX GAME SWAP 1–3`),
  +0x10 flags (bit 31 = in use), +0x14 first sector, +0x18 sector count, +0x1C reserved. **[C]**
- **Source**: the module's default table, replaced at disk set-up (table entry 4) by **sector 0 of
  `\Device\Harddisk0\partition0`** when that sector begins with the same 16-byte magic (0x1F0 bytes copied). **[C]**
  (The stock disk's sector 0 holds the "BRFR" refurb block, not a PARTINFO table, on retail units.)
- **Partition geometry** (table entry 1, replacing the retail partition-number switch that knew only 1–5): partition
  n = 1…14 is taken from entry n; not in use or out of range → STATUS_OBJECT_NAME_NOT_FOUND (0xC0000034); the
  partition's offset is the entry's first sector, its length the sector count × 512. **[C]**

## 4. Device control
Table entry 5 replaces the start of the disk's device-control handler: the retail request codes (0x4D028, 0x70000,
0x70014, 0x74004) go to the retail code; the extra code **0xCAFEBABE** returns the
module's information (the block at 0x8003C5C0 and the partition table) to the caller (dashboards read the table this
way). Exact output layout **[O]**.

## 5. Dashboard launcher
Replaces the retail "launch the dashboard" step (spec/stock/05 §3): the retail XBE launch routine (0x8002F26D, path,
1, 0) is tried for, in order: **[C]**
1. `\Device\Harddisk0\Partition2\evoXdash.xbe`
2. `\Device\Harddisk0\Partition1\Dash\UnleashX\unleashx.xbe`
3. `\Device\Harddisk0\Partition1\Dashboard\default.xbe`
4. `\Device\Harddisk0\Partition2\xboxdash.xbe`
The first that loads wins. If none does and this is a cold boot: SMC LED sequence 0xE1, then the retail fatal error
(13, 13 + launch-data type, or 20 when a title disc is present; spec/stock/05 §6), then reboot (0x8001542D(4)). **[C]**

## 6. Fan and LED at title launch
In the phase-1 thread, right after the XBE's entry point has returned (it starts the title's threads), and before
the thread ends itself (`PsTerminateSystemThread`): **[C]**
- if the LED byte ≠ 0x0F: SMC command 0x08 (LED sequence) = the byte, then 0x07 (LED mode) = 1;
- if the fan byte ≠ 0x0A: SMC command 0x06 (fan speed) = the byte, then 0x05 (fan mode) = 1 (manual).
Each write goes straight to the MCPX SMBus host (I/O 0xC000–0xC008: address 0x20, command, data, start 0x1A) and is
followed by a ~1 000 000-iteration busy wait; no status is checked.

## 7. In-game reset (IGR)
Installed as a hook on `KeRaiseIrqlToDpcLevel` (every call goes through 0x8002454A; spec 00 §5.1): **[C]**
- It acts only when the caller's code at the return address begins with the bytes `8B 54 24 0C` (identifies the
  gamepad input path, **[I]**), reading the caller's report structure (pointer at the caller's stack +0x18, report at
  +0x14).
- The report is the standard gamepad state: a 16-bit button word (0x01 up, 0x02 down, 0x04 left, 0x08 right,
  0x10 START, 0x20 BACK, 0x40 left thumb, 0x80 right thumb) followed by 8 analog bytes (A, B, X, Y, BLACK, WHITE,
  left trigger, right trigger).
- **Combo**: both triggers fully pressed (analog bytes 6 and 7 == 0xFF) together with BACK + START (and A, B
  released), or with BACK + BLACK (BLACK non-zero) → reset.
- **Sequence**: a 14-step table of button masks at 0x8003F042 (up, up, down, down, left, right, left, right, left
  thumb, right thumb, BACK, START, left, left); each change of the button word that matches the current step's mask
  advances the count (a word above 0xFFFF in the combined dword restarts it); after the 14th step the reset happens
  only while the running XBE's header holds `EvoX` at +0x180 (the EvoX dashboard) **[I]** for the exact matching rule.
- **Reset**: if the flag at 0x8003F1BC == 1, `HalReturnToFirmware`-style reboot 0x8001542D(2) (back to the
  dashboard); otherwise SMC command 0x02 = 1 and halt.
- Then (or when nothing matched) the original `KeRaiseIrqlToDpcLevel` body runs: old IRQL returned, IRQL = 2.

## Open questions
- Extra-partition layout rules (§2) and the exact 0xCAFEBABE output (§4).
- Where the LED/fan bytes and the IGR mode flag are set (configuration source: the EvoX configuration file is
  handled by the dashboard, the kernel holds only the bytes) **[O]**.
