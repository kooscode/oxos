# Spec 17 — AvSetDisplayMode, video encoder tables (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile of 0x8003045c (`AvSetDisplayMode`, ord 3) and disassembly of
the INIT table set-up 0x800626e0. This spec covers the tables' layout and how they are indexed. The encoder register
values themselves (Conexant and Focus) are recorded as plain register maps in spec 20 and spec 21 (functional
hardware constants, see `dmca-compliance.md` §3); the NV2A and CRTC rows are not reproduced.

## 1. Which encoder
At boot (INIT 0x800626e0, cold boot) the kernel picks one of two table sets by **XboxHardwareInfo.Flags bit 0x20**
(spec 02 §5), not by probing the SMBus:
- flag 0x20 set → **Focus FS454** (SMBus 0xD4), header at 0x80012AA4
- clear → **Conexant CX25871** (SMBus 0x8A), header at 0x80012AB4
The chosen set is copied from INIT into the STICKY section (so quick reboots keep it), the encoder address is stored
at 0x8003B354 and a pointer to the header at 0x8003C434. (The 1.6 Xcalibur encoder, 0xE0, is not in this kernel; the 5838 kernel's Xcalibur driver is spec 19.)

### Header (eight 16-bit counts)
| field | Conexant | Focus | meaning |
|---|---|---|---|
| 0 | 22 | 48 | encoder registers written for every SDTV mode (common part) |
| 1 | 63 | 61 | encoder registers per SDTV mode (bytes per row of the per-mode table) |
| 2 | 13 | 64 | encoder registers per HDTV mode |
| 3 | 6 | 28 | copy-protection registers, 525-line (AvSendTVEncoderOption 0x0A) |
| 4 | 1 | 28 | copy-protection registers, 625-line (same) |
| 5 | 104 | 104 | bytes per row of the NV2A register table (26 dword writes) |
| 6 | 34 | 34 | CRTC registers per row |
| 7 | 12 | 14 | registers of the mode-bit-29 (SCART RGB) table, written by 0x8002FF23 (§8.3) |

### Tables (STICKY)
Each table is *register list* (one row of register numbers / offsets) followed by *value rows*, one per index:
- 0x8003C810: SDTV common encoder registers: field 0 register numbers, then field 0 values (one row).
- 0x8003BCD0: SDTV per-mode encoder table: field 1 register numbers, then rows of field-1 values.
- 0x8003C710: HDTV per-mode encoder table: field 2 register numbers, then rows.
- 0x8003B358: NV2A registers: field 5 / 4 dword *offsets* (relative to the NV2A base), then rows of dword values.
- 0x8003C4C8: CRTC: field 6 CRTC indexes, then rows of values.
- 0x800362B0 (in .data, 4 bytes per NV row index): the value for NV2A 0x680880 (PRAMDAC).

## 2. Mode word (`Mode`, argument 3; 0 → 0x0F010101)
| bits | meaning |
|---|---|
| 0–6 | encoder row (SDTV or HDTV table) |
| 8–15 | CRTC row |
| 16–23 | NV2A register row (and PRAMDAC 0x680880 entry) |
| 24–27 | DAC output enable bits for the encoder (`0xBA ← (bits 24–27) | 0x20` on Conexant) |
| 28 | `0x80D6` bit 0 (GPIO: written as `(bit 28) | 4`) |
| 29 | **SCART RGB**: call 0x8002FF23 (writes the field-7 table row: SCART output, RGB input; §8.3) and clear NV 0x680630, 0x6808C4, 0x68084C (the NV2A then sends RGB). Earlier versions of this spec called it "HDTV"; the decoded table shows SCART |
| 30–31 | `10b` → use the HDTV encoder table instead of the SDTV ones |
The title-side D3D library builds these words, so **row numbers are ABI**: our tables need the same row meaning.

## 3. Format and pitch
`Format`: 0x10 / 0x1C → CRTC depth code 2 (16 bpp), general control 0x100030; 0x11 → depth 0x2000011 (565),
general control 0x101030; 0x12 / 0x1E → depth code 3 (32 bpp), 0x100030; others keep the 32-bit defaults. Pitch is
programmed in 8-byte units: CRTC 0x13 = pitch/8, CRTC 0x19 bits 5–7 = (pitch/8 >> 3) & 0xE0, CRTC 0x25 bit 5 =
(pitch/8 >> 6) & 0x20.

## 4. Same mode as last time (0x8003B34C)
Only: PRAMDAC general control (0x680600), unlock CRTC (0x1F ← 0x57), pitch registers, CRTC 0x28 ← depth | 0x80,
framebuffer start (0x600800), then `AvSendTVEncoderOption(base, 0x0B, 5)` and `(base, 0x0E, 0)`.

## 5. Mode change (full sequence)
1. `out 0x80D3, 5`; wait until NV 0x6806A0 bits 0 and 4 agree (vertical blank phase).
2. CRTC unlock (0x1F ← 0x57), 0x21 ← 0xFF, 0x28 ← 0 (screen off); PRAMDAC 0x680880 ← 0x21121111 (bytes 11 11 12 21).
3. Conexant only: 0xBA ← 0x80 (reset), 0xBA ← 0x3F, 0x6C ← 0x46; stall 1 µs; 0x6C ← 0xC6.
4. Encoder registers: HDTV table row (bits 30–31 = 10b) **or** the SDTV common row then the SDTV per-mode row.
   SDTV extra for console region **2** (0x8003B158 == 2, Japan) and encoder rows 0x0D, 0x0E, 0x12: Conexant
   0xA2 ← 8, 0xA4 ← 0xF0, 0xAC ← 0x9A; Focus word write 0x4E ← 0x13C.
5. Bit 29 → SCART set-up routine 0x8002FF23(base, row): the field-7 table row (§8.3).
6. `out 0x80D6, (bit 28) | 4`; `out 0x80D8, 4`; NV 0x68050C |= 0x10020000; VGA 0x3C3 ← 1, misc output 0x3C2 ← 0xE3;
   PRAMDAC general control.
7. NV2A register row (field 5 bytes of dword writes at the listed offsets).
8. Bit 29: clear NV 0x680630, 0x6808C4, 0x68084C (NV2A output back to RGB, §8.4).
9. VGA sequencer (3 registers: 0x03, 0x21, 0x0F at indexes 0–2), graphics controller and attribute controller
   defaults (short fixed lists), then CRTC 0x11 ← 0.
10. CRTC row (field 6 registers; the pitch registers 0x13/0x19/0x25 get the pitch bits merged in).
11. Restore NV 0x6806A0 from step 1; CRTC 0x28 ← depth | 0x80 (screen on); wait for 3 vertical retraces (VGA
    status 0x3DA bit 3 high→low).
12. PRAMDAC 0x680880 ← the per-row value; stall 1 µs; Conexant: 0x6C |= 0x80, 0xBA ← (bits 24–27) | 0x20.
13. Framebuffer start 0x600800 ← `FrameBuffer`; remember the mode; 0x8002FDD2 (saved-data / splash bookkeeping).

## 5a. AvSendTVEncoderOption: encoder options (Conexant, Focus)
Evidence: batch_decompile of `AvSendTVEncoderOption` (ord 2, 0x8002FFDE), the WSS/CGMS writer 0x8002FDD2 and the
Macrovision commit 0x8002FBAA. "Class" = mode word bits 30–31 (00 525-line SD, 01 625-line SD, 10 HD, 11 VGA).
Conexant writes are bytes; Focus registers 0xAE–0xB2, 0xC2 and 0x0E are words.

| Option | Conexant (0x8A) | Focus (0xD4) |
|---|---|---|
| 2 closed captions on (class 00 only) | 0xC4 ← 0xC4 \| 0x38 (ECCF2, ECCF1, ECCGATE), then 4 caption-timing registers from a per-row table | 0xB2 ← 0xB2 \| 0xC000 |
| 3 closed captions off (class 00) | 0xC4 ← 0xC4 & 0xCF | 0xB2 ← 0xB2 & 0x3FFF |
| 4 caption bytes (class 00; argument bit 0 = field, data bytes d0 and d2) | field 0: 0xBC ← d2, 0xBE ← d0; field 1: 0xC0 ← d2, 0xC2 ← d0 | field 0: 0xB0 ← d2 << 8 \| d0; field 1: 0xAE ← the same |
| 5 caption status (class 00, else 0) | register 2 bits 2–3 | bit 0 = 0xAE ≠ 0, bit 1 = 0xB0 ≠ 0 |
| 0x0A Macrovision commit | 0xFC ← 0 / 0x63 / 0xE3 (0x42 in 480p) by level | 0x0E bit 15 ← level ≠ 0 |
| 0x0B flicker filter, level 0–5 | level 0: 0xC8 \|= 0x40 (DIS_FFILT); else n = 1, 2, 3 for levels 1–3, 0 for 4–5: 0xC8 ← (0xC8 & 0x80) \| n << 3 \| n (F_SELC = F_SELY = n), 0x34 ← 0x80 (ADPT_FF) for level 5, else 0 | 0x26 ← level × 16 / 5, 0x27 ← 0 |
| 0x0E luma filter on/off | 0x96 ← (0x96 & 0x0F) \| (on ? 0x10 : 0) (YLPF) | 0xC2 ← on ? 0x0052 : 0x00EE |
| 0x0F current field | register 6 bit 0 | 0 |
| 0x12 CGMS bits, 0x13 widescreen | store the value (0x13: mode word bit 28), then WSS/CGMS update | the same |

**WSS/CGMS update (0x8002FDD2)**, also after option 0x0A (and each mode set): class 00 → the 20-bit CGMS-A word (widescreen
bit 0, CGMS bits 6–7, Macrovision level bits 8–9, 6-bit CRC in 14–19; IEC 61880); class 01 → (widescreen ? 7 : 8) \|
CGMS << 12 (ETSI EN 300 294 WSS); other classes nothing. Conexant: 0x60 ← (word & 0xF) \| 0xC0 (EWSSF1, EWSSF2),
0x62 ← word >> 4, 0x64 ← word >> 12. Focus: 0x85 ← word & 0xF, 0x84 ← word >> 4, 0x83 ← word >> 12 (field 1), and the
same into 0x88, 0x87, 0x86 (field 0).
**Ours:** the same option behaviour; the clock-dependent Conexant values (caption timing CCR_START / CC_ADD, the WSS
bit clock WSSINC) are computed from the datasheet's equations per mode (spec/oxos-internal/06-tv-encoders.md §13), not
taken from the reference's table. Macrovision stays off (options 1 and 0x0A do nothing; the CGMS-A word carries
level 0).

## 6. Ours (status)
`src/hal/av.cpp` programs the NV2A CRTC timing for the picture size the mode word asks for (§7). On Conexant and
Focus encoders, SD (composite, S-video, SCART, component) and HD component mode words also program the encoder and
the NV2A flat-panel timing from OXOS's own computed tables (spec/oxos-internal/06-tv-encoders.md, milestone 7); VGA and
the Xcalibur leave the encoder alone. Not yet confirmed on real hardware.

The NV extended CRTC register 0x39 (interlace half-line position) must be 0xFF for progressive output: the
reference leaves it at 0xFF, and xemu treats any other value as interlaced and shows each field at half height
(observed: Halo's menu squished into the top half of the window). Line compare (CR18) is likewise 0xFF (never).

Of step 6 ours does the GPIO writes (`0x80D6 ← (bit 28) | 4`, `0x80D8 ← 4`) on every mode change. Port 0x80D6 is
ACPI PM GPIO 0x16 (PM base 0x8000 + 0xC0 + 0x16), the AV connector's widescreen line: titles that render 16:9 set
mode bit 28 (option 0x13 or their D3D mode word), and xemu's "auto" aspect ratio shows 16:9 exactly when the last
value written there is 5. Without the write every title shows 4:3 in xemu (anamorphic pictures look squashed).
The line is raised only for SDTV modes: in the reference, option 0x13 just updates bit 28 of the stored mode word
and the WSS encoder registers (0x8002fdd2, SDTV only; nothing for HDTV-table modes), and titles' D3D leaves bit 28
clear in HDTV mode words (Halo 2 and Brute Force in 480p: 0x88070701). So in 480p/720p/1080i xemu's "auto" shows
4:3 under both kernels. **Ours (deviation since milestone 13):** the line is also raised for 720p and 1080i mode words
(HDTV table, CRTC rows 0x0A and 0x0C), which are always 16:9, so xemu's "auto" shows them at 16:9; 480p keeps the
reference rule (bit 28 only), since 480p can be 4:3 or 16:9.

`AvSendTVEncoderOption`: queries 6 (AV capabilities), 0x0D (current mode word), 0x05 (caption status), 0x0F (current
field, Conexant reg 6 bit 0), 0x10 (encoder type: 0 Conexant, 1 Focus, 2 Xcalibur, from the bus probe), 0x11 (mode
table version, 0); options 9 (blank), 0x0C (null mode), 0x13 (widescreen) and, on Conexant and Focus, the encoder
options of §5a (captions, flicker filter, luma filter, WSS/CGMS). Macrovision (1, 0x0A) stays off.

## 7a. Display timings per mode (ours; milestone 13)
**Row meanings (observed, not from the reference's tables):** the D3D mode words for each picture size, as listed
by nxdk's `lib/hal/video.c` (MIT) and seen in titles' AvSetDisplayMode calls: the table bits 30–31 (10b HDTV,
11b VGA, else SDTV) and the CRTC row (bits 8–15) give the size. HDTV: row 7 = 640×480p (0x88070701), 8 =
720×480p (0x88080801), 0x0A = 1280×720p (0x880B0A02), 0x0C = 1920×1080i (0x880E0C03); VGA: 6 = 640×480, 3 =
800×600, 4 = 1024×768; SDTV: 1/3 = 640×480 (NTSC/PAL), 2/4 = 720×480. Enter the Matrix with 1080i allowed asks for
0x880E0C03 with pitch 0x1000, with only 720p allowed 0x880B0A02 with pitch 0x1400 (logged with a gdb breakpoint on
AvSetDisplayMode).
**Ours:** timings from the public standards for those sizes (CEA-861 480p, SMPTE 296M 720p: 1650×750 total,
SMPTE 274M 1080i: 2200×1125, programmed per field with CR39 = interlace half-line, VESA 800×600/1024×768); unknown
rows fall back to 640×480. High bits in the NV extended CRTC registers CR25 (vertical bit 10s, kept with the mode
across quick reboots and merged with the pitch bit) and CR2D (horizontal bit 8s). xemu sizes its display from
CR01, CR12/CR07 and CR39 (doubling interlaced heights): measured 1920×1080 interlaced and 1280×720 for the two modes
above (before: 640×480 for every mode).
**xemu note:** Enter the Matrix in 720p or 1080i aborts xemu at render scale 2 or more (assertion in
`surface_download_to_buffer`: one of the title's render surfaces has a pitch smaller than width × bytes per
pixel, which xemu's downscale path rejects); at scale 1 it runs. The retail BIOS we have returns this title to the
dashboard within 0.3 s in xemu, so there is no reference run to compare.
**1080i scanout (observed):** the title renders each field as 1920×540 with 8192-byte lines and passes half of that
(0x1000) as the pitch: in interlaced mode the CRTC offset is stepped twice per field line. xemu's display code
expects this (it divides the surface pitch by the CRTC offset); ours programs the pitch as given.

## 7. Boot splash image (ours; no reference counterpart)
The reference kernel's boot animation lives in its INIT section and is not reproduced. Ours shows a picture
during cold boot (`src/hal/splash.cpp`):

- `tools/mkbios.py --splash IMAGE` (the Makefile passes `kernel/splash/splash.png` when present) keeps a
  640×480 palette PNG as it is (no Pillow needed; the committed splash is one, and `--save-splash PNG`
  writes such a file); any other image needs Pillow and is scaled to fit 640×480 keeping its aspect ratio (black bars),
  pixels with every channel ≤ 6 are set to black (background noise that costs bytes but is invisible), and it is
  reduced to 256 colours without dithering: octree by default (`SPLASH_QUANT=octree`; smallest, suits textured
  art: 93 KB for the current splash against 241 KB with median cut), or `SPLASH_QUANT=median` for images with
  smooth gradients, where octree shows banding. The PNG is re-chunked to a single IDAT and
  stored in flash bank 1 (offset
  0x40000, physical 0xFFF40000) behind a 20-byte header: `"XKSPLASH"`, u32 version 1, u32 PNG length, u32
  CRC-32 of the PNG. Banks 0 and 3 must stay as they are: the MCPX boot ROM reads its init table at 0xFF000000
  (bank 0 of a 1 MiB chip) and the 2BL/kernel from the top bank. A 256 KiB image has no spare bank, so no splash.
- The splash thread maps the bank with `MmMapIoSpace(…, PAGE_READONLY | PAGE_NOCACHE)` and decodes it with
  `src/hal/png.cpp`: inflate (RFC 1951), Adler-32 check, PNG scanline un-filtering. Supported input is exactly
  what mkbios writes, plus 8-bit RGB/RGBA: not interlaced, one IDAT. The inflated scanlines (≤ 1.2 MB) go to a
  temporary buffer below 16 MB.
- When there is no valid header or decoding fails, the drawn emblem and caption are shown instead.
  `HalpSplashSource` (1 = drawn, 2 = flash image) and `HalpSplashStatus` (0 = shown; 0x101 no header,
  0x102 bad header, 0x103 out of memory, 1–10 decoder step) record what happened.
- It fades in over 24 frames (~1 s) while boot continues; there is no fixed hold time.
- **Reference behaviour (observed: objdump of 0x8001e990–0x8001e9e2):** the boot screen costs the
  title nothing. 0x8001e9f5 takes the saved display block, records the physical address of its pixels at
  0x8003b2d0 and **returns its pages to the free lists** (tail insertion, contiguous count decremented) while the
  scanout keeps showing them. Measured at the XBE entry point with the boot animation on and off: no pages besides
  the kernel image, page 0, the page directory, the GPU instance memory and the PTE array are mapped (spec/stock/09 §7).
  Why (inferred): freed pages go to the end of the colour lists, so the picture survives until the title's video
  set-up replaces it, at no cost to the title's budget.
- **Ours:** the launcher waits for the fade, then frees both the decode buffer and the screen buffer and leaves the
  scanout on the screen buffer. Until 2026-10-17 the screen buffer was kept as the persisted display
  (`AvSetSavedDataAddress`), which held 301 pages (1.2 MB) at the title's entry point that the reference does not.

## 8. Encoder programming per class (reference behaviour, 4627)
Evidence: the INIT tables at 0x800a7c18–0x800aa0c0 (Focus and Conexant sets; layout §1), decoded field by field
with the chips' datasheets (sources in spec/oxos-internal/06-tv-encoders-sources.md). This section records **what
the reference programs and what it means**. Per-mode values (timings, clocks, gains, filter taps) are table data
and are recorded in spec 20 (Conexant) and spec 21 (Focus). "Class" = mode word bits 30–31.
The 5838 kernel's Conexant tables are identical to 4627's except the NV2A rows; its Focus tables differ.

### 8.1 Conexant CX25871, which registers per class
- **SD common row (22 registers, every SD mode):** WSS / caption control, pad drive and input-format bits (0x32),
  EN_OUT (0xC4), input format (0xC6), flicker filter (0xC8), and a few constant set-up registers.
- **SD per-mode row (63 registers):** the output mode (0xD6), the full timing set (H_CLKO … V_SCALE), PLL, the
  standard bits (0xA2: SETUP, 625LINE, VSYNC_DUR, PAL_MD), levels (SYNC_AMP, BST_AMP, MCR, MCB, MY), the subcarrier
  (MSC), WSSINC, DAC routing (0xCE), the flicker / luma filters and the copy-protection registers.
- **HD row (13 registers):** output mode (0xD6), HDTV control (0x2E), pad drive / CSC (0x32), component gains
  (0x3C/0x3E/0x40), EN_OUT (0xC4), input format (0xC6), DAC routing (0xCE), PLL (0xA0/0x9E/0x9C), timing reset
  (0x6C).
- Rows exist for SD encoder rows 0x01–0x1D (composite / S-video rows 0x01–0x0C, SD component rows 0x0D–0x1A among
  them) and HD rows 1–3 (480p, 720p, 1080i).

### 8.2 Conexant interface facts (decoded)
| Fact | SD (composite / S-video, rows 0x01–0x0C) | SD component (rows 0x0D–0x1A) | SCART (bit 29) | HD (480p, 720p, 1080i) |
|---|---|---|---|---|
| Input format (IN_MODE) | alternate 24-bit **YCrCb** multiplexed | YCrCb | alternate 24-bit **RGB** multiplexed | YCrCb |
| Pad drive (DRVS) | 1.8 V | 1.8 V | 1.8 V | **1.5 / 1.3 V** |
| Interface | pseudo-master (EN_BLANKO), active-high HSYNC / VSYNC inputs, FLD_MODE 10, EACTIVE | same | same | same |
| Clock source (EN_XCLK) | encoder PLL | encoder PLL | encoder PLL | **CLKI for 480p only**, PLL for 720p / 1080i |
| Output mode (OUT_MODE) | 00 (CVBS / Y / C / Y_DLY) | 10 (V / Y / U / Y_DLY) | 11 + EN_SCART | 11 + HDTV_EN |
| DAC routing (OUT_MUX A/B/C/D) | A = Y, B = C, C = Y, D = CVBS | **A = Y, B = Pr (V), C = Pb (U)**, D = Y_DLY | A = G, B = R, C = B, D = CVBS (sync) | **A = Y, B = Pr, C = Pb** |
| Setup (7.5 IRE) | NTSC-M yes; NTSC-J, PAL no | **NTSC component yes**, PAL no | none | n/a |
| Vertical sync (VSYNC_DUR) | 3 lines for 525-line NTSC rows, **2.5 lines for PAL-60** and 625-line rows | as SD | as SD | n/a |
| HDTV control | n/a | n/a | n/a | tri-level sync off on Pr and Pb (EIA-770.3), RGB2YPRPB off (YCrCb in), HD_SYNC_EDGE for 480p only, RASTER_SEL per format, CSC_SEL for 720p / 1080i |
| Component gains | n/a | own values (V gain differs from the datasheet's Table 1-27) | n/a | own values, not Table E-1 / E-2's |
| Sequence | SRESET, 0xBA ← 0x3F, 0x6C ← 0x46, 1 µs, 0x6C ← 0xC6 (§5 step 3), common row, per-mode row | same | then the bit-29 row | HD row |

The DAC routing for component and SCART is the same mapping (A = the Y / G pin, B = the Pr / R pin, C = the Pb / B
pin), and matches the AV connector pinout (xboxdevwiki AV_Cables: pin 11 Y/G, 22 C/R/Pr, 9 B/Pb, 24 CVBS) together
with the composite routing (A carries Y to pin 11, B carries C to pin 22, D carries CVBS to pin 24).

### 8.3 Bit 29: the SCART table (field 7)
12 Conexant registers (14 Focus): the output mode with EN_SCART (or the Focus VID_CNTL0 plus its colour matrix),
the RGB input format, the DAC routing above and the standard bits. Rows exist only for the composite encoder rows;
the SD component rows' entries are empty. So bit 29 means "SCART RGB instead of composite", and the reference
switches the NV2A to RGB for it (§8.4).

### 8.4 NV2A registers per mode (field 5 rows, 26 dwords)
| Register | Role (name from nouveau where known) | Reference behaviour |
|---|---|---|
| 0x680800–0x680818 | flat-panel vertical: display end, total, CRTC, sync start / end, valid start / end | per mode |
| 0x680820–0x680838 | flat-panel horizontal: the same seven | per mode; display end = width − 1 in every row; the horizontal "CRTC" value is not the width (it differs per row) |
| 0x680848 | FP_TG_CONTROL | written every mode: DISPEN_POS, READ_PROG, positive HSYNC and VSYNC; MODE = centre for SD and 720p, scale for 480p; 1080i sets further bits (12–13, 16–17) not named by nouveau (interlace, inferred) |
| 0x68084C | FP_MARGIN_COLOR | per mode; cleared for SCART (bit 29) |
| 0x680630 | NV_PRAMDAC_630: the NV2A's output colour space (Cromwell: 0 = RGB, 2 = YUV) | **YCrCb** for every SD and HD row (an SD and an HD variant), cleared (RGB) for SCART |
| 0x6808C0, 0x6808C4 | unnamed (nouveau writes 0x8C0 = 0 for TV) | per mode; 0x6808C4 cleared for SCART |
| 0x680898, 0x68089C | unnamed | per mode |
| 0x680680–0x680690 | TV-out block: 0x680680 holds the encoder's H_CLKO; the others are not understood | per mode |
| 0x680880 | FP_DEBUG_0 (scaling) | 0x21121111 during the change (§5 step 2), then a per-row value from .data (§5 step 12) |

- **1080i** is programmed as a full frame (1125 lines, 1080 active) with the extra TG_CONTROL bits, not as one
  field.
- **HD rasters:** the flat-panel totals are the standard ones (858 × 525, 1650 × 750, 2200 × 1125) for every HD
  row; sync positions and widths differ from the nominal CEA-861 / SMPTE positions (the 720p and 1080i horizontal
  sync is wider than the standard's).
- **SD:** totals follow the encoder's H_CLKI / V_LINESI; the horizontal sync width is a constant per encoder.

### 8.5 Focus FS453/4, which registers per class
- **Not used:** the quick-program register QPR (0xC4) is never written. The reference programs every register
  explicitly.
- **Common row (48 registers):** clock / interface registers including MISC (0x0E: UIM_MOD 5, a mode not in
  the GPL header's list, plus UIM_DCLK and BRDG_RST), DAC_CNTL (A–D = video 0–3, identity), PWR_MGNT (SD: HD
  clock off), PLL M / P, the colour matrix and flicker / sharpness.
- **SD per-mode row (61 registers):** CR (with PAL_NTSCIN set for the PAL rows), the scaler (IHO, IVO, IHW, VSC,
  HSC), NCO (NCON / NCOD), PLL N, the TV-standard block 0x38–0x80, VID_CNTL0, and the caption and WSS registers.
- **HD row (64 registers):** CR, BYPASS **0** (scaler not bypassed), MISC, VID_CNTL0, DAC_CNTL, PWR_MGNT (SD clock
  off), NCO, PLL, the HD timing block 0x92–0xBF and the colour matrix.

### 8.6 Focus interface facts (decoded)
| Fact | SD composite / S-video | SD component | SCART (bit 29) | HD |
|---|---|---|---|---|
| VID_CNTL0 | VID_MODE composite, MATRIX_BYP (input already YCrCb), INT_PROG | VID_MODE YPrPb, MATRIX_BYP, INT_PROG | VID_MODE SCART / RGB (two table rows; which mode rows use which is not decoded) | VID_MODE HD, MATRIX_BYP, SYNC_ADD; 480p sets SYNC_BI_TRI (bi-level, inferred from SMPTE 293M), 720p / 1080i clear it; 1080i INT_PROG |
| Input | YCrCb (NV 0x680630 as Conexant) | YCrCb | RGB | YCrCb |
| Clock | NCO + PLL per row | same | same | NCO + PLL per row |
