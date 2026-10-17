# Spec 17 — AvSetDisplayMode, video encoder tables (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile of 0x8003045c (`AvSetDisplayMode`, ord 3) and disassembly of
the INIT table set-up 0x800626e0. **No table contents are reproduced here**: only their layout and how they are
indexed. Our tables must be derived independently (encoder datasheets, NV2A CRTC timing arithmetic).

## 1. Which encoder
At boot (INIT 0x800626e0, cold boot) the kernel picks one of two table sets by **XboxHardwareInfo.Flags bit 0x20**
(spec 02 §5), not by probing the SMBus:
- flag 0x20 set → **Focus FS454** (SMBus 0xD4), header at 0x80012AA4
- clear → **Conexant CX25871** (SMBus 0x8A), header at 0x80012AB4
The chosen set is copied from INIT into the STICKY section (so quick reboots keep it), the encoder address is stored
at 0x8003B354 and a pointer to the header at 0x8003C434. (The 1.6 Xcalibur encoder, 0xE0, is not in this kernel.)

### Header (eight 16-bit counts)
| field | Conexant | Focus | meaning |
|---|---|---|---|
| 0 | 22 | 48 | encoder registers written for every SDTV mode (common part) |
| 1 | 63 | 61 | encoder registers per SDTV mode (bytes per row of the per-mode table) |
| 2 | 13 | 64 | encoder registers per HDTV mode |
| 3 | 6 | 28 | (not used by AvSetDisplayMode; AvSendTVEncoderOption) |
| 4 | 1 | 28 | (same) |
| 5 | 104 | 104 | bytes per row of the NV2A register table (26 dword writes) |
| 6 | 34 | 34 | CRTC registers per row |
| 7 | 12 | 14 | (not used here) |

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
| 29 | **HDTV** (component): call 0x8002FF23 (HDTV encoder set-up) and clear NV 0x680630, 0x6808C4, 0x68084C |
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
5. Bit 29 → HDTV set-up routine 0x8002FF23(base, row).
6. `out 0x80D6, (bit 28) | 4`; `out 0x80D8, 4`; NV 0x68050C |= 0x10020000; VGA 0x3C3 ← 1, misc output 0x3C2 ← 0xE3;
   PRAMDAC general control.
7. NV2A register row (field 5 bytes of dword writes at the listed offsets).
8. Bit 29: clear NV 0x680630, 0x6808C4, 0x68084C.
9. VGA sequencer (3 registers: 0x03, 0x21, 0x0F at indexes 0–2), graphics controller and attribute controller
   defaults (short fixed lists), then CRTC 0x11 ← 0.
10. CRTC row (field 6 registers; the pitch registers 0x13/0x19/0x25 get the pitch bits merged in).
11. Restore NV 0x6806A0 from step 1; CRTC 0x28 ← depth | 0x80 (screen on); wait for 3 vertical retraces (VGA
    status 0x3DA bit 3 high→low).
12. PRAMDAC 0x680880 ← the per-row value; stall 1 µs; Conexant: 0x6C |= 0x80, 0xBA ← (bits 24–27) | 0x20.
13. Framebuffer start 0x600800 ← `FrameBuffer`; remember the mode; 0x8002FDD2 (saved-data / splash bookkeeping).

## 6. Ours (status)
`src/hal/av.cpp` programs the NV2A CRTC timing for the picture size the mode word asks for (§7) and ignores the
encoder and NV2A table rows: enough for xemu (which does not emulate the encoders). For real hardware the encoder
tables must be built from datasheets with the same row numbering — tracked as milestone 7.

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

`AvSendTVEncoderOption` queries implemented: 6 (AV capabilities), 0x0D (current mode word), 0x05 (Conexant status
bits, reg 2 bits 2–3; 0 for HDTV modes), 0x0F (current field, Conexant reg 6 bit 0), 0x10 (encoder type),
0x11 (mode table version, 0); options 9 (blank), 0x0C (null mode), 0x13 (mode flag 0x10000000). Other options are
encoder writes and are no-ops until the tables exist.

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
  640×480 palette PNG as it is; any other image is scaled to fit 640×480 keeping its aspect ratio (black bars),
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
  the kernel image, page 0, the page directory, the GPU instance memory and the PTE array are mapped (spec/09 §7).
  Why (inferred): freed pages go to the end of the colour lists, so the picture survives until the title's video
  set-up replaces it, at no cost to the title's budget.
- **Ours:** the launcher waits for the fade, then frees both the decode buffer and the screen buffer and leaves the
  scanout on the screen buffer. Until 2026-10-17 the screen buffer was kept as the persisted display
  (`AvSetSavedDataAddress`), which held 301 pages (1.2 MB) at the title's entry point that the reference does not.
