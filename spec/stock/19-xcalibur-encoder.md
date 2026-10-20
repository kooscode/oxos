# Spec 19 — Xcalibur video encoder (xboxkrnl 1.0.5838.1)

Clean-room behavioural spec of how the retail kernel for 1.6 consoles drives the **Xcalibur** video encoder (SMBus
address 0xE0), next to the Conexant (0x8A) and Focus (0xD4) paths that spec 17 describes for 4627. Evidence:
batch_decompile of the 5838 kernel's `AvSetDisplayMode` (ord 3, 0x80030F3D), `AvSendTVEncoderOption` (ord 2,
0x80030822), the encoder register writer 0x80030026, the Macrovision routines 0x8003026F / 0x8003045E, the component
set-up 0x800306FF, the WSS/CGMS routine 0x800305C8, the table loader 0x800315D4, hardware detection 0x80054921 and
the SMBus routines 0x800145F1 / 0x80014743 / 0x800146D1 / 0x80014D1D; the AV table blob's header was read from the
5838 BIOS image (layout only). The Cerbios 3.1.0 kernel's Xcalibur path (`AvSetDisplayMode` 0x80036FCB) was used as a
cross-check: it follows the same sequence.

**No table contents are reproduced here** (register values, value pools, row maps, flicker or Macrovision rows):
only their layout, sizes and how they are indexed. OXOS's values must come from public sources (open-source
drivers with a compatible licence, documentation) or measurement — never from a Microsoft or Cerbios image.

Register numbers below are the 8-bit SMBus command (register) numbers the kernel uses. Every Xcalibur register is
**32 bits wide**. Their names are not known; meanings are given where the behaviour shows them.

## 1. Which encoder
### 1.1 Detection (cold boot, `XboxHardwareInfo`, stock spec 02 §5)
5838 decides from a byte of the BIOS image, not from the bus:
- Map 4 bytes of physical **0xFFF00078** (flash offset 0x78; the 1.6 BIOS image holds 0x90 there, retail 4627
  images 0x60). If the byte is **≥ 0x80**: Flags |= 0x10, then
  - byte **== 0x90** → Flags |= **0x400** (Xcalibur);
  - otherwise, port 0x80CE bit 5 set → Flags |= 0x30 (Focus, as in 4627).
- (4627 only tests the byte for == 0x80 and has no 0x400 case.)

The encoder address is then chosen at every boot (cold and quick, in the table loader 0x800315D4):
**Flags 0x20 → Focus 0xD4; else Flags 0x400 → Xcalibur 0xE0; else Conexant 0x8A**. Titles read
`XboxHardwareInfo`, so on a 1.6 console Flags bit 0x400 must be set and bit 0x20 clear.

**Cerbios cross-check (one image for every revision):** Cerbios ignores the flash byte and probes the bus at boot:
an SMBus byte read (protocol 0x0A, command 0) at 0x8A, then 0xD4, then 0xE0; the first that completes (host status
0x10) is the encoder. Focus → Flags |= 0x20; Xcalibur → Flags |= 0x400.

### 1.2 Encoder type query
`AvSendTVEncoderOption` option **0x10** returns **0** Conexant, **1** Focus, **2** Xcalibur.

## 2. SMBus: 32-bit transfers
`HalReadSMBusValue` / `HalWriteSMBusValue` keep their signature (address, command, `WordFlag`, value). In 5838 the
transfer size is:
- `WordFlag` = 0 → 1 byte (protocol code 0x1A, byte data);
- `WordFlag` ≠ 0 and **(address & 0xF8) == 0xE0** → **4 bytes** (protocol code **0x1D**, block);
- `WordFlag` ≠ 0, other addresses → 2 bytes (protocol code 0x1B, word data), as in 4627.

4-byte write: address (write) → 0xC004, command → 0xC008, the four value bytes **low byte first** → the block FIFO
0xC009, byte count 4 → 0xC006, clear status (read 0xC000, write it back), then the protocol code → 0xC002.
4-byte read: address | 1 → 0xC004, command → 0xC008, clear status, protocol 0x1D → 0xC002; on completion the count
(0xC006 bits 0–5) must be 4 — otherwise the request fails with STATUS_IO_DEVICE_ERROR (0xC0000185) — and four bytes
are read from 0xC009, low byte first. The rest (lock, retries, completion event) is as in stock spec 12.

All Xcalibur accesses below use the 4-byte form.

## 3. Mode tables: the AV blob in flash
5838 does not compile mode tables into the kernel. At boot the table loader maps the top 4 MB of physical memory
(the flash, PA 0xFFC00000) with one large-page PDE at VA 0xC0C00000 (mapped once, reference-counted) and reads an
**AV blob** at PA **0xFFFF57A8** (offset 0x357A8 of the 256 KB image), **0x4658 bytes** long. It sits right after the
kernel's 0x610 bytes of initialised data in the image (the region 5838 re-copies from flash on a quick reboot,
§8).

### 3.1 Layout (all offsets relative to the blob start)
- `u32 [3]` at 0: offset of the **descriptor** for Conexant, Focus, Xcalibur (in the 5838 image: 0x0C, 0x38, 0x64).
- Descriptor: `u32 [11]` table offsets:

| # | table | used for Xcalibur |
|---|---|---|
| 0 | SDTV common encoder registers | no (count 0) |
| 1 | per-mode encoder table | **yes**: SDTV and HD rows alike |
| 2 | HDTV encoder table | no (count 0) |
| 3 | Macrovision, SDTV | yes |
| 4 | Macrovision, 625-line SD | yes |
| 5 | NV2A register table (dword offsets + dword rows) | yes (as for all encoders) |
| 6 | CRTC table | yes |
| 7 | component set-up (read-modify-write rows) | no (fixed writes, §5 step 7) |
| 8 | **value pool** (32-bit values in blocks) | Xcalibur only |
| 9 | **dword list** (32-bit values) | Xcalibur only |
| 10 | counts header: `u16 [10]`, the row length of tables 0–9 | |

- Each table is a **register row** (the register numbers, or NV2A dword offsets, or CRTC indexes) followed by
  **value rows** of the same length.
- Counts in the 5838 blob (row lengths in bytes):

| table | Conexant | Focus | Xcalibur |
|---|---|---|---|
| 0 SDTV common | 22 | 49 | 0 |
| 1 per-mode | 63 | 64 | **42** |
| 2 HDTV | 13 | 64 | 0 |
| 3 Macrovision SD | 6 | 28 | 26 |
| 4 Macrovision 625-line | 1 | 28 | 26 |
| 5 NV2A | 104 | 104 | 104 |
| 6 CRTC | 34 | 34 | 34 |
| 7 component | 12 | 14 | 0 |
| 8 value pool | 0 | 0 | 3044 (761 dwords) |
| 9 dword list | 0 | 0 | 368 (92 dwords) |

- Row counts (from the table sizes in the 5838 blob): Xcalibur per-mode table = register row + 38 mode rows; NV2A
  = register row + 18; CRTC = register row + 16; Xcalibur Macrovision SD = register row + 3 (levels 1–3, no level-0
  row), 625-line = register row + 1.
- **Validation:** every table pointer must lie inside the blob, and *pointer + row length + a per-table row count*
  must not pass its end (row counts checked: common 2, per-mode 30 or **36 for Xcalibur**, HDTV 4, Macrovision 5 / 3,
  NV2A 19, CRTC 17, component 30 (Conexant) or 3, pool 1, list 1). A failure **bug-checks** (code 0). This is a weak
  sanity check, not a full bounds check.
- The PRAMDAC 0x680880 values per NV2A row stay a small dword table in the kernel's initialised data (as in 4627).

### 3.2 Xcalibur per-mode rows: (register, index) pairs
For Xcalibur each per-mode row holds, under the register row's 42 register numbers, one **index byte** per
register. The register writer (§4) turns (register, index) into one or more 32-bit writes. Row columns **24–41**
(byte offset 0x18 onward) are the ones that depend on the filter variant (§4); option 0x0E rewrites only those.

## 4. The Xcalibur register writer (`write(register, index)`)
For Conexant and Focus the writer is a plain 8-bit register write. For Xcalibur it is an indexed 32-bit write with a
**variant** v = (current mode word bit 29, component HDTV ? 2 : 0) | (luma filter on, option 0x0E ? 1 : 0):

| register | action |
|---|---|
| 0x20–0x23 | only if register − 0x20 == v: write **16 registers 0x20–0x2F** from pool block (index × 16 dwords) at pool byte offset 0x3A4 |
| 0x31–0x34 | only if register − 0x31 == v: write **8 registers 0x31–0x38** from pool block (index × 8 dwords) at pool offset 0x1C4 |
| 0x3A–0x3D | only if register − 0x3A == v: write **8 registers 0x3A–0x41** from pool block (index × 8 dwords) at pool offset 0x84 |
| 0x43–0x44 | only if register − 0x43 == (v & 1): write **0x43** and **0x44** from the pool pair at offset 0x4C + index × 8 |
| 0x46 | write **0x46, 0x47, 0x48** from the pool triple at offset (index − 1) × 12 |
| any other | write the register with **dword list[index]** |

- **Index 0** in a block entry (0x20–0x23, 0x31–0x34, 0x3A–0x3D, 0x43, 0x46) writes **zeros** to the whole block.
- A row therefore lists all four variants of each coefficient bank (e.g. registers 0x20, 0x21, 0x22, 0x23); only the
  one matching the current variant is written. The banks look like filter coefficient sets (16 + 8 + 8 values plus
  pairs) selected by output type (component or not) and the luma filter.
- **Indirect sub-registers:** some settings go through a three-register port: **0x0C ← sub-address, 0x0D ← value,
  0x0E ← 1** (commit). Below, "sub(a) ← x" means that sequence.

## 5. `AvSetDisplayMode` on Xcalibur
Mode word fields as in spec 17 §2 (`Mode` 0 → 0x0F010101). Differences for Xcalibur:

### 5.1 Row selection
- The encoder row (bits 0–6) is **remapped** to an Xcalibur per-mode row through one of four small byte maps chosen by
  HD (bits 30–31 = 10b) and console region 2 (Japan, 0x8003C158 == 2): SD map (32 entries) or HD map (8 entries),
  each with a Japan variant.
- Special case: encoder row 1 with CRTC row (bits 8–15) 7 and HD → treated as encoder row **4** before the HD map.
- The CRTC row (bits 8–15) and NV2A row (bits 16–23) are used unmapped, as for the other encoders.
- OXOS note: the mode word is the ABI (titles from every XDK send the same words); the remap is internal, so OXOS can
  key its own Xcalibur rows directly by (mode word fields, region).

### 5.2 Same mode as last time
As spec 17 §4 (pitch, depth, framebuffer start), then options 0x0B (5) and 0x0E (0).

### 5.3 Mode change (full sequence; steps marked * are Xcalibur-specific)
1. * Read register **4**, write it back **| 0x0F** (DAC outputs off). Read register **0**, write it back
   **& ~0x02** (output off). Clear the luma-filter state and the flicker cache (§6, options 0x0E / 0x0B).
2. `out 0x80D3, 5`; wait for the vertical blank phase (NV 0x6806A0 bits 0 and 4 agree); CRTC unlock, screen off;
   PRAMDAC 0x680880 ← 0x21121111 (as spec 17 §5 steps 1–2).
3. Stall 1 µs (the Conexant-only resets are skipped).
4. * Encoder registers: **only the per-mode table**: for each of the 42 columns, `write(register, index)` (§4) from
   the remapped row. (Conexant/Focus write the SDTV common row then the per-mode row, or the HDTV row.)
5. * Region 2 (Japan) and remapped row **0x0D, 0x0E or 0x12**: read register **1**, write it back **& ~0x8000**.
   (Conexant: 0xA2/0xA4/0xAC; Focus: 0x4E ← 0x13C — spec 17.)
6. Mode bit 29 (component HDTV) → component set-up, step 7.
7. * Component set-up: register **1** ← (register 1 & 0xFCFFFFFF) | **0x00C00008**; register **0x60** ← register 0x60
   & ~0x01000000; register **0x58** ← 0.
8. * HD (bits 30–31 = 10b): register **7** ← 0 (WSS/CGMS off). Any of bits 30–31 set (625-line, HD or VGA): register **9**
   ← 0 (closed captions off). Register **0** ← register 0 **| 0x02** (output on). **sub(0x0F) ← 0** (Macrovision off).
   Then option 0x0B with level 5 (default flicker filter, §6).
9. GPIO (`0x80D6`, `0x80D8`), NV2A table row, the component clears (bit 29: NV 0x680630, 0x6808C4, 0x68084C ← 0),
   VGA attribute/sequencer set-up, CRTC row with pitch patches, wait three vertical blanks, PRAMDAC 0x680880 ← the
   per-row value, stall 1 µs — all as spec 17 §5.
10. * Register **4** ← mode bits 24–27 (the DAC enable bits; on Conexant this goes to 0xBA).
11. Framebuffer start (NV 0x600800); WSS/CGMS update (§7).

## 6. `AvSendTVEncoderOption` on Xcalibur
Mode classes by bits 30–31: **00** 525-line SD (NTSC, PAL-60), **01** 625-line SD (PAL 50 Hz), **10** HD, **11** VGA
(nxdk's mode words: e.g. 0x44030307 PAL 50 Hz 640×480, 0x88070701 480p). "SD" below means class 00 or 01.

| option | Xcalibur behaviour |
|---|---|
| 1 (Macrovision level 0–3) | Only in class 00 or 01. Level 0 → **sub(0x0F) ← 0** and done. Otherwise, for each of the 26 columns of the Macrovision table (525-line or 625-line): sub(register) ← value from row *level* (525-line: levels 1–3 = value rows 1–3; 625-line: any level ≥ 1 = value row 1). The level is also kept for WSS/CGMS (§7). |
| 2 (closed captions on) | Class 00 (525-line) only: register **9** ← 7. |
| 3 (closed captions off) | Class 00 only: register **9** ← 0. |
| 4 (send caption bytes; arg = field, data = bytes d0, d2) | Class 00 only: register **0x0A** ← d2 & 0x7F; field 0: **0x0B** ← 4, **0x0A** ← d0 & 0x7F, **0x0B** ← 8; field 1: **0x0B** ← 1, **0x0A** ← d0 & 0x7F, **0x0B** ← 2. |
| 5 (caption status) | Class 00 only: read register **0x14**; result = (~value) & 3. |
| 6 (AV capabilities) | as 4627 (EEPROM-based, encoder-independent). |
| 9 (blank screen) | on: VGA sequencer 1 ← 0x21. Off: sequencer 1 ← 1, register **4** ← mode bits 24–27, `out 0x80D3, 4`. |
| 0x0A (Macrovision commit) | Only in class 00, 01, or **480p** ((mode & 0xCF0000FF) == 0x88000001: HD class, HD row 1, component DACs). Value: level 0 → 0; level 1 → 0x36; level ≥ 2 → 0x3E in class 00, 0x36 otherwise; 480p → 0x24 when non-zero. **sub(0x0F) ← value**. |
| 0x0B (flicker filter 0–5) | Not in HD. Read register **0x60**; set A if bits 22–23 = 10b, else set B. Level → entry: 0→0, 1→1, 2→(A ? 2 : 1), 3→(A ? 3 : 2), 4/5→(A ? 4 : 2). Write the entry's **7 dwords to registers 0x60–0x66**, skipped if it is the entry written last (the cache is cleared at each mode change). With mode bit 29: then register 0x60 ← register 0x60 & ~0x01000000. The two sets (5 entries × 7 dwords) are kernel data. |
| 0x0C (zero mode) | forget the current mode (next `AvSetDisplayMode` does a full change). |
| 0x0D (query mode) | current mode word. |
| 0x0E (luma filter on/off) | Store the state (variant bit 0, §4). If a mode is set and the state changed: re-run `write(register, index)` for per-mode columns **24–41** of the current (remapped) row. |
| 0x0F (guess field) | ~(port 0x80C0 bit 5) & 1 (encoder-independent). |
| 0x10 (encoder type) | **2**. |
| 0x11 (mode table version) | 0. |
| 0x12 (CGMS) | store the CGMS bits; WSS/CGMS update (§7). |
| 0x13 (widescreen) | mode bit 28 ← argument; WSS/CGMS update (§7). |

## 7. WSS / CGMS-A on Xcalibur
The kernel builds one word and sends it to the encoder; only the final write differs per encoder.
- **525-line SD (class 00):** data (14 bits): bit 0 = widescreen (mode bit 28); bits 6–7 = CGMS (option 0x12); bit 8 = Macrovision level
  bit 1, bit 9 = level bit 0 (option 1). A **6-bit CRC** over data bits 0–13 (polynomial x⁶ + x + 1, initial value
  all ones, data processed from bit 0, as a shift register) goes into bits 14–19 — the 20-bit CGMS-A word of IEC 61880 (525-line).
- **625-line SD (class 01):** word = (widescreen ? 7 : 8) | CGMS << 12 (no CRC): the WSS of ETSI EN 300 294, aspect
  code 8 = 4:3 full format, 7 = 16:9 anamorphic, with the copy bits in the upper group.
- **HD:** nothing is sent.
- **Xcalibur write:** register **7** ← ((word & 0xFFFFF) | 0x01800000) << 1.
  (Focus: registers 0x83–0x88; Conexant: 0x60/0x62/0x64 — spec 17.)

## 8. Context: other 5838 changes tied to the encoder
- `HalReadSMBusValue` / `HalWriteSMBusValue` gained the 4-byte form (§2).
- The flicker-filter sets, Macrovision commit values and row maps are kernel data; the mode tables moved to the flash
  blob (§3) for all three encoders.
- The quick-reboot path re-reads the kernel's initialised data from flash and compares its SHA-1 with the cold-boot
  value (a mismatch hangs); this is an integrity check of the image, not part of the driver.

## For OXOS (notes, not reference behaviour)
- **Detection:** probe the bus like Cerbios (§1.1) rather than trusting flash byte 0x78: OXOS's open BIOS has 0x70
  there, and one image should serve every revision. Report Flags 0x400 / 0x20 exactly as the reference would for the
  detected encoder, and option 0x10 = 2.
- **SMBus:** add the 4-byte block form (§2) to the HAL, keyed by address 0xE0–0xE7 and `WordFlag`, so titles and the
  kernel see the reference's behaviour.
- **Tables:** OXOS needs its own Xcalibur values for: per-mode rows (42 registers, coefficient banks per variant),
  the value pool and dword list (or an equivalent flat form — the pool/index scheme is a compression choice, not
  ABI), Macrovision rows, flicker sets A/B, and NV2A/CRTC rows (shared with the other encoders). Their layout need
  not follow the blob; only the mode-word meaning, the option behaviour and the register sequence matter.
- **Sources for values:** candidates are open-source Xbox video drivers with a GPL-compatible licence (e.g.
  XboxDev/cromwell's encoder support — provenance of its values to be checked first), public documentation, or
  measurement on a 1.6 console (I²C capture of a retail kernel's writes is *not* an option for OXOS's tables:
  those writes are the reference's data).
- **Order of work:** detection + SMBus 4-byte form + option 0x10 first (harmless on 1.0–1.5), then the mode-change
  sequence with an SDTV 480i row, then HD rows, then Macrovision/CGMS/captions.

## Open
- Meanings of Xcalibur registers 0, 1, 4, 7, 9, 0x0A, 0x0B, 0x14, 0x58, 0x60 and the coefficient banks beyond what
  the behaviour shows (DAC enables in 4 bits 0–3, output enable in 0 bit 1, WSS in 7, captions in 9–0x0B/0x14).
- Meaning of register 0x60 bits 22–23 (selects flicker set A or B) and bit 24 (cleared for component).
- Whether the 5838 BIOS's other hardware (1.6 SMC, fan) needs further kernel changes outside the encoder.
