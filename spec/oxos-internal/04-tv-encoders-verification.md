# TV encoders: verification (phase 5)

State verified: commit 931a7c6 (code), against the tagged design of 04-tv-encoders.md §14. The **R** fixes of the
conflicts register were implemented on 2026-10-20; §1.2 and §1.4 show both states, and §3 gives the confidence
for both. Comparisons with the
retail kernel (4627) were run in scratch and are reported as **agree / differ / not present** only; no reference
values appear here.

## 1. Our values, decoded
Decoded from the generator's output (`tools/gen_tv_encoders.py`) with the datasheet's equations: PLL → clock,
H_CLKO → line time, MSC → subcarrier, H_ACTIVE / H_CLKI → active picture time, field registers → levels.

### 1.1 Conexant SD (all 14 rows)
| Rows | Clock | Line | Field rate | Subcarrier | Active line (standard) | Active lines per field | HSYNC | Burst | Sync level |
|---|---|---|---|---|---|---|---|---|---|
| NTSC-M / NTSC-J 640 (0x01, 0x03, 0x0D) | 24.325 MHz | 63.556 µs | 59.940 Hz | 3 579 545.1 Hz | 52.62 µs (52.66) | 242 (240 + 2 filtered) | 4.69 µs | 5.30–7.93 µs | 286 mV (NTSC 40 IRE: 286) |
| NTSC-M / NTSC-J 720 (0x02, 0x04, 0x0E) | 27.378 MHz | 63.556 µs | 59.940 Hz | 3 579 545.2 Hz | 52.60 µs | 242 | 4.68 µs | 5.30–7.93 µs | 286 mV |
| PAL 50 Hz 640 (0x07, 0x14) | 24.625 MHz | 64.000 µs | 50.000 Hz | 4 433 618.1 Hz | 51.98 µs (52.00) | 242 of 288 (bars) | 4.67 µs | 5.60–7.88 µs | 300 mV |
| PAL 50 Hz 720 (0x08, 0x15) | 27.719 MHz | 64.000 µs | 50.000 Hz | 4 433 618.1 Hz | 51.95 µs | 242 of 288 | 4.69 µs | 5.59–7.86 µs | 300 mV |
| PAL-60 640 / 720 (0x0B, 0x0C, 0x19, 0x1A) | 24.325 / 27.378 MHz | 63.556 µs | 59.940 Hz | 4 433 618.6–.8 Hz | 52.62 / 52.60 µs | 242 | 4.69 µs | 5.59–7.85 µs | 300 mV |

- Every subcarrier is within 1 Hz of nominal (PAL tolerance ±5 Hz, NTSC ±10 Hz).
- Every line time and field rate is exact to the standard.
- The active line is within 0.06 µs of nominal (full-area choice).
- SD component rows (0x0D–0x1A): 300 mV sync on Y only, 700 mV white, no setup (EIA-770.2).

### 1.2 Conexant HD
| Format | Clock | Raster (flat panel) | Routing A / B / C (931a7c6 → now) | Pad drive | Clock source |
|---|---|---|---|---|---|
| 480p | 27.000 MHz | 858 × 525, 59.94 Hz | Pr / Y / Pb → **Y / Pr / Pb** | 1.8 V → **1.5 V** | PLL → **CLKI** |
| 720p | 74.250 MHz | 1650 × 750, 60 Hz | Pr / Y / Pb → **Y / Pr / Pb** | 1.8 V → **1.5 V** | PLL |
| 1080i | 74.250 MHz | 2200 × 562.5 per field, 60 fields/s | Pr / Y / Pb → **Y / Pr / Pb** | 1.8 V → **1.5 V** | PLL |

In 931a7c6, Y left on the Pr pin (22) and Pr on the Y pin (11): a component input would have seen no usable luma
or sync (conflicts X1, X2, X3). Clocks and rasters were right. SD component got the same routing fix.

### 1.3 Focus
- HD: PLL 27.000 / 74.250 MHz (27 MHz × M / N / P within the PLL limits).
- SD: 27.000 MHz PLL, NCO 1:1, the GPU sending 858 × 525 (59.94 Hz) or 864 × 625 (50 Hz) progressive. That is
  exactly the TV line rate, so no NCO dithering.
- Everything the QPR preset loads is not decodable, since no public source lists the presets' contents (F1).

### 1.4 NV2A flat panel
- Totals and syncs match §14.4.
- Display end was width + 7 on every path; now width + 7 for Conexant SD only, width − 1 elsewhere (X7).
- PRAMDAC 0x630 / 0x84C / 0x8C4 / 0x880 were not written; now cleared (RGB) at every TV mode set (X5).
- TG_CONTROL is still not written (X6: waits for the boot-value dump).

## 2. Field-by-field comparison with the reference

### 2.1 Conexant SD composite / S-video (rows 0x01–0x0C)
| Group | Result |
|---|---|
| Interface: DRVS, IN_MODE[3], HSYNCI, VSYNCI, EN_BLANKO, FLD_MODE, EACTIVE, EN_REG_RD | agree |
| Input format IN_MODE[2:0] | differ (by design: RGB vs YCrCb; X4) |
| Output: OUT_MODE, OUT_MUXA–D, EN_SCART | agree |
| Standard bits: SETUP, 625LINE, PAL_MD | agree |
| VSYNC_DUR | agree, except PAL-60 rows: differ (X15) |
| Vertical, 525-line rows: V_ACTIVEI, V_LINESI, V_BLANKI, V_ACTIVEO, V_BLANKO, V_SCALE | agree |
| Vertical, PAL 50 Hz rows | V_ACTIVEI agrees, rest differ (our unscaled + bars choice, X18) |
| Clock: H_CLKO, H_CLKI, PLL | 720-wide 525-line rows: agree; 640-wide and PAL 50 Hz rows: differ (X18) |
| Picture area: H_ACTIVE, H_BLANKO, H_BLANKI | differ (X18) |
| HSYNC_WIDTH | agree, except PAL 640: differ |
| Burst window HBURST_BEGIN / END | differ in most rows (X19) |
| Levels: SYNC_AMP | NTSC rows differ, PAL rows agree |
| Levels: BST_AMP, MY, MCR, MCB | differ (X16; partly the input format) |
| MSC | agree on the 720-wide NTSC rows; differ elsewhere, including PAL-60 720, where the clock agrees |
| WSSINC | agree where the clock agrees, else differ (follows the clock) |
| MCOMPY / U / V | agree |

### 2.2 Conexant SD component (rows 0x0D–0x1A)
As 2.1, plus:
- OUT_MODE, OUT_MUXC, OUT_MUXD agree; **OUT_MUXA, OUT_MUXB differ (X1)**;
- MCOMPY and MCOMPU agree, MCOMPV differs (X14);
- SETUP differs on the NTSC rows (X13).

### 2.3 Conexant SCART (bit 29; rows 0x01, 0x07, 0x0B)
- Agree: IN_MODE (both RGB), OUT_MODE, all four OUT_MUX, EN_SCART.
- Differ: SYNC_AMP, MY, MCR, MCB (X16), and SETUP on NTSC-M (X17).

### 2.4 Conexant HD (480p, 720p, 1080i)
- Agree: HDTV_EN, RPR_SYNC_DIS, BPB_SYNC_DIS, HD_SYNC_EDGE, RASTER_SEL, CSC_SEL, IN_MODE[3], HSYNCI, VSYNCI,
  EN_BLANKO, PLL_INT, PLL_FRACT, OUT_MODE, OUT_MUXC, and EN_XCLK for 720p / 1080i.
- Differ:
  - **DRVS (X2)**;
  - **OUT_MUXA, OUT_MUXB, OUT_MUXD (X1)**;
  - **EN_XCLK for 480p (X3)**;
  - IN_MODE, RGB2PRPB and MCOMPY / U / V: by design, since the input format differs (X4, X11).

### 2.5 Focus
| Group | Result |
|---|---|
| QPR | ours only (the reference never writes it; F1) |
| MISC, VID_CNTL0, DAC_CNTL, colour matrix | not present in ours (F1, F2, F3, F5) |
| CR | agree on NTSC rows and HD; differ on PAL rows |
| BYPASS in HD | differ (F4) |
| HD clock: NCO off, PLL output frequency | agree (divider choice differs) |
| SD clocking | differ in kind: the reference uses the NCO with a scaled GPU raster; ours runs a 1:1 PLL raster |
| SD scaler IHO, IVO, HSC | differ (picture-area choice and clocking) |
| SD IHW | agree; VSC agrees on NTSC rows |
| PWR_MGNT clock-off bits | agree |

### 2.6 NV2A
| Register | Result |
|---|---|
| Flat-panel totals, HD | agree (standard rasters) |
| Flat-panel totals, SD | differ (follow the encoder clock choice) |
| Display end | differ on every path (X7) |
| Horizontal CRTC value | differ (X8) |
| Sync widths, SD Conexant | agree |
| Sync position and width, 480p | differ slightly (≤ 2 clocks / lines) |
| Sync position and width, 720p and 1080i | differ substantially (X10) |
| 1080i layout | differ in kind: field vs frame (X9) |
| TG_CONTROL, 0x680630, 0x68084C, 0x6808C0 / C4, 0x680898 / 9C, 0x680680–90 | not present in ours (X5, X6) |

## 3. Confidence per path
"Works" means a stable, correctly coloured picture on a typical TV or HDMI adapter. The estimates are judgement
calls from the evidence above, not measurements.

| Path | As built (931a7c6) | Now (R fixes implemented) | Main remaining risks |
|---|---|---|---|
| Conexant composite / S-video | 70 % | 75 % | X6 sync polarity (boot state), X16 levels, X7 |
| Conexant SCART RGB | 55 % | 65 % | X16, X5 (now written) |
| Conexant SD component | 5 % | 60 % | X13, X14, X16 |
| Conexant 480p | 5 % | 65 % | X3 (two variants), X6; the fixed path then matches Cromwell's hardware-run 480p except EN_XCLK |
| Conexant 720p | 5 % | 40 % | X10 (no board source), X6 |
| Conexant 1080i | 2 % | 15 % | X9 (field vs frame), X10 |
| Focus SD | 30 % | 40 % | F1 (the preset's contents), F5 coefficient format |
| Focus 480p | 20 % | 35 % | F1, F4 |
| Focus 720p / 1080i | 15 % / 5 % | 25 % / 10 % | F6 (no clean timing source), F7 |

## 4. Hardware test plan
Ordered by what the user can test first: an HDMI adapter (component in) on a Conexant console (1.0–1.3), then SD
outputs, then a Focus console (1.4–1.5).

**Step 0: diagnostics build (to be written).** A BIOS variant that, before the first mode set, records the boot
values of PRAMDAC 0x680848, 0x680630, 0x680880, 0x68084C and 0x6808C4 and the encoder's chip ID. It shows them on
screen on composite (the most likely path to work) and blinks them on the front LED if there is no picture. This
settles X5 and X6 before any variant testing. The test console will also have serial output through the
SuperIO (debug serial, spec/stock/18), so the build can print these values and every mode set's register writes
there, to be read directly during the session.

| # | Console / cable | Mode (how to get it) | Variants | Pass criteria | Conflicts |
|---|---|---|---|---|---|
| 1 | Conexant, HDTV pack → HDMI adapter | 480p: dashboard with 480p enabled, or `HD=1` boot self-test mode | EN_XCLK set / clear | adapter locks; correct colours (red / green / blue bars, skin tones); no horizontal shift | X1, X2, X3, X6 |
| 2 | same | 720p (Enter the Matrix with 720p enabled) | sync position nominal / nominal + 2; TG polarity | lock; picture horizontally centred; no edge roll | X10, X6 |
| 3 | same | 1080i (Enter the Matrix with 1080i) | per field / full frame | lock; both fields at the right height (no line pairing or bounce) | X9, X10 |
| 4 | Conexant, HDTV pack → TV component input (if available) | 480i (dashboard SD on the HDTV pack) | — | colour; black level; red saturation | X1, X13, X14 |
| 5 | Conexant, composite / S-video cable | NTSC-M, PAL-60, PAL 50 Hz | display end + 7 / − 1 | stable lock; colour (burst); brightness and saturation vs a reference source | X7, X15, X16, X19 |
| 6 | Conexant, SCART RGB (PAL region) | PAL 50 Hz, PAL-60 with RGB | — | RGB picture, correct levels | X16, X17 |
| 7 | Focus, HDTV pack → HDMI adapter | 480p, then 720p / 1080i | BYPASS set / clear | as 1–3 | F1–F7 |
| 8 | Focus, composite / S-video | NTSC, PAL 50 Hz | — | as 5 | F1, F5 |

Each test records: console revision, encoder found (option 0x10), cable / adapter model, mode word, variant, and
what was seen (photo if possible). A variant that fails is a bug report against the design row in §14 that chose
it, and the conflict entry is updated with the result.
