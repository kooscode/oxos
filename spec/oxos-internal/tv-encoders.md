# TV encoders: Conexant CX25871 and Focus FS454 (design)

Status: design, not implemented. Milestone 7 (real-hardware video). Xcalibur (1.6 consoles) is out of scope until
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
1. Probe + flags + option 0x10 + the per-encoder SMBus writes (no visible change in xemu), self-test with each
   xemu encoder.
2. Generator + Conexant SD (NTSC-M first, then PAL, NTSC-J, PAL-60), validation 1–3.
3. Focus SD.
4. HD on both; SD component; VGA.
5. Options: flicker filter, WSS/CGMS, captions.
6. Hardware testing; Xcalibur when a clean source exists.

## Open
- Whether titles send other encoder rows (PAL-M, PAL-N, NTSC-J 480p…): to be logged with the tracer.
- The exact picture size users prefer on real TVs (§3.3: full area for now; an option later).
- The Conexant pseudo-master clock details on the Xbox board (which pins drive the NV2A pixel clock) — from the
  datasheet's interface modes and, if needed, testing.
