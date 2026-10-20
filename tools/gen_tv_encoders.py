#!/usr/bin/env python3
"""Generate OXOS's TV encoder mode tables (spec/oxos-internal/tv-encoders.md).

Computes, for every SD mode word titles use, the Conexant CX25871 register values and the matching NV2A timing, from:
- the CX25870/871 datasheet (Conexant doc. 100381B): the register bit map (Table 2-1), the field definitions and
  equations (Table 2-5, sections 1.3.6, 1.3.13, 1.3.17, 1.3.26, 1.3.29), the target video parameters per standard
  (Tables A-1 to A-3), and its autoconfiguration modes (Appendix C), which this script reproduces as a self-check;
- the video standards those tables summarise (SMPTE 170M, ITU-R BT.470);
- XboxDev/cromwell's drivers/video/conexant.c and nvtv's calc_bt.c (both GPL): the FIFO placement of the input
  blanking (H_BLANKI) and the Xbox board's interface and output wiring (DAC routing, sync polarity, pad drive).
No register values are taken from Microsoft's or Cerbios's tables.

Usage:
  gen_tv_encoders.py --check            verify against the datasheet's autoconfiguration modes, print our modes
  gen_tv_encoders.py --out FILE.inc     write the kernel table (kernel/src/hal/av_modes.inc)
"""
import math
import sys

FXTAL = 13.5e6  # the Xbox's encoder crystal; 1.3.6: Fclk = Fxtal * (PLL_INT + PLL_FRACT / 2^16) / 6

# Target video parameters (datasheet Tables A-1..A-3; times in seconds, levels in volts).
#   tto/ato: total and active line time; centre: image centre after the sync edge; first/last: active field lines;
#   centre_line: the field line the vendor's V_BLANKO values centre on (Appendix C: 140 for 525 lines, 167 for 625);
#   burst_end: the 50% point of the last burst cycle as the vendor's own modes place it (Appendix C: 7.944 us in
#   both NTSC modes, 7.86 us in both PAL modes; Table A-2's 7.82 / 7.85 us end the burst slightly earlier).
STANDARDS = {
    "NTSC-M": dict(fsc=3579545.0, fsc_per_line=455 / 2, burst_start=5.3e-6, burst_end=7.944e-6, hsync=4.7e-6,
                   tto=63.55556e-6, ato=52.65556e-6, centre=35.667e-6, alo=243, tlo=262.5, lines=525, setup=True,
                   v100=0.661, sync_v=0.286, burst_v=0.2857, pal=False, centre_line=140),
    "NTSC-J": dict(fsc=3579545.0, fsc_per_line=455 / 2, burst_start=5.3e-6, burst_end=7.944e-6, hsync=4.7e-6,
                   tto=63.55556e-6, ato=52.65556e-6, centre=35.667e-6, alo=243, tlo=262.5, lines=525, setup=False,
                   v100=0.714, sync_v=0.286, burst_v=0.2857, pal=False, centre_line=140),
    "PAL-BDGHI": dict(fsc=4433618.75, fsc_per_line=1135 / 4 + 1 / 625, burst_start=5.6e-6, burst_end=7.86e-6,
                      hsync=4.7e-6, tto=64.0e-6, ato=52.0e-6, centre=36.407e-6, alo=288, tlo=312.5, lines=625,
                      setup=False, v100=0.7, sync_v=0.3, burst_v=0.3, pal=True, centre_line=167),
    # PAL colour on 525-line, 59.94 Hz timing (Table A-1: line rate 15734.264 Hz, no setup, 0.7 V white).
    "PAL-60": dict(fsc=4433618.75, fsc_per_line=4433618.75 * 63.55556e-6, burst_start=5.6e-6, burst_end=7.86e-6,
                   hsync=4.7e-6, tto=63.55556e-6, ato=52.65556e-6, centre=35.667e-6, alo=243, tlo=262.5, lines=525,
                   setup=False, v100=0.7, sync_v=0.3, burst_v=0.3, pal=True, centre_line=140),
}

VFS = 1.28           # DAC full-scale output (1.3.13)
LSB_MV = 1.25e-3     # SYNC_AMP / BST_AMP step (Table 2-5)
PIPELINE_HBLANKO = 15  # output pipeline delay in CLKO clocks: H_BLANKO of every Appendix C mode = centring + 15

# Conexant register bit map (Table 2-1): field -> [(register, register bit, field bit, width)].
FIELDS = {
    "H_CLKO": [(0x76, 0, 0, 8), (0x86, 0, 8, 4)],
    "H_ACTIVE": [(0x78, 0, 0, 8), (0x86, 4, 8, 3)],
    "HSYNC_WIDTH": [(0x7A, 0, 0, 8)],
    "HBURST_BEGIN": [(0x7C, 0, 0, 8), (0x38, 2, 8, 1)],
    "HBURST_END": [(0x7E, 0, 0, 8), (0x38, 3, 8, 1)],
    "H_BLANKO": [(0x80, 0, 0, 8), (0x9A, 6, 8, 2)],
    "V_BLANKO": [(0x82, 0, 0, 8)],
    "V_ACTIVEO": [(0x84, 0, 0, 8), (0x86, 7, 8, 1)],
    "H_FRACT": [(0x88, 0, 0, 8)],
    "H_CLKI": [(0x8A, 0, 0, 8), (0x8E, 0, 8, 3)],
    "H_BLANKI": [(0x8C, 0, 0, 8), (0x8E, 3, 8, 1), (0x38, 0, 9, 1)],
    "V_LINESI": [(0x90, 0, 0, 8), (0x96, 0, 8, 2), (0x38, 1, 10, 1)],
    "V_BLANKI": [(0x92, 0, 0, 8)],
    "V_ACTIVEI": [(0x94, 0, 0, 8), (0x96, 2, 8, 2)],
    "V_SCALE": [(0x98, 0, 0, 8), (0x9A, 0, 8, 6)],
    "PLL_FRACT": [(0x9C, 0, 0, 8), (0x9E, 0, 8, 8)],
    "PLL_INT": [(0xA0, 0, 0, 6)],
    "NI_OUT": [(0xA2, 0, 0, 1)], "SETUP": [(0xA2, 1, 0, 1)], "625LINE": [(0xA2, 2, 0, 1)],
    "VSYNC_DUR": [(0xA2, 3, 0, 1)], "PAL_MD": [(0xA2, 5, 0, 1)],
    "SYNC_AMP": [(0xA4, 0, 0, 8)], "BST_AMP": [(0xA6, 0, 0, 8)],
    "MCR": [(0xA8, 0, 0, 8)], "MCB": [(0xAA, 0, 0, 8)], "MY": [(0xAC, 0, 0, 8)],
    "MSC": [(0xAE, 0, 0, 8), (0xB0, 0, 8, 8), (0xB2, 0, 16, 8), (0xB4, 0, 24, 8)],
    # interface and output
    "DRVS": [(0x32, 5, 0, 2)], "IN_MODE3": [(0x32, 3, 0, 1)],
    "FLD_MODE": [(0x6C, 0, 0, 2)], "EACTIVE": [(0x6C, 2, 0, 1)], "EN_SCART": [(0x6C, 3, 0, 1)],
    "EN_REG_RD": [(0x6C, 6, 0, 1)],
    "IN_MODE": [(0xC6, 0, 0, 3)], "HSYNCI": [(0xC6, 3, 0, 1)], "VSYNCI": [(0xC6, 4, 0, 1)],
    "EN_BLANKO": [(0xC6, 7, 0, 1)],
    "OUT_MUXA": [(0xCE, 0, 0, 2)], "OUT_MUXB": [(0xCE, 2, 0, 2)], "OUT_MUXC": [(0xCE, 4, 0, 2)],
    "OUT_MUXD": [(0xCE, 6, 0, 2)], "OUT_MODE": [(0xD6, 2, 0, 2)],
    # HDTV output (Appendix E, Table E-6)
    "RASTER_SEL": [(0x2E, 0, 0, 2)], "HD_SYNC_EDGE": [(0x2E, 2, 0, 1)], "BPB_SYNC_DIS": [(0x2E, 3, 0, 1)],
    "GY_SYNC_DIS": [(0x2E, 4, 0, 1)], "RPR_SYNC_DIS": [(0x2E, 5, 0, 1)], "RGB2PRPB": [(0x2E, 6, 0, 1)],
    "HDTV_EN": [(0x2E, 7, 0, 1)], "CSC_SEL": [(0x32, 0, 0, 1)],
    "MCOMPY": [(0x3C, 0, 0, 8)], "MCOMPU": [(0x3E, 0, 0, 8)], "MCOMPV": [(0x40, 0, 0, 8)],
    "EN_XCLK": [(0xA0, 7, 0, 1)],
}


def pack(fields):
    """Field values -> {register: byte}; a register is only produced if one of its fields is set."""
    regs = {}
    for name, value in fields.items():
        for reg, rbit, fbit, width in FIELDS[name]:
            part = (value >> fbit) & ((1 << width) - 1)
            regs[reg] = (regs.get(reg, 0) & ~(((1 << width) - 1) << rbit)) | (part << rbit)
    return regs


def unpack(regs, name):
    v = 0
    for reg, rbit, fbit, width in FIELDS[name]:
        v |= ((regs.get(reg, 0) >> rbit) & ((1 << width) - 1)) << fbit
    return v


def sinx(fsc, fclk):
    x = math.pi * fsc / fclk
    return math.sin(x) / x


def h_blanki(std, xres, h_clki, h_clko, h_blanko, v_linesi):
    """Input back porch (H_BLANKI): place the input blanking so the 1024-pixel FIFO neither over- nor underflows
    (nvtv calc_bt.c / Cromwell conexant_calc_blankings, GPL; only the no-drift case is needed when V_SCALE is exact)."""
    MFP, MBP, FIFO = 14, 4, 1024
    vsr = v_linesi / std["tlo"]
    vsrq = int(vsr * 4096 + 0.5) / 4096
    total_in = h_clki - xres
    front_in = max(MFP, min(total_in / 8, total_in - MBP))
    back_in = total_in - front_in
    drift = (vsrq - vsr) * std["tlo"] * h_clki
    total_out = (h_clko - xres * 2) / h_clko * vsr * h_clki
    back_out = h_blanko / h_clko * vsr * h_clki
    front_out = total_out - back_out
    margin = (FIFO - xres) * 2 / h_clko * vsr * h_clki
    head = 32.0
    if total_out + margin >= total_in + abs(drift):
        maxhr = (total_out + margin) - (total_in - abs(drift))
        if maxhr < head * 2:
            head = int(maxhr / 2)
        if (front_out + margin) - head < front_in - min(drift, 0):
            front_in = max(MFP, front_out + margin + min(drift, 0) - head)
            back_in = total_in - front_in
        if back_out - head < back_in + max(drift, 0):
            back_in = max(MBP, back_out - max(drift, 0) - head)
    elif drift < 0:
        back_in = min(back_out - 1, total_in - MFP)
    else:
        front_in = min(front_out + margin - 1, total_in - MBP)
        back_in = total_in - front_in
    return int(back_in)


def conexant_mode(std_name, xres, yres, h_clko, h_clki, v_linesi):
    """All timing, clock and colour fields for one SD mode (Table 2-5 definitions, Appendix A relations)."""
    s = STANDARDS[std_name]
    fclk = h_clko / s["tto"]                       # CLKO: H_CLKO clocks per output line
    vsr = v_linesi / s["tlo"]                      # input lines per output line (1.3.17)
    v_activeo = -(-yres * s["tlo"] // v_linesi) + 2  # active output lines per field, plus the two filtered lines
    v_activeo = int(v_activeo)
    h_blanko = 2 * int(s["centre"] / (2 * s["tto"]) * h_clko + 0.5) - xres + PIPELINE_HBLANKO
    v_blanko = int(s["centre_line"] - v_activeo / 2 + 0.5)
    v_blanki = int((v_blanko - 1) * vsr)
    ratio = 6 * fclk / FXTAL
    pll = int(ratio * 65536 + 0.5)
    sx = sinx(s["fsc"], fclk)
    burst = s["burst_v"] / 2 / (math.sqrt(2) if s["pal"] else 1)
    f = {
        "H_CLKO": h_clko, "H_ACTIVE": xres, "V_ACTIVEI": yres, "H_CLKI": h_clki, "V_LINESI": v_linesi,
        "HSYNC_WIDTH": int(s["hsync"] * fclk),  # truncated, as all Appendix C modes
        "HBURST_BEGIN": int(s["burst_start"] * fclk + 0.5),
        "HBURST_END": int(s["burst_end"] * fclk + 0.5) - 128,
        "H_BLANKO": h_blanko, "V_BLANKO": v_blanko, "V_ACTIVEO": v_activeo, "H_FRACT": 0,
        "H_BLANKI": h_blanki(s, xres, h_clki, h_clko, h_blanko, v_linesi), "V_BLANKI": v_blanki,
        "V_SCALE": int((vsr - 1) * 4096 + 0.5),
        "PLL_INT": pll >> 16, "PLL_FRACT": pll & 0xFFFF,
        "NI_OUT": 0, "SETUP": int(s["setup"]), "625LINE": int(s["lines"] == 625),
        "VSYNC_DUR": int(s["lines"] == 525), "PAL_MD": int(s["pal"]),
        "SYNC_AMP": int(s["sync_v"] / LSB_MV + 0.5),
        "BST_AMP": int(burst / LSB_MV / sx + 0.5),
        "MY": int(s["v100"] / (255 * VFS) * 65536 + 0.5),
        "MCR": int((128 / 127) * s["v100"] * 0.877 / (127 * VFS * sx) * 32768 + 0.5),
        "MCB": int((128 / 127) * s["v100"] * 0.493 / (127 * VFS * sx) * 32768 + 0.5),
        "MSC": int(s["fsc_per_line"] / h_clko * 2 ** 32 + 0.5),
    }
    return f, fclk


# Interface and outputs on the Xbox board (pseudo-master: the encoder clocks the NV2A, the NV2A sends syncs).
# Values as in Cromwell's conexant.c (GPL), which drives retail Xboxes: 1.8 V pads, alternate 24-bit RGB
# multiplexed input, active-high syncs, no BLANK* input, FLD_MODE 10, register read-back on.
INTERFACE = {"DRVS": 1, "IN_MODE3": 1, "IN_MODE": 0, "HSYNCI": 1, "VSYNCI": 1, "EN_BLANKO": 1,
             "FLD_MODE": 2, "EACTIVE": 1, "EN_REG_RD": 1}
OUTPUTS = {  # DAC routing on the AV connector (Cromwell conexant.c, GPL)
    "composite": {"EN_SCART": 0, "OUT_MODE": 0, "OUT_MUXA": 1, "OUT_MUXB": 2, "OUT_MUXC": 1, "OUT_MUXD": 0},
    "scart": {"EN_SCART": 1, "OUT_MODE": 3, "OUT_MUXA": 1, "OUT_MUXB": 0, "OUT_MUXC": 2, "OUT_MUXD": 3},
}


def our_timing(std_name, xres):
    """OXOS's picture size (tv-encoders.md §3.3): no vertical scaling (V_LINESI = 2 x TLO: 480 lines interlaced,
    centred, black bars on 625 lines), and the input width across the full nominal active line: the smallest even
    H_CLKO whose active area is not wider than ATO (HOC >= 0)."""
    s = STANDARDS[std_name]
    v_linesi = int(2 * s["tlo"])
    h_clko = math.ceil(2 * xres / (s["ato"] / s["tto"]))
    h_clko += h_clko & 1                       # V_LINESI * H_CLKI = TLO * H_CLKO with V_LINESI = 2 TLO
    return h_clko, h_clko // 2, v_linesi


# The SD encoder rows of the mode words titles send (nxdk lib/hal/video.c, stock spec 17 §7a).
SD_ROWS = [
    (0x01, "NTSC-M", 640), (0x02, "NTSC-M", 720), (0x03, "NTSC-J", 640), (0x04, "NTSC-J", 720),
    (0x07, "PAL-BDGHI", 640), (0x08, "PAL-BDGHI", 720), (0x0B, "PAL-60", 640), (0x0C, "PAL-60", 720),
]


def nv2a_timing(xres, yres, f):
    """NV2A flat-panel timing generator for the encoder's input (PRAMDAC 0x680800-0x680838; Cromwell
    VideoInitialization.c): totals = the encoder's H_CLKI / V_LINESI, syncs placed so the first active pixel arrives
    H_BLANKI clocks after HSYNC (the -7 / +1 are the NV2A pipeline offsets Cromwell uses on hardware)."""
    hs = f["H_CLKI"] - f["H_BLANKI"] - 7
    vs = f["V_LINESI"] - f["V_BLANKI"] + 1
    return dict(htotal=f["H_CLKI"], hsync_start=hs, hsync_end=hs + 32, vtotal=f["V_LINESI"], vsync_start=vs,
                vsync_end=vs + 3, xres=xres, yres=yres)


# --- self-check against the datasheet's autoconfiguration modes (Appendix C, Table C-1/C-2: vendor values)
APPENDIX_C = {
    # mode: (standard, xres, yres, H_CLKO, H_CLKI, V_LINESI, {register: value})
    0: ("NTSC-M", 640, 480, 1792, 784, 600, {0x38: 0x00, 0x78: 0x80, 0x7E: 0x60, 0x7A: 0x84, 0x7C: 0x96, 0x80: 0x7D, 0x82: 0x22, 0x84: 0xD4,
        0x86: 0x27, 0x8A: 0x10, 0x8C: 0x7E, 0x8E: 0x03, 0x90: 0x58, 0x92: 0x4B, 0x94: 0xE0, 0x96: 0x36,
        0x98: 0x92, 0x9A: 0x54, 0x9C: 0x0E, 0x9E: 0x88, 0xA0: 0x0C, 0xB0: 0x00, 0xB2: 0x80, 0xB4: 0x20}),
    1: ("PAL-BDGHI", 640, 480, 0x760, 944, 625, {0x38: 0x00, 0x78: 0x80, 0x7E: 0x68, 0x7A: 0x8A, 0x7C: 0xA6, 0x80: 0xC1, 0x82: 0x2E, 0x84: 0xF2,
        0x86: 0x27, 0x8A: 0xB0, 0x8C: 0x0A, 0x8E: 0x0B, 0x90: 0x71, 0x92: 0x5A, 0x94: 0xE0, 0x96: 0x36,
        0x98: 0x00, 0x9A: 0x50, 0x9C: 0x72, 0x9E: 0x1C, 0xA0: 0x0D, 0xB0: 0x8C, 0xB2: 0x79, 0xB4: 0x26}),
    8: ("NTSC-M", 640, 400, 0x750, 936, 525, {0x38: 0x00, 0x78: 0x80, 0x7E: 0x6A, 0x7A: 0x8A, 0x7C: 0x9C, 0x80: 0xA9, 0x82: 0x27, 0x84: 0xCA,
        0x86: 0x27, 0x8A: 0xA8, 0x8C: 0x03, 0x8E: 0x0B, 0x90: 0x0D, 0x92: 0x4C, 0x94: 0x90, 0x96: 0x36,
        0x98: 0x00, 0x9A: 0x50, 0x9C: 0x46, 0x9E: 0x17, 0xA0: 0x0D, 0xA4: 0xE5, 0xA6: 0x75, 0xA8: 0x79,
        0xAA: 0x44, 0xAC: 0x85}),
    9: ("PAL-BDGHI", 640, 400, 0x740, 1160, 500, {0x38: 0x00, 0x78: 0x80, 0x7E: 0x64, 0x7A: 0x88, 0x7C: 0xA2, 0x80: 0xAF, 0x82: 0x29, 0x84: 0xFC,
        0x86: 0x27, 0x8A: 0x88, 0x8C: 0x6B, 0x8E: 0x0C, 0x90: 0xF4, 0x92: 0x40, 0x94: 0x90, 0x96: 0x35,
        0x98: 0x9A, 0x9A: 0x49, 0x9C: 0x8E, 0x9E: 0xE3, 0xA0: 0x0C, 0xA4: 0xF0, 0xA6: 0x58, 0xA8: 0x82,
        0xAA: 0x49, 0xAC: 0x8C}),
}
# Allowed differences (in field units) where the vendor's tool used other rounding than the datasheet formulas.
TOLERANCE = {"MY": 1, "MCR": 1, "MCB": 1, "BST_AMP": 1, "H_BLANKI": 2, "PLL_FRACT": 8, "HBURST_BEGIN": 1,
             "HBURST_END": 1}


def check():
    ok = True
    for mode, (std, xres, yres, h_clko, h_clki, v_linesi, ref) in APPENDIX_C.items():
        mode_ok = True
        f, fclk = conexant_mode(std, xres, yres, h_clko, h_clki, v_linesi)
        ours = pack(f)
        for name in FIELDS:
            regs = {r for r, *_ in FIELDS[name]}
            if not regs <= set(ref) or name not in f:
                continue
            want, got = unpack(ref, name), f[name]
            if abs(want - got) > TOLERANCE.get(name, 0):
                print(f"  appendix mode {mode}: {name} = {got} (0x{got:X}), datasheet {want} (0x{want:X})")
                ok = mode_ok = False
        print(f"appendix C mode {mode} ({std} {xres}x{yres}, CLK {fclk / 1e6:.4f} MHz): "
              f"{'matches' if mode_ok else 'DIFFERS'}")
    ok &= check_hd()
    for row, std, xres in SD_ROWS:
        h_clko, h_clki, v_linesi = our_timing(std, xres)
        f, fclk = conexant_mode(std, xres, 480, h_clko, h_clki, v_linesi)
        s = STANDARDS[std]
        hoc = 1 - (xres / (h_clko / 2)) / (s["ato"] / s["tto"])
        line = h_clko / fclk
        fsc = f["MSC"] / 2 ** 32 * fclk
        nv = nv2a_timing(xres, 480, f)
        print(f"row 0x{row:02X} {std:9} {xres}x480: H_CLKO {h_clko} H_CLKI {h_clki} V_LINESI {v_linesi} "
              f"CLK {fclk / 1e6:.4f} MHz HOC {hoc * 100:.2f}% Fsc {fsc:.1f} Hz V_ACTIVEO {f['V_ACTIVEO']} "
              f"H_BLANKI {f['H_BLANKI']} NV hsync {nv['hsync_start']} vsync {nv['vsync_start']}")
        if abs(fsc - s["fsc"]) > 10:
            print("   subcarrier off by more than 10 Hz")
            ok = False
    return ok


# --- HD component (Appendix E): the encoder passes the NV2A's raster through and adds the sync; the GPU must send the
# standard timing (SMPTE 293M / 296M / 274M) at the encoder's clock.
HD_FORMATS = {  # HD row (mode word bits 0-6 in the HD class) -> settings (Table E-1, YPbPr out)
    1: dict(name="480p", raster=1, edge=1, csc=0, mcompu=0x48, mcompv=0x5B, clk=27.0e6),
    2: dict(name="720p", raster=2, edge=0, csc=1, mcompu=0x45, mcompv=0x51, clk=74.25e6),
    3: dict(name="1080i", raster=3, edge=0, csc=1, mcompu=0x45, mcompv=0x51, clk=74.25e6),
}
# Table E-1 itself (datasheet columns 1080i / 720p / 480p), for the self-check; registers our HD set writes.
TABLE_E1 = {3: {0xD6: 0x0C, 0x2E: 0xC3, 0x32: 0x01, 0x3C: 0x80, 0x3E: 0x45, 0x40: 0x51, 0xCE: 0x24, 0xA0: 0x21,
                0x9E: 0x00, 0x9C: 0x00},
            2: {0xD6: 0x0C, 0x2E: 0xC2, 0x32: 0x01, 0x3C: 0x80, 0x3E: 0x45, 0x40: 0x51, 0xCE: 0x24, 0xA0: 0x21,
                0x9E: 0x00, 0x9C: 0x00},
            1: {0xD6: 0x0C, 0x2E: 0xC5, 0x32: 0x00, 0x3C: 0x80, 0x3E: 0x48, 0x40: 0x5B, 0xCE: 0x24, 0xA0: 0x8C,
                0x9E: 0x00, 0x9C: 0x00}}
# Where ours differs from Table E-1, and why:
#  0x2E: + RPR_SYNC_DIS and BPB_SYNC_DIS, the table's own note for EIA-770.3 compliance (consumer YPbPr inputs);
#  0x32: + DRVS = 01 and IN_MODE[3] = 1, the Xbox board's pads and alternate RGB input (as for SD);
#  0xA0 (480p): EN_XCLK clear: the table's 8C sets it, which selects CLKI as the clock source and contradicts the
#        pseudo-master interface the same table states (CLKO generated by the PLL, 13.5 MHz x 12 / 6 = 27 MHz).
HD_DEVIATIONS = {0x2E: 0x28, 0x32: 0x28, 0xA0: 0x80}


def conexant_hd(row):
    h = HD_FORMATS[row]
    pll = int(6 * h["clk"] / FXTAL * 65536 + 0.5)
    f = {"HDTV_EN": 1, "RGB2PRPB": 1, "RPR_SYNC_DIS": 1, "BPB_SYNC_DIS": 1, "RASTER_SEL": h["raster"],
         "HD_SYNC_EDGE": h["edge"], "CSC_SEL": h["csc"], "DRVS": 1, "IN_MODE3": 1,
         "MCOMPY": 0x80, "MCOMPU": h["mcompu"], "MCOMPV": h["mcompv"], "EN_XCLK": 0,
         "PLL_INT": pll >> 16, "PLL_FRACT": pll & 0xFFFF,
         "IN_MODE": 0, "HSYNCI": 1, "VSYNCI": 1, "EN_BLANKO": 1, "EACTIVE": 1, "EN_REG_RD": 1, "FLD_MODE": 2,
         "OUT_MODE": 3, "OUT_MUXA": 0, "OUT_MUXB": 1, "OUT_MUXC": 2, "OUT_MUXD": 0}
    return pack(f)


def check_hd():
    ok = True
    for row, ref in TABLE_E1.items():
        ours = conexant_hd(row)
        for reg, want in ref.items():
            got = ours.get(reg, 0)
            if (got ^ want) & ~HD_DEVIATIONS.get(reg, 0):
                print(f"  HD {HD_FORMATS[row]['name']}: register 0x{reg:02X} = 0x{got:02X}, Table E-1 0x{want:02X}")
                ok = False
        print(f"Table E-1 {HD_FORMATS[row]['name']}: {'matches (with the documented deviations)' if ok else 'DIFFERS'}")
    return ok


OUTPUT_REGS = (0x6C, 0xCE, 0xD6)  # the registers whose value depends on the output (composite/S-video or SCART RGB)


def emit(path):
    rows = []
    for row, std, xres in SD_ROWS:
        f, fclk = conexant_mode(std, xres, 480, *our_timing(std, xres))
        regs = pack({**f, **INTERFACE, **OUTPUTS["composite"]})
        rows.append((row, std, xres, fclk, f, regs, nv2a_timing(xres, 480, f)))
    reglist = sorted(r for r in rows[0][5] if r not in OUTPUT_REGS)
    outputs = {name: pack({**INTERFACE, **o}) for name, o in OUTPUTS.items()}
    out = ["// Generated by tools/gen_tv_encoders.py from the CX25870/871 datasheet and the video standards",
           "// (spec/oxos-internal/tv-encoders.md). Do not edit; regenerate.", "",
           "// Conexant registers written for every SD mode, in this order, after a soft reset.",
           f"static const UCHAR AvpCxRegs[{len(reglist)}] = {{" + ", ".join(f"0x{r:02X}" for r in reglist) + "};",
           "", "// Output-dependent registers 0x6C, 0xCE, 0xD6: [0] composite + S-video, [1] SCART RGB (mode bit 29).",
           "static const UCHAR AvpCxOutputRegs[2][3] = {"]
    for name in ("composite", "scart"):
        out.append("    {" + ", ".join(f"0x{outputs[name][r]:02X}" for r in OUTPUT_REGS) + f"}}, // {name}")
    out += ["};", "", "struct AVP_SD_MODE {",
            "    UCHAR EncoderRow;              // mode word bits 0-6",
            "    USHORT Width, Height;",
            "    USHORT HTotal, HSyncStart, VTotal, VSyncStart; // NV2A flat-panel timing (pixels / lines)",
            f"    UCHAR Cx[{len(reglist)}];                 // values for AvpCxRegs",
            "};", "", "static const AVP_SD_MODE AvpSdModes[] = {"]
    for row, std, xres, fclk, f, regs, nv in rows:
        out.append(f"    // {std} {xres}x480: CLK {fclk / 1e6:.4f} MHz, H_CLKO {f['H_CLKO']}, H_CLKI {f['H_CLKI']}, "
                   f"V_LINESI {f['V_LINESI']}")
        vals = ", ".join(f"0x{regs[r]:02X}" for r in reglist)
        out.append(f"    {{0x{row:02X}, {xres}, 480, {nv['htotal']}, {nv['hsync_start']}, {nv['vtotal']}, "
                   f"{nv['vsync_start']}, {{{vals}}}}},")
    out.append("};")
    hd = {row: conexant_hd(row) for row in HD_FORMATS}
    hdregs = sorted(hd[1])
    out += ["", "// HD component (Table E-1): registers written after a soft reset, then EN_OUT, 75 ms, TIMING_RST.",
            f"static const UCHAR AvpCxHdRegs[{len(hdregs)}] = {{" + ", ".join(f"0x{r:02X}" for r in hdregs) + "};",
            f"static const UCHAR AvpCxHdValues[3][{len(hdregs)}] = {{ // HD rows 1 (480p), 2 (720p), 3 (1080i)"]
    for row in sorted(hd):
        out.append("    {" + ", ".join(f"0x{hd[row][r]:02X}" for r in hdregs) + f"}}, // {HD_FORMATS[row]['name']}")
    out.append("};")
    open(path, "w").write("\n".join(out) + "\n")
    print(f"{path}: {len(rows)} SD modes, {len(reglist)} + {len(OUTPUT_REGS)} Conexant registers each")


if __name__ == "__main__":
    if "--out" in sys.argv:
        if not check():
            sys.exit("self-check failed; nothing written")
        emit(sys.argv[sys.argv.index("--out") + 1])
        sys.exit(0)
    if "--check" in sys.argv:
        sys.exit(0 if check() else 1)
    print(__doc__)
