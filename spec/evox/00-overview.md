# EvoX M8 spec 00: overview, layout and patch inventory

Clean-room: behaviour, formats and addresses only; no copied code, and no key bytes (keys are named, not quoted).
Sources: `xbox-files/bios/evox.m8plus.fc.137.nodvd.bin` (256 KiB, user-supplied, boots the MS dashboard on the
no-DVD 1.4 console from a Xenium bank) and its unpacked parts in `xbox-files/evoxm8_unpacked/` (that folder's README
describes the flash layout and the loader); compared byte by byte with the retail 5838 kernel
(`xbox-files/5838-unpacked/xboxkrnl.exe`, same build 1.0.5838.1) and disassembled with objdump. The name
"m8plus.fc.137.nodvd" reads as: EvoX M8+, fan control, "137" (the 137 GB LBA28 boundary that its LBA48 code removes, spec 01 §2), no DVD drive required.

Status marks: **[C]** confirmed from the code, **[I]** inferred (structure / constants only), **[O]** open.

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
| 0x800102C0 `.text` | — | 22 patched places, below **[C]** |
| 0x800367C0 `IDEXPRDT`, 0x800368E0 `.data` | — | unchanged (see §1 for `.data`) |
| 0x8003BEE0 `STICKY` | 0x4C4 bytes | **0x84E0 bytes**: the retail STICKY plus the first 0x8000 bytes of the retail INIT section **[C]** |
| 0x800443C0 `INIT` | started at 0x8003C3C0 | starts 0x8000 later; its bytes up to 0x800A8CA0 are retail INIT with 12 patched places; **0x800A8CA0–0x800BA530 (0x11890 bytes) are new** **[C]** |

- The retail INIT bytes now counted as STICKY (0x8003C3C0–0x800443C0) are unchanged in the file. The relabelling
  keeps that range from being freed with INIT after start-up, because EvoX **overwrites it at run time** with its
  resident module (§3). Until then the retail INIT functions in it (boot-time code: boot animation and others) run
  as usual. **[C]**
- New INIT bytes: 0x800A8CA0–0x800B252F = the **boot module** (start-up-only code and data, freed with INIT);
  0x800B2530–0x800BA52F = the **resident module image** (0x8000 bytes, copied out at install). **[C]**
- `STICKY` +0x118 (VA 0x8003BFF8, the slot the retail entry fills with the second 2BL key) holds the retail
  **certificate key** in the file. **[C]**

## 3. Install of the resident module
At the end of phase 1, where retail calls the routine at 0x8001EB79 (just before the title's entry point is
called), EvoX jumps to 0x800B2540 instead: **[C]**
1. the marker dword at 0x8003C3C0 is cleared;
2. 0x7FFC bytes are copied from 0x800B2534 to 0x8003C3C4 (module image after its 4-byte marker);
3. the 0x200 bytes at 0x8003EFC0 are bit-inverted (XOR 0xFFFFFFFF per dword, last dword first): a small block
   stored inverted in the image (content **[O]**);
4. the marker 0xCAFEBABE (the first dword of the image) is written to 0x8003C3C0;
5. jump to the module's init at 0x8003C410 (which then does the retail work it replaced: **[I]**).
Relocation rule: an address A in the image 0x800B2534… runs at A − 0x76170. Every hook into the resident module
therefore has two targets: **if [0x8003C3C0] == 0xCAFEBABE jump to the resident copy, else to the same code still
in INIT** (used before the install). **[C]**

## 4. Kernel entry and keys
- The entry routine no longer copies the two 2BL keys (EEPROM key to 0x8003C140 = `XboxEEPROMKey`, second key to
  0x8003BFF8); it only clears the 2BL's 0x6000 bytes at 0x80400000 and continues to start-up. Arguments are
  ignored. **[C]**
- Certificate key: preset in STICKY (§2). **[C]**
- **EEPROM key autodetection** (0x800B1466, called in phase 1 where retail calls the EEPROM security-section
  routine 0x800549C8): for each of three built-in 16-byte keys — a third key **[O]**, the retail EEPROM key, and the
  key the retail 5838 BIOS carries (inferred: the 1.6 console's) — copy it to `XboxEEPROMKey` and run the retail
  routine; stop at the first that returns 0. If none does: SMC/LED fatal sequence (0x80014F9B with 0xA0), then
  0x80021DC0(0). **[C]**
- The retail routine is patched so that a failed security-section check **returns −1** instead of the fatal path,
  and success returns 0 (INIT 0x80054A93). **[C]**

## 5. Patch inventory
### 5.1 `.text`
| Address | Retail | EvoX | Effect |
|---|---|---|---|
| 0x800127D3 | string `\Device\CdRom0\default.xbe` | cut to `\Device\CdRom0\` | the boot-time "launch the disc" path no longer names a file **[C]** (effect on title choice **[I]**) |
| 0x80013D04 | `KeRaiseIrqlToDpcLevel` | jump to 0x8002454A: if installed → 0x8003F07A (resident), else the original body | **in-game reset**, spec 01 §7 **[C]** |
| 0x8001598D | call 0x800549C8 (EEPROM security section) | call 0x800B1466 | EEPROM key autodetection (§4) **[C]** |
| 0x80015A60 | call 0x8001EB79 | jump 0x800B2540 | module install (§3) **[C]** |
| 0x80015A71 | after the title's entry returns: call 0x80021DC0 | call 0x8003E6C0 (resident) | **fan speed and LED** set at each title launch, spec 01 §6 **[C]** |
| 0x8001C748 | quick-reboot `.data` SHA-1 (cold: store digest at 0x8003C2D0; quick: compare, halt on mismatch) | `ret` | check removed **[C]** |
| 0x8001FF28 | write a code-segment descriptor (0x8003C2A0) and reload CS — used once (0x8001EC2A) to **shrink the CS limit** after start-up | `ret 8` | CS stays flat 4 GB: code may run above the old limit (the resident module, title-loaded code) **[C]** |
| 0x800243AA, 0x80024534, 0x80024759, 0x800248B1 | IDE: program the LBA28 task file (0x1F6 device/head, 0x1F3–0x1F5, 0x1F2 count) then the command (READ/WRITE DMA 0xC8/0xCA …) | if installed → resident 0x8003C60F / 0x8003C60A / 0x8003C619 / 0x8003C605, else INIT 0x800B277F / 0x800B277A / 0x800B2789 / 0x800B2775 | **LBA48** command issue (0x80024534 DMA, 0x800243AA the other read/write path), PARTINFO partition geometry (0x800248B1), device control with the extra code 0xCAFEBABE (0x80024759): spec 01 §2–§4 **[C]** |
| 0x8002454A–0x80024569 | (inside the 0x80024534 patch) | the `KeRaiseIrqlToDpcLevel` dispatcher | **[C]** |
| 0x8002EF6D | XBE section load: `repe cmpsd` of the section's SHA-1 against the header digest, fail → 0xC0000190 | `rep movsd` + unconditional branch | section digests no longer checked **[C]** |
| 0x8002F057–0x8002F192, 0x8002F272–0x8002F70E (several runs) | XBE header RSA signature check (`XePublicKeyData`, SHA-1 of 0x10104…) and the XBE image loader that calls it | rewritten: signature check gone; the freed space holds EvoX code (partition 6 / "F:" sizing from 0xEE8AB0 sectors, the string `\Device\Harddisk0\Partition2\evoxdash.xbe`); the loader ends in 0x8002F704 → resident 0x8003E7C0 / INIT 0x800B4930 | unsigned XBEs load **[C]**; the dashboard launcher with its fallback list, spec 01 §5 **[C]**; remaining loader detail **[O]** |
| 0x800308E5, 0x800308FA | `AvSendTVEncoderOption` cases 1 (Macrovision level) and 0x0A (Macrovision commit) | jump to the function's end | **Macrovision off** **[C]** |

### 5.2 `INIT` (retail boot-time code)
| Address | Effect |
|---|---|
| 0x800548F2 | kernel entry: keys not copied (§4) **[C]** |
| 0x80054922 | encoder detection by SMBus probe instead of the flash marker, spec 02 §2 **[C]** |
| 0x80054A93 | EEPROM security section: −1 / 0 instead of the fatal path (§4) **[C]** |
| 0x8005531E | `jne` removed: the flag byte 0x8003C2F4 is set to 1 regardless of bit 0 of a value read just before (the XBE loader tests 0x8003C2F4) **[C]**; meaning **[O]** |
| 0x80055459, 0x800554DB | disk set-up → 0x800B2770 / 0x800B2784 = INIT copies of module entries 0 and 4: disk size incl. LBA48, PARTINFO from sector 0 (spec 01 §2–§3) **[C]** |
| 0x800556A1, 0x800556E2, 0x800556F8, 0x80055714 | DVD start-up: four failure branches made unconditional / removed → **no fatal error without a DVD drive** **[C]** (per-branch detail **[O]**) |
| 0x800566F1 | call 0x8003DDF0 → 0x800B1513 (run 0x800B24C6, then the original) **[C]** |
| 0x800572CE, 0x800572D4 | calls 0x80057242 / 0x800566FB → 0x800B14FB / 0x800B1507 (run 0x800B1C46 / 0x800B1D86, then the originals) **[C]**; these are boot-animation routines **[I]** |
| 0x800597AE | 3-byte constant 0xA0FF60 → 0xCADE00 in the boot animation (a colour, **[I]**) |

## 6. Hook map (original code → new code)
`0x8001598D→0x800B1466`, `0x80015A60→0x800B2540`, `0x80015A71→0x8003E6C0`, `0x80013D04→0x8002454A→0x8003F07A`,
IDE `→0x8003C605/0A/0F/19`, XBE loader `0x8002F704→0x8003E7C0`, INIT `0x80054922→0x800B145A`,
`0x80055459→0x800B2770`, `0x800554DB→0x800B2784`, `0x800566F1→0x800B1513`, `0x800572CE→0x800B14FB`,
`0x800572D4→0x800B1507`. Data pointers into the new code (function tables, IDT): **[O]**.

## 7. What OXOS can take from this (hints, to be specified and implemented independently)
- Booting without a DVD drive: the retail start-up treats a missing drive as fatal (spec/stock/04); EvoX removes
  exactly those branches. OXOS already continues without a drive (its own deviation).
- EEPROM key autodetection over a set of known keys, as OXOS does with key states (spec/stock/01 §5).
- No Macrovision; flat CS; unsigned XBEs; a dashboard path on the hard disk.

## Open questions
- Details listed in specs 01 and 02 (extra-partition layout, the 0xCAFEBABE output, the boot-logo code).
- The third built-in EEPROM key; the meaning of flag 0x8003C2F4; the stream `.data` differences.
- Where the fan / LED bytes and the IGR mode flag come from (they are fixed bytes in this image) **[O]**.
