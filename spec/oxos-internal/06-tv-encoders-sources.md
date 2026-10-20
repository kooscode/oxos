# TV encoders: source inventory (phase 1)

Every source used, or looked for, by the TV-encoder design (06-tv-encoders.md). Each source says which kinds of fact
it is **authoritative** for:

- **CHIP:** what a register field means and what the chip does with it.
- **BOARD:** how the Xbox board wires and drives the chip: pad voltage, DAC-to-pin routing, sync polarity, the
  NV2A's output format.
- **REFERENCE:** what the retail kernel does. Behaviour only, never values.
- **STANDARD:** what the TV expects.

A fact may only come from a source authoritative for its kind. In particular, a BOARD fact never comes from a chip
datasheet's example tables, which describe the vendor's evaluation board.

## 1. Sources used

| # | Source | Licence / provenance | Authoritative for | Not authoritative for |
|---|---|---|---|---|
| S1 | Conexant CX25870/871 datasheet, doc 100381B (291 pp.), `xbox-files/datasheets/cx.txt`, `reg-details.txt` | public vendor document; facts only, no text copied | CHIP: every register field, the timing / scaling / subcarrier / caption / WSS equations, the HDTV tables E-1 (RGB in) and E-2 (YPrPb in), the low-voltage interface (§1.3.3) | BOARD: Tables E-1/E-2 route the DACs and set DRVS for Conexant's evaluation board ("adjust this register as necessary", "should be adjusted to nonzero value for low voltage interface") |
| S2 | FOCUS FS453/4 & FS455/6 Data Sheet: Hardware Reference v3.0 (2005), `fs.txt` | public vendor document | CHIP: register map and reset defaults, scaler equations, PLL limits, input formats (RGB or YCrCb with a programmable matrix, §2.2), 1080i input must already be interlaced (Table 1 note c) | BOARD |
| S3 | MontaVista `fs453.h` (in public i.MX/EZX kernel trees, e.g. `muromec_linux-ezxdev.h`) | GPL-2, 2004 | CHIP: Focus field layouts (QPR, CR, MISC, BYPASS, PLL, VID_CNTL0, DAC_CNTL, PWR_MGNT) | BOARD; MISC UIM_MOD lists Intel 0, NVIDIA 1, National 3 only |
| S4 | Freescale `drivers/video/mxc/fs453.c` | GPL-2+, 2005–2007 | CHIP: a working programming order on a non-Xbox board | BOARD |
| S5 | XboxDev/cromwell 86f5473: `drivers/video/conexant.c`, `focus.c`, `VideoInitialization.c`, `BootVgaInitialization.c` | GPL-2, an open Xbox BIOS run on real consoles | BOARD (Xbox-specific): Conexant pad drive (SD DRVS 1.8 V, HD 1.5 V), active-high syncs, DAC routing (composite, SCART and HDTV), the NV2A in **RGB** for Conexant/Focus (PRAMDAC 0x630 = 0 "Other encoders use RGB"; YUV only for Xcalibur), PRAMDAC 0x84C / 0x8C4 / 0x880 cleared, flat-panel display end (+7 only for Conexant SD, width − 1 for Focus, Xcalibur and HDTV), Focus MISC and DAC_CNTL defaults | Anything Cromwell never enabled: its HDTV code says "Only 480p supported at present", so its 720p / 1080i settings never ran. Excluded files: `xcalibur-regs.h` (its own history calls it a port of the kernel's table; values confirmed to match), Conexant `default_mode[]` (partly matches the reference's table), Focus 0x94–0x99 for 720p/1080i (match the reference byte for byte). `focus_defaults[]` has no stated provenance (some single registers agree with the reference, probably a dump of a running console): use single board fields only, never the bulk table |
| S6 | nouveau `nvreg.h`, `dispnv04/tvnv17.c` (formerly `nv17_tv.c`; MIT) | MIT | CHIP (NV family): PRAMDAC flat-panel register names and bits: FP_TG_CONTROL 0x680848 (VSYNC_POS bit 0, HSYNC_POS bit 4, MODE scale/center/native bits 8–9, READ_PROG bit 20, WIDTH_12 bit 24, DISPEN_POS bit 28), FP_MARGIN_COLOR 0x68084C, FP_DEBUG_0 0x680880 (scaling enables), NV_PRAMDAC_630; the flat-panel convention display end = width − 1, sync = position − 1 | BOARD; NV2A-specific bits (NV17 is a later chip of the same family). Its 0x630 comment is a guess ("turn off green mode (tv test pattern?)") |
| S7 | xboxdevwiki `AV_Cables` (pinout) and `Video_Encoder` | wiki; facts only | BOARD: AVIP pins 24 = V / CVBS, 22 = C / R / Pr, 11 = Y / G, 9 = B / Pb (partial pinout, marked FIXME on the wiki); which encoder per revision | Register values (Video_Encoder: Xcalibur known only from reverse engineering) |
| S8 | nxdk `lib/hal/video.c` | MIT | REFERENCE-adjacent: the mode words titles send (row meanings, stock spec 17 §7a) | encoder values |
| S9 | Retail kernels 4627 and 5838 (`xbox-files/`, never committed) | Microsoft, copyrighted | REFERENCE: behaviour (sequence, which registers each class writes, decoded interface facts); stock specs 17 and 19 | Values: never copied. Comparisons with them stay in scratch and are reported as agree / differ only |
| S10 | Standards: SMPTE 170M, ITU-R BT.470 / BT.601 / BT.709, SMPTE 293M, 296M, 274M, CEA-861, EIA-770.2/.3, IEC 61880, ETSI EN 300 294, EIA-608 | facts | STANDARD: rasters, sync, levels, colour matrices, WSS / CGMS / caption formats | board, chip |

## 2. Looked for, not found
- **nvtv** (Dirk Thierbach's GPL NVIDIA TV-out tool, which supported the CX25871 on PCs): searches found no copy of
  its source. It is known here only through Cromwell, whose `conexant_calc_blankings` is "ripped from
  nvtv/calc_bt.c" (conexant.c:413). That is why the generator cites calc_bt.c, for the H_BLANKI placement.
- **xboxfb** (the Xbox Linux framebuffer driver with Conexant/Focus support): no public copy found.
- **A full AVIP pinout** with the DAC-to-pin wiring for Focus consoles: xboxdevwiki's is partial (FIXME). The Focus
  DAC routing comes from Cromwell only.
- **Any public description of NV2A PRAMDAC 0x630, 0x6808C0 / 0x6808C4 and 0x680680–0x680690.** Only Cromwell's
  comment for 0x630 (0 = RGB, 2 = YUV) and nouveau's names exist; 0x680680 is understood as the encoder's H_CLKO
  copy from the reference's use.
- **A hardware report of Cromwell's 720p / 1080i** (never enabled) or of any Focus HD output: none.
- **The Xbox board's DAC load network** (termination, RSET), which sets the absolute output levels: no public
  schematic.

## 3. What this means for the design
- Board facts for the Conexant have two independent public sources (Cromwell and the pinout) for routing, and one
  (Cromwell) for pad drive, sync polarity and the NV2A's colour space.
- Focus board facts rest on Cromwell's defaults alone.
- 720p, 1080i and all Focus HD output have **no** Xbox-specific public source that ran on hardware. Those paths rely
  on CHIP + STANDARD facts and hardware testing (06-tv-encoders-verification.md).
