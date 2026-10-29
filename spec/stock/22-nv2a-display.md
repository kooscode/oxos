# Spec 22 — NV2A display set-up: mode change, clocking, framebuffer, boot display (xboxkrnl 1.0.4627.1, 1.0.5838.1)

The NV2A side of a display mode set and of the boot display, for consoles 1.0–1.6, with the Focus FS453/FS454 (1.4–1.5)
as the worked case; the Conexant CX25871 (1.0–1.3) and Xcalibur (1.6) rows, clocking and differences are §3.7–§3.12,
§5.1–§5.2 and §13–§15. The encoder side is spec 18 (Conexant), 19 (Focus), 20 (Xcalibur); the shared driver is spec 17;
the chipset bring-up before the kernel is spec 21. NV2A registers are offsets from the register base the caller passes
(0xFD000000 on every console). "CR*xx*" is CRTC index *xx* (index port base + 0x6013D4, data + 0x6013D5); "SR*xx*" the
sequencer (+0x0C03C4 / +0x0C03C5), "GR*xx*" the graphics controller (+0x0C03CE / +0x0C03CF), "AR*xx*" the attribute
controller (+0x6013C0, index and data written to the same address). Every VGA-style access is a byte access to this
memory-mapped window; the reference never uses I/O ports 0x3C0–0x3DF. Register names in parentheses are nouveau's
where it has one (used as labels only). **Status: reference only.**

## 1. Who programs what
| Stage | NV2A display / memory registers written | Source |
|---|---|---|
| X-codes (flash, before the 2BL) | PFB 0x100200 / 0x100204 (memory configuration, §6), PFB 0x100210, 0x100228, 0x1002C0–0x1002D4, 0x100328–0x100338, 0x100410; NVPLL 0x680500; PLL select 0x68050C ← 0x000A0400 | (flash header) |
| 2BL | only the pad calibration 0x001214–0x001248 (spec 21 §3); no display register | (Complex 4627 2BL) (5838 2BL) |
| Kernel, `AvSetDisplayMode` | PRAMDAC general control, PLL select (OR only), the per-mode NV2A row (flat-panel timing, colour space, TV block), PRAMDAC 0x680880, 0x6806A0, VGA sequencer / graphics / attribute, CRTC row, framebuffer start (§2) | (4627 kernel) (5838 kernel) |
| Kernel, boot animation's graphics device (the kernel's own copy of the title graphics library) | PMC / PBUS / PFIFO / PGRAPH / PTIMER / PCRTC interrupt set-up, instance memory, CRTC FIFO registers, PCRTC 0x600804, PVIDEO 0x8088 / 0x808C (§7); framebuffer start on each flip (§8) | (4627 kernel) (5838 kernel) |
| Title | its own graphics library repeats the §7 device set-up and calls `AvSetDisplayMode` | — |

- No stage writes the video PLL (VPLL, 0x680508) or the memory PLL (MPLL, 0x680504). The kernel only *reads*
  0x680500, 0x680504 and 0x680508 (§7 step 4) (4627 kernel) (5838 kernel) (Cerbios). This holds for every encoder,
  for the HD and VGA-pack modes and with no encoder (§13).
- `AvSetDisplayMode` performs no PMC, PFB, PTIMER or instance-memory access, and never writes PCRTC 0x600804
  (4627 kernel) (5838 kernel).

## 2. `AvSetDisplayMode`: the NV2A writes in order (4627 and 5838)
Arguments: register base, step (ignored; the function always returns 0), mode word (0 → 0x0F010101), format, pitch
(bytes), framebuffer. Mode word layout: spec 17 §2. Encoder steps (**[E]**): spec 17 §5 and specs 18–20.

### 2.1 Before anything else
- Format → general control (GC) and depth code (D): 0x10 / 0x1C → GC 0x00100030, D 2; 0x11 → GC 0x00101030, D 2;
  0x12 / 0x1E → GC 0x00100030, D 3; any other format → GC 0x00100030 and D 0 (4627: D keeps the format's own top byte,
  which is 0 for every valid format code). GC bit 12 (nouveau: ALT_MODE_SEL) selects 5:6:5 for format 0x11.
- P8 = pitch / 8 (pitch in bytes, truncated).

### 2.2 Same mode word as the last full change
Writes, in order: 0x680600 ← GC; CR1F ← 0x57 (unlock); CR13 ← P8 & 0xFF; CR19 ← (P8 >> 3) & 0xE0 (all other CR19
bits written 0); CR28 ← D | 0x80; 0x600800 ← framebuffer; then the encoder options 0x0B (level 5) and 0x0E (off).
CR25 is not written on this path (its pitch bit keeps the value of the last full change).

### 2.3 Full change (mode word differs, or option 0x0C forgot the mode)
| # | Write / access | Condition |
|---|---|---|
| 0 | **[E]** 5838: Focus PWR_MGNT DACs off; Xcalibur outputs off | 5838 |
| 1 | port 0x80D3 ← 5; read 0x6806A0 until its bit 0 equals its bit 4; keep the last value V | always |
| 2 | CR1F ← 0x57; CR21 ← 0xFF; CR28 ← 0x00 (pixel format off = display off) | always |
| 3 | 0x680880 ← 0x21121111 (one dword write) | always |
| 4 | **[E]** encoder reset (Conexant only), 1 µs stall, encoder rows, Japan fix-up, bit-29 set-up | always |
| 5 | port 0x80D6 ← (mode bit 28) \| 4; port 0x80D8 ← 4 | always |
| 6 | 0x68050C ← 0x68050C \| 0x10020000 (read-modify-write) | always |
| 7 | misc output: byte 0x0C03C3 ← 0x01, then 0x0C03C2 ← 0xE3 | always |
| 8 | 0x680600 ← GC | always |
| 9 | NV2A row (mode bits 16–23): 26 dword writes in the register order of §3 | always |
| 10 | 0x680630 ← 0, 0x6808C4 ← 0, 0x68084C ← 0 (dword writes) | mode bit 29 |
| 11 | SR00..SR04 ← 0x03, 0x21, 0x0F, 0x00, 0x06 | always |
| 12 | GR00..GR08 ← 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF | always |
| 13 | AR00..AR14: for each index *i*, write *i* then the value to 0x6013C0: 0x00–0x0F identity, AR10 0x01, AR11 0x4A, AR12 0x0F, AR13 0x00, AR14 0x00; then 0x20 (palette address source = display) | always |
| 14 | CR11 ← 0x00 (unlocks CR00–CR07) | always |
| 15 | CRTC row (mode bits 8–15): 34 writes in the index order of §4; CR13 ← P8 & 0xFF instead of the row byte; CR19 ← row \| ((P8 >> 3) & 0xE0); CR25 ← row \| ((P8 >> 6) & 0x20) | always |
| 16 | 0x6806A0 ← (~V) & 1 (the complement of bit 0 of the value read in #1; all other bits 0) | always |
| 17 | CR28 ← D \| 0x80 (pixel format on, bit 7 = "TV" timing per Cromwell) | always |
| 18 | three vertical retraces: for each, wait while 0x6013DA bit 3 is 1, then while it is 0 | always |
| 19 | 0x680880 ← per-row value (§3.3), indexed by mode bits 16–23 | always |
| 20 | 1 µs stall; **[E]** encoder DACs on (Conexant: 0x6C bit 7, 0xBA; Xcalibur: register 4) | always |
| 21 | 0x600800 ← framebuffer (unchanged, §6.2); remember the mode word; WSS / CGMS update | always |

- The sequencer list leaves **SR01 = 0x21 (screen off)**. The picture appears only when `AvSendTVEncoderOption`
  option 9 ("blank") is called with 0, which writes SR01 ← 0x01 and port 0x80D3 ← 4; the graphics library does this
  after its first frame (§8) (4627 kernel) (5838 kernel).
- The AR writes are not preceded by a read of 0x6013DA (the reference relies on the index/data flip-flop being in the
  index state).
- 4627 stores the new mode word at #21; 5838 stores it at the start of the full change (before #0). Same result unless
  the call is re-entered (5838 kernel).
- The order and values are identical in 4627, 5838 and Cerbios except where marked (Cerbios: §10).

## 3. The NV2A row (mode bits 16–23)
### 3.1 Layout and selection
- The table is a register row of 26 dword offsets followed by value rows of 26 dwords; mode bits 16–23 select value row
  *n* (row 0 is the register row itself, so valid rows are 1–0x12). 4627: compiled into the kernel, one table per
  encoder family (Conexant set, Focus set; chosen by `XboxHardwareInfo` flag 0x20, spec 17 §1). 5838: from the AV
  blob (spec 17 §1a.2), Focus descriptor table 5. Register order (both kernels):
  0x680898, 0x68089C, 0x6808C0, 0x6808C4, 0x68084C, 0x680630, 0x680800, 0x680804, 0x680808, 0x68080C, 0x680810,
  0x680814, 0x680818, 0x680820, 0x680824, 0x680828, 0x68082C, 0x680830, 0x680834, 0x680838, 0x680848, 0x680680,
  0x680684, 0x680688, 0x68068C, 0x680690.
- The 0x680880 value of step 19 comes from a separate 19-dword table in the kernel's initialised data, indexed the
  same way (entry 0 holds the register offset 0x680880). It is shared by all encoders and identical in 4627 and 5838.

### 3.2 Register meaning
| Register | Name (nouveau) | Reference values and observed meaning |
|---|---|---|
| 0x680800 / 0x680820 | FP_VDISPLAY_END / FP_HDISPLAY_END | active lines − 1 / active pixels − 1 |
| 0x680804 / 0x680824 | FP_VTOTAL / FP_HTOTAL | total lines − 1 / total pixels − 1 of the raster sent to the encoder |
| 0x680808 / 0x680828 | FP_VCRTC / FP_HCRTC | **not** the active size: SD 640 rows 0x257 (600), SD 720 rows 0x2A7 (680), 480p 0x29F, 720p 0x4FF or 0x4CF, 1080i 0x759–0x7AB, vertical usually = active − 1 (1080i 0x43B / 0x43C) |
| 0x68080C, 0x680810 / 0x68082C, 0x680830 | FP_VSYNC_START, _END / FP_HSYNC_START, _END | sync position; Focus SD rows: HSYNC 64 pixels wide (5838 rows 4 and 6: 84), VSYNC 2 lines; 480p 6 lines |
| 0x680814, 0x680818 / 0x680834, 0x680838 | FP_VVALID_START, _END / FP_HVALID_START, _END | 0 and active − 1, except the scaled HD rows (row 9: 0xA0–0x45F, 960 pixels; row 0x0C: 0xF0–0x68F, 1440 pixels; row 0x11: 0x20–0x2AD) |
| 0x680848 | FP_TG_CONTROL | 0x10100111 = DISPEN_POS \| READ_PROG \| MODE_CENTER \| HSYNC_POS \| VSYNC_POS; 0x10100011 the same with MODE_SCALE; 1080i adds bits 12, 13, 16, 17 (0x10133011 / 0x10133111; meaning not named, interlace inferred) |
| 0x680630 | (NV_PRAMDAC_630) | output colour space: 2 = YCrCb for SD and 480p rows, **3** for the 720p / 1080i rows (rows 9–0x0E); 0 = RGB (step 10) |
| 0x68084C | FP_MARGIN_COLOR | 0x00801080 (YCrCb black: Cr 0x80, Y 0x10, Cb 0x80) |
| 0x6808C4 | — | 0x40801080 (YCrCb black with bit 30) |
| 0x6808C0 | — | 0 in every row |
| 0x680898, 0x68089C | — | 0x10000000 in every unscaled row; scale-like fractions in the scaled HD rows (row 9: 0x0AA94000; rows 0x0C / 0x0D: 0x071AE000 / 0x07183800). Inferred: horizontal / vertical scaling factors, 0x10000000 = 1.0 |
| 0x680680, 0x680684 | — | a numerator / denominator pair whose quotient is the number of NV2A pixel clocks per line of the TV output (inferred): SD rows 2 × HT × raster lines / TV lines (NTSC 640: 2 × 936 × 525 / 525 = 1872; PAL 640×480: 2 × 864 × 525 / 625 = 1451.5); 5838 HD rows the horizontal total over 1; 4627 HD rows 2 × HT × VT over 1 (§3.5) |
| 0x680688, 0x68068C | — | 1/5 and 4/5 of a horizontal total: of the *Conexant* row's total in the SD rows (also in the Focus table: 155 / 620 for the 640 rows), of the standard total for HD |
| 0x680690 | — | 0 for SD rows, 1 for HD rows |
| 0x680880 | FP_DEBUG_0 | during the change 0x21121111; after it 0x21101100 (bilinear X / Y, weight rounding, bit 29 = TMDS power-down per nouveau; scaling off) or with scale-enable bits (rows 7, 9, 0x0A, 0x0C, 0x0D, 0x11) |

### 3.3 Mode words and the rows they select
The title / boot mode words (the mode list of §8.2, identical in the 4627 and 5838 graphics code) select these rows; the
same row means the same raster on every encoder except where the Conexant and Focus NV rows differ (§3.6).

| Mode word(s) | Class | Encoder row | CRTC row | NV row | Picture |
|---|---|---|---|---|---|
| 0x04010101, 0x04010103, 0x0401010B, 0x20010101 / 03 / 0B (SCART), 0x0801010D, 0x08010119 (component) | 525 SD | 0x01, 0x03, 0x0B, 0x0D, 0x19 | 1 | 1 | 640×480 (and 640×240 field mode) |
| 0x04020202, 0x04020204, 0x0402020C, 0x2002020x, 0x0802020E, 0x0802021A | 525 SD | 0x02, 0x04, 0x0C, 0x0E, 0x1A | 2 | 2 | 720×480 |
| 0x44030307, 0x60030307, 0x48030314 | 625 SD | 0x07, 0x14 | 3 | 3 | 640×480 in 625 lines |
| 0x44040408, 0x60040408, 0x48040415 | 625 SD | 0x08, 0x15 | 4 | 4 | 720×480 in 625 lines |
| 0x44050509, 0x60050509, 0x48050516 | 625 SD | 0x09, 0x16 | 5 | 5 | 640×576 |
| 0x4406060A, 0x6006060A, 0x48060617 | 625 SD | 0x0A, 0x17 | 6 | 6 | 720×576 |
| 0x88070701 | HD | 1 | 7 | 7 | 640×480p |
| 0x88080801 | HD | 1 | 8 | 8 | 720×480p |
| 0x880B0A02 | HD | 2 | 0x0A | 0x0B | 1280×720p |
| 0x880E0C03 | HD | 3 | 0x0C | 0x0E | 1920×1080i |
| 0x040F0D0F / 10 / 11, 0x200F0D0F / 10 / 11, 0x080F0D12, 0x080F0D1B | 525 SD | 0x0F–0x12, 0x1B | 0x0D | 0x0F | 640×480 with 10:11 pixels (704-wide encoder rows) |
| 0x44100E13, 0x60100E13, 0x48100E18 | 625 SD | 0x13, 0x18 | 0x0E | 0x10 | the same, 625 lines |
| 0x88110F01 | HD | 1 | 0x0F | 0x11 | 640×480p with 10:11 pixels |
| 0x4412101C, 0x6012101C, 0x4812101D | 625 SD | 0x1C, 0x1D | 0x10 | 0x12 | 640×576 with 10:11 pixels |

- Bit 28 (widescreen) may be added to any of these words; bits 24–27 are 4 (composite / S-video), 8 (component) or 0
  (SCART, with bit 29). NV rows 9, 0x0A, 0x0C and 0x0D are not used by any word in the reference mode list.
- The Conexant and the Xcalibur use the same row numbers for the same pictures, with their own NV2A and CRTC values
  (§3.7–§3.11); the encoder rows each word selects on each encoder: §3.12. No reference word has class 11 (VGA, §13).

### 3.4 Focus NV2A rows, 5838 (AV blob; recommended set for 1.4–1.5, matching the 5838 encoder tables of spec 19 §5.2)
Register map; all values hexadecimal dwords, written to the register in the column heading.

Vertical timing:

| row | 0x680800 | 0x680804 | 0x680808 | 0x68080C | 0x680810 | 0x680814 | 0x680818 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x1DF | 0x20C | 0x1DF | 0x1F2 | 0x1F4 | 0x0 | 0x1DF |
| 0x02 | 0x1DF | 0x20C | 0x1DF | 0x1F2 | 0x1F4 | 0x0 | 0x1DF |
| 0x03 | 0x1DF | 0x20C | 0x1DF | 0x1F4 | 0x1F6 | 0x0 | 0x1DF |
| 0x04 | 0x1DF | 0x20C | 0x1DF | 0x1F4 | 0x1F6 | 0x0 | 0x1DF |
| 0x05 | 0x23F | 0x270 | 0x23F | 0x256 | 0x258 | 0x0 | 0x23F |
| 0x06 | 0x23F | 0x270 | 0x23F | 0x256 | 0x258 | 0x0 | 0x23F |
| 0x07 | 0x1DF | 0x20C | 0x1DF | 0x1E6 | 0x1EC | 0x0 | 0x1DF |
| 0x08 | 0x1DF | 0x20C | 0x1DF | 0x1E6 | 0x1EC | 0x0 | 0x1DF |
| 0x09 | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0A | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0B | 0x2CF | 0x2ED | 0x2CF | 0x2D2 | 0x2D7 | 0x0 | 0x2CF |
| 0x0C | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0D | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0E | 0x437 | 0x464 | 0x43B | 0x437 | 0x441 | 0x0 | 0x437 |
| 0x0F | 0x1DF | 0x20C | 0x1DF | 0x1F2 | 0x1F4 | 0x0 | 0x1DF |
| 0x10 | 0x1DF | 0x20C | 0x1DF | 0x1F4 | 0x1F6 | 0x0 | 0x1DF |
| 0x11 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EE | 0x0 | 0x1DF |
| 0x12 | 0x23F | 0x270 | 0x23F | 0x256 | 0x258 | 0x0 | 0x23F |

Horizontal timing:

| row | 0x680820 | 0x680824 | 0x680828 | 0x68082C | 0x680830 | 0x680834 | 0x680838 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x27F | 0x3A7 | 0x257 | 0x2F3 | 0x333 | 0x0 | 0x27F |
| 0x02 | 0x2CF | 0x3A7 | 0x2A7 | 0x31B | 0x35B | 0x0 | 0x2CF |
| 0x03 | 0x27F | 0x35F | 0x257 | 0x2CF | 0x30F | 0x0 | 0x27F |
| 0x04 | 0x2CF | 0x35F | 0x2A7 | 0x2F7 | 0x34B | 0x0 | 0x2CF |
| 0x05 | 0x27F | 0x35F | 0x257 | 0x2CF | 0x30F | 0x0 | 0x27F |
| 0x06 | 0x2CF | 0x35F | 0x2A7 | 0x2F7 | 0x34B | 0x0 | 0x2CF |
| 0x07 | 0x2CF | 0x359 | 0x29F | 0x2E1 | 0x320 | 0x0 | 0x2CF |
| 0x08 | 0x2CF | 0x359 | 0x29F | 0x2E1 | 0x320 | 0x0 | 0x2CF |
| 0x09 | 0x4FF | 0x671 | 0x4FF | 0x545 | 0x595 | 0xA0 | 0x45F |
| 0x0A | 0x4FF | 0x671 | 0x4CF | 0x545 | 0x595 | 0x0 | 0x4FF |
| 0x0B | 0x4FF | 0x671 | 0x4FF | 0x545 | 0x595 | 0x0 | 0x4FF |
| 0x0C | 0x77F | 0x897 | 0x7AA | 0x7AB | 0x803 | 0xF0 | 0x68F |
| 0x0D | 0x77F | 0x897 | 0x759 | 0x7AB | 0x803 | 0x0 | 0x77F |
| 0x0E | 0x77F | 0x897 | 0x7AB | 0x7AC | 0x804 | 0x0 | 0x77F |
| 0x0F | 0x27F | 0x3A7 | 0x257 | 0x2F3 | 0x333 | 0x0 | 0x27F |
| 0x10 | 0x27F | 0x35F | 0x257 | 0x2CF | 0x30F | 0x0 | 0x27F |
| 0x11 | 0x2CF | 0x359 | 0x2CF | 0x2DF | 0x31E | 0x20 | 0x2AD |
| 0x12 | 0x27F | 0x35F | 0x257 | 0x2CF | 0x30F | 0x0 | 0x27F |

Other registers (0x680880 is written at step 19, from the separate table of §3.1):

| row | 0x680848 | 0x680630 | 0x68084C | 0x6808C4 | 0x6808C0 | 0x680898 | 0x68089C | 0x680680 | 0x680684 | 0x680688 | 0x68068C | 0x680690 | 0x680880 (step 19) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0x01 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000750 | 0x00000001 | 0x0000009B | 0x0000026C | 0x00000000 | 0x21101100 |
| 0x02 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000750 | 0x00000001 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x03 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000DD7C0 | 0x00000271 | 0x000000BE | 0x000002F8 | 0x00000000 | 0x21101100 |
| 0x04 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000DD7C0 | 0x00000271 | 0x000000D2 | 0x00000348 | 0x00000000 | 0x21101100 |
| 0x05 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00107AC0 | 0x00000271 | 0x0000009D | 0x00000276 | 0x00000000 | 0x21101100 |
| 0x06 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00107AC0 | 0x00000271 | 0x000000B1 | 0x000002C4 | 0x00000000 | 0x21101100 |
| 0x07 | 0x10100011 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101101 |
| 0x08 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101100 |
| 0x09 | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x0AA94000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101115 |
| 0x0A | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101111 |
| 0x0B | 0x10100111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000672 | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101100 |
| 0x0C | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x071AE000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101155 |
| 0x0D | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101151 |
| 0x0E | 0x10133111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000898 | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101100 |
| 0x0F | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000750 | 0x00000001 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x10 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000DD7C0 | 0x00000271 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x11 | 0x10100011 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101101 |
| 0x12 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00107AC0 | 0x00000271 | 0x0000009D | 0x00000276 | 0x00000000 | 0x21101100 |

### 3.5 4627 Focus rows against 5838
4627 values (5838 values in parentheses); every register not listed is equal (4627 kernel) (5838 kernel):
- rows 1, 2, 0x0F: 0x680680 = 0x000EFF10 (0x750), 0x680684 = 0x20D (0x1) — the same quotient, 1872; row 0x0F also
  0x680828 = 0x2A7 (0x257).
- rows 4, 6: 0x680828 = 0x2AF (0x2A7), 0x68082C = 0x30B (0x2F7).
- rows 7, 8: 0x68080C = 0x1E8 (0x1E6), 0x680810 = 0x1EE (0x1EC), 0x680680 = 0x000DBF24 (0x35A).
- row 0x0B (720p): 0x68080C = 0x2D4 (0x2D2), 0x680810 = 0x2D9 (0x2D7), 0x680680 = 0x0025C3F8 (0x672).
- row 0x0E (1080i): 0x68080C = 0x43B (0x437), 0x680810 = 0x445 (0x441), 0x680680 = 0x004B87F0 (0x898).
- rows 9, 0x0A, 0x0C, 0x0D, 0x11: 0x680680 = 0x0025C3F8 / 0x004B87F0 / 0x000DBF24 (all 0x35A).
- row 0x10: 0x680684 = 0x20D (0x271).

### 3.6 Conexant rows against Focus rows (4627)
- HD rows 7–0x0E and row 0x11 are identical for both encoders.
- SD rows 1–6, 0x0F, 0x10, 0x12 differ in the horizontal total (0x680824), the sync positions (0x68080C, 0x680810,
  0x68082C, 0x680830; row 0x10 also 0x680828) and 0x680680: the Conexant raster follows its own input timing
  (H_CLKI; e.g. row 1: 776 × 525, HSYNC 0x28D–0x2AD, VSYNC 0x1EA–0x1ED). The Conexant CRTC rows differ from the Focus ones in
  the SD CRTC rows (§4).

### 3.7 Conexant NV2A rows, 5838 (AV blob; recommended set for 1.0–1.3, identical in Cerbios)
Register map; all values hexadecimal dwords, written to the register in the column heading. Conexant descriptor of
the AV blob (spec 17 §1a.2), table 5; the 4627 table differs only in 0x680680 / 0x680684 (§3.8). These rows are
written on every console whose encoder is the Conexant (4627: `XboxHardwareInfo` flag 0x20 clear; 5838: flags 0x20
and 0x400 clear) (4627 kernel) (5838 kernel) (Cerbios).

Vertical timing:

| row | 0x680800 | 0x680804 | 0x680808 | 0x68080C | 0x680810 | 0x680814 | 0x680818 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x1DF | 0x20C | 0x1DF | 0x1EA | 0x1ED | 0x0 | 0x1DF |
| 0x02 | 0x1DF | 0x20C | 0x1DF | 0x1EA | 0x1ED | 0x0 | 0x1DF |
| 0x03 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EB | 0x0 | 0x1DF |
| 0x04 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EB | 0x0 | 0x1DF |
| 0x05 | 0x23F | 0x270 | 0x23F | 0x249 | 0x24C | 0x0 | 0x23F |
| 0x06 | 0x23F | 0x270 | 0x23F | 0x249 | 0x24C | 0x0 | 0x23F |
| 0x07 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EE | 0x0 | 0x1DF |
| 0x08 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EE | 0x0 | 0x1DF |
| 0x09 | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0A | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0B | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0C | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0D | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0E | 0x437 | 0x464 | 0x43B | 0x43B | 0x445 | 0x0 | 0x437 |
| 0x0F | 0x1DF | 0x20C | 0x1DF | 0x1EA | 0x1ED | 0x0 | 0x1DF |
| 0x10 | 0x1DF | 0x20C | 0x1DF | 0x1EA | 0x1ED | 0x0 | 0x1DF |
| 0x11 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EE | 0x0 | 0x1DF |
| 0x12 | 0x23F | 0x270 | 0x23F | 0x249 | 0x24C | 0x0 | 0x23F |

Horizontal timing:

| row | 0x680820 | 0x680824 | 0x680828 | 0x68082C | 0x680830 | 0x680834 | 0x680838 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x27F | 0x307 | 0x257 | 0x28D | 0x2AD | 0x0 | 0x27F |
| 0x02 | 0x2CF | 0x365 | 0x2A7 | 0x2DF | 0x2FF | 0x0 | 0x2CF |
| 0x03 | 0x27F | 0x3B5 | 0x257 | 0x2B7 | 0x2D7 | 0x0 | 0x27F |
| 0x04 | 0x2CF | 0x419 | 0x2AF | 0x30B | 0x32B | 0x0 | 0x2CF |
| 0x05 | 0x27F | 0x313 | 0x257 | 0x28F | 0x2AF | 0x0 | 0x27F |
| 0x06 | 0x2CF | 0x375 | 0x2AF | 0x2E1 | 0x302 | 0x0 | 0x2CF |
| 0x07 | 0x2CF | 0x359 | 0x29F | 0x2E1 | 0x320 | 0x0 | 0x2CF |
| 0x08 | 0x2CF | 0x359 | 0x29F | 0x2E1 | 0x320 | 0x0 | 0x2CF |
| 0x09 | 0x4FF | 0x671 | 0x4FF | 0x545 | 0x595 | 0xA0 | 0x45F |
| 0x0A | 0x4FF | 0x671 | 0x4CF | 0x545 | 0x595 | 0x0 | 0x4FF |
| 0x0B | 0x4FF | 0x671 | 0x4FF | 0x545 | 0x595 | 0x0 | 0x4FF |
| 0x0C | 0x77F | 0x897 | 0x7AA | 0x7AB | 0x803 | 0xF0 | 0x68F |
| 0x0D | 0x77F | 0x897 | 0x759 | 0x7AB | 0x803 | 0x0 | 0x77F |
| 0x0E | 0x77F | 0x897 | 0x7AB | 0x7AC | 0x804 | 0x0 | 0x77F |
| 0x0F | 0x27F | 0x355 | 0x2A7 | 0x2B0 | 0x2D0 | 0x0 | 0x27F |
| 0x10 | 0x27F | 0x419 | 0x2A7 | 0x2DB | 0x2FB | 0x0 | 0x27F |
| 0x11 | 0x2CF | 0x359 | 0x2CF | 0x2DF | 0x31E | 0x20 | 0x2AD |
| 0x12 | 0x27F | 0x363 | 0x257 | 0x2B0 | 0x2D0 | 0x0 | 0x27F |

Other registers (0x680880 is written at step 19, from the separate table of §3.1):

| row | 0x680848 | 0x680630 | 0x68084C | 0x6808C4 | 0x6808C0 | 0x680898 | 0x68089C | 0x680680 | 0x680684 | 0x680688 | 0x68068C | 0x680690 | 0x680880 (step 19) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0x01 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000610 | 0x00000001 | 0x0000009B | 0x0000026C | 0x00000000 | 0x21101100 |
| 0x02 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006CC | 0x00000001 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x03 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000063C | 0x00000001 | 0x000000BE | 0x000002F8 | 0x00000000 | 0x21101100 |
| 0x04 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006E4 | 0x00000001 | 0x000000D2 | 0x00000348 | 0x00000000 | 0x21101100 |
| 0x05 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000628 | 0x00000001 | 0x0000009D | 0x00000276 | 0x00000000 | 0x21101100 |
| 0x06 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006EC | 0x00000001 | 0x000000B1 | 0x000002C4 | 0x00000000 | 0x21101100 |
| 0x07 | 0x10100011 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101101 |
| 0x08 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101100 |
| 0x09 | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x0AA94000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101115 |
| 0x0A | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101111 |
| 0x0B | 0x10100111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000672 | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101100 |
| 0x0C | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x071AE000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101155 |
| 0x0D | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101151 |
| 0x0E | 0x10133111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000898 | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101100 |
| 0x0F | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006AC | 0x00000001 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x10 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006E4 | 0x00000001 | 0x000000AE | 0x000002B8 | 0x00000000 | 0x21101100 |
| 0x11 | 0x10100011 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AB | 0x000002AE | 0x00000001 | 0x21101101 |
| 0x12 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006C8 | 0x00000001 | 0x0000009D | 0x00000276 | 0x00000000 | 0x21101100 |

- SD rows 1–6, 0x0F, 0x10, 0x12 use the Conexant's own raster (§5.1). The HD rows 7–0x0E and row 0x11 are the
  5838 Focus rows of §3.4 except the VSYNC positions (0x68080C / 0x680810) of rows 7, 8, 0x0B and 0x0E, which keep
  the 4627 values (§3.5).
- HSYNC is 32 pixels wide in every SD row (row 6: 33), VSYNC 3 lines (0x68080C → 0x680810); FP_HCRTC (0x680828) is
  0x257 in the 640-pixel 525-line rows and 0x2A7 / 0x2AF in the 720-pixel and 704-input rows.
- 0x680680 / 0x680684 (5838): the Conexant's H_CLKO for the encoder rows that use the row (row 1: 0x610 = 1552,
  row 3: 0x63C = 1596): NV2A pixel clocks per TV output line, as for the Focus (§3.2).
- 0x680688 / 0x68068C = 1/5 and 4/5 of the row's own horizontal total (row 1: 155 / 620 of 775), except rows
  0x0F, 0x10, 0x12, which carry the values of rows 2, 2 and 5.

### 3.8 4627 Conexant rows against 5838
4627 values (5838 values in parentheses); every register not listed is equal (4627 kernel) (5838 kernel):
- row 0x01: 0x680680 = 0xC6ED0 (0x610), 0x680684 = 0x20D (0x1)
- row 0x02: 0x680680 = 0xDF05C (0x6CC), 0x680684 = 0x20D (0x1)
- row 0x03: 0x680680 = 0xF387C (0x63C), 0x680684 = 0x271 (0x1)
- row 0x04: 0x680680 = 0x10D2A4 (0x6E4), 0x680684 = 0x271 (0x1)
- row 0x05: 0x680680 = 0xF07A8 (0x628), 0x680684 = 0x271 (0x1)
- row 0x06: 0x680680 = 0x10E62C (0x6EC), 0x680684 = 0x271 (0x1)
- row 0x07: 0x680680 = 0xDBF24 (0x35A)
- row 0x08: 0x680680 = 0xDBF24 (0x35A)
- row 0x09: 0x680680 = 0x25C3F8 (0x35A)
- row 0x0A: 0x680680 = 0x25C3F8 (0x35A)
- row 0x0B: 0x680680 = 0x25C3F8 (0x672)
- row 0x0C: 0x680680 = 0x4B87F0 (0x35A)
- row 0x0D: 0x680680 = 0x4B87F0 (0x35A)
- row 0x0E: 0x680680 = 0x4B87F0 (0x898)
- row 0x0F: 0x680680 = 0xDAEBC (0x6AC), 0x680684 = 0x20D (0x1)
- row 0x10: 0x680680 = 0x10D2A4 (0x6E4), 0x680684 = 0x20D (0x1)
- row 0x11: 0x680680 = 0xDBF24 (0x35A)
- row 0x12: 0x680680 = 0x108E48 (0x6C8), 0x680684 = 0x271 (0x1)
- The quotient 0x680680 / 0x680684 is the same in every SD row except row **0x10**: 4627 divides by 0x20D (525),
  giving 2100, 5838 by 0x271 (625), giving 1764 = the H_CLKO of encoder rows 0x13 / 0x18 (the 625-line rows that use
  NV row 0x10). The 4627 HD rows hold 2 × HT × VT over 1 (as the Focus rows, §3.5).

### 3.9 Conexant CRTC rows (4627 = 5838 = Cerbios)
Same 34 indexes and merging as §4 (CR13, CR19, CR25 get the pitch bits). Rows 7–0x0C and 0x0F are the Focus rows of
§4; rows 1–6, 0x0D, 0x0E, 0x10 are the Conexant's own (4627 kernel) (5838 kernel) (Cerbios).

| row | values in register order |
|---|---|
| 0x01 | 56 4F 4F 9C 51 35 0B 3E 00 40 00 00 00 00 00 00 EB 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x02 | 65 59 59 89 5B BF 0B 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x03 | 6F 4F 4F 93 55 B9 0B 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x04 | 6F 59 59 93 5B BF 0B 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x05 | 56 4F 4F 9C 51 35 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x06 | 65 59 59 89 5B BF 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x07 | 59 4F 4F 9D 51 39 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x08 | 63 59 59 87 5B A3 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x09 | 78 4F 4F 9C 57 A1 FC 1F 00 40 00 00 00 00 00 00 E4 09 DF 00 00 DF FD E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0A | C8 9F 9F 8C A7 31 EC F0 00 60 00 00 00 00 00 00 D4 09 CF 00 00 CF ED E3 FF 00 38 05 80 10 00 11 FF 00 |
| 0x0B | 67 4F 4F 8B 54 BF 03 11 00 40 00 00 00 00 00 00 F1 06 EF 00 00 EF 04 E3 FF 00 3A 05 80 10 00 11 36 00 |
| 0x0C | 04 EF EF 88 F4 3F 2F F0 00 60 00 00 00 00 00 00 1D 02 1B 00 00 1B 30 E3 FF 00 38 05 80 00 01 11 10 00 |
| 0x0D | 65 4F 4F 89 5B BF 0B 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0E | 7E 4F 4F 82 5B 3F 0B 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 00 00 11 FF 00 |
| 0x0F | 61 57 57 85 59 BF 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x10 | 65 4F 4F 9C 51 35 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |

- The vertical CRTC timing equals the row's NV2A raster in the SD rows (CR06 / CR07 → 525 or 625 lines; CR12 → 480
  or 576 active lines); the horizontal CRTC total does not (row 1: CR00 0x56 → (0x56 + 5) × 8 = 728 pixels against
  the NV2A's 776). The same holds for the HD rows of every encoder (row 7: 752 against 858). Inferred: with CR28 bit 7
  set the flat-panel timing generator of the NV2A row drives the output; the CRTC row's horizontal values only have to
  fit within it.
- Fixed bytes as in §4 (CR1A 0x3A / 0x38, CR1B 0x05, CR20 0x80, CR33 0x11, CR41 0); CR10 (vertical retrace start)
  is not always equal to the NV2A VSYNC start (row 1: 0x1EB against 0x1EA; row 2: 0x1E8 against 0x1EA).

### 3.10 Xcalibur NV2A rows, 5838 (AV blob; the only set for 1.6, identical in Cerbios)
Register map; all values hexadecimal dwords, written to the register in the column heading. Xcalibur descriptor of
the AV blob, table 5: the Xcalibur has its **own** NV2A rows, neither the Focus nor the Conexant ones (the table
loader stores the Xcalibur descriptor's tables 5 and 6 for the mode change when flag 0x400 is set) (5838 kernel).
Cerbios carries the same values (Cerbios).

Vertical timing:

| row | 0x680800 | 0x680804 | 0x680808 | 0x68080C | 0x680810 | 0x680814 | 0x680818 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x1DF | 0x20C | 0x1DF | 0x1E7 | 0x1E9 | 0x0 | 0x1DF |
| 0x02 | 0x1DF | 0x20C | 0x1DF | 0x1E7 | 0x1ED | 0x0 | 0x1DF |
| 0x03 | 0x1DF | 0x207 | 0x1DF | 0x1E1 | 0x1EB | 0x0 | 0x1DF |
| 0x04 | 0x1DF | 0x207 | 0x1DF | 0x1E1 | 0x1EB | 0x0 | 0x1DF |
| 0x05 | 0x23F | 0x270 | 0x23F | 0x246 | 0x24C | 0x0 | 0x23F |
| 0x06 | 0x23F | 0x270 | 0x23F | 0x246 | 0x24C | 0x0 | 0x23F |
| 0x07 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1ED | 0x0 | 0x1DF |
| 0x08 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1ED | 0x0 | 0x1DF |
| 0x09 | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0A | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0B | 0x2CF | 0x2ED | 0x2CF | 0x2D4 | 0x2D9 | 0x0 | 0x2CF |
| 0x0C | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0D | 0x437 | 0x464 | 0x43C | 0x43C | 0x446 | 0x0 | 0x437 |
| 0x0E | 0x437 | 0x464 | 0x43B | 0x43B | 0x445 | 0x0 | 0x437 |
| 0x0F | 0x1DF | 0x20C | 0x1DF | 0x1E7 | 0x1ED | 0x0 | 0x1DF |
| 0x10 | 0x1DF | 0x207 | 0x1DF | 0x1E1 | 0x1EB | 0x0 | 0x1DF |
| 0x11 | 0x1DF | 0x20C | 0x1DF | 0x1E8 | 0x1EE | 0x0 | 0x1DF |
| 0x12 | 0x23F | 0x270 | 0x23F | 0x246 | 0x24C | 0x0 | 0x23F |

Horizontal timing:

| row | 0x680820 | 0x680824 | 0x680828 | 0x68082C | 0x680830 | 0x680834 | 0x680838 |
|---|---|---|---|---|---|---|---|
| 0x01 | 0x27F | 0x30B | 0x257 | 0x2AB | 0x2AD | 0x0 | 0x27F |
| 0x02 | 0x2CF | 0x359 | 0x2A7 | 0x2F9 | 0x2FF | 0x0 | 0x2CF |
| 0x03 | 0x27F | 0x31F | 0x257 | 0x2BF | 0x2D7 | 0x0 | 0x27F |
| 0x04 | 0x2CF | 0x383 | 0x2AF | 0x2F9 | 0x32B | 0x0 | 0x2CF |
| 0x05 | 0x27F | 0x35F | 0x257 | 0x2FF | 0x2D0 | 0x0 | 0x27F |
| 0x06 | 0x2CF | 0x35F | 0x2CC | 0x2D5 | 0x302 | 0x0 | 0x2CF |
| 0x07 | 0x27F | 0x30B | 0x257 | 0x2AB | 0x2AD | 0x0 | 0x27F |
| 0x08 | 0x2CF | 0x359 | 0x2A7 | 0x2F9 | 0x2FF | 0x0 | 0x2CF |
| 0x09 | 0x4FF | 0x671 | 0x4FF | 0x545 | 0x595 | 0xA0 | 0x45F |
| 0x0A | 0x4FF | 0x671 | 0x4CF | 0x545 | 0x595 | 0x0 | 0x4FF |
| 0x0B | 0x4FF | 0x671 | 0x4FF | 0x56D | 0x595 | 0x0 | 0x4FF |
| 0x0C | 0x77F | 0x897 | 0x7AA | 0x7AB | 0x803 | 0xF0 | 0x68F |
| 0x0D | 0x77F | 0x897 | 0x759 | 0x7AB | 0x803 | 0x0 | 0x77F |
| 0x0E | 0x77F | 0x897 | 0x7AB | 0x7DC | 0x804 | 0x0 | 0x77F |
| 0x0F | 0x27F | 0x359 | 0x2A7 | 0x2D1 | 0x2D0 | 0x0 | 0x27F |
| 0x10 | 0x27F | 0x383 | 0x2A7 | 0x2D1 | 0x2FB | 0x0 | 0x27F |
| 0x11 | 0x2CF | 0x359 | 0x2CF | 0x2F9 | 0x31E | 0x20 | 0x2AD |
| 0x12 | 0x27F | 0x35F | 0x257 | 0x2D7 | 0x2D0 | 0x0 | 0x27F |

Other registers (0x680880 is written at step 19, from the separate table of §3.1):

| row | 0x680848 | 0x680630 | 0x68084C | 0x6808C4 | 0x6808C0 | 0x680898 | 0x68089C | 0x680680 | 0x680684 | 0x680688 | 0x68068C | 0x680690 | 0x680880 (step 19) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0x01 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000618 | 0x00000001 | 0x0000009E | 0x00000273 | 0x00000000 | 0x21101100 |
| 0x02 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006B4 | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000000 | 0x21101100 |
| 0x03 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000CB200 | 0x00000271 | 0x0000009F | 0x00000279 | 0x00000000 | 0x21101100 |
| 0x04 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000E4840 | 0x00000271 | 0x000000B3 | 0x000002C8 | 0x00000000 | 0x21101100 |
| 0x05 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006C0 | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000000 | 0x21101100 |
| 0x06 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006C0 | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000000 | 0x21101100 |
| 0x07 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000030C | 0x00000001 | 0x0000009E | 0x00000273 | 0x00000001 | 0x21101101 |
| 0x08 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000001 | 0x21101100 |
| 0x09 | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x0AA94000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101115 |
| 0x0A | 0x10100011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x0000014A | 0x00000528 | 0x00000001 | 0x21101111 |
| 0x0B | 0x10100111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000672 | 0x00000001 | 0x00000157 | 0x00000529 | 0x00000001 | 0x21101100 |
| 0x0C | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x071AE000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101155 |
| 0x0D | 0x10133011 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x07183800 | 0x0000035A | 0x00000001 | 0x000001B8 | 0x000006E0 | 0x00000001 | 0x21101151 |
| 0x0E | 0x10133111 | 0x00000003 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x00000898 | 0x00000001 | 0x000001BF | 0x000006E2 | 0x00000001 | 0x21101100 |
| 0x0F | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006B4 | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000000 | 0x21101100 |
| 0x10 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000E4840 | 0x00000271 | 0x000000B3 | 0x000002C8 | 0x00000000 | 0x21101100 |
| 0x11 | 0x10100011 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x0000035A | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000001 | 0x21101101 |
| 0x12 | 0x10100111 | 0x00000002 | 0x00801080 | 0x40801080 | 0x00000000 | 0x10000000 | 0x10000000 | 0x000006C0 | 0x00000001 | 0x000000AE | 0x000002B2 | 0x00000000 | 0x21101100 |

- Rows 9, 0x0A, 0x0C, 0x0D (not used by any reference mode word) are the Focus / Conexant rows. Every other row
  differs from both other encoders.
- The SD rasters are the Xcalibur's input raster of its register 0x54 (§5.2): 780 × 525 (rows 1, 7), 858 × 525
  (rows 2, 8, 0x0F, 0x11), 800 × 520 (row 3), 900 × 520 (rows 4, 0x10), 864 × 625 (rows 5, 6, 0x12); the 625-line
  modes with 480-line pictures use a **520-line** raster (0x680804 = 0x207), not 525.
- **640 × 480p (row 7)** is a plain 640-pixel raster of 780 × 525 (24.5454 MHz), FP_TG_CONTROL 0x10100111 (centre,
  no scaling), where the Focus and Conexant row 7 scales 640 pixels onto 720 of an 858 × 525 raster (0x10100011).
  The 0x680880 value of step 19 still comes from the shared table (row 7: 0x21101101) (5838 kernel). 720 × 480p
  (row 8) and row 0x11 keep 858 × 525.
- Sync widths are small and irregular: HSYNC 2 pixels in rows 1 and 7 (0x2AB → 0x2AD), 6 in rows 2 and 8, and
  **HSYNC end below HSYNC start** in rows 5, 0x0F and 0x12 (e.g. row 5: 0x2FF → 0x2D0); VSYNC 2–10 lines. Inferred:
  the Xcalibur uses only the leading edges (§5.2).
- 720p (row 0x0B) and 1080i (row 0x0E) differ from the 5838 Focus rows only in the HSYNC start (0x56D, 0x7DC), the
  0x680688 / 0x68068C pair and the VSYNC positions (the 4627 Focus values, as the Conexant rows); row 0x11 only in
  the HSYNC start (0x2F9) and 0x680688 / 0x68068C.
- 0x680680 / 0x680684: SD rows 2 × HT over 1 (row 1: 0x618 = 1560) or 2 × HT × VT / 625 for the 520-line rows
  (row 3: 0xCB200 / 0x271 = 1331.2); HD rows HT over 1 (row 7: 0x30C = 780).
- 0x680630, 0x68084C, 0x6808C4: YCrCb as for the other encoders (2 in SD / 480p, 3 in 720p / 1080i), cleared for
  mode bit 29 by the shared step 10 (§2.3).

### 3.11 Xcalibur CRTC rows (5838)
Same 34 indexes and merging as §4. Rows 9 and 0x0B are the Focus rows; all others are the Xcalibur's own
(5838 kernel).

| row | values in register order |
|---|---|
| 0x01 | 5D 4F 4F 9C 54 35 0B 3E 00 40 00 00 00 00 00 00 E9 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x02 | 66 59 59 89 5D BF 0B 3E 00 40 00 00 00 00 00 00 E9 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x03 | 5F 4F 4F 80 55 B9 06 3E 00 40 00 00 00 00 00 00 EB 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x04 | 6C 59 59 86 5F BF 0B 3E 00 40 00 00 00 00 00 00 EB 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x05 | 65 4F 4F 9C 52 35 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x06 | 67 59 59 69 62 DC 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x07 | 59 4F 4F 9D 51 39 0B 3E 00 40 00 00 C0 00 00 00 E8 0E DF 40 00 DF 0C E3 FF 38 3A 85 80 10 E0 11 FF 00 |
| 0x08 | 63 59 59 87 5B A3 0B 3E 00 40 00 00 D0 00 00 00 E8 0E DF 80 00 DF 0C E3 FF 37 3A 85 80 10 E0 11 FF 00 |
| 0x09 | 78 4F 4F 9C 57 A1 FC 1F 00 40 00 00 00 00 00 00 E4 09 DF 00 00 DF FD E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0A | C8 9F 9F 8C A7 31 EC F0 00 60 00 00 20 00 00 00 D4 09 CF 80 00 CF ED E3 FF 5C 38 85 80 10 C0 11 FF 00 |
| 0x0B | 67 4F 4F 8B 54 BF 03 11 00 40 00 00 00 00 00 00 F1 06 EF 00 00 EF 04 E3 FF 00 3A 05 80 10 00 11 36 00 |
| 0x0C | 04 EF EF 88 F4 3F 2F F0 00 60 00 00 B0 00 00 00 1D 02 1B 00 00 1B 30 E3 FF 00 38 85 80 00 A1 11 10 00 |
| 0x0D | 65 4F 4F 89 5B BF 0B 3E 00 40 00 00 00 00 00 00 E9 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0E | 5F 4F 4F 86 5B 3F 06 3E 00 40 00 00 00 00 00 00 E8 0B DF 00 00 DF 0C E3 FF 00 3A 05 80 00 00 11 FF 00 |
| 0x0F | 61 57 57 85 59 BF 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 40 00 DF 0C E3 FF 38 3A 85 80 10 00 11 FF 00 |
| 0x10 | 65 4F 4F 9C 52 35 6F F0 00 60 00 00 00 00 00 00 45 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |

- CR06 / CR07 give 525 lines in rows 1, 2, 4, 7, 8, 0x0D, 0x0F (row 4: 525 against the NV row's 520), 520 in rows 3
  and 0x0E, 625 in rows 5, 6, 0x10.
- Rows 7, 8, 0x0A, 0x0C and 0x0F carry bytes no other encoder's rows have: CR0C (VGA start address high) 0xC0, 0xD0,
  0x20, 0xB0, 0x00; CR13 0x40, 0x80, 0x80, 0x00, 0x40 (replaced by the pitch at #15); CR19 0x38, 0x37, 0x5C, 0x00,
  0x38 (ORed with the pitch bits at #15, so the row's bits 0–4, nouveau: start address bits 16–20, survive);
  CR1B **0x85**; CR2D 0xE0, 0xE0, 0xC0, 0xA1, 0x00. Their effect on the scanout is not determined (Open); 1.6
  consoles show 480p and 1080i with them (inferred from the retail kernel shipping them).
- **Cerbios** uses the same Xcalibur CRTC rows except **CR19 = 0x00 in rows 7, 8, 0x0A and 0x0F** (Cerbios).

### 3.12 Rows each reference mode word selects, per encoder
The same mode word selects the same row numbers on every encoder (§3.3); the encoder rows that carry the timing are:

| Mode word | NV / CRTC row | Conexant encoder row (spec 18) | Xcalibur row (spec 20 §6.1; Japan in parentheses) |
|---|---|---|---|
| 0x04010101 / 0x20010101 / 0x0801010D | 1 / 1 | SD 0x01 / 0x01 + bit-29 row / 0x0D | 0x01 / 0x01 / 0x0D (0x0E) |
| 0x04010103, 0x0401010B, 0x08010119 | 1 / 1 | SD 0x03, 0x0B, 0x19 | 0x03, 0x0B, 0x1C |
| 0x04020202, 0x04020204, 0x0402020C, 0x0802020E, 0x0802021A | 2 / 2 | SD 0x02, 0x04, 0x0C, 0x0E, 0x1A | 0x02, 0x04, 0x0C, 0x0F (0x10), 0x1D |
| 0x44030307, 0x48030314 | 3 / 3 | SD 0x07, 0x14 | 0x07, 0x17 |
| 0x44040408, 0x48040415 | 4 / 4 | SD 0x08, 0x15 | 0x08, 0x18 |
| 0x44050509, 0x48050516 | 5 / 5 | SD 0x09, 0x16 | 0x09, 0x19 |
| 0x4406060A, 0x48060617 | 6 / 6 | SD 0x0A, 0x17 | 0x0A, 0x1A |
| 0x040F0D0F / 10 / 11, 0x080F0D12, 0x080F0D1B | 0x0F / 0x0D | SD 0x0F–0x12, 0x1B | 0x11–0x13, 0x14 (0x15), 0x1E |
| 0x44100E13, 0x48100E18 | 0x10 / 0x0E | SD 0x13, 0x18 | 0x16, 0x1B |
| 0x4412101C, 0x4812101D | 0x12 / 0x10 | SD 0x1C, 0x1D | 0x1F, 0x20 |
| 0x88070701 (640 × 480p) | 7 / 7 | HD 0x01 | 0x25 (0x26) (encoder row 1 with CRTC row 7) |
| 0x88080801 (720 × 480p), 0x88110F01 | 8 / 8, 0x11 / 0x0F | HD 0x01 | 0x21 (0x22) |
| 0x880B0A02 (720p) | 0x0B / 0x0A | HD 0x02 | 0x23 |
| 0x880E0C03 (1080i) | 0x0E / 0x0C | HD 0x03 | 0x24 |

## 4. CRTC rows (mode bits 8–15)
- 34 indexes per row, in this order: CR00–CR1B, CR20, CR25, CR2D, CR33, CR39, CR41. 16 value rows (row 0 is the index
  row). The Focus set is identical in 4627 and 5838; the Conexant set differs in rows 1–6, 0x0D, 0x0E and 0x10
  (4627 kernel) (5838 kernel).
- Focus CRTC rows (bytes in the index order above; CR13, CR19, CR25 get the pitch bits merged in, §2.3 #15):

| row | values in register order |
|---|---|
| 0x01 | 70 4F 4F 94 5D BF 0B 3E 00 40 00 00 00 00 00 00 F2 04 DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x02 | 70 59 59 94 62 A4 0B 3E 00 40 00 00 00 00 00 00 F2 04 DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x03 | 67 4F 4F 8B 59 BB 0B 3E 00 40 00 00 00 00 00 00 F4 06 DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x04 | 67 59 59 8B 5E BF 0B 3E 00 40 00 00 00 00 00 00 F4 06 DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x05 | 67 4F 4F 8B 59 BB 6F F0 00 60 00 00 00 00 00 00 56 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x06 | 67 59 59 8B 5E BF 6F F0 00 60 00 00 00 00 00 00 56 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x07 | 59 4F 4F 9D 51 39 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x08 | 63 59 59 87 5B A3 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x09 | 78 4F 4F 9C 57 A1 FC 1F 00 40 00 00 00 00 00 00 E4 09 DF 00 00 DF FD E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0A | C8 9F 9F 8C A7 31 EC F0 00 60 00 00 00 00 00 00 D4 09 CF 00 00 CF ED E3 FF 00 38 05 80 10 00 11 FF 00 |
| 0x0B | 67 4F 4F 8B 54 BF 03 11 00 40 00 00 00 00 00 00 F1 06 EF 00 00 EF 04 E3 FF 00 3A 05 80 10 00 11 36 00 |
| 0x0C | 04 EF EF 88 F4 3F 2F F0 00 60 00 00 00 00 00 00 1D 02 1B 00 00 1B 30 E3 FF 00 38 05 80 00 01 11 10 00 |
| 0x0D | 70 4F 4F 94 5D BF 0B 3E 00 40 00 00 00 00 00 00 F2 04 DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x0E | 67 4F 4F 8B 59 BF 0B 3E 00 40 00 00 00 00 00 00 F4 06 DF 00 00 DF 0C E3 FF 00 3A 05 80 00 00 11 FF 00 |
| 0x0F | 61 57 57 85 59 BF 0B 3E 00 40 00 00 00 00 00 00 E8 0E DF 00 00 DF 0C E3 FF 00 3A 05 80 10 00 11 FF 00 |
| 0x10 | 67 4F 4F 94 59 BF 6F F0 00 60 00 00 00 00 00 00 56 08 3F 00 00 3F 70 E3 FF 00 3A 05 80 10 00 11 FF 00 |

- Fixed in every row: CR08 = 0, CR0A–CR0F = 0 (so the VGA start address CR0C / CR0D is 0), CR09 = 0x40 or 0x60, CR14 = 0, CR17 = 0xE3, CR18 = 0xFF
  (no line compare), CR1B = 0x05 (nouveau: FIFO burst), CR20 = 0x80 (nouveau: FIFO low-water mark), CR33 = 0x11,
  CR41 = 0. CR1A = 0x3A (0x38 in rows 0x0A and 0x0C). CR25 = 0x10 except rows 0x0C and 0x0E (0x00). CR2D = 0 except row 0x0C
  (0x01).
- CR39 (interlace half-line position) = 0xFF in every progressive row; **0x10 in the 1080i row 0x0C**; 0x36 in row 0x0B
  (not used by any mode word).
- The CRTC raster agrees with the NV2A row it is used with (Focus row 1: CR00 0x70 → (0x70 + 5) × 8 = 936 pixels;
  CR06 / CR07 → 525 lines; CR10 / CR07 → VSYNC at line 498 = 0x68080C).
- The device set-up of §7 writes CR1B ← 0x05 and CR20 ← 0x29 before the first mode set; the CRTC row then sets CR20
  to 0x80 on every full change (4627 kernel) (5838 kernel).
- Conexant CRTC rows: §3.9; Xcalibur CRTC rows: §3.11.

## 5. Clocking with a Focus encoder (question 2)
- **Pixel clock source:** the NV2A takes its pixel clock from the encoder. The X-codes set 0x68050C (nouveau:
  PLL_COEFF_SELECT) to 0x000A0400 (bit 10 = NVPLL programmed; bits 17 and 19 = TV_PCLK1 / TV_PCLK2: the pixel clocks
  come from the TV encoder); every full mode change ORs in 0x10020000 (bit 28 = VCLK_RATIO_DB2 in nouveau's naming,
  and bit 17 again), giving **0x100A0400** (flash header) (4627 kernel) (5838 kernel).
- The VPLL (0x680508) is never programmed; its contents do not matter for TV output (inferred from the above).
- **Sync:** the NV2A's flat-panel timing generator produces HSYNC and VSYNC (FP_TG_CONTROL in every row: both
  enabled, positive), together with the pixel data, at the encoder's clock. The Focus rows leave CR (0x0C/0x0D) bit 7
  (SYNC_MS in `fs453.h`) clear in every SD and HD row. This is the FS453 **pseudo-master mode** of the FS453/4
  hardware reference (the encoder generates CLKOUT from its 27 MHz crystal through the NCO and PLL; the graphics
  controller returns the clock with data and syncs).
- **Consequence:** the NV2A raster must exactly match the Focus clock: Focus clock = HT × VT × raster rate of the NV
  row. The reference rows satisfy this (clock: 27 MHz × NCON / NCOD / N × M / P with the PLL fields of spec 19,
  M = field + 17, N = field + 1, P = field + 1; NCO not used when NCON = 0):

| NV row | Focus encoder rows | Focus clock | NV raster (HT × VT) | Raster rate |
|---|---|---|---|---|
| 1, 2, 0x0F | 0x01–0x06, 0x0B–0x12, 0x19–0x1B (525-line and 480-line SD) | 29.4545 MHz | 936 × 525 | 59.94 Hz (progressive, the encoder interlaces) |
| 3, 4, 0x10 | 0x07, 0x08, 0x13–0x15, 0x18 (625-line, 480-line input) | 22.68 MHz | 864 × 525 | 50 Hz |
| 5, 6, 0x12 | 0x09, 0x0A, 0x16, 0x17, 0x1C, 0x1D (576-line input) | 27.0 MHz | 864 × 625 | 50 Hz |
| 7, 8, 0x11 | HD 1 (480p) | 27.0 MHz (PLL only) | 858 × 525 | 59.94 Hz |
| 0x0B | HD 2 (720p) | 74.25 MHz (PLL only) | 1650 × 750 | 60 Hz |
| 0x0E | HD 3 (1080i) | 74.25 MHz (PLL only) | 2200 × 1125 (one full frame) | 30 Hz |

- If the raster does not match, the encoder's input FIFO between the NV2A raster and its own TV timing over- or
  underflows while its output timing (from the crystal) stays locked: a stable signal with a corrupted picture
  (inferred from the FS453/4 hardware reference's description of the modes).
- **Conexant:** the same NV2A side (same code, same 0x68050C value); the Conexant is the clock master and timing
  slave (pseudo-master) from the reset step on; details §5.1 (4627 kernel) (5838 kernel).
- **Xcalibur:** the same NV2A side; the Xcalibur supplies the clock (inferred), at the rate of its register 0x54
  input raster; details §5.2 (5838 kernel).

### 5.1 Clocking and sync with a Conexant CX25871 (1.0–1.3)
- **Interface:** pseudo-master without BLANK* (CX25870/871 datasheet, Table 1-9): SLAVER = 1 (register 0xBA bit 5),
  EN_BLANKO = 1, EN_DOT = 0 (0xC6 = 0x9C in the SD common block and the HD rows), EN_OUT = 1 (0xC4 = 0x01). SLAVER is
  set by the **reset step** (0xBA ← 0x3F right after the SRESET write 0xBA ← 0x80), not first by the DAC-on write;
  the DAC-on write 0xBA ← 0x20 | (mode bits 24–27) keeps it (4627 kernel) (5838 kernel).
- **Clock:** in every SD row the encoder makes the pixel clock from its crystal through its PLL (0xA0 bit 7 EN_XCLK =
  0) and sends it on CLKO: f = 13.5 MHz × (PLL_INT + PLL_FRACT / 65536) / 6 (datasheet §1.3.6; PLL_INT = 0xA0 bits
  0–5, PLL_FRACT = 0x9E:0x9C). The NV2A takes it through 0x68050C (§5) and returns it with the data and syncs.
- **Sync:** the NV2A's flat-panel timing generator drives HSYNC and VSYNC (positive; HSYNCI = VSYNCI = 1 in 0xC6, so
  the encoder expects active-high syncs); the encoder only uses the leading edges (datasheet §1.3.14–1.3.15).
- **The NV2A raster equals the encoder's input timing exactly:** HT (0x680824 + 1) = H_CLKI, VT (0x680804 + 1) =
  V_LINESI, and HT × VT × field rate = the PLL clock; 0x680680 / 0x680684 = H_CLKO (4627 kernel) (5838 kernel):

| NV row | Conexant SD rows | H_CLKI × V_LINESI = NV2A HT × VT | 0xA0, PLL_FRACT | Clock (MHz) | Rate (Hz) | H_CLKO | H_BLANKI / HT − HSYNC start | V_BLANKI / VT − VSYNC start | H_ACTIVE / NV active |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0x01, 0x03, 0x0B, 0x0D, 0x19 | 776 × 525 | 0x0A, 0xDA68 | 24.4196 | 59.94 | 1552 | 115 / 123 | 36 / 35 | 648 / 640 |
| 2 | 0x02, 0x04, 0x0C, 0x0E, 0x1A | 870 × 525 | 0x0C, 0x2AF7 | 27.3776 | 59.94 | 1740 | 127 / 135 | 36 / 35 | 728 / 720 |
| 3 | 0x07, 0x14 | 950 × 525 | 0x0B, 0x1555 | 24.9375 | 50 | 1596 | 247 / 255 | 35 / 37 | 648 / 640 |
| 4 | 0x08, 0x15 | 1050 × 525 | 0x0C, 0x4000 | 27.5625 | 50 | 1764 | 264 / 271 | 35 / 37 | 728 / 720 |
| 5 | 0x09, 0x16 | 788 × 625 | 0x0A, 0xF1C7 | 24.6250 | 50 | 1576 | 126 / 133 | 42 / 40 | 648 / 640 |
| 6 | 0x0A, 0x17 | 886 × 625 | 0x0C, 0x4E39 | 27.6875 | 50 | 1772 | 141 / 149 | 42 / 40 | 728 / 720 |
| 0x0F | 0x0F–0x12, 0x1B | 854 × 525 | 0x0B, 0xF1AE | 26.8741 | 59.94 | 1708 | 131 / 166 | 36 / 35 | 704 / 640 |
| 0x10 | 0x13, 0x18 | 1050 × 525 | 0x0C, 0x4000 | 27.5625 | 50 | 1764 | 283 / 319 | 35 / 35 | 704 / 640 |
| 0x12 | 0x1C, 0x1D | 868 × 625 | 0x0C, 0x0E39 | 27.1250 | 50 | 1736 | 143 / 180 | 42 / 40 | 704 / 640 |

- Rates are exact: e.g. 13.5 × (10 + 0xDA68 / 65536) / 6 = 24.41958 MHz = 776 × 525 × 59.94006 Hz.
- PAL-M rows 0x05 / 0x06 (not used by any reference mode word) have the timing of rows 0x01 / 0x02.
- **Horizontal position:** HT − HSYNC start (the NV2A's distance from the HSYNC leading edge to its first active pixel)
  is H_BLANKI + 7 or 8 in the 640 / 720 rows (the 640- or 720-pixel picture centred in the 648 / 728 H_ACTIVE window,
  inferred), and H_BLANKI + (704 − 640) / 2 + 3 to 5 in the 704-input rows. Vertical: VT − VSYNC start is
  V_BLANKI − 1 in the 525-line rows, V_BLANKI + 2 in rows 3 and 4, V_BLANKI in row 0x10 and V_BLANKI − 2 in the
  576-line rows. The encoder's tolerance to other positions is not determined (Open).
- **HD rows:** 720p and 1080i: EN_XCLK = 0, PLL_INT 0x21, PLL_FRACT 0 → 74.25 MHz from the encoder, matching the NV2A
  rows 0x0B (1650 × 750 × 60) and 0x0E (2200 × 1125 × 30). **480p: 0xA0 = 0x8C** (EN_XCLK = 1, PLL_INT 12): per the
  datasheet the encoder then takes its clock from CLKI and three-states CLKO (pin description, §1.3.6). No reference
  stage programs an NV2A clock (no VPLL write, §1), so the source of the 27 MHz in 480p on a Conexant console is not
  determined (Open). The datasheet's own 480p table (Table E-1) uses the same 0x8C (datasheet).
- **Timing reset:** 0x6C bit 7 (TIMING_RST, self-clearing) is set twice: in the reset step (0x6C ← 0xC6, before any
  row) and at the DAC-on step (0x6C read back, written with bit 7 set), i.e. after the NV2A row is running and three
  of its vertical retraces have passed (§2.3 #18–#20). The datasheet asks for a wait (75 ms in Table E-1) before
  the timing reset; the reference's wait is the three retraces plus 1 µs.
- **Consequence:** a Conexant NV2A raster must reproduce H_CLKI × V_LINESI exactly; the encoder's PLL makes
  HT × VT × field rate, so any other HT or VT makes the encoder's input FIFO drift against its output (inferred, as §5
  for the Focus).

### 5.2 Clocking and sync with the Xcalibur (1.6)
- **NV2A side:** identical to the other encoders: 0x68050C |= 0x10020000 (TV pixel clocks), no VPLL write, the
  flat-panel timing generator drives positive HSYNC / VSYNC (FP_TG_CONTROL bits 4 / 8 in every row) (5838 kernel)
  (Cerbios). Inferred: the Xcalibur supplies the pixel clock, as the Conexant and the Focus do.
- **Register 0x54 = the NV2A raster:** bits 16–27 = HT, bits 0–11 = VT, bit 15 = interlaced input (set only in the
  1080i row). In every Xcalibur row it equals the NV2A row's HT (0x680824 + 1) and VT (0x680804 + 1) (5838 kernel):

| NV row | Xcalibur rows (§3.12) | 0x54 (HT × VT) | Clock = HT × VT × rate (MHz) | 0x50 (output raster) | 0x56 bits 0–11 + 1 / HT − HSYNC start | 0x55 bits 0–7 / VT − VSYNC start |
|---|---|---|---|---|---|---|
| 1 | 0x01, 0x03, 0x0B, 0x0D, 0x0E, 0x1C | 780 × 525 | 24.5454 | 780 × 525 interlaced | 97 / 97 | 38 / 38 |
| 2 | 0x02, 0x04, 0x0C, 0x0F, 0x10, 0x1D | 858 × 525 | 27.0 | 858 × 525 interlaced | 97 / 97 | 38 / 38 |
| 3 | 0x07, 0x17 | 800 × 520 | 20.8 | 768 × 625 interlaced | 97 / 97 | 39 / 39 |
| 4 | 0x08, 0x18 | 900 × 520 | 23.4 | 864 × 625 interlaced | 139 / 139 | 39 / 39 |
| 5 | 0x09, 0x19 | 864 × 625 | 27.0 | 768 × 625 interlaced | 97 / 97 | 46 / 43 |
| 6 | 0x0A, 0x1A | 864 × 625 | 27.0 | 864 × 625 interlaced | 139 / 139 | 46 / 43 |
| 0x0F | 0x11–0x15, 0x1E | 858 × 525 | 27.0 | 858 × 525 interlaced | 97 / 137 | 38 / 38 |
| 0x10 | 0x16, 0x1B | 900 × 520 | 23.4 | 864 × 625 interlaced | 139 / 179 | 39 / 39 |
| 0x12 | 0x1F, 0x20 | 864 × 625 | 27.0 | 864 × 625 interlaced | 97 / 137 | 46 / 43 |
| 7 | 0x25, 0x26 | 780 × 525 | 24.5454 | 780 × 525 | 97 / 97 | 37 / 37 |
| 8, 0x11 | 0x21, 0x22 | 858 × 525 | 27.0 | 858 × 525 | 97 / 97 | 37 / 37 |
| 0x0B | 0x23 | 1650 × 750 | 74.25 | 1650 × 750 | 261 / 261 | 26 / 26 |
| 0x0E | 0x24 | 2200 × 1125 (interlaced) | 74.25 | 2200 × 1125 interlaced | 188 / 188 | 41 / 42 |

- Rate = 59.94 Hz (525-line and 480p rows), 50 Hz (625-line rows, also the 520-line rasters), 60 Hz (720p), 30 Hz
  frames (1080i).
- Register 0x50 has the layout of 0x54 for the output side (bit 15 = interlaced output). 0x56 bits 20–31 hold the
  input active width (0x280 = 640, 0x2D0 = 720, 0x501 / 0x781 for 720p / 1080i with bit 16 set), 0x55 bits 20–31 the
  input active lines (0x1E1, 0x1E0, 0x241, 0x2D0, 0x43A); 0x51 / 0x52 the same for the output side. Meanings beyond
  the totals are inferred from the matching values.
- **Horizontal position:** HT − HSYNC start = 0x56 bits 0–11 + 1 in every row except the 704-input rows (NV rows
  0x0F, 0x10, 0x12: + 40 more, the 640-pixel picture centred in the 720-pixel input window). **Vertical:** VT − VSYNC
  start = 0x55 bits 0–7 except the 576-line rows (3 lines less) and 1080i (1 more).
- The HSYNC and VSYNC widths of the rows vary from 2 pixels to a negative width (§3.10); inferred: only the leading
  edges matter.
- No Xcalibur register written by the mode change is identified as a PLL or clock register; registers 0x5A, 0x5B and
  0x80 change with the clock and raster (Open).
- **Consequence:** an NV2A raster derived from 0x54 gets HT and VT right; sync positions derived from 0x55 / 0x56
  differ from the reference rows in the 576-line, 704-input and 1080i rows (above).

## 6. Framebuffer and scanout (question 3)
### 6.1 Format and pitch
- CR28 bits 0–1 = depth code (2 = 16 bpp, 3 = 32 bpp), bit 7 always set; 0x680600 = 0x00100030 or 0x00101030 (§2.1).
- Pitch in 8-byte units: CR13 = P8 bits 0–7, CR19 bits 5–7 = P8 bits 8–10, CR25 bit 5 = P8 bit 11.
- The graphics library computes the pitch of a linear surface as (bytes per pixel × width + 63) & ~63 (64-byte
  multiple); the boot animation's 640 × 480 × 32-bit front buffer has pitch 2560 (CR13 0x40, CR19 bits 5–7 = 1,
  CR25 bit 5 = 0) (4627 kernel).

### 6.2 Start address (0x600800)
- `AvSetDisplayMode` writes its framebuffer argument to 0x600800 unchanged: no translation, masking or alignment
  (4627 kernel) (5838 kernel) (Cerbios).
- Every caller passes a **physical address**: the graphics library stores a surface's GPU address as its CPU (virtual)
  address & 0x03FFFFFF, and contiguous memory is identity-offset (virtual 0x80000000 + physical, or 0xF0000000 +
  physical), so the low 26 bits are the physical address (4627 kernel).
- Surfaces used for scanout are allocated with `MmAllocateContiguousMemoryEx` (lowest 0, highest 0x03FFB000 − 1,
  alignment 0x4000, write-combined read-write protection), so a front buffer is 16 KB aligned and below 64 MB.
- The graphics library writes 0x600800 itself on every frame change (§8.5); the kernel writes it when it moves the saved
  boot picture (new start = new block's physical address + the same offset), then writes 0x600100 ← 1 and waits for
  0x600100 bit 0 (the next vertical blank) before freeing the old block (4627 kernel). Inferred: a new start address
  takes effect at a vertical blank.
- **PCRTC 0x600804** (nouveau: PCRTC_CONFIG): the graphics library's device set-up writes it as read, bits 0 and 2
  cleared, bit 1 set (low bits = 2; nouveau: START_ADDRESS_HSYNC). Nothing else in the reference writes it
  (§1). Its power-on value and whether the scanout then uses 0x600800 is not determined.

### 6.3 Memory interface before scanout
Read from the X-code tables of xbox-files/bios (Complex_4627.bin: the last 256 KB bank; 5838.bin;
cerbios-hybrid-3.1.0.bin; evox.m8plus.fc.137.nodvd.bin) and of OXOS's own image (XboxDev/cromwell's table,
kernel/boot/xcodes.S). X-code entries are 9 bytes from bank offset 0x80: opcode, operand 1, operand 2; offsets below are
bank offsets of the entry. Opcode 0x08 **jumps when the current result is not equal** to operand 1 (seen from the
memory probe: a lane whose +32 MB read-back is not 0x5A5A5A5A goes to the "faulty" marking). The tables were replayed
for a 1.4 console (NV2A PMC 0x000000 low byte 0xA3, strap 0x861D4401, SMBus devices SMC, Focus encoder and EEPROM
present, nothing at the 1.6 encoder's address 0x70 or the Conexant's 0x45) to see which branch such a console takes.

**Branches by console.**
- Complex 4627 (one table for 1.0-1.5): NV2A revision 0xA1 vs other. On a non-A1 NV2A: 0x0010B0 ← 0x07633461,
  0x0010CC ← 0x66660000, 0x0010D4 ← 9 (entries 0x0158-0x0173); on A1: 0x07633451, 0, 5 (0x012B-0x0146). Strap
  0x101000 ← 0x803D4401 on A1 only (0x0203-0x020C). NVPLL 0x680500 ← 0x00011701 on A1, else 0x00011C01
  (0x0467-0x0482).
- Cerbios 3.1.0 and EvoX M8+: first an SMBus read of slave 0x70 (address byte 0xE1; Cerbios entries 0x012B-0x0185):
  a device there (the 1.6 encoder) selects the 1.6 table (0x0010B0 ← 0x01000010, ...); no answer jumps to a 1.0-1.5
  table (Cerbios from 0x0857, EvoX from 0x0ED8) that equals OXOS's/Cromwell's table below for an A3 NV2A, including
  the strap rewrite (peek 0x101000, keep its other bits, set bit 31 and bits from the revision: 0x861D4401 here).
- 5838: the 1.6 table only (0x0010B0 ← 0x01000010 at 0x0134, then a jump over the 1.0-1.5 values).
- A 1.4 console therefore takes the 4627 non-A1 branch, Cerbios's/EvoX's 1.0-1.5 table, or (OXOS) Cromwell's table.

**Values on a 1.4 console (NV2A A3), final value of each register written by the X-codes:**
| Register | Complex 4627 | Cerbios / EvoX (1.0-1.5 table) | OXOS (Cromwell) | 5838 (1.6 table) |
|---|---|---|---|---|
| 0x0010B0, 0x0010B8, 0x0010BC | 0x07633461, 0xFFFF0000, 0x5866 | same | same | 0x01000010, 0, 0x5860 |
| 0x0010C4, 0x0010C8, 0x0010CC, 0x0010D4 | 0x0351C858, 0x30007D67, 0x66660000, 9 | same | same | 0xAAAA0000, 0x7D67, 0x66660000, 0xF |
| 0x0010DC, 0x0010E8 | 0xA0423635, 0x0C6558C6 | same | same | 0, 0x04000000 |
| 0x001210, 0x001218 | 0x10, 0x00010000 | same | same | 0, 0 |
| 0x001214 (before calibration) | 0x12121212 (strap bits 18/19 set; 0x10101010 if clear) | 0x09090909 | 0x09090909 | 0 |
| 0x00122C, 0x001234, 0x001238 | 0xAAAAAAAA | same | same | 0, 0xAAAAAAAA, 0xAAAAAAAA |
| 0x001230, 0x001240 | 0xAAAAAAAA, 0x8B8B8B8B | 0xFFFFFFFF, 0xFFFFFFFF | same as Cerbios | 0xFFFFFFFF |
| 0x00123C, 0x001244, 0x001248 | 0x8B8B8B8B | same | same | same |
| 0x00124C, 0x001250 | 0xAA8BAA8B, 0x0000AA8B | same | same | 0xAA8BAA8B, 0x00005474 |
| 0x1002C0/C8 sequence; 0x1002C4/CC | 0x11, 0x32, 0x132; 0x00100042 | same | same | same; 0x00100000 |
| 0x1002D0 (twice), 0x1002D4 | 1, 1 | same | same | same |
| 0x100210 | 0x80000000 | same | same | same |
| 0x100228 | 0x081205FF | same | same | same |
| 0x100328, 0x10032C, 0x100330, 0x100338, 0x100410 | 1, 0xFFFFCFFF, 0x84848888, 0xDF, 0x11000016 | same | same | same |
| 0x101000 (strap) | not written on A3 | rewritten (0x861D4401) | rewritten (0x861D4401) | rewritten |
| NVPLL 0x680500, 0x68050C | 0x00011C01, 0x000A0400 | same | same | same |
| host 0:0:3 0x6C | 0x00230801 then 0x01230801 (both at the end, before the memory probe) | same | same | same |
| host 0:0:3 0x6C, earlier | 0x01000000 unless SMC register 0x01 reads 0x50 (4627 0x03FB-0x044C) | same condition | not written | same condition |
So for NV2A < D3 the refresh/timing values (0x100210, 0x100228), the mode-register sequence (0x1002C0-0x1002D4),
the controller set-up at 0x100328-0x100410, NVPLL and the memory PLL writes are identical in every image. The
differences on a 1.4 console are the pre-calibration pad defaults (0x1214, 0x1230, 0x1240, all rewritten by the 2BL
calibration, spec 21 §3), the 4627 table not rewriting the strap on A3, and the early SMC-dependent host 0x6C write
that Cromwell's table lacks (whether it happens on a given console depends on its SMC version byte; to be logged).

**Memory-size probe** (Complex 4627 entries 0x0671-0x0B66; the same logic in Cerbios from 0x0DF7, EvoX from 0x14ED,
5838 from 0x07FD; OXOS does it in boot/start.S because Cromwell's table has none): 0x100200 ← 0x03070103 and
0x100204 ← 0x11448000; PCI 0:2:0 and 0:3:0 register 0x3C cleared. For each of 8 lanes *n*, address *a* =
0x00555548 + 16*n* (*n* = 0-3, below 64 MB) or 0x04555548 + 16(*n* − 4) (*n* = 4-7, at 64 MB and up): write
0xAAAAAAAA at *a* and 0x5A5A5A5A at *a* + 32 MB; if *a* + 32 MB does not read back 0x5A5A5A5A the lane is marked
faulty (bit *n* of 0:3:0 0x3C); else if *a* still reads 0xAAAAAAAA (no alias at +32 MB) the lane is marked "large"
(bit *n* of 0:2:0 0x3C); then 0x55555555 at *a* must read back, else faulty. Then (0x0B15-0x0B8A):
- **default, written first:** 0x100200 ← 0x03070003, host 0x84 ← 0x03FFFFFF (64 MB);
- "large" mask 0: 0x100200 ← 0x03070103, 0x100204 ← 0x11338000 (64 MB);
- "large" mask 0xFF: 0x100200 ← 0x03070103, host 0x84 ← 0x07FFFFFF (128 MB);
- any other mask: the default stays (0x03070003, 0x11448000, 64 MB).
The masks go to the SMC (SMBus 0x20, commands 0x13 "large", 0x12 "faulty").
**What a retail 1.0-1.5 console gets:** its 64 MB are four 32-bit chips on the first 64 MB of address space; lanes
4-7 address the second 64 MB, which is not populated, so they fail the +32 MB read-back (faulty) and are not large;
lanes 0-3 do not alias at +32 MB (the 0x11448000 layout spreads the 64 MB so that *a* and *a* + 32 MB are different
cells), so they are large. Large mask 0x0F, faulty mask 0xF0: the "any other mask" row, **0x100200 = 0x03070003,
0x100204 = 0x11448000, 64 MB**. This is the ordinary retail result on such a console, observed on the 1.4 console
(OXOS's start.S makes the same decision every boot and logs 0x03070003 / 0x11448000 at kernel entry). The mask-0 row
(0x03070103 + 0x11338000) is a different chip population: every lane aliasing at +32 MB, i.e. memory on all eight
lanes but only 32 MB per 64 MB half.
- 0x100204 layout, inferred (not documented in the binaries): the two rows differ only in bits 20-27 (0x44 vs 0x33).
  Taking each nibble as a chip-select's row-address width minus 8 fits both rows: 0x4 = 12 row bits, which with 8
  column bits, 4 banks and 32-bit chips gives 16 MB per chip (128 Mbit) and 64 MB for four; 0x3 = 11 row bits, 8 MB
  per chip (64 Mbit), 64 MB for eight chips on two chip selects. This is an interpretation that matches the probe's
  logic, not a register description.
- 2BL: pad calibration (spec 21 §3) only.

### 6.3a After kernel entry: the stock kernels' accesses to the memory interface
A search of xboxkrnl-4627.exe and the 5838 kernel (xbox-files/5838-unpacked/xboxkrnl.exe) for every 32-bit
immediate or displacement naming PFB 0x100200-0x10023C, 0x1002C0-0x1002DC, 0x100300, 0x100328-0x100338, 0x100410,
the strap 0x101000, PBUS 0x1210-0x1264 and the PLLs 0x680500-0x68050C, each hit checked in the disassembly (hits that
decode as data are not listed). Addresses are 4627; the 5838 kernel has each at a parallel place (e.g. 0x80046F3E ->
0x800467DE).
| Where (4627) | Access | Register | Value |
|---|---|---|---|
| 0x800482F1, 0x800482FD (graphics device creation) | read, saved in the device | PFB 0x100200, 0x100204 | - |
| 0x80048309 | read, bit 8 tested | PBUS 0x1218 | - |
| 0x800497D6, 0x800497EE | read, copied to PGRAPH 0x4009A4 / 0x4009A8 | PFB 0x100200, 0x100204 | - |
| 0x8004656C | read | PFB 0x10020C (memory size) | - |
| 0x8004828D, 0x800481B3, 0x80048222 | read (clock computation) | NVPLL 0x680500, MPLL 0x680504, VPLL 0x680508 | - |
| 0x800483AC-0x800483B1 | read-modify-write | PFB 0x100214 | bit 0 cleared |
| 0x80046CB9, 0x80046D9D | write | PFB 0x100300 + 4·i (one register per surface region) | region value / 0 |
| 0x800424AE-0x800424CA | read-modify-write, then poll until bit 16 clears | PFB 0x100410 | bit 16 set |
| 0x800466A3-0x800466B1 | PBUS 0x184C written with bits 8-9 cleared, then its old value; then PMC 0x200 | PMC 0x000200 | 0xFFFFFFFF |
| 0x80046F3E, 0x80046F4A, 0x80046F56 (device teardown) | write back the saved values | PFB 0x100200, 0x100204, PMC 0x000200 | as read at creation |
| 0x80030779 (AvSetDisplayMode) | read-modify-write | 0x68050C | pixel-clock source (§2) |
| 0x800550BC | read | strap 0x101000 | - |
- Not written by either kernel: PFB 0x100210, 0x100228, 0x1002C0-0x1002D4, 0x100328-0x100338, the pad registers
  0x1214-0x1250 (0x1218 is only read), the strap, NVPLL / MPLL. No write of a host-bridge configuration address
  (0x8000036C, 0x8000006C, 0x80000084) occurs as an encoded constant; the kernels reach PCI configuration space
  through HalReadWritePCISpace with separate bus / slot / register arguments, so a call-site audit for host 0:0:3
  0x6C is still open.
- PMC 0x200: the only writes are 0xFFFFFFFF and the teardown's write-back of the value read at creation; nothing
  clears a bit that was set before the graphics device started (OXOS reads 0x01110000 there at kernel entry on the
  1.4 console), so whichever bit enables the memory controller is never toggled.
- Two writes to "+0x200" in .text (0x8001AB74, 0x8001ACC5) are the floating-point save area of a thread (fxsave /
  ldmxcsr nearby), not NV2A.

- Kernel `AvSetDisplayMode`: nothing (§1).
- Kernel graphics library (§7): PMC 0x200 ← 0xFFFFFFFF (all engines enabled), saves PFB 0x100200 / 0x100204 and writes
  them back when the device is destroyed, clears PFB 0x100214 bit 0, claims the GPU instance memory and zeroes
  0x5000 bytes of it.
- Inferred: the scanout works from X-code state alone (Cromwell shows a picture without the §7 set-up), so §7 is not a
  prerequisite for scanout as such.

## 7. Device set-up by the kernel's graphics library (boot animation; titles repeat it)
In order (4627 kernel) (5838 kernel) (Cerbios: same):
1. PBUS 0x1804 (PCI command mirror) |= 4 (bus master); PCRTC 0x600140 ← 0 (vertical-blank interrupt off); PTIMER
   0x9140 ← 0.
2. Read the PCI device and revision mirrors 0x1800 / 0x1808 and the memory size PFB 0x10020C.
3. PBUS 0x1830 ← 0; PBUS 0x180C ← 0xF800 (latency timer).
4. Read PMC 0x200 (if 0, write 0); save PMC 0x200 and 0x140; PMC 0x200 ← 0xFFFFFFFF. Read NVPLL 0x680500, MPLL
   0x680504 and VPLL 0x680508 and compute the clocks from a 16.666666 MHz reference: f = ref × N / (M × 2^P) (N = bits
   8–15, M = bits 0–7, P = bits 16–18).
5. PTIMER 0x9200 / 0x9210 (numerator / denominator) from the core clock and 31.25 MHz; PTIMER 0x9420 ← 0xFFFFFFFF.
6. Save PFB 0x100200 / 0x100204; read PBUS 0x1218 bit 8; claim the GPU instance memory; PFIFO 0x2210 / 0x2214
   (hash table and context area) from it; PFB 0x100214 bit 0 ← 0; zero 0x5000 bytes of PRAMIN.
7. CRTC: CR1F read (lock state), CR1F ← 0x57; **CR52 ← CR52 + 4** (nouveau: TV-out latency); PVIDEO 0x8088 ← (value &
   0xFFFFF43F) \| 0x400, then (value & 0xF43FFFFF) \| 0x4000000; PVIDEO 0x808C ← (value & 0xFFFFF40F) \| 0x400, then
   (value & 0xF40FFFFF) \| 0x4000000; **CR20 ← 0x29; CR1B ← 0x05**; **0x600804 ← (value & ~5) \| 2**; CR1F ← 0x99 if
   CR1F read 0 before (re-lock).
8. Further PFIFO / PGRAPH set-up (not needed for scanout; not catalogued here).
9. Gamma: three identity ramps (0–255) in two copies, loaded by the library (registers not catalogued).
10. Port 0x80C0 ← 1 (inferred: the field-parity GPIO of spec 17 option 0x0F set to input).
11. PBUS 0x184C: read, write with bits 8–9 cleared, write the original back; PMC 0x200 ← 0xFFFFFFFF; PMC 0x140 ← 1;
    PCRTC 0x600100 ← 1, 0x600140 ← 1 (vertical-blank interrupt on); PTIMER 0x9400 ← 0, 0x9410 ← the time of day;
    PGRAPH and PFIFO interrupt set-up.
12. Allocate the surfaces (§8.4), choose the mode word (§8.2), call `AvSetDisplayMode(base, 0, mode, format, pitch, 0)`.

## 8. Boot display (question 4)
### 8.1 Sequence from kernel entry to the title
- Before the boot animation the reference does not program the display: a search of the kernel for NV2A display
  register offsets and encoder accesses finds none between kernel entry and the animation's device creation other than
  the encoder table selection (spec 17 §1) (4627 kernel).
- The boot animation creates its graphics device (§7), which calls `AvSetDisplayMode` with framebuffer **0**: a full
  change, ending with SR01 = 0x21 (screen off) and port 0x80D3 = 5 (spec 17 §5).
- First frame presented: after the GPU has finished it, 0x600800 ← the front buffer's physical address; if a saved boot
  picture exists it is released (`MmPersistContiguousMemory(…, FALSE)`, `MmFreeContiguousMemory`,
  `AvSetSavedDataAddress(0)`); then `AvSendTVEncoderOption(9, 0)`: SR01 ← 0x01, port 0x80D3 ← 4 (and on 5838 the
  Focus DACs on). The picture becomes visible here (4627 kernel).
- Each further frame: the flip is applied at vertical blank (§8.5).
- End of the animation: a contiguous block of the front buffer's size and format is allocated, the last frame copied
  into it, `AvSetDisplayMode(base, 0, same mode, format, pitch, block's physical address)` (the same-mode path),
  `MmPersistContiguousMemory(block, size, TRUE)`, `AvSetSavedDataAddress(block)`. Spec 17 §6 then returns its pages
  to the free lists; the title's first presented frame releases it as above.
- Waits: only those inside `AvSetDisplayMode` (vertical-blank phase, three retraces) and the GPU-idle wait before the
  first flip. No encoder status is polled.

### 8.2 Choosing the mode word
- Presentation parameters of the boot animation: 640 × 480; back-buffer format 6 (A8R8G8B8 → the front buffer uses the
  linear format 0x12, 32 bpp); one back buffer; multisample type 0x2021 (the render target is 1280 × 480 and is
  filtered into a separate 640 × 480 front buffer, inferred); depth / stencil format 0x2A; refresh rate 0 (from the
  EEPROM); flags: 0x20 (interlaced) **if the AV pack is HDTV (4)**, 0x10 (widescreen) if the animation's widescreen
  flag is set (source: Open) (4627 kernel).
- AV information word (option 6, spec 12): bits 0–7 AV pack (SMC 0x04 & 7: 0 → 3 SCART, 1 → 4 HDTV, 2 → 5 VGA,
  3 → 2 RF, 4 → 6 S-video, 6 → 1 composite, other → 0), bits 8–15 the video standard and bits 22–23 the refresh
  (0x400000 = 60 Hz, 0x800000 = 50 Hz) from factory setting 0x103 (default 0x00400100), ORed with the upper half of
  user setting 8 (video flags: bit 16 widescreen, 17 720p, 18 1080i, 19 480p, 22 60 Hz).
- Mode list entries (190 entries plus an end marker; identical in 4627 and 5838): a key (bits 0–7 AV pack, 0 = any
  other pack; bits 8–15 standard; bit 16 widescreen; 17 / 18 / 19 needs 720p / 1080i / 480p; 21 interlaced; 22 60 Hz;
  23 50 Hz; 24 field (half-height) mode; 25 10:11 pixels), width, height, mode word.
- Search: the first entry whose standard equals the AV standard; from it, the first entry whose pack byte equals the AV
  pack or is 0; then, among the consecutive entries with that pack byte, the first with: equal width and height; key
  bit 16 = flag 0x10, bit 24 = flag 0x80, bit 25 = flag 0x100; key & refresh ≠ 0 (refresh = AV bits 22–23, or 0x800000
  / 0x400000 when the caller asks for 50 / 60 Hz); for pack 4, an entry with any of bits 17–19 only if one of them is
  enabled in the AV word; interlace: flags & 0x60 (or & 0x40 when flag 0x80 is set) = 0 → any, 0x20 → key bit 21
  required, 0x40 → key bit 21 must be clear. No match → 0 (`AvSetDisplayMode` then uses 0x0F010101).

### 8.3 Boot mode word per AV pack (4:3; add bit 28 for widescreen on SD packs)
| Standard (setting 0x103 bits 8–15) | Composite / S-video / RF | SCART | HDTV (component) | VGA |
|---|---|---|---|---|
| 1 (NTSC-M, 60 Hz) | 0x04010101 | 0x20010101 | **0x0801010D** | 0x88070701 |
| 2 (NTSC-J, 60 Hz) | 0x04010103 | 0x20010103 | 0x0801010D | 0x88070701 |
| 3 (PAL, 50 Hz) | 0x44030307 | 0x60030307 | 0x48030314 | no entry (the VGA entries are 60 Hz) → 0x0F010101 unless the 60 Hz flag is set |
- The boot picture is 640 × 480, X8R8G8B8-like 32 bpp (format 0x12), pitch 2560, 16 KB-aligned physical address from
  the contiguous allocator (exact address not fixed).

### 8.4 Surfaces
- Render target and front buffer are separate contiguous allocations (§6.2); only the front buffer is scanned out.

### 8.5 Flip and field adjustment
At each vertical blank the library writes 0x600800 ← the front buffer address, adjusted by the field (port 0x80C0
bit 5, inverted for 625-line modes): in a field-mode SD word (key bit 24), on field 1 the address minus pitch / 2;
in an HD interlaced mode (key bit 21, class HD: 1080i), on field 1 the address plus the pitch (4627 kernel).

## 9. HDTV (component) pack on a Focus (question 6)
- Boot: mode word **0x0801010D** (NTSC-M / NTSC-J; 0x1801010D widescreen; PAL: 0x48030314), because the boot animation
  asks for an interlaced mode on the HDTV pack, even when 480p / 720p / 1080i are enabled in the EEPROM.
- That word selects: Focus SD common block + SD row 0x0D (component YPbPr 480i, 640-pixel input; Japan: BLACK_LVL
  fix, spec 19 §1), NV row 1 (936 × 525 raster at 59.94 Hz, 29.4545 MHz from the Focus, YCrCb output), CRTC row 1,
  0x680880 = 0x21101100, DAC bits 8.
- An HD row (480p 0x88070701 / 0x88080801, 720p 0x880B0A02, 1080i 0x880E0C03) is used only when a title (or the
  dashboard) asks for a progressive or HD mode and the EEPROM enables it.

## 10. Cerbios (question 7)
- Same `AvSetDisplayMode` write sequence and values; the NV rows are re-encoded (per-encoder rows of byte indexes into
  a shared dword pool) and decode to exactly the 5838 rows and 0x680880 values for the Focus (Cerbios).
- Focus-specific (not in 4627 or 5838): after the 1 µs stall, byte write Focus 0x0C ← 0x0C; at step 20, read Focus
  0x0C and write it back with bit 7 (SYNC_MS) set, then byte write Focus 0xBA ← (mode bits 24–27) | 0x20 (the
  Conexant DAC-enable value; on the Focus 0xBA is HACT_WD low). Purpose not determined (Cerbios).
- An optional setting for HDTV or VGA packs in HD modes: separate HD encoder rows and NV2A RGB output (0x680630,
  0x6808C4, 0x68084C ← 0, also on the same-mode path).
- Optional mode-word remapping before the mode is applied; the same-mode path is skipped once (on the third call), and
  for 0x880E0C03 with a setting the pitch writes are skipped.
- Graphics library device set-up (§7) unchanged; the boot mode list has two extra entries (NTSC-J HDTV 640 × 480p with
  10:11 pixels → 0x88110F01).

## 11. Cromwell (question 8, cross-check; it shows a correct picture on Focus consoles)
- NV2A output **RGB** for Conexant and Focus (0x680630, 0x68084C, 0x6808C4 ← 0), YCrCb only for the Xcalibur; the
  Focus is programmed for RGB input (colour matrix on, VID_CNTL0 matrix bypass off) (Cromwell).
- Computes its own Focus NCO / PLL so that the Focus clock equals its NV raster × field rate (raster width = 1.25 ×
  picture width, lines = picture lines × TV lines / visible TV lines): the same pseudo-master arrangement as the
  reference, with a different raster.
- Writes 0x680880 ← 0, 0x680884 ← 0, 0x680888 ← 0, 0x68088C ← 0x10001000, 0x680890–0x68089C ← 0x10000000, and
  PFB 0x100200 ← 0x03070103 (128 MB) or 0x03070003 (64 MB); 0x600800 ← the physical framebuffer address.
- Leaves 0x680848, 0x680680–0x680690, 0x6808C0, 0x68050C (X-code value, without bit 28), 0x600804, CR52 and PVIDEO
  as the X-codes / reset left them.
- CRTC: CR14 0, CR17 0xE3, CR19 0x10, CR1B 0x05, CR22 0xFF, CR33 0x11, CR28 0x80 | depth, CR1A 0x04 (narrower than
  1280) or 0x00, CR2D bit 0 for long lines; flat-panel HCRTC = width − 1, HSYNC 32 pixels at 0.95 × total, VSYNC
  3 lines, valid area = the picture.
- Focus: all registers 0x00–0xC3 written as bytes with 500 µs between writes; at the end CR bit 0 (soft reset)
  cleared; port 0x80D3 ← 5 at the start and ← 4 at the end; SR01 ← 0x01 at the end.

## 12. What a picture on hardware depends on (inferred from §2–§11)
Registers the reference writes on every full mode change and xemu does not need (xemu shows a picture without them):
- the NV2A row's non-timing registers: 0x680848 (timing-generator control), 0x680680–0x680690 (TV block),
  0x680898 / 0x68089C (1.0 unless scaled), 0x6808C0 (0), 0x680880 (step 3 and step 19 values);
- the NV2A raster itself: it must reproduce the Focus clock exactly (§5), including FP_HCRTC (0x680828) and the sync
  positions of the row;
- 0x68050C bit 28 (and bits 17 / 19 from the X-codes);
- the CRTC row's FIFO / arbitration bytes CR1A, CR1B, CR20, CR33, CR41 and the sequencer SR04 = 0x06, AR10 = 0x01,
  AR11 = 0x4A;
- for a picture shown without the graphics library (no title running): the §7 step 7 registers (CR52, PVIDEO 0x8088 /
  0x808C, CR1B / CR20, 0x600804), which otherwise only a title's device set-up provides;
- with a Conexant or an Xcalibur: the NV2A row of that encoder (§3.7, §3.10), not the Focus row and not a raster
  from public video standards: HT × VT must equal the encoder's input raster (Conexant H_CLKI × V_LINESI, Xcalibur
  register 0x54), because the encoder makes the clock for exactly that raster (§5.1, §5.2); the sync leading edges
  sit at the distances of §5.1 / §5.2 from the encoder's active window;
- the encoder steps around the raster: Conexant reset and timing reset at the start, second timing reset and DACs only
  after three retraces of the running raster (spec 18 §1); Xcalibur outputs off first, output enable (register 0
  bit 1) before the raster, DACs (register 4) after it (spec 20 §3).

## 13. HD and VGA modes per encoder
- **VGA pack:** the reference has no VGA rows. The boot / dashboard mode list gives the VGA pack (AV pack code 5) the
  HD words 0x88070701, 0x88080801, 0x880B0A02, 0x880E0C03 (§8.2), i.e. the HD encoder rows (YPbPr levels, DAC bits
  8). No word of class 11 (bits 30–31 = 11) is in the list; 800 × 600 and 1024 × 768 do not exist in the reference
  (4627 kernel) (5838 kernel).
- A class-11 word sent by a title (e.g. nxdk's 0xC0060601, 0xC0030303, 0xC0040404) takes the **SD** path on every
  encoder: the SD common block and SD per-mode row (Conexant, Focus) or the SD row map (Xcalibur), with the NV2A and
  CRTC rows its bytes name (rows 6, 3, 4: the 576-line / 625-line rasters) (4627 kernel) (5838 kernel).
- HD words per encoder (NV row / CRTC row; clock source):

| Word | NV / CRTC row | Focus | Conexant | Xcalibur |
|---|---|---|---|---|
| 0x88070701 (640 × 480p) | 7 / 7 | 858 × 525, 640 scaled to 720; 27 MHz from the Focus PLL | 858 × 525 scaled; encoder row HD 1 with EN_XCLK = 1 (clock from CLKI, source Open) | 780 × 525 unscaled, row 0x25; 24.5454 MHz (inferred from the Xcalibur) |
| 0x88080801 (720 × 480p) | 8 / 8 | 858 × 525; 27 MHz | 858 × 525; as above | 858 × 525, row 0x21; 27 MHz |
| 0x88110F01 (480p, 10:11) | 0x11 / 0x0F | 858 × 525, valid 0x20–0x2AD | as Focus | as Focus, HSYNC 0x2F9, row 0x21 |
| 0x880B0A02 (720p) | 0x0B / 0x0A | 1650 × 750; 74.25 MHz | 1650 × 750; 74.25 MHz from the encoder PLL (PLL_INT 0x21) | 1650 × 750, row 0x23; 74.25 MHz |
| 0x880E0C03 (1080i) | 0x0E / 0x0C | 2200 × 1125 frame; 74.25 MHz | the same | the same, row 0x24 |

- **VPLL:** no stage writes 0x680508 for any mode or encoder: the only accesses in the 4627, 5838 and Cerbios
  kernels are the reads of §7 step 4 (4627 kernel) (5838 kernel) (Cerbios). 0x68050C gets the same OR 0x10020000 in
  every mode (HD, VGA-pack and SD alike).
- **No encoder answering:** the table set still follows `XboxHardwareInfo` (Conexant set when flags 0x20 and 0x400
  are clear), the encoder writes are issued without checking their result, and the NV2A rows are written as usual
  (4627 kernel) (5838 kernel). Inferred: the NV2A then has no pixel clock (0x68050C selects the encoder's), so no
  picture; whether the two waits of §2.3 (#1 on 0x6806A0, #18 on 0x6013DA, neither with a timeout) finish without a
  clock is not determined (Open).

## 14. Boot display per encoder
- The sequence of §8.1 and the mode-word choice of §8.2–8.3 do not depend on the encoder; the same word selects the
  rows of §3.12. Examples: NTSC composite 0x04010101 → Conexant SD row 1 on a 776 × 525 raster (24.4196 MHz);
  Xcalibur row 1 on 780 × 525 (24.5454 MHz); Focus 936 × 525 (29.4545 MHz). HDTV pack 0x0801010D → Conexant SD row
  0x0D (Japan: 0xA2 ← 0x08, 0xA4 ← 0xF0, 0xAC ← 0x9A); Xcalibur row 0x0D (Japan: row 0x0E and register 1 bit 15
  cleared). VGA pack 0x88070701 → §13.
- Picture on (option 9 with 0, after the first frame): Conexant — nothing beyond the shared SR01 / port 0x80D3 writes
  (its DACs are on since #20); Xcalibur — register 4 ← mode bits 24–27 again; Focus (5838) — PWR_MGNT DACs on
  (spec 17 §5a, specs 18–20) (4627 kernel) (5838 kernel).
- The kernel's graphics-library set-up (§7) has no encoder-dependent step.
- AV pack events (spec 12 §4), 5838: removal → Conexant 0xBA ← 0x3F and port 0x80D3 ← 5; Focus word 0xA0 ←
  0x020F; Xcalibur register 4 ← 0x0F. Re-insertion of the same pack → Conexant 0xBA ← 0x20 | (mode bits 24–27);
  Focus word 0xA0 ← 0x0200 | (0x0F if the bits are 0x0F, else 0); Xcalibur register 4 ← mode bits 24–27; with no
  mode set yet the bits are taken as 0x0F (all DACs off) (5838 kernel). 4627: Conexant / Focus 0xBA writes only, as
  spec 12 §4 (4627 kernel).

## 15. Cerbios and Cromwell with a Conexant or an Xcalibur (cross-check)
- **Cerbios:** the Conexant and Xcalibur NV2A rows, the 0x680880 values and the Conexant CRTC rows are the 5838 ones
  (re-encoded as byte indexes into a shared dword pool, decoded and compared); the Xcalibur CRTC rows differ only in
  CR19 (§3.11). The Conexant encoder tables (SD common, SD per-mode, HD) equal 4627's. Differences in the sequence:
  a 1 µs stall after the Xcalibur outputs-off step 0; the optional HD RGB setting (§10) applies to the Conexant and
  the Focus only (cleared for the Xcalibur), with its own Conexant HD rows (0x2E ← 0x00, 0x32 ← 0x48, 0x3C / 0x3E / 0x40 ←
  0x80, 0xC6 ← 0x98 RGB input; the other registers as the HD rows) (Cerbios).
- **Cromwell, Conexant** (shows a picture on 1.0–1.3): computes its own encoder timing from overscan ratios and sets
  the NV2A raster to its H_CLKI × V_LINESI (the same rule as the reference); NV2A output RGB and Conexant RGB input;
  in 480p the encoder PLL makes the clock (0xA0 = PLL_INT 12, EN_XCLK = 0; 858 × 525). It writes every register
  0x2E–0xFE with 500 µs between writes, with TIMING_RST and EN_OUT held off, then a timing reset, EN_OUT on, the luma
  / chroma gains at 0 until the display is enabled, then 0xA8 / 0xAA / 0xAC ← 0x81 / 0x49 / 0x8C; 0x680880 ← 0
  (Cromwell).
- **Cromwell, Xcalibur** (shows a picture on 1.6): register 4 ← 0x0F and register 0 ← 0 first, then registers
  0x00–0x8F in order (500 µs apart) from fixed tables; NV2A left in YCrCb (0x680630 ← 2, 0x68084C / 0x6808C4 as the
  reference), 0x680880 ← 0x21101100; raster totals and sync starts of reference NV rows 1 (NTSC: 780 × 525, HSYNC
  683, VSYNC 487) and 3 (PAL: 800 × 520, HSYNC 703, VSYNC 481), 480p 780 × 525, but HSYNC 32 pixels and VSYNC 3
  lines wide (Cromwell). This supports the inference that the Xcalibur keys on the sync leading edges.

## 16. Open
- Meaning of 0x680680–0x680690, 0x680898 / 0x68089C, 0x6808C0 / 0x6808C4 bit 30, 0x6806A0 (bits 0 and 4) and
  0x680630 = 3 versus 2; whether the hardware uses 0x680680 / 0x680684 (4627 and 5838 hold different HD values).
- Meaning of 0x68050C bit 28 on the NV2A TV path, and whether the picture is correct without it.
- Power-on value of PCRTC 0x600804 and of the PVIDEO 0x8088 / 0x808C fields; whether scanout reads 0x600800 with the
  power-on 0x600804 (Cromwell suggests yes).
- Whether the NV2A ignores 0x600800 bits above the memory size (the reference never sets them).
- Where the boot animation's widescreen flag comes from (no direct writer found; probably the EEPROM widescreen bit).
- The meaning of FP_HCRTC (0x680828) values that are not width − 1, and the tolerance of the Focus to a raster that is
  off by a pixel in sync position.
- Purpose of the Cerbios Focus 0x0C / 0xBA writes.
- NV rows 9, 0x0A, 0x0C, 0x0D: which title mode words (if any) use them.
- Conexant 480p (HD row 1, 0xA0 = 0x8C, EN_XCLK set): where the 27 MHz on CLKI comes from on a 1.0–1.3 board, given
  that no stage programs the VPLL and 0x68050C selects the encoder's clock.
- Xcalibur: which registers set its clock (0x5A, 0x5B, 0x80 vary with the raster); the meaning of the 0x50–0x56
  fields beyond the totals; why the 576-line, 704-input and 1080i rows put the sync edges where §5.2 shows.
- Xcalibur CRTC rows 7, 8, 0x0A, 0x0C, 0x0F: effect of CR0C, CR19 bits 0–4, CR1B bit 7 and CR2D bits 5–7, and why
  Cerbios clears CR19 in four of them.
- The Conexant's and the Xcalibur's tolerance to sync positions other than the reference rows' (§5.1, §5.2).
- Xcalibur row 7: why the step-19 value 0x21101101 (scaling bit set) is kept for an unscaled 640-pixel row.
- Without an encoder: whether the waits of §2.3 #1 and #18 complete.
