# TV encoders: conflicts register (phase 3)

Every point where OXOS's implementation (as of 931a7c6) and a source disagree, or where the sources disagree with
each other. Rules:

- No conflict is settled by preference, or by calling a source's value a typo.
- An entry is **resolved** only when a source authoritative for that kind of fact (06-tv-encoders-sources.md)
  decides it.
- Otherwise it goes on the **hardware test** list (06-tv-encoders-verification.md §4), with the variants to try.

The reference (S9) is reported as agree / differ only.

Status key: **R** = resolved by evidence (change required in OXOS; **done** = implemented 2026-10-20), **K** = resolved, OXOS already right, **T** =
hardware test, **C** = our choice, documented (not a defect).

## Conexant CX25871

| # | Topic | OXOS now | Sources | Status |
|---|---|---|---|---|
| X1 | DAC routing, SD component and HD | A = Pr, B = Y, C = Pb (Table E-1's evaluation-board routing) | BOARD: Cromwell HDTV and SCART routing A = Y/G, B = Pr/R, C = Pb/B (S5); the pinout puts DAC A on pin 11 (Y/G), B on 22 (C/R/Pr), C on 9 (B/Pb) (S7 + Cromwell composite routing). CHIP: Table E-2 "adjust this register as necessary". REFERENCE: differs from OXOS | **R, done:** A = Y, B = Pr, C = Pb, D = Y_DLY. OXOS took a BOARD fact from a CHIP example table |
| X2 | Pad drive (DRVS) in HD | 1.8 V (SD value reused) | BOARD: Cromwell HDTV uses 1.5 V for every HD format (S5). CHIP: Table E-1/E-2 leave it to the board. REFERENCE: differs from OXOS | **R, done:** 1.5 V in HD |
| X3 | 480p clock source (EN_XCLK) | clear (OXOS's "correction" of Table E-1) | CHIP: Tables E-1 **and** E-2 set it for 480p only. BOARD: Cromwell clears it for 480p. REFERENCE: differs from OXOS | **T.** In pseudo-master mode CLKI is the NV2A's copy of CLKO, so both may work. Design follows the chip vendor's tables (two tables, one author): **set, done**; test both |
| X4 | Input colour space | RGB (alternate 24-bit multiplexed) for SD, HD and SCART | BOARD: Cromwell puts the NV2A in RGB (PRAMDAC 0x630 = 0, "Other encoders use RGB") and uses RGB input on Conexant SD, SCART and HDTV. REFERENCE: YCrCb for SD / HD (with 0x630 set), RGB for SCART | **K** for the encoder side: RGB input is valid with a matching NV2A. But see X5 |
| X5 | NV2A output colour space (PRAMDAC 0x630, 0x84C, 0x8C4, 0x880) | **not written**: whatever the boot left | BOARD: Cromwell writes all four (0 for Conexant / Focus). REFERENCE: writes them every mode | **R, done:** written (all 0, RGB) with the flat-panel timing at every Conexant / Focus mode set |
| X6 | FP_TG_CONTROL (0x680848) | not written | CHIP (NV family, S6): sync polarity, display-enable polarity, scaling mode, READ_PROG. BOARD: Cromwell doesn't write it either (its boot state works for Cromwell's SD). REFERENCE: writes it every mode (positive syncs) | **T.** The encoder is set for active-high syncs (agreed by Cromwell and the reference), so the NV2A must send positive syncs. Dump the boot value on hardware; design: write DISPEN_POS, READ_PROG, HSYNC_POS, VSYNC_POS explicitly (nouveau's TV setting) |
| X7 | Flat-panel horizontal display end | width + 7 on **every** path | BOARD: Cromwell uses width + 7 **only for Conexant SD**; width − 1 for HDTV, Focus and Xcalibur. CHIP (S6): width − 1. REFERENCE: width − 1 everywhere | **R, done** for HD and Focus: width − 1 (OXOS misapplied Cromwell's Conexant-SD value). **T** for Conexant SD (Cromwell ran width + 7 on hardware; the reference differs) |
| X8 | Flat-panel horizontal "CRTC" value | width − 1 (Cromwell) | CHIP (S6): nouveau sets it above the width (hdisplay + a small margin). REFERENCE: not width − 1 either | **T:** meaning not documented. Keep Cromwell's value (ran on hardware for SD); variant: nouveau's formula |
| X9 | 1080i on the flat-panel generator | one field (540 lines + half line via CRTC 0x39), progressive TG_CONTROL | CHIP: the encoder needs interlaced input with VSYNC on alternate half lines (Appendix E). REFERENCE: full frame (1125 lines) with extra TG_CONTROL bits nouveau does not name. No public NV2A source | **T.** Highest-risk item. Variants: per field (now), full frame with the reference's TG interlace bits (bit meanings would need their own RE), and 720p fallback |
| X10 | HD sync position and width | standard positions + 2 clocks (Cromwell's 480p offset applied to all formats); standard widths | BOARD: Cromwell 480p only (+2). STANDARD: CEA-861 / SMPTE 296M / 274M nominal. REFERENCE: 480p within 2 clocks of OXOS; 720p and 1080i differ a lot (wider syncs, different position) | **T** for 720p / 1080i (horizontal position, sync lock): nominal now, the +2 offset kept for 480p only (done); **K** for 480p |
| X11 | HD component gains | Table E-1 (RGB in) | CHIP: E-1 for RGB input (what OXOS sends); BOARD: Cromwell uses the same. REFERENCE: own values for YCrCb input (not comparable) | **K** |
| X12 | Pr / Pb tri-level sync | off (EIA-770.3 note) | CHIP: E-1 enables, but its note says disable for EIA-770.3; E-2 disables. BOARD: Cromwell disables. REFERENCE: agrees with OXOS | **K** |
| X13 | NTSC SD component setup | none (Tables 1-27 / 1-28, EIA-770.2) | CHIP + STANDARD: no setup. REFERENCE: setup on NTSC component | **T** (low): black level. Both give a picture; ours follows the standard |
| X14 | SD component V (Pr) gain | Table 1-27 | CHIP: Table 1-27. REFERENCE: Y and U agree, V differs | **T** (low): red saturation on SD component |
| X15 | PAL-60 vertical sync (VSYNC_DUR) | 3 lines | CHIP: "3 lines … PAL-60" (register text). REFERENCE: 2.5 lines | **K** by the chip's own text; **T** (low): vertical lock on PAL-60 |
| X16 | SD output levels (SYNC_AMP, BST_AMP, MY, MCR, MCB) | datasheet equations for a doubly-terminated 37.5 Ω load | CHIP: equations assume the vendor's load. BOARD: no public schematic of the Xbox DAC network. REFERENCE: differs (also because its input is YCrCb) | **T:** brightness, saturation and sync amplitude on composite / S-video / SCART |
| X17 | SCART on NTSC rows: setup | NTSC-M keeps setup in SCART | STANDARD: SCART RGB carries no setup. REFERENCE: none | **R, done** (low): no setup with EN_SCART |
| X18 | SD picture area (H_CLKO, H_ACTIVE, H_BLANKO, V_BLANKO, PLL) | full active line, 480 lines unscaled | OUR CHOICE (decided 2026-10-19). REFERENCE: differs (as expected) | **C** |
| X19 | Colour-burst window (HBURST_BEGIN / END) | derived from Appendix C modes | CHIP: Appendix C / Table A-2 differ slightly from each other. REFERENCE: differs | **T** (low): colour lock on composite |

## Focus FS453/4

| # | Topic | OXOS now | Sources | Status |
|---|---|---|---|---|
| F1 | How the chip is programmed | QPR preset, then clock and scaler | CHIP: QPR is documented (S3, S4) but **what each preset loads is not**: input colour space, matrix, sync type, DAC routing. BOARD: Cromwell programs VID_CNTL0, the matrix and DAC_CNTL itself. REFERENCE: never writes QPR | **R, done (partly):** keep QPR, then write MISC and DAC_CNTL (the board values, F2 / F3), as Freescale's driver also writes them after QPR. The preset's input colour space is CHIP-documented: QPR's QK_YC_IN bit (fs453.h) selects YCrCb input, so with it clear (ours) the preset sets up the RGB-input matrix. VID_CNTL0 and the matrix therefore stay the preset's. **T:** the result |
| F2 | Input interface mode (MISC UIM_MOD) | whatever QPR's "NVIDIA" bit sets | BOARD: Cromwell's default MISC value (UIM_MOD 5, UIM_DCLK, BRDG_RST). CHIP: header lists 0 / 1 / 3 only. REFERENCE: agrees with Cromwell | **R, done:** Cromwell's board value. Its meaning is unknown, but it is Xbox-specific and ran on hardware |
| F3 | DAC routing (DAC_CNTL) | QPR default | BOARD: Cromwell default (identity A–D = video 0–3). REFERENCE: agrees with Cromwell | **R, done:** written explicitly |
| F4 | Bridge bypass in HD (BYPASS) | set (Cromwell) | BOARD: Cromwell sets it (480p only ever ran). CHIP: Table 1 note (a): no scaling in HD. REFERENCE: differs (not bypassed) | **T:** both plausible. Keep Cromwell's |
| F5 | Colour matrix | preset's | STANDARD: BT.601 / BT.709 RGB→YCbCr coefficients. BOARD: Cromwell programs BT.601-like coefficients | **K** by CHIP: QK_YC_IN clear makes the preset convert RGB input (F1). No hand-written matrix: the hardware reference does not document the coefficient format |
| F6 | HD timing registers (0x94–0x99 etc.) | preset's | No clean source (Cromwell's 720p / 1080i values match the reference). CHIP: preset | **T:** relies on the preset; no alternative source |
| F7 | 1080i input | per field | CHIP: "input data must be interlaced" (Table 1 note c) | **T** with X9 |
| F8 | Flat-panel display end | width + 7 | BOARD: Cromwell Focus width − 1 | **R, done:** width − 1 (as X7) |
| F9 | PAL-60 and NTSC-J composite | NTSC preset (NTSC colour, 7.5 IRE setup) | CHIP: QPR knows NTSC and PAL 50 Hz only | **C** (documented limitation) |
