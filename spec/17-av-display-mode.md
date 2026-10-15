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
`src/hal/av.cpp` programs a fixed 640×480 VGA timing on the NV2A CRTC and ignores the mode word's table rows: enough
for xemu (which does not emulate the encoders) and for the nxdk dashboard. For real hardware the tables above must
be built from datasheets with the same row numbering — tracked as milestone 7 (needs encoder datasheets and a way
to learn the row ↔ video-standard mapping that the D3D library uses).
