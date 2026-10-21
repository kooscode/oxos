# Spec 17 — AvSetDisplayMode and AvSendTVEncoderOption (xboxkrnl 1.0.4627.1, 1.0.5838.1)

Clean-room behavioural spec of what every video encoder shares: how the kernel picks the encoder, the table layouts,
the mode word, the mode-change sequence, the encoder options and the NV2A registers per mode. What each encoder does
in its own steps, and its register tables, is in its own spec: **spec 18 Conexant CX25871** (1.0–1.3),
**spec 19 Focus FS453/FS454** (1.4–1.5), **spec 20 Xcalibur** (1.6).
Evidence: batch_decompile of 4627 `AvSetDisplayMode` (ord 3, 0x8003045C), `AvSendTVEncoderOption` (ord 2,
0x8002FFDE), the bit-29 set-up 0x8002FF23, the WSS/CGMS writer 0x8002FDD2 and the INIT table set-up 0x800626E0; of
5838 `AvSetDisplayMode` (0x80030F3D), `AvSendTVEncoderOption` (0x80030822), the encoder register writer 0x80030026,
the bit-29 set-up 0x800306FF, the WSS/CGMS routine 0x800305C8, the table loader 0x800315D4 and hardware detection
0x80054921.
The NV2A and CRTC table rows are not reproduced. OXOS's own display choices are spec/oxos-internal/02-display-modes.md.

## 1. Which encoder
### 1.1 4627
At boot (INIT 0x800626E0, cold boot) the kernel picks one of two table sets by **XboxHardwareInfo.Flags bit 0x20**
(spec 02 §5), not by probing the SMBus:
- flag 0x20 set → **Focus** (SMBus 0xD4), header at 0x80012AA4;
- clear → **Conexant** (SMBus 0x8A), header at 0x80012AB4.
The chosen set is copied from INIT into the STICKY section (so quick reboots keep it), the encoder address is stored
at 0x8003B354 and a pointer to the header at 0x8003C434. 4627 has no Xcalibur support.

### 1.2 5838
Detection (cold boot, `XboxHardwareInfo`, spec 02 §5) uses a byte of the BIOS image, not the bus:
- Map 4 bytes of physical **0xFFF00078** (flash offset 0x78; the 1.6 BIOS image holds 0x90 there, retail 4627
  images 0x60). If the byte is **≥ 0x80**: Flags |= 0x10, then
  - byte **== 0x90** → Flags |= **0x400** (Xcalibur);
  - otherwise, port 0x80CE bit 5 set → Flags |= 0x30 (Focus, as in 4627).
- (4627 only tests the byte for == 0x80 and has no 0x400 case.)

The encoder address is chosen at every boot (cold and quick, in the table loader 0x800315D4):
**Flags 0x20 → Focus 0xD4; else Flags 0x400 → Xcalibur 0xE0; else Conexant 0x8A**. Titles read
`XboxHardwareInfo`, so on a 1.6 console Flags bit 0x400 must be set and bit 0x20 clear.

## 1a. Table layouts
### 1a.1 4627: header (eight 16-bit counts) and tables in STICKY
| field | Conexant | Focus | meaning |
|---|---|---|---|
| 0 | 22 | 48 | encoder registers written for every SDTV mode (common part) |
| 1 | 63 | 61 | encoder registers per SDTV mode (bytes per row of the per-mode table) |
| 2 | 13 | 64 | encoder registers per HDTV mode |
| 3 | 6 | 28 | copy-protection registers, 525-line (option 1) |
| 4 | 1 | 28 | copy-protection registers, 625-line (same) |
| 5 | 104 | 104 | bytes per row of the NV2A register table (26 dword writes) |
| 6 | 34 | 34 | CRTC registers per row |
| 7 | 12 | 14 | registers of the mode-bit-29 table (§5 step 5) |

Each table is a *register list* (one row of register numbers / offsets) followed by *value rows*, one per index:
- 0x8003C810: SDTV common encoder registers: field 0 register numbers, then field 0 values (one row).
- 0x8003BCD0: SDTV per-mode encoder table: field 1 register numbers, then rows of field-1 values.
- 0x8003C710: HDTV per-mode encoder table: field 2 register numbers, then rows.
- 0x8003BB68: the bit-29 table: field 7 register numbers, then rows (meaning per encoder: specs 18, 19).
- 0x8003B358: NV2A registers: field 5 / 4 dword *offsets* (relative to the NV2A base), then rows of dword values.
- 0x8003C4C8: CRTC: field 6 CRTC indexes, then rows of values.
- 0x800362B0 (in .data, 4 bytes per NV row index): the value for NV2A 0x680880 (PRAMDAC).

### 1a.2 5838: the AV blob in flash
5838 does not compile mode tables into the kernel. At boot the table loader maps the top 4 MB of physical memory
(the flash, PA 0xFFC00000) with one large-page PDE at VA 0xC0C00000 (mapped once, reference-counted) and reads an
**AV blob** at PA **0xFFFF57A8** (offset 0x357A8 of the 256 KB image), **0x4658 bytes** long. It sits right after the
kernel's 0x610 bytes of initialised data in the image (the region 5838 re-copies from flash on a quick reboot).

- `u32 [3]` at 0: offset of the **descriptor** for Conexant, Focus, Xcalibur (in the 5838 image: 0x0C, 0x38, 0x64).
- Descriptor: `u32 [11]` table offsets: 0 SDTV common, 1 per-mode, 2 HDTV, 3 Macrovision SD, 4 Macrovision 625-line,
  5 NV2A (dword offsets + dword rows), 6 CRTC, 7 bit-29 table, 8 value pool (Xcalibur), 9 dword list (Xcalibur),
  10 counts header (`u16 [10]`, the row length of tables 0–9).
- Each table is a **register row** (register numbers, NV2A dword offsets or CRTC indexes) followed by **value rows** of
  the same length.
- Row lengths in the 5838 blob (bytes):

| table | Conexant | Focus | Xcalibur |
|---|---|---|---|
| 0 SDTV common | 22 | 49 | 0 |
| 1 per-mode | 63 | 64 | 42 |
| 2 HDTV | 13 | 64 | 0 |
| 3 Macrovision SD | 6 | 28 | 26 |
| 4 Macrovision 625-line | 1 | 28 | 26 |
| 5 NV2A | 104 | 104 | 104 |
| 6 CRTC | 34 | 34 | 34 |
| 7 bit-29 table | 12 | 14 | 0 |
| 8 value pool | 0 | 0 | 3044 (761 dwords) |
| 9 dword list | 0 | 0 | 368 (92 dwords) |

- Row counts: NV2A = register row + 18; CRTC = register row + 16; per-mode tables as in specs 18–20.
- **Validation:** every table pointer must lie inside the blob, and *pointer + row length + a per-table row count*
  must not pass its end (row counts checked: common 2, per-mode 30 or 36 for Xcalibur, HDTV 4, Macrovision 5 / 3,
  NV2A 19, CRTC 17, bit-29 table 30 (Conexant) or 3, pool 1, list 1). A failure **bug-checks** (code 0). This is a
  weak sanity check, not a full bounds check.
- The PRAMDAC 0x680880 values per NV2A row stay a small dword table in the kernel's initialised data (as in 4627).
- The quick-reboot path re-reads the kernel's initialised data from flash and compares its SHA-1 with the cold-boot
  value (a mismatch hangs): an integrity check of the image, not part of the driver.

## 2. Mode word (`Mode`, argument 3; 0 → 0x0F010101)
| bits | meaning |
|---|---|
| 0–6 | encoder row (SDTV or HDTV table; Xcalibur maps it first, spec 20) |
| 8–15 | CRTC row |
| 16–23 | NV2A register row (and PRAMDAC 0x680880 entry) |
| 24–27 | DAC output enable bits for the encoder (§5 step 12) |
| 28 | `0x80D6` bit 0 (GPIO: written as `(bit 28) | 4`); widescreen |
| 29 | the encoder's bit-29 set-up (§5 step 5): **SCART RGB** on Conexant and Focus; and clear NV 0x680630, 0x6808C4, 0x68084C (the NV2A then sends RGB) |
| 30–31 | class: `00` 525-line SD, `01` 625-line SD, `10` HD (the HDTV encoder table), `11` VGA |
The title-side D3D library builds these words, so **row numbers are ABI**: the same row means the same mode on
every encoder.

## 2a. Mode words titles send (observed)
The D3D mode words for each picture size, as listed by nxdk's `lib/hal/video.c` (MIT) and seen in titles'
AvSetDisplayMode calls: the class (bits 30–31) and the CRTC row (bits 8–15) give the size.
- HD: CRTC row 7 = 640×480p (0x88070701), 8 = 720×480p (0x88080801), 0x0A = 1280×720p (0x880B0A02), 0x0C =
  1920×1080i (0x880E0C03).
- VGA: 6 = 640×480, 3 = 800×600, 4 = 1024×768.
- SD: 1/3 = 640×480 (NTSC/PAL), 2/4 = 720×480.
- Enter the Matrix with 1080i allowed asks for 0x880E0C03 with pitch 0x1000, with only 720p allowed 0x880B0A02 with
  pitch 0x1400 (logged with a gdb breakpoint on AvSetDisplayMode).
- **1080i scanout:** the title renders each field as 1920×540 with 8192-byte lines and passes half of that (0x1000)
  as the pitch: in interlaced mode the CRTC offset is stepped twice per field line.

## 3. Format and pitch
`Format`: 0x10 / 0x1C → CRTC depth code 2 (16 bpp), general control 0x100030; 0x11 → depth 0x2000011 (565),
general control 0x101030; 0x12 / 0x1E → depth code 3 (32 bpp), 0x100030; others keep the 32-bit defaults. Pitch is
programmed in 8-byte units: CRTC 0x13 = pitch/8, CRTC 0x19 bits 5–7 = (pitch/8 >> 3) & 0xE0, CRTC 0x25 bit 5 =
(pitch/8 >> 6) & 0x20.

## 4. Same mode as last time (4627 0x8003B34C, 5838 0x8003C364)
Only: PRAMDAC general control (0x680600), unlock CRTC (0x1F ← 0x57), pitch registers, CRTC 0x28 ← depth | 0x80,
framebuffer start (0x600800), then `AvSendTVEncoderOption(base, 0x0B, 5)` and `(base, 0x0E, 0)`.

## 5. Mode change (full sequence)
Steps marked **[E]** are encoder-specific: specs 18–20 say what each encoder does there (nothing, if not listed).
0. **[E]** Before anything else (5838): Focus and Xcalibur switch their outputs off.
1. `out 0x80D3, 5`; wait until NV 0x6806A0 bits 0 and 4 agree (vertical blank phase).
2. CRTC unlock (0x1F ← 0x57), 0x21 ← 0xFF, 0x28 ← 0 (screen off); PRAMDAC 0x680880 ← 0x21121111 (bytes 11 11 12 21).
3. **[E]** Encoder reset (Conexant), with a 1 µs stall in the middle (all encoders stall 1 µs here).
4. **[E]** Encoder registers: class HD → the HDTV table row; else the SDTV common row, then the SDTV per-mode row
   (Xcalibur: only its per-mode row). Then, for console region **2** (Japan) and encoder rows 0x0D, 0x0E, 0x12, a
   per-encoder fix-up.
5. **[E]** Bit 29 → the encoder's bit-29 set-up.
5a. **[E]** Xcalibur only: further fixed writes and the flicker filter (spec 20).
6. `out 0x80D6, (bit 28) | 4`; `out 0x80D8, 4`; NV 0x68050C |= 0x10020000; VGA 0x3C3 ← 1, misc output 0x3C2 ← 0xE3;
   PRAMDAC general control.
   Port 0x80D6 is ACPI PM GPIO 0x16 (PM base 0x8000 + 0xC0 + 0x16), the AV connector's widescreen line. Option 0x13
   only updates bit 28 of the stored mode word and the WSS word (SD only), and titles' D3D leaves bit 28 clear in HD
   mode words (Halo 2 and Brute Force in 480p: 0x88070701), so in HD modes the line stays low.
7. NV2A register row (field 5 bytes of dword writes at the listed offsets, §7).
8. Bit 29: clear NV 0x680630, 0x6808C4, 0x68084C (NV2A output back to RGB, §7).
9. VGA sequencer (3 registers: 0x03, 0x21, 0x0F at indexes 0–2), graphics controller and attribute controller
   defaults (short fixed lists), then CRTC 0x11 ← 0.
10. CRTC row (field 6 registers; the pitch registers 0x13/0x19/0x25 get the pitch bits merged in).
11. Restore NV 0x6806A0 from step 1; CRTC 0x28 ← depth | 0x80 (screen on); wait for 3 vertical retraces (VGA
    status 0x3DA bit 3 high→low).
12. PRAMDAC 0x680880 ← the per-row value; stall 1 µs; **[E]** the encoder's DACs on from mode bits 24–27.
13. Framebuffer start 0x600800 ← `FrameBuffer`; remember the mode; WSS/CGMS update (§5a).

## 5a. AvSendTVEncoderOption
"Class" = mode word bits 30–31 of the current mode. Options marked **[E]** are carried out by the encoder (specs
18–20); the table gives the shared rules.

| Option | Behaviour |
|---|---|
| 1 Macrovision level 0–3 | **[E]**; the level is also kept for the WSS/CGMS word |
| 2 closed captions on | class 00 only, else no-op; **[E]** |
| 3 closed captions off | class 00 only; **[E]** |
| 4 caption bytes (argument bit 0 = field, data bytes d0 and d2) | class 00 only; **[E]** |
| 5 caption status | class 00, else 0; **[E]** |
| 6 AV capabilities | EEPROM-based, encoder-independent |
| 9 blank screen | on: VGA sequencer 1 ← 0x21. Off: sequencer 1 ← 1, **[E]** DACs back on, `out 0x80D3, 4` |
| 0x0A Macrovision commit | **[E]**; then WSS/CGMS update |
| 0x0B flicker filter, level 0–5 | **[E]** |
| 0x0C null mode | forget the current mode (the next `AvSetDisplayMode` does a full change) |
| 0x0D query mode | the current mode word |
| 0x0E luma filter on/off | **[E]** |
| 0x0F current field | 4627: Conexant register 6 bit 0, other encoders 0. 5838: ~(port 0x80C0 bit 5) & 1, for every encoder |
| 0x10 encoder type | 0 Conexant, 1 Focus, 2 Xcalibur (4627: 1 for Focus, else 0) |
| 0x11 mode table version | 0 |
| 0x12 CGMS bits | store; WSS/CGMS update |
| 0x13 widescreen | mode word bit 28 ← argument; WSS/CGMS update |

**WSS/CGMS update** (4627 0x8002FDD2, 5838 0x800305C8), after options 0x0A, 0x12, 0x13 and each mode set. The kernel
builds one word; only the final write differs per encoder **[E]**.
- **Class 00 (525-line SD):** data (14 bits): bit 0 = widescreen (mode bit 28); bits 6–7 = CGMS (option 0x12); bit 8
  = Macrovision level bit 1, bit 9 = level bit 0 (option 1). A **6-bit CRC** over data bits 0–13 (polynomial
  x⁶ + x + 1, initial value all ones, data processed from bit 0, as a shift register) goes into bits 14–19: the
  20-bit CGMS-A word of IEC 61880.
- **Class 01 (625-line SD):** word = (widescreen ? 7 : 8) | CGMS << 12 (no CRC): the WSS of ETSI EN 300 294, aspect
  code 8 = 4:3 full format, 7 = 16:9 anamorphic, with the copy bits in the upper group.
- **HD and VGA:** nothing is sent.

## 6. Boot animation (reference)
The boot animation lives in the INIT section. Its screen costs the title nothing (observed: objdump of
0x8001E990–0x8001E9E2): 0x8001E9F5 takes the saved display block, records the physical address of its pixels at
0x8003B2D0 and **returns its pages to the free lists** (tail insertion, contiguous count decremented) while the
scanout keeps showing them. Measured at the XBE entry point with the boot animation on and off: no pages besides the
kernel image, page 0, the page directory, the GPU instance memory and the PTE array are mapped (spec 09 §7).
Why (inferred): freed pages go to the end of the colour lists, so the picture survives until the title's video set-up
replaces it, at no cost to the title's budget.

## 7. NV2A registers per mode (field 5 rows, 26 dwords; 4627)
| Register | Role (name from nouveau where known) | Reference behaviour |
|---|---|---|
| 0x680800–0x680818 | flat-panel vertical: display end, total, CRTC, sync start / end, valid start / end | per mode |
| 0x680820–0x680838 | flat-panel horizontal: the same seven | per mode; display end = width − 1 in every row; the horizontal "CRTC" value is not the width (it differs per row) |
| 0x680848 | FP_TG_CONTROL | written every mode: DISPEN_POS, READ_PROG, positive HSYNC and VSYNC; MODE = centre for SD and 720p, scale for 480p; 1080i sets further bits (12–13, 16–17) not named by nouveau (interlace, inferred) |
| 0x68084C | FP_MARGIN_COLOR | per mode; cleared for bit 29 |
| 0x680630 | NV_PRAMDAC_630: the NV2A's output colour space (Cromwell: 0 = RGB, 2 = YUV) | **YCrCb** for every SD and HD row (an SD and an HD variant), cleared (RGB) for bit 29 |
| 0x6808C0, 0x6808C4 | unnamed (nouveau writes 0x8C0 = 0 for TV) | per mode; 0x6808C4 cleared for bit 29 |
| 0x680898, 0x68089C | unnamed | per mode |
| 0x680680–0x680690 | TV-out block: 0x680680 holds the encoder's H_CLKO; the others are not understood | per mode |
| 0x680880 | FP_DEBUG_0 (scaling) | 0x21121111 during the change (§5 step 2), then a per-row value from .data (§5 step 12) |

- **1080i** is programmed as a full frame (1125 lines, 1080 active) with the extra TG_CONTROL bits, not as one
  field.
- **HD rasters:** the flat-panel totals are the standard ones (858 × 525, 1650 × 750, 2200 × 1125) for every HD
  row; sync positions and widths differ from the nominal CEA-861 / SMPTE positions (the 720p and 1080i horizontal
  sync is wider than the standard's).
- **SD:** totals follow the encoder's input timing (Conexant H_CLKI / V_LINESI); the horizontal sync width is a
  constant per encoder.
- The 5838 kernel's NV2A rows differ from 4627's.
