# TV encoders: Conexant CX25871 and Focus FS454 (design)

Status: Conexant done for SD (NTSC-M, NTSC-J, PAL 50 Hz, PAL-60 at 640 and 720 wide; composite + S-video and SCART
RGB, **SD component** 480i/576i) and **HD component (480p, 720p, 1080i)**, computed by `tools/gen_tv_encoders.py` and programmed with the NV2A
flat-panel timing; **Focus HD component** (§11) and **Focus SD** (§12: composite, S-video, SCART, component); bus probe, flags, encoder type, 4-byte SMBus form;
`make test-encoders` and a boot self-test of every mode on both encoders. Not yet tried on a real console. To do:
encoder options (flicker filter, WSS/CGMS, captions), hardware testing. Priority (decided 2026-10-20): HD
component first, as HDMI adapters take component.
Milestone 7 (real-hardware video). Xcalibur (1.6 consoles) is out of scope until
its register values have a clean source (stock spec 19 describes the reference driver; its values are not public).

OXOS programs the NV2A timing today and leaves the encoder registers alone, which is enough for xemu (it does not
emulate video encoding) but gives no usable picture on a real console. This design adds the encoder side: register
values **computed by OXOS** from the chips' datasheets and the video standards, written in the order the reference
uses (stock spec 17 §5), for every mode word titles send.

## 1. Sources
Allowed (clean, public, licence-compatible), cited in the generator and the code:

| Source | Used for |
|---|---|
| Conexant CX25870/871 datasheet (291 pages: functional description, register chapter 2) | every Conexant register field, the timing/overscan/scaling equations (§1.3.6, 1.3.17), subcarrier (§1.3.26), flicker filter (§1.3.18–19), HDTV output (§1.3.42), SCART (§1.3.43), SD component (§1.3.44), power (§1.3.47) |
| FOCUS FS453/4 & FS455/6 Data Sheet: Hardware Reference v3.0 (2005) | Focus register map and defaults (§6), scaling equations VSC/HSC and the VTOTAL rule (§4), clocks and PLL limits (§9.4) |
| MontaVista `fs453.h` (GPL-2, 2004, in public i.MX/EZX kernel trees) | Focus register fields bit by bit: QPR, CR, MISC, BYPASS, PLL, VID_CNTL0, DAC_CNTL, PWR_MGNT |
| Freescale `drivers/video/mxc/fs453.c` (GPL-2+, 2005–2007) | Focus programming order (clock level, soft reset, bypass, QPR, NCO/PLL + latch, power, scaling, output control) |
| XboxDev/cromwell `drivers/video/conexant.c`, `focus.c` (GPL) | cross-check only: our generator's output is compared with theirs for the same modes |
| nxdk `lib/hal/video.c` (MIT) | the mode words titles send (row meanings, stock spec 17 §7a) |
| Video standards | NTSC (SMPTE 170M), PAL (ITU-R BT.470), BT.601 sampling, 480p (SMPTE 293M / CEA-861), 720p (SMPTE 296M), 1080i (SMPTE 274M), CGMS-A (IEC 61880), WSS (ETSI EN 300 294), captions (EIA-608) |

Excluded: the encoder tables of the 4627 and 5838 kernels and BIOS, Cerbios's tables, Cromwell's `xcalibur-regs.h`
(its history calls it a port of the kernel's tables; values confirmed to match) and Cromwell's Conexant
`default_mode[]` (partly matches the reference's table; OXOS derives those registers from the datasheet instead).

## 2. Architecture
- **Host generator** `tools/gen_tv_encoders.py` (Python, floating point allowed): for each supported mode it computes
  the encoder register set **and** the matching NV2A CRTC timing, and writes `kernel/src/hal/av_modes.inc`
  (generated, committed, with a header naming the generator and its inputs). The kernel stays integer-only.
- **Kernel** (`src/hal/av.cpp`): looks up the mode word, programs CRTC/NV2A as today, then the encoder registers in
  the reference's order (stock spec 17 §5), and implements the encoder options (§6).
- **One encoder per console**, found at boot by probing the bus (as Cerbios does; stock spec 19 §1.1): 0x8A, then
  0xD4 (then 0xE0, recognised but unsupported: the kernel leaves the encoder alone and keeps today's behaviour).
  `XboxHardwareInfo.Flags` bit 0x20 is set for Focus (and 0x400 for Xcalibur), as the reference reports them.

## 3. Modes
### 3.1 Mode words → what to produce
Mode word fields as stock spec 17 §2. Bits 30–31 give the class: **00** 525-line SD, **01** 625-line SD (PAL 50 Hz),
**10** HD, **11** VGA. Bit 29 = RGB-type output (SCART RGB in SD, the component set-up in the reference). Bits 24–27 =
DAC enables. Encoder rows (bits 0–6) seen from nxdk and titles:

| Encoder row | Standard | Input | Output |
|---|---|---|---|
| 0x01 / 0x02 | NTSC-M | 640×480 / 720×480 | composite + S-video (SCART RGB with bit 29) |
| 0x03 / 0x04 | NTSC-J (no setup, 0 IRE black) | 640 / 720 | as above |
| 0x07 / 0x08 | PAL-B/D/G/H/I 50 Hz | 640 / 720 | as above |
| 0x0B / 0x0C | PAL-60 | 640 / 720 | as above |
| 0x0D / 0x0E | NTSC 480i | 640 / 720 | SD component YPbPr (HDTV pack) |
| 0x14 / 0x15 | PAL 50 Hz 576i | 640 / 720 | SD component |
| 0x19 / 0x1A | PAL-60 | 640 / 720 | SD component |
| HD rows 1 / 2 / 3 | 480p / 720p / 1080i | 640 or 720×480, 1280×720, 1920×1080 | HD component |
| VGA (class 11) | VGA | 640×480 (and 800×600, 1024×768 if titles use them) | RGB (phase 4) |

Other rows (e.g. PAL-M, PAL-N) are added when a title is seen to use them; an unknown row falls back to the row for
the same standard and width.

### 3.2 SD: the encoder sets the NV2A timing
In SD the encoder scales the graphics into the TV raster, and the graphics timing must follow from the encoder's
settings (Conexant: H_CLKI / V_LINESI and the pixel clock it generates, datasheet §1.3.6 and §1.3.17; Focus: the
VTOTAL ratio rule, hardware reference §4.1). The generator therefore produces, per SD row, the encoder registers and
the CRTC totals/sync positions together; the CRTC rows for SD in stock spec 17 §7a (VGA-like timings, enough for
xemu) are replaced by the generated ones. xemu sizes its display from the active area only, so this does not change
what xemu shows.

### 3.3 Picture size on the TV (decided 2026-10-19)
The reference's overscan amounts are not known to us and are not copied. OXOS's default (a generator parameter):
- **NTSC / PAL-60:** 640 or 720 input pixels across the full nominal active line (52.66 µs), 480 input lines on 480
  of the 486 active lines (no vertical scaling): the picture fills the screen as on a TV broadcast; titles keep
  their own title-safe margins.
- **PAL 50 Hz:** the same horizontally; 480 input lines shown **unscaled**, centred in the 576 active lines (black
  bars above and below; decided 2026-10-19). PAL-60 is the recommended setting for PAL users: full height, no
  scaling.
- Both encoders get the same geometry, so a title looks the same on any 1.0–1.5 console.

### 3.4 HD
- **Conexant:** HDTV output mode (datasheet §1.3.42): tri-level sync, YPbPr matrix, PLL for the 27 / 74.25 MHz
  pixel clocks; CRTC timings are the standard ones OXOS already programs (CEA-861 480p, SMPTE 296M, SMPTE 274M).
- **Focus:** QPR output standard 480p / 720p / 1080i with YPbPr output, then the HD timing registers (HD_FP_SYNC,
  HD_YOFF_BP, HACT_ST/WD, VACT_ST/HT) from the same standard timings.

## 4. Conexant register set (per SD mode)
Computed from the datasheet (field names as in chapter 2):
- **Timing:** H_CLKO, H_ACTIVE, HSYNC_WIDTH, HBURST_BEGIN/END, H_BLANKO, V_BLANKO, V_ACTIVEO, H_FRACT, H_CLKI,
  H_BLANKI, V_LINESI, V_BLANKI, V_ACTIVEI, V_SCALE, from the standard's line/field timing, the input size and §3.3,
  with the overscan equations of §1.3.17; the generator searches the integer H_CLKI / V_LINESI that keep the clock
  ratio and pixel clock within the NV2A's and the datasheet's limits.
- **Clock:** PLL_INT / PLL_FRACT for the pixel clock from the 13.5 MHz reference (pseudo-master mode, the encoder
  clocks the NV2A).
- **Colour:** MSC (subcarrier increment) for 3.579545 / 4.43361875 MHz and the standard's phase/burst settings;
  MCOMPY/U/V and setup (7.5 IRE for NTSC-M, none for NTSC-J and PAL); RGB input format (24-bit multiplexed) and the
  RGB→YUV path.
- **Outputs:** DAC routing for composite + S-video, SCART RGB, or SD component (§1.3.43–44); DAC enables from mode
  bits 24–27 (register 0xBA, stock spec 17 §5).
- **Flicker filter:** the standard filter coefficients for the requested level (§6), adaptive filter off by
  default.

## 5. Focus register set (per SD mode)
Order from the Freescale driver: CR ← clock level; soft reset (CR bit 0 set, then clear); BYPASS; **QPR** with
init code 9, NVIDIA input interface, TV standard (NTSC/PAL), input size (640×480 / 720×480), output (composite+S-video
/ YPbPr / SCART), SDTV; NCON/NCOD (the NCO ratio), PLL M/N/post-divider (27 MHz crystal, PLL within the datasheet's
limits), then CR NCO latch (bit 1 set, then clear); PWR_MGNT (unused DACs off); IHO/IVO/IHW, **VSC** and **HSC** from
the hardware reference's equations and §3.3; MISC (NVIDIA interface mode); VID_CNTL0 (output mode, sync); DAC_CNTL
(routing); FIFO_LAT. The QPR preset supplies the TV-standard registers (0x40–0x8D); OXOS only overrides what §3.3 and
the options need.

## 6. Encoder options (`AvSendTVEncoderOption`)
Per encoder, from the datasheets; the option numbers and query results as the reference (stock spec 17, spec 19 §6):
- **Flicker filter (0x0B, levels 0–5):** Conexant flicker-filter fields (§1.3.18); Focus FLK (0x26) and SHP (0x24).
- **Widescreen (0x13) and CGMS (0x12):** the 20-bit CGMS-A word with its CRC for 525 lines, the WSS word for 625 lines
  (stock spec 19 §7: encoder-independent), written to the Conexant WSS fields or Focus WSS_DATAF0/F1 (0x83/0x86).
- **Closed captions (2–5, 525-line only):** the Conexant caption registers; Focus CC_FIELD_1/2, CC_CONTROL (0xAE–0xB2).
- **Blank (9):** DAC enables off / back to mode bits 24–27.
- **Macrovision (1, 0x0A):** disabled (decided 2026-10-19: a licensed technology; OXOS has no licence and no
  values). The options are accepted and do nothing; the encoders' copy-protection features stay off, and the WSS/CGMS
  word reports no copy protection.
- **Queries** as today (0x0D, 0x0F, 0x10: 0 Conexant / 1 Focus, 0x11).

## 7. Validation
1. **Generator self-check:** decode every generated register set back to what the chip will output (total and
   active lines, line period, active-line duration, subcarrier frequency, pixel clock) and assert it matches the
   standard within the datasheet's tolerances.
2. **Cross-check:** run Cromwell's Conexant and Focus calculations on the host for the same modes and compare the
   timing fields; differences are explained in the generator's comments (e.g. a different overscan choice).
3. **xemu:** it models all three encoders as SMBus devices (`-machine video-encoder=conexant|focus|xcalibur`). A new
   self-test phase boots with each and checks that the probe finds the right chip, that the flags match, and that a
   mode change leaves the generated values in the encoder's registers (read back).
4. **Real hardware:** a 1.0–1.5 console on composite, S-video, SCART and component; reports welcome, since xemu
   cannot show the analog picture.

## 8. Plan
1. **Done:** probe + flags + option 0x10 + the 4-byte SMBus form; `make test-encoders` boots with each xemu encoder.
   xemu's Xcalibur model is byte-wide (a block read returns its register 0 as the count), so 32-bit reads are only
   checked for completing on the bus.
2. **Done:** generator + Conexant SD (NTSC-M, NTSC-J, PAL 50 Hz, PAL-60), validation 1 and 3 (§9 records the
   derivation).
3. Focus SD.
4. HD on both; SD component; VGA.
5. Options: flicker filter, WSS/CGMS, captions.
6. Hardware testing; Xcalibur when a clean source exists.

## 9. Conexant SD as implemented (phase 2)
- **Self-check first:** the generator reproduces the datasheet's autoconfiguration modes 0, 1, 8 and 9 (Appendix C:
  640×480 and 640×400, NTSC and PAL) from their H_CLKO / H_CLKI / V_LINESI: every timing, clock, subcarrier and gain
  field matches, within one LSB for the gains and burst start (the vendor's tool rounds differently).
- **Derived from those modes, not from the text:** HSYNC_WIDTH is truncated; H_BLANKO = centring + 15 clocks (the
  output pipeline); V_BLANKO centres on field line 140 (525) / 167 (625); the burst ends at 7.944 µs (NTSC) /
  7.86 µs (PAL), slightly later than Table A-2.
- **Ours:** V_LINESI = 2 × the output lines per field (480 lines, no vertical scaling), H_CLKO = the smallest even
  value with HOC ≥ 0 (640 or 720 pixels across the full active line): e.g. NTSC 640: H_CLKO 1546, H_CLKI 773, 525
  lines, 24.325 MHz; PAL 50 Hz: 242 active lines centred in 288 (bars).
- **Board facts from Cromwell (GPL), unverifiable from the datasheet:** 1.8 V pads (DRVS 01), alternate 24-bit RGB
  multiplexed input, active-high syncs, no BLANK* input, FLD_MODE 10; DAC routing (composite/S-video 0xCE = 0x19,
  SCART 0xE1 with OUT_MODE 11); NV2A sync offsets (flat-panel HSYNC at H_CLKI − H_BLANKI − 7, VSYNC at V_LINESI −
  V_BLANKI + 1).
- **Kernel order:** SRESET, 0xBA ← 0x3F, the 35 + 3 generated registers (read back as a diagnostic), NV2A
  flat-panel and CRTC timing, then EN_OUT (0xC4), TIMING_RST and 0xBA ← DAC enables | SLAVER.

## 10. Conexant HD component as implemented
- **Encoder:** the datasheet's Table E-1 (24-bit RGB in, YPbPr out) per format, checked by the generator, with three
  documented deviations: Pr/Pb sync disabled (the table's own EIA-770.3 note), the Xbox pads and alternate RGB input
  in 0x32 (as for SD), and EN_XCLK clear for 480p (the table's 8C would take the clock from CLKI, contradicting its
  own pseudo-master setting). PLL: 27 MHz (480p), 74.25 MHz (720p, 1080i: 60 Hz family). Sequence: SRESET, 0xBA ←
  0x3F, the 12 registers (read back), NV2A timing, EN_OUT, **75 ms** (Table E-1's wait), TIMING_RST, DAC enables.
- **NV2A:** the standard rasters (CEA-861 / SMPTE 293M: 858 × 525; SMPTE 296M: 1650 × 750; SMPTE 274M: 2200 × 1125,
  per field) on the flat-panel generator, HSYNC two clocks after nominal (Cromwell's 480p value). 640 × 480p is
  centred in the 720-wide 480p raster (sync at 696 instead of 736), so all three HD formats run at the standard
  clock.
- **Tested in xemu** (`run.py --hd`, `make run HD=1`): Halo picks 480p (0x88070701), Enter the Matrix 720p
  (0x880B0A02) or 1080i (0x880E0C03); the encoder registers read back as generated. xemu does not show the analog
  side, so the picture on a real TV is untested.
- **Unverified:** 1080i on the flat-panel generator (the encoder needs VSYNC within ±5 clocks of mid-line on alternate
  fields; whether the NV2A's flat-panel timing does this like its CRTC is unknown), the HSYNC offset, and 60 Hz vs
  59.94 Hz for 720p/1080i (74.25 vs 74.176 MHz; both are standard).

## 10a. Conexant SD component as implemented
- Rows 0x0D/0x0E (NTSC), 0x14/0x15 (PAL 50 Hz), 0x19/0x1A (PAL-60): the SD timing of the composite twin (0x01/0x02,
  0x07/0x08, 0x0B/0x0C) plus the datasheet's Tables 1-27/1-28 for EIA-770.2-A component (checked by the generator):
  MCOMPY/U/V = 0x80/0x90/0x66, OUT_MODE 10 (Pr / Y / Pb / Y_DLY: DAC A = Pr, B = Y, C = Pb, the HD wiring), no
  setup, SYNC_AMP 0xF0 (300 mV, on Y only), MY 0x8C (700 mV white). The row says which output it is, so mode bit
  29 only selects SCART for the composite rows.

## 11. Focus HD component as implemented
- **The chip's own presets:** after CR ← GCC_CK_LVL and a soft reset, BYPASS ← bridge bypass (the scaler is unused
  in HD), then **QPR** = init code 9 | NVIDIA input interface | output standard (480p / 720p / 1080i) | YPbPr |
  input size (640 or 720 × 480 for 480p): the quick-program register loads the FS453/4's built-in settings for that
  standard (field layout from MontaVista's GPL fs453.h; sequence from Freescale's GPL fs453.c).
- **Clock:** NCO off (NCON = NCOD = 0), PLL 27 MHz / N × M / P: 480p 125 / 1000 / 8 = 27 MHz, 720p and 1080i
  200 / 1100 / 2 = 74.25 MHz (registers M − 17, N − 1, P − 1 for both post-dividers), then the CR NCO latch pulse.
  These dividers are Cromwell's (hardware-tested, not in Microsoft's tables); the generator checks them against the
  PLL limits (100–1000 kHz phase detector, 100–300 MHz VCO).
- **Last:** PWR_MGNT (0xA0) ← SD clock off (CLK_SOFF = 2) | DAC disables = mode bits 24–27 (the same meaning as the
  Conexant's 0xBA bits). NV2A: the same standard rasters as Conexant HD (§10).
- **Not used:** Cromwell's Focus 720p / 1080i timing values (registers 0x94–0x99) match Microsoft's tables byte for
  byte (checked against the 4627 kernel and the 5838 BIOS), so OXOS relies on the QPR preset instead; its 480p
  values (no match) are not needed either.
- **Reference quirk kept:** the AV-pack unplug/replug handler writes register 0xBA at the encoder's address whatever
  the chip (4627, 0x800148f4 / 0x80014a7b); on a Focus that register is HACT_WD.
- **Unverified on hardware:** that the QPR preset alone gives the right HD timing and colour matrix for the NV2A's
  RGB input (Cromwell sets the matrix and timing registers by hand), the PLL charge-pump bits (left 0, as Cromwell).

## 12. Focus SD as implemented
- **Clocking without scaling:** the GPU sends the TV's frame proportions (hardware reference §4.1, no vertical
  scaling: VSC = 0): 858 × 525 at 59.94 Hz (NTSC, NTSC-J, PAL-60) or 864 × 625 at 50 Hz (PAL), progressive, which is
  exactly 27 MHz, made by the PLL alone (§2.11; the 480p dividers 125 / 1000 / 8; NCON = NCOD = 1 as Freescale's SD
  presets).
- **QPR preset:** init 9 | NVIDIA | SDTV | flicker filter | output (composite/S-video 0, YPrPb 1, SCART 2) | input
  640 or 720 × 480 | NTSC or PAL. Then IHO / IVO / IHW (§4.3: IHO = HTOTAL − HSYNC position, IVO likewise, centring
  480 lines in 487 or, for PAL 50 Hz, in 576 with bars), HSC for the full active line (711 / 702 TV pixels:
  640 upscales by HUSC 28, 720 downscales by HDSC 252, §4.2), FIFO_LAT 130 (up) / 164 (down) (fs453.h constants),
  BYPASS 0. PWR_MGNT last: HD clock off (CLK_SOFF 1) | DAC disables.
- **Limits:** the QPR knows NTSC and PAL 50 Hz only. NTSC-J uses the NTSC preset (its 7.5 IRE setup stays, so black is
  slightly lifted on Japanese TVs); PAL-60 uses it too: right for component and SCART RGB (no subcarrier), but
  composite / S-video then carry NTSC colour (PAL-60's subcarrier would need the chroma registers, whose field
  layout no public source documents).
- **Tested in xemu:** the boot self-test sets all 15 mode words on the Focus model (`make test-encoders`); the xemu
  dashboard picks 480i component (0x0801010D) on it and reports "Focus FS454".

## Open
- Whether titles send other encoder rows (PAL-M, PAL-N, NTSC-J 480p…): to be logged with the tracer.
- The exact picture size users prefer on real TVs (§3.3: full area for now; an option later).
- Hardware verification of the board facts above (pad level, sync polarity and offsets, DAC routing), the picture
  position (H_BLANKO pipeline constant) and colour levels.
