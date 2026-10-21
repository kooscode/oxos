#!/usr/bin/env python3
"""Generate the kernel's video encoder tables (kernel/src/hal/av_modes.inc) from the encoder specs.

The register maps are read from spec/stock/18-conexant-encoder.md, 19-focus-encoder.md and 20-xcalibur-encoder.md
(the "Video Encoder Mode" blocks); nothing else is an input. For each encoder the NV2A timing of the standard-definition
rows is derived from the encoder's own settings (spec/oxos-internal/02-display-modes.md §1):
  Conexant: input clocks per line H_CLKI, input lines V_LINESI, input blanking H_BLANKI / V_BLANKI, H_ACTIVE.
  Focus:    pixel clock from PLL and NCO, input lines from VSC, sync positions from IHO / IVO, width IHW.
  Xcalibur: input totals and active starts from registers 0x54-0x56.

usage: gen_av_tables.py --out FILE      write the tables
       gen_av_tables.py --check FILE    exit 1 if FILE is not what --out would write
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "spec", "stock")
CX_SPEC, FO_SPEC, XC_SPEC = "18-conexant-encoder.md", "19-focus-encoder.md", "20-xcalibur-encoder.md"

PLAIN = re.compile(r"^- Register (0x[0-9A-F]+): Write Value (0x[0-9A-F]+) -> (.*)$")
RMW = re.compile(r"^- Register (0x[0-9A-F]+): Clear Mask (0x[0-9A-F]+), Write Value (0x[0-9A-F]+) -> ")
BANK = re.compile(r"^- Registers (0x[0-9A-F]+)–(0x[0-9A-F]+) \(([^;]+); table entry (0x[0-9A-F]+), index (\d+)\): "
                  r"Write Values (.*?) -> (.*)$")
SECTION = re.compile(r"^#{2,4} (\d+(?:\.\d+)*) ")
MODE = re.compile(r"^#{3,5} Video Encoder Mode: (.*)$")


def blocks(name):
    """The spec's register maps: [{title, section, writes}] in file order. A write is ("w", reg, value, late),
    ("rmw", reg, mask, value) or ("bank", condition, [(reg, value)], late); late = rewritten by option 0x0E."""
    out, cur, section = [], None, None
    for line in open(os.path.join(SPEC, name), encoding="utf-8"):
        line = line.rstrip("\n")
        m = MODE.match(line)
        if m:
            cur = {"title": m.group(1), "section": section, "writes": []}
            out.append(cur)
            continue
        m = SECTION.match(line)
        if m:
            section, cur = m.group(1), None
            continue
        if cur is None:
            continue
        if line.startswith("Used by: "):
            cur["used"] = line
        m = BANK.match(line)
        if m:
            vals = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"(0x[0-9A-F]+)=(0x[0-9A-F]+)", m.group(6))]
            cur["writes"].append(("bank", m.group(3), vals, "column ≥ 24" in m.group(7)))
            continue
        m = RMW.match(line)
        if m:
            cur["writes"].append(("rmw", int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)))
            continue
        m = PLAIN.match(line)
        if m:
            cur["writes"].append(("w", int(m.group(1), 16), int(m.group(2), 16), "column ≥ 24" in m.group(3)))
    return out


def row_of(title):
    m = re.search(r"\brow (0x[0-9A-F]{2})\b", title)
    return int(m.group(1), 16) if m else None


def picture(title):
    """(width, height, 625-line) of an SD mode from its name, e.g. "NTSC-M 480i, 640x480 input (...)"."""
    m = re.search(r" (480i|576i), (\d+)x(\d+) input", title)
    return (int(m.group(2)), int(m.group(3)), m.group(1) == "576i") if m else None


def section(bl, sec):
    return [b for b in bl if b["section"] == sec]


def table(bl, sec, nrows):
    """Register list and value rows [0..nrows) (missing rows are zeros) of one table; every block must have the
    same register list."""
    rows = section(bl, sec)
    regs = [w[1] for w in rows[0]["writes"]]
    vals = [[0] * len(regs) for _ in range(nrows)]
    present = [False] * nrows
    for b in rows:
        assert [w[1] for w in b["writes"]] == regs, (sec, b["title"])
        r = row_of(b["title"])
        vals[r] = [w[2] for w in b["writes"]]
        present[r] = True
    return regs, vals, present


def last_values(regs, vals):
    d = {}
    for r, v in zip(regs, vals):
        d[r] = v
    return d


# ---- Conexant (spec 18 §5) -------------------------------------------------------------------------------------
def cx_field(d, parts):
    v = 0
    for reg, rbit, fbit, width in parts:
        v |= ((d.get(reg, 0) >> rbit) & ((1 << width) - 1)) << fbit
    return v


CX_FIELDS = {  # datasheet Table 2-1: field -> [(register, register bit, field bit, width)]
    "H_ACTIVE": [(0x78, 0, 0, 8), (0x86, 4, 8, 3)],
    "H_CLKI": [(0x8A, 0, 0, 8), (0x8E, 0, 8, 3)],
    "H_BLANKI": [(0x8C, 0, 0, 8), (0x8E, 3, 8, 1), (0x38, 0, 9, 1)],
    "V_LINESI": [(0x90, 0, 0, 8), (0x96, 0, 8, 2), (0x38, 1, 10, 1)],
    "V_BLANKI": [(0x92, 0, 0, 8)],
    "V_ACTIVEI": [(0x94, 0, 0, 8), (0x96, 2, 8, 2)],
}


def cx_timing(common, regs, vals):
    """NV2A input timing for a Conexant SD row: totals = H_CLKI x V_LINESI, the first active pixel H_BLANKI clocks
    after HSYNC and the first active line V_BLANKI lines after VSYNC (the -7 / +1 are the NV2A pipeline offsets
    Cromwell uses on hardware), the active width H_ACTIVE (the framebuffer width is H_ACTIVE rounded down to 16)."""
    d = {**common, **last_values(regs, vals)}
    f = {k: cx_field(d, p) for k, p in CX_FIELDS.items()}
    if not f["H_CLKI"]:
        return None
    hs, vs = f["H_CLKI"] - f["H_BLANKI"] - 7, f["V_LINESI"] - f["V_BLANKI"] + 1
    return dict(width=f["H_ACTIVE"] - f["H_ACTIVE"] % 16, active=f["H_ACTIVE"], height=f["V_ACTIVEI"],
                htotal=f["H_CLKI"], hsync=hs, vtotal=f["V_LINESI"], vsync=vs)


# ---- Focus (spec 19 §5.2, the 5838 set) ------------------------------------------------------------------------
FO_XTAL = 27e6


def fo_timing(common, regs, vals, standard_lines, field_rate, height):
    """NV2A input timing for a Focus SD row (hardware reference §4): the pixel clock is the PLL (27 MHz / N x M / P,
    PLL_M = M - 17, PLL_N = N - 1, PLL_PDIV = P - 1) times NCON / NCOD when the NCO is used; the input frame has the
    TV's field rate, its line count follows from VSC (0: 1:1; below 0x8000: upscaling, VSC = (TV / GCC - 1) x 65536;
    else downscaling, VSC = TV / GCC x 65536), and the line length from the clock; HSYNC is IHO pixels and VSYNC IVO
    lines before the end of the line / frame."""
    d = {**common, **last_values(regs, vals)}
    w16 = lambda a: d.get(a, 0) | d.get(a + 1, 0) << 8
    w32 = lambda a: w16(a) | w16(a + 2) << 16
    m, n, p = (w16(0x18) & 0x7FF) + 17, (w16(0x1A) & 0x3FF) + 1, (w16(0x1C) & 0x0F) + 1
    clk = FO_XTAL / n * m / p
    ncon, ncod = w32(0x10), w32(0x14)
    if ncon and ncod:
        clk = clk * ncon / ncod
    vsc = w16(0x06)
    if vsc == 0:
        vtotal = standard_lines
    elif vsc < 0x8000:
        vtotal = standard_lines / (1 + vsc / 65536)
    else:
        vtotal = standard_lines / (vsc / 65536)
    vtotal = round(vtotal)
    htotal = round(clk / field_rate / vtotal)
    return dict(width=w16(0x04), active=w16(0x04), height=height, htotal=htotal, hsync=htotal - w16(0x00),
                vtotal=vtotal, vsync=vtotal - w16(0x02), clock=clk)


# ---- Xcalibur (spec 20 §6) -------------------------------------------------------------------------------------
def xc_variant(cond):
    if cond == "always":
        return 0xFF
    m = re.fullmatch(r"only when v == (\d)", cond)
    if m:
        return int(m.group(1))
    m = re.fullmatch(r"only when \(v & 1\) == (\d)", cond)
    assert m, cond
    return 0x10 | int(m.group(1))


def xc_timing(writes):
    """NV2A input timing for an Xcalibur row: register 0x54 = input total pixels (bits 16-27) and lines (0-11),
    0x55 bits 0-7 = lines from VSYNC to the first active line, bits 20-31 the active lines (bit 20 cleared),
    0x56 bits 0-11 = pixels from HSYNC to the first active pixel, bits 20-31 the active width (bit 20 cleared)."""
    d = {w[1]: w[2] for w in writes if w[0] == "w"}
    r54, r55, r56 = d[0x54], d[0x55], d[0x56]
    htotal, vtotal = (r54 >> 16) & 0xFFF, r54 & 0xFFF
    width, height = (r56 >> 20) & 0xFFE, (r55 >> 20) & 0xFFE
    return dict(width=width, active=width, height=height, htotal=htotal, hsync=htotal - (r56 & 0xFFF),
                vtotal=vtotal, vsync=vtotal - (r55 & 0xFF))


def c_bytes(vals):
    return "{" + ", ".join(f"0x{v:02X}" for v in vals) + "}"


def timing_row(t):
    if not t:
        return "{0, 0, 0, 0, 0, 0, 0}"
    return f"{{{t['width']}, {t['active']}, {t['height']}, {t['htotal']}, {t['hsync']}, {t['vtotal']}, {t['vsync']}}}"


def generate():
    out = ["// Generated by tools/gen_av_tables.py from spec/stock/18-conexant-encoder.md, 19-focus-encoder.md and",
           "// 20-xcalibur-encoder.md (the encoder register maps). Do not edit; regenerate.", "",
           "// NV2A input timing of an SD row (spec/oxos-internal/02-display-modes.md §1): framebuffer width, active",
           "// width the encoder takes, height, totals and sync starts (pixels / lines).",
           "struct AVP_TV_TIMING { USHORT Width, Active, Height, HTotal, HSyncStart, VTotal, VSyncStart; };", ""]

    # Conexant
    cx = blocks(CX_SPEC)
    common = section(cx, "5.1")[0]["writes"]
    out += ["// ---- Conexant CX25871 (spec/stock/18 §5)",
            f"static const UCHAR AvpCxCommonRegs[{len(common)}] = {c_bytes([w[1] for w in common])};",
            f"static const UCHAR AvpCxCommon[{len(common)}] = {c_bytes([w[2] for w in common])};"]
    cd = last_values([w[1] for w in common], [w[2] for w in common])
    regs, vals, present = table(cx, "5.2", 0x1E)
    out += [f"static const UCHAR AvpCxSdRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpCxSd[0x1E][{len(regs)}] = {{ // SD rows 0x00 (unused) - 0x1D"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};"]
    timings = [cx_timing(cd, regs, v) if present[i] else None for i, v in enumerate(vals)]
    out += ["static const AVP_TV_TIMING AvpCxSdTiming[0x1E] = {"] + [f"    {timing_row(t)}," for t in timings] + ["};"]
    regs, vals, _ = table(cx, "5.3", 4)
    out += [f"static const UCHAR AvpCxHdRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpCxHd[4][{len(regs)}] = {{ // HD rows 0 (unused), 1 480p, 2 720p, 3 1080i"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};"]
    regs, vals, present = table(cx, "5.4", 0x1E)
    out += [f"static const UCHAR AvpCxScartRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpCxScartValid[0x1E] = {c_bytes([int(p) for p in present])};",
            f"static const UCHAR AvpCxScart[0x1E][{len(regs)}] = {{"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};"]
    regs, vals, present = table(cx, "5.5", 0x14)
    out += [f"static const UCHAR AvpCxCaptionRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpCxCaption[0x14][{len(regs)}] = {{ // rows 0x01-0x13"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};", ""]

    # Focus: the 5838 set (spec 19 §5.2)
    fo = blocks(FO_SPEC)
    common = section(fo, "5.2.1")[0]["writes"]
    out += ["// ---- Focus FS453/4 (spec/stock/19 §5.2: the 5838 tables; byte writes)",
            f"static const UCHAR AvpFoCommonRegs[{len(common)}] = {c_bytes([w[1] for w in common])};",
            f"static const UCHAR AvpFoCommon[{len(common)}] = {c_bytes([w[2] for w in common])};"]
    cd = last_values([w[1] for w in common], [w[2] for w in common])
    regs, vals, present = table(fo, "5.2.2", 0x1E)
    out += [f"static const UCHAR AvpFoSdRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpFoSd[0x1E][{len(regs)}] = {{"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};"]
    sd_rows = {row_of(b["title"]): b["title"] for b in section(fo, "5.2.2")}
    timings = []
    for i, v in enumerate(vals):
        if not present[i]:
            timings.append(None)
            continue
        width, height, l625 = picture(sd_rows[i])
        lines, rate = (625, 50.0) if l625 else (525, 60000 / 1001)
        t = fo_timing(cd, regs, v, lines, rate, height)
        t["width"] = width
        timings.append(t)
    out += ["static const AVP_TV_TIMING AvpFoSdTiming[0x1E] = {"] + [f"    {timing_row(t)}," for t in timings] + ["};"]
    regs, vals, _ = table(fo, "5.2.3", 4)
    out += [f"static const UCHAR AvpFoHdRegs[{len(regs)}] = {c_bytes(regs)};",
            f"static const UCHAR AvpFoHd[4][{len(regs)}] = {{"]
    out += [f"    {c_bytes(v)}," for v in vals] + ["};"]
    scart = section(fo, "5.2.4")[0]["writes"]
    out += [f"static const UCHAR AvpFoScartRegs[{len(scart)}] = {c_bytes([w[1] for w in scart])};",
            f"static const UCHAR AvpFoScartMask[{len(scart)}] = {c_bytes([w[2] for w in scart])};",
            f"static const UCHAR AvpFoScartValue[{len(scart)}] = {c_bytes([w[3] for w in scart])};", ""]

    # Xcalibur
    xc = blocks(XC_SPEC)
    maps = []
    in_maps = False
    for line in open(os.path.join(SPEC, XC_SPEC), encoding="utf-8"):
        if line.startswith("### 6.1 "):
            in_maps = True
        elif line.startswith("### 6.2 "):
            break
        elif in_maps and re.match(r"^\| 0x[0-9A-F]{2} \|", line):
            maps.append([c.strip() for c in line.strip().strip("|").split("|")])
    assert len(maps) == 32
    cols = [[int(r[c], 16) if r[c] else 0 for r in maps] for c in range(1, 5)]
    out += ["// ---- Xcalibur (spec/stock/20 §6; 32-bit registers)",
            "// Row maps (mode word bits 0-6 -> Xcalibur row): [0] SD, [1] SD Japan, [2] HD, [3] HD Japan.",
            "static const UCHAR AvpXcMap[4][32] = {"] + [f"    {c_bytes(c)}," for c in cols] + ["};",
            "// One table entry: Count 32-bit writes to Register.. from AvpXcValues[Value..]; Variant 0xFF = always,",
            "// 0-3 = only for that variant, 0x10 | b = only when (variant & 1) == b; Late = rewritten by option 0x0E.",
            "struct AVP_XC_WRITE { UCHAR Register, Variant, Count, Late; USHORT Value; };"]
    rows = [b for b in section(xc, "6.2")]
    values, writes, starts, timings = [], [], [], [None]
    index = {}
    for b in rows:
        assert row_of("row " + b["title"].split()[-1]) == len(starts) + 1, b["title"]
        starts.append(len(writes))
        for w in b["writes"]:
            if w[0] == "w":
                regs_vals, variant, late = [(w[1], w[2])], 0xFF, w[3]
            else:
                regs_vals, variant, late = w[2], xc_variant(w[1]), w[3]
            seq = tuple(v for _, v in regs_vals)
            if seq not in index:
                index[seq] = len(values)
                values.extend(seq)
            writes.append((regs_vals[0][0], variant, len(seq), int(late), index[seq]))
        t = xc_timing(b["writes"])
        pic = picture(b.get("used", ""))
        if pic and "SD" in b["used"].split("map")[0]:
            t["width"], t["height"] = pic[0], pic[1]
        timings.append(t)
    starts.append(len(writes))
    out += [f"static const AVP_XC_WRITE AvpXcWrites[{len(writes)}] = {{"]
    out += ["    " + ", ".join(f"{{0x{r:02X}, 0x{v:02X}, {c}, {l}, {i}}}" for r, v, c, l, i in writes[k:k + 6]) + ","
            for k in range(0, len(writes), 6)] + ["};"]
    out += [f"static const USHORT AvpXcRowStart[{len(starts)}] = {{ // row r (1-{len(starts) - 1}): entries "
            f"[AvpXcRowStart[r - 1], AvpXcRowStart[r])"]
    out += ["    " + ", ".join(str(s) for s in starts) + "};"]
    out += [f"static const ULONG AvpXcValues[{len(values)}] = {{"]
    out += ["    " + ", ".join(f"0x{v:08X}" for v in values[k:k + 8]) + "," for k in range(0, len(values), 8)] + ["};"]
    out += [f"static const AVP_TV_TIMING AvpXcTiming[{len(timings)}] = {{"] + \
           [f"    {timing_row(t)}," for t in timings] + ["};"]
    flicker = section(xc, "6.3")
    fregs = [w[1] for w in flicker[0]["writes"]]
    out += [f"static const UCHAR AvpXcFlickerRegs[{len(fregs)}] = {c_bytes(fregs)};",
            f"static const ULONG AvpXcFlicker[2][5][{len(fregs)}] = {{ // set A, set B; entries 0-4"]
    for s in "AB":
        out.append("    {")
        for e in range(5):
            b = [x for x in flicker if x["title"].startswith(f"flicker filter set {s}, entry {e}")][0]
            assert [w[1] for w in b["writes"]] == fregs
            out.append("        {" + ", ".join(f"0x{w[2]:08X}" for w in b["writes"]) + "},")
        out.append("    },")
    out += ["};"]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] in ("--out", "--check"):
        text = generate()
        if sys.argv[1] == "--out":
            open(sys.argv[2], "w").write(text)
            sys.exit(0)
        sys.exit(0 if os.path.exists(sys.argv[2]) and open(sys.argv[2]).read() == text else 1)
    sys.exit(__doc__)
