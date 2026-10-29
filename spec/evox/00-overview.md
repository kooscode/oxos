# EvoX M8 spec 00: overview

Clean-room: behaviour, formats and addresses only; no copied code, and no key bytes (keys are named, not quoted).
Sources: `xbox-files/bios/evox.m8plus.fc.137.nodvd.bin` (256 KiB, user-supplied, boots the MS dashboard on the
no-DVD 1.4 console from a Xenium bank) and its unpacked parts in `xbox-files/evoxm8_unpacked/` (that folder's README
describes the flash layout and the loader); compared byte by byte with the retail 5838 kernel
(`xbox-files/5838-unpacked/xboxkrnl.exe`, same build 1.0.5838.1) and disassembled with objdump. The name
"m8plus.fc.137.nodvd" reads as: EvoX M8+, fan control, "137" (the 137 GB LBA28 boundary that its LBA48 code removes,
spec 03 §1), no DVD drive required.

Status marks: **[C]** confirmed from the code, **[I]** inferred (structure / constants only), **[O]** open.

The specs are organised by what EvoX changes, not by where its code lives:
| Spec | Concern |
|---|---|
| 00 (this) | the image, its loader, the kernel layout, where the new code lives, feature index, patch inventory |
| [01](01-startup.md) | start-up: kernel entry and keys, EEPROM key autodetection, no DVD drive, video encoder detection, boot logo |
| [02](02-title-launch.md) | launching titles: dashboard choice, unsigned XBEs, flat code segment, Macrovision |
| [03](03-disk.md) | the hard disk: LBA48, extra partitions, PARTINFO, device control |
| [04](04-runtime.md) | while a title runs: fan and LED, in-game reset |

## 1. Flash image and loader (see the unpacked README for offsets)
- Not a Microsoft chain: X-codes for MCPX 1.0 and 1.1, ending with a 32-byte stub at RAM 0 (the ROM-bypass trick
  Cerbios and OXOS also use); a plaintext loader derived from the MS 2BL at flash 0x3C900, run at 0x90000. **[C]**
- The loader decompresses the kernel (LDI/LZX stream, no RC4, no digest), copies the kernel's `.data` initializer
  from flash (DOS header +0x30, +0x2C bytes) and zero-fills +0x28 more, then calls the entry point with **two zero
  arguments** (retail 2BLs pass the boot parameters and a key pointer). **[C]**
- Flash 0x382A8–0x3C900 is the retail 5838 AV blob, unchanged; the kernel reads it from flash at
  (+0x30) + (+0x2C) (spec/stock/17 §1a.2). **[C]**
- The `.data` initializer in flash is **identical to retail 5838**. The `.data` copy inside the compressed kernel
  stream differs from retail in 1 031 of its first 0x610 bytes, but the loader overwrites exactly those bytes with
  the flash copy at boot, so at run time `.data` is retail. **[C]** (Why the stream copy differs: **[O]**.)

## 2. Kernel image layout (PE 1.0.5838.1, 0xAA530 bytes; retail 0x98CA0)
| Range | Retail | EvoX |
|---|---|---|
| 0x80010000 header | — | SizeOfImage 0xAA530; STICKY and INIT headers changed (below) **[C]** |
| 0x800102C0 `.text` | — | 22 patched places (§6.1) **[C]** |
| 0x800367C0 `IDEXPRDT`, 0x800368E0 `.data` | — | unchanged (see §1 for `.data`) |
| 0x8003BEE0 `STICKY` | 0x4C4 bytes | **0x84E0 bytes**: the retail STICKY plus the first 0x8000 bytes of the retail INIT section **[C]** |
| 0x800443C0 `INIT` | started at 0x8003C3C0 | starts 0x8000 later; its bytes up to 0x800A8CA0 are retail INIT with 12 patched places; **0x800A8CA0–0x800BA530 (0x11890 bytes) are new** **[C]** |
- The retail INIT bytes now counted as STICKY (0x8003C3C0–0x800443C0) are unchanged in the file. The relabelling
  keeps that range from being freed with INIT after start-up, because EvoX **overwrites it at run time** with its
  resident module (§3). Until then the retail INIT functions in it (boot-time code: boot animation and others) run
  as usual. **[C]**
- `STICKY` +0x118 (VA 0x8003BFF8, the slot the retail entry fills with the second 2BL key) holds the retail
  **certificate key** in the file (spec 01 §1). **[C]**

## 3. Where the new code lives
- **Boot module**: new INIT bytes 0x800A8CA0–0x800B252F, start-up-only code and data, freed with INIT after
  start-up: EEPROM key autodetection, encoder detection, boot-logo code (spec 01). **[C]**
- **Resident module**: new INIT bytes 0x800B2530–0x800BA52F, a 0x8000-byte image copied at title launch to
  0x8003C3C4–0x800443C0 (the relabelled STICKY range) and kept for the life of the kernel: disk support (spec 03),
  dashboard launcher (spec 02 §1), fan / LED and in-game reset (spec 04). Its strings name its origin: "Kernel
  expansion support" and "LBA48 support by Paul Bartholomew (oz_paulb), Copyright (c) 2003 – Released under GNU
  Public License". **[C]**
- **Install** (at the end of phase 1, where retail calls the routine at 0x8001EB79 just before the title's entry
  point is called, EvoX jumps to 0x800B2540 instead): **[C]**
  1. the marker dword at 0x8003C3C0 is cleared;
  2. 0x7FFC bytes are copied from 0x800B2534 to 0x8003C3C4 (module image after its 4-byte marker);
  3. the 0x200 bytes at 0x8003EFC0 are bit-inverted (XOR 0xFFFFFFFF per dword, last dword first): the in-game
     reset code, stored inverted in the image (spec 04 §2);
  4. the marker 0xCAFEBABE (the first dword of the image) is written to 0x8003C3C0;
  5. jump to the module's init at 0x8003C410, which calls the retail routine 0x8001EB79 (whose CS-limit step is now
     a no-op, spec 02 §3) and resumes phase 1 at 0x80015A65. **[C]**
- **Relocation rule**: an address A in the image 0x800B2534… runs at A − 0x76170. Every hook into the resident
  module therefore has two targets: **if [0x8003C3C0] == 0xCAFEBABE jump to the resident copy, else to the same code
  still in INIT** (used before the install, e.g. by the disk set-up). **[C]**
- **Resident module layout** (run-time addresses):
| Run-time range | Content |
|---|---|
| 0x8003C3C0 | marker 0xCAFEBABE (written last by the installer) |
| 0x8003C3D0–0x8003C40F | installer copy (unused after install) |
| 0x8003C410 | module init (above) **[C]** |
| 0x8003C4C0, 0x8003C6C0 | the two credit strings **[C]** |
| 0x8003C5C0 | an 8-byte block returned through device control (spec 03 §4): dword 3, dword 2 (**[I]** version / feature words) |
| 0x8003C5C4–0x8003C5C5 (INIT copy 0x800B2734–35) | disk option bytes (spec 03 §2) **[I]** |
| 0x8003C600 | jump table, 6 entries (spec 03) **[C]** |
| 0x8003C740 | default partition table, `PARTINFO` format (spec 03 §3) **[C]** |
| 0x8003E640, 0x8003E641 | LED and fan bytes (spec 04 §1) **[C]** |
| 0x8003E6C0 | fan / LED at title launch (spec 04 §1) **[C]** |
| 0x8003E7C0 | dashboard launcher (spec 02 §1) **[C]** |
| 0x8003ED44… | dashboard paths (spec 02 §1) **[C]** |
| 0x8003EFC0–0x8003F1BF | in-game reset (spec 04 §2), stored bit-inverted in the image **[C]** |

## 4. Feature index
| Feature | Spec | Where it hooks |
|---|---|---|
| Kernel entry without 2BL keys; certificate key preset | 01 §1 | INIT 0x800548F2 |
| EEPROM key autodetection | 01 §2 | 0x8001598D → 0x800B1466; INIT 0x80054A93 |
| Quick-reboot `.data` check removed | 01 §3 | 0x8001C748 |
| Booting without a DVD drive | 01 §4 | INIT 0x800556A1, 0x800556E2, 0x800556F8, 0x80055714 |
| Video encoder detection by SMBus | 01 §5 | INIT 0x80054922 → 0x800B145A |
| Boot-animation logo | 01 §6 | INIT 0x800566F1, 0x800572CE, 0x800572D4, 0x800597AE |
| Dashboard launcher with fallbacks | 02 §1 | XBE loader 0x8002F704 → 0x8003E7C0 |
| Boot-time disc path shortened; flag 0x8003C2F4 | 02 §2 | 0x800127D3; INIT 0x8005531E |
| Unsigned XBEs (no RSA signature, no section digests) | 02 §3 | 0x8002F057–0x8002F192, 0x8002F272–0x8002F70E, 0x8002EF6D |
| Flat code segment | 02 §4 | 0x8001FF28 |
| Macrovision off | 02 §5 | 0x800308E5, 0x800308FA |
| Disk size, LBA48 commands | 03 §1 | INIT 0x80055459 → 0x800B2770; IDE 0x80024534, 0x800243AA |
| Extra partitions; PARTINFO table; partition geometry | 03 §2–§3 | INIT 0x800554DB → 0x800B2784; IDE 0x800248B1 |
| Device control 0xCAFEBABE | 03 §4 | IDE 0x80024759 |
| Fan and LED at title launch | 04 §1 | 0x80015A71 → 0x8003E6C0 |
| In-game reset | 04 §2 | 0x80013D04 → 0x8002454A → 0x8003F07A |
| Resident module install | §3 | 0x80015A60 → 0x800B2540 |

## 5. What OXOS can take from this (hints, to be specified and implemented independently)
- Booting without a DVD drive: the retail start-up treats a missing drive as fatal (spec/stock/04); EvoX removes
  exactly those branches. OXOS already continues without a drive (its own deviation).
- EEPROM key autodetection over a set of known keys, as OXOS does with key states (spec/stock/01 §5).
- No Macrovision; flat CS; unsigned XBEs; a dashboard path on the hard disk.

## 6. Patch inventory (every changed place; behaviour in the spec named)
### 6.1 `.text`
| Address | Retail | EvoX | Spec |
|---|---|---|---|
| 0x800127D3 | string `\Device\CdRom0\default.xbe` | cut to `\Device\CdRom0\` | 02 §2 |
| 0x80013D04 | `KeRaiseIrqlToDpcLevel` | jump to 0x8002454A: if installed → 0x8003F07A (resident), else the original body | 04 §2 |
| 0x8001598D | call 0x800549C8 (EEPROM security section) | call 0x800B1466 | 01 §2 |
| 0x80015A60 | call 0x8001EB79 | jump 0x800B2540 | §3 |
| 0x80015A71 | after the title's entry returns: call 0x80021DC0 | call 0x8003E6C0 (resident) | 04 §1 |
| 0x8001C748 | quick-reboot `.data` SHA-1 | `ret` | 01 §3 |
| 0x8001FF28 | write a code-segment descriptor (0x8003C2A0) and reload CS | `ret 8` | 02 §4 |
| 0x800243AA, 0x80024534, 0x80024759, 0x800248B1 | IDE: program the LBA28 task file (0x1F6 device/head, 0x1F3–0x1F5, 0x1F2 count) then the command (READ/WRITE DMA 0xC8/0xCA …) | if installed → resident 0x8003C60F / 0x8003C60A / 0x8003C619 / 0x8003C605, else INIT 0x800B277F / 0x800B277A / 0x800B2789 / 0x800B2775 | 03 §1–§4 |
| 0x8002454A–0x80024569 | (inside the 0x80024534 patch) | the `KeRaiseIrqlToDpcLevel` dispatcher | 04 §2 |
| 0x8002EF6D | XBE section load: `repe cmpsd` of the section's SHA-1 against the header digest, fail → 0xC0000190 | `rep movsd` + unconditional branch | 02 §3 |
| 0x8002F057–0x8002F192, 0x8002F272–0x8002F70E (several runs) | XBE header RSA signature check (`XePublicKeyData`, SHA-1 of 0x10104…) and the XBE image loader that calls it | rewritten: signature check gone; the freed space holds EvoX code (partition 6 / "F:" sizing from 0xEE8AB0 sectors, the string `\Device\Harddisk0\Partition2\evoxdash.xbe`); the loader ends in 0x8002F704 → resident 0x8003E7C0 / INIT 0x800B4930 | 02 §1, §3; 03 §2 |
| 0x800308E5, 0x800308FA | `AvSendTVEncoderOption` cases 1 (Macrovision level) and 0x0A (Macrovision commit) | jump to the function's end | 02 §5 |
### 6.2 `INIT` (retail boot-time code)
| Address | Change | Spec |
|---|---|---|
| 0x800548F2 | kernel entry: keys not copied | 01 §1 |
| 0x80054922 | encoder detection by SMBus probe instead of the flash marker | 01 §5 |
| 0x80054A93 | EEPROM security section: −1 / 0 instead of the fatal path | 01 §2 |
| 0x8005531E | `jne` removed: flag byte 0x8003C2F4 set to 1 regardless | 02 §2 |
| 0x80055459, 0x800554DB | disk set-up → 0x800B2770 / 0x800B2784 (INIT copies of module entries 0 and 4) | 03 §1, §3 |
| 0x800556A1, 0x800556E2, 0x800556F8, 0x80055714 | DVD start-up failure branches made unconditional / removed | 01 §4 |
| 0x800566F1 | call 0x8003DDF0 → 0x800B1513 (run 0x800B24C6, then the original) | 01 §6 |
| 0x800572CE, 0x800572D4 | calls 0x80057242 / 0x800566FB → 0x800B14FB / 0x800B1507 | 01 §6 |
| 0x800597AE | 3-byte constant 0xA0FF60 → 0xCADE00 | 01 §6 |
Data pointers into the new code (function tables, IDT): **[O]**.

## Open questions
- The `.data` stream differences (§1); data pointers into the new code (§6).
- Per concern: the third built-in EEPROM key and byte 0x8003C15C (spec 01); flag 0x8003C2F4 and the remaining XBE
  loader detail (spec 02); the extra-partition layout and the exact 0xCAFEBABE output (spec 03); where the fan / LED
  bytes and the in-game-reset mode flag come from (spec 04).
