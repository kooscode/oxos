# Display modes (OXOS choices)

OXOS's own choices where it does not follow the reference's display behaviour (stock spec 17). First implemented
2026-10-15 (progressive CRTC), extended 2026-10-17 (HD and VGA timings, the widescreen line). Code:
`kernel/src/hal/av.cpp`.

## 1. Timings per mode
- The reference programs each mode from its CRTC and NV2A table rows (stock spec 17 §5, §7). OXOS programs the NV2A
  CRTC from the public standards for the picture size the mode word asks for (the mode words: stock spec 17 §2a):
  CEA-861 480p; SMPTE 296M 720p (1650×750 total); SMPTE 274M 1080i (2200×1125, programmed per field with CR39 =
  interlace half-line); VESA 800×600 and 1024×768. Unknown rows fall back to 640×480.
- High bits go to the NV extended CRTC registers CR25 (vertical bit 10s, kept with the mode across quick reboots and
  merged with the pitch bit) and CR2D (horizontal bit 8s).
- xemu sizes its display from CR01, CR12/CR07 and CR39 (doubling interlaced heights): measured 1920×1080 interlaced
  and 1280×720 for 0x880E0C03 and 0x880B0A02 (before: 640×480 for every mode).
- **Progressive output:** CR39 (interlace half-line position) must be 0xFF, as the reference leaves it: xemu treats
  any other value as interlaced and shows each field at half height (observed: Halo's menu squished into the top half
  of the window). Line compare (CR18) is likewise 0xFF (never).
- The pitch is programmed as the title passes it (for 1080i, half the line length: stock spec 17 §2a).
- On Conexant and Focus encoders, SD and HD component mode words also program the encoder and the NV2A flat-panel
  timing; VGA and the Xcalibur leave the encoder alone.

## 2. The widescreen line
- OXOS writes the GPIO ports of stock spec 17 §5 step 6 (`0x80D6 ← (bit 28) | 4`, `0x80D8 ← 4`) on every mode
  change, as the reference does. xemu's "auto" aspect ratio shows 16:9 exactly when the last value written to 0x80D6
  is 5; without the write every title shows 4:3 in xemu (anamorphic pictures look squashed).
- **Deviation:** the line is also raised for 720p and 1080i mode words (HDTV table, CRTC rows 0x0A and 0x0C), which
  are always 16:9, so xemu's "auto" shows them at 16:9. Titles leave bit 28 clear in HD mode words, so on the
  reference xemu shows them at 4:3. 480p keeps the reference rule (bit 28 only), since 480p can be 4:3 or 16:9.

## 3. Encoder options
The option behaviour of stock spec 17 §5a, except Macrovision: it stays off (options 1 and 0x0A do nothing; the
CGMS-A word carries level 0).

## 4. Encoder detection
The reference decides from its own BIOS image (stock spec 17 §1). OXOS probes the bus instead, as Cerbios 3.1.0 does
(its kernel's `AvSetDisplayMode`, 0x80036FCB): an SMBus byte read (protocol 0x0A, command 0) at 0x8A, then 0xD4, then
0xE0; the first that completes (host status 0x10) is the encoder. Reason: OXOS's open BIOS has 0x70 at flash offset
0x78, and one image should serve every revision. Flags 0x20 (Focus) / 0x400 (Xcalibur) are reported exactly as the
reference would, and option 0x10 returns 0 / 1 / 2.

## 5. Xcalibur
- **SMBus (done):** the 4-byte block form (stock spec 20 §1) in the HAL, keyed by address 0xE0–0xE7 and `WordFlag`,
  so titles and the kernel see the reference's behaviour (`src/hal/smbus.cpp`).
- **Tables (planned):** the pool / index scheme is a compression choice, not ABI: OXOS can store the expanded rows of stock
  spec 20 §6.2 directly, keyed by (mode word fields, region). Only the mode-word meaning, the option behaviour and
  the register sequence matter.

## 6. xemu notes
- Enter the Matrix in 720p or 1080i aborts xemu at render scale 2 or more (assertion in `surface_download_to_buffer`:
  one of the title's render surfaces has a pitch smaller than width × bytes per pixel, which xemu's downscale path
  rejects); at scale 1 it runs. The retail BIOS we have returns this title to the dashboard within 0.3 s in xemu, so
  there is no reference run to compare.
- xemu's display code expects the 1080i pitch convention (it divides the surface pitch by the CRTC offset).
