#!/usr/bin/env python3
"""Converts an SVG into the vector boot splash's scene (spec/oxos_internal/00_boot_splash.md §1.1), which the kernel
draws at boot (kernel/src/bootscreen/vsplash.cpp). tools/mkbios.py calls it for a --splash file ending in .svg.

    svg_splash.py IN.svg OUT.bin      write the scene (without the "XKSPLASH" header)

The SVG is scaled to the full 640-pixel width and centred vertically (black bars above and below a wide picture).
Curves are flattened to polygons and strokes turned into outlines here, so the kernel only fills polygons.

Supported: <svg viewBox>, <g>, <rect> (no rounded corners), <circle>, <ellipse>, <polygon>, <polyline>, <path> (M L H
V Q T C S Z, absolute and relative; no arcs); transform (matrix, translate, scale, rotate); fill, stroke,
stroke-width, fill-opacity, stroke-opacity, opacity, fill-rule, inherited from groups and as style="..."; colours
#rgb, #rrggbb, rgb(), a few names; <linearGradient> and <radialGradient> (userSpaceOnUse or objectBoundingBox,
gradientTransform, stops with stop-opacity); filter="url(#id)" with an <feGaussianBlur>, alone (the shape replaced by
its blur) or merged under the SourceGraphic (a glow).

The animation: an element (or group) with data-oxos-phase="start" is drawn only in the first picture, "end" only in
the last; everything else in both. The kernel fades the first picture in, then cross-fades to the last.
"""
import math
import re
import struct
import sys
import xml.etree.ElementTree as ET

W, H = 640, 480
SVG_NS = "{http://www.w3.org/2000/svg}"
PHASE_START, PHASE_END = 1, 2
NAMED = {"black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0), "green": (0, 128, 0),
         "blue": (0, 0, 255), "gray": (128, 128, 128), "grey": (128, 128, 128), "yellow": (255, 255, 0)}
INHERITED = ("fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "fill-rule", "stroke-linejoin")


class SplashError(Exception):
    pass


# --- Geometry: 2x3 affine matrices (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def mul(m, n):
    """m after n."""
    a, b, c, d, e, f = m
    a2, b2, c2, d2, e2, f2 = n
    return (a * a2 + c * b2, b * a2 + d * b2, a * c2 + c * d2, b * c2 + d * d2, a * e2 + c * f2 + e,
            b * e2 + d * f2 + f)


def apply(m, x, y):
    return m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]


def invert(m):
    a, b, c, d, e, f = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        raise SplashError("a transform that cannot be inverted")
    return (d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det)


def parse_transform(text):
    m = IDENTITY
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", text or ""):
        v = [float(x) for x in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", args)]
        if name == "matrix" and len(v) == 6:
            t = tuple(v)
        elif name == "translate":
            t = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == "scale":
            t = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == "rotate":
            r = math.radians(v[0])
            t = (math.cos(r), math.sin(r), -math.sin(r), math.cos(r), 0, 0)
            if len(v) == 3:
                t = mul((1, 0, 0, 1, v[1], v[2]), mul(t, (1, 0, 0, 1, -v[1], -v[2])))
        else:
            raise SplashError(f"unsupported transform {name}({args})")
        m = mul(m, t)
    return m


def numbers(text):
    return [float(x) for x in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", text or "")]


def flatten_quad(p0, p1, p2, scale):
    n = max(2, min(32, int(math.ceil(math.dist(p0, p1) + math.dist(p1, p2)) * scale / 3)))
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in (i / n for i in range(1, n + 1))]


def flatten_cubic(p0, p1, p2, p3, scale):
    n = max(2, min(48, int(math.ceil(math.dist(p0, p1) + math.dist(p1, p2) + math.dist(p2, p3)) * scale / 3)))
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def path_contours(d, scale):
    """Contours (lists of points, local coordinates) of an SVG path; open subpaths are returned too (the fill closes
    them, the stroke leaves them open). Each item: (points, closed)."""
    tokens = re.findall(r"[MmLlHhVvQqTtCcSsZzAa]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d or "")
    out, cur, start = [], [], (0.0, 0.0)
    pos, cmd, i = (0.0, 0.0), None, 0
    last_ctrl, last_cmd = None, None

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
        elif cmd is None:
            raise SplashError("path data does not start with a command")
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = pos if rel else (0.0, 0.0)
        if c == "M":
            if len(cur) > 1:
                out.append((cur, False))
            pos = (ox + num(), oy + num())
            cur, start = [pos], pos
            cmd = "l" if rel else "L"  # further pairs are lines
        elif c == "L":
            pos = (ox + num(), oy + num())
            cur.append(pos)
        elif c == "H":
            pos = ((pos[0] if rel else 0) + num(), pos[1])
            cur.append(pos)
        elif c == "V":
            pos = (pos[0], (pos[1] if rel else 0) + num())
            cur.append(pos)
        elif c in "QT":
            if c == "Q":
                ctrl = (ox + num(), oy + num())
            else:
                ctrl = (2 * pos[0] - last_ctrl[0], 2 * pos[1] - last_ctrl[1]) if last_cmd in "QT" else pos
            end = (ox + num(), oy + num())
            cur += flatten_quad(pos, ctrl, end, scale)
            last_ctrl, pos = ctrl, end
        elif c in "CS":
            if c == "C":
                c1 = (ox + num(), oy + num())
            else:
                c1 = (2 * pos[0] - last_ctrl[0], 2 * pos[1] - last_ctrl[1]) if last_cmd in "CS" else pos
            c2 = (ox + num(), oy + num())
            end = (ox + num(), oy + num())
            cur += flatten_cubic(pos, c1, c2, end, scale)
            last_ctrl, pos = c2, end
        elif c == "Z":
            if len(cur) > 1:
                out.append((cur, True))
            pos, cur = start, [start]
        else:
            raise SplashError(f"unsupported path command {cmd} (arcs are not supported)")
        last_cmd = c
    if len(cur) > 1:
        out.append((cur, False))
    return out


def ellipse_points(cx, cy, rx, ry, scale):
    n = max(24, min(128, int(2 * math.pi * max(rx, ry) * scale / 3)))
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def stroke_outline(points, closed, width):
    """Polygons (output coordinates, all wound the same way) whose union is the stroke: a quad per segment and a
    triangle each side of every joint (bevel joins; the strokes here are a pixel or two wide)."""
    hw = width / 2
    pts = [p for k, p in enumerate(points) if k == 0 or math.dist(p, points[k - 1]) > 1e-6]
    if closed and len(pts) > 2 and math.dist(pts[0], pts[-1]) < 1e-6:
        pts.pop()
    segs = list(zip(pts, pts[1:] + pts[:1])) if closed else list(zip(pts, pts[1:]))
    polys, normals = [], []
    for p, q in segs:
        dx, dy = q[0] - p[0], q[1] - p[1]
        length = math.hypot(dx, dy)
        n = (-dy / length * hw, dx / length * hw)
        normals.append(n)
        polys.append([(p[0] + n[0], p[1] + n[1]), (q[0] + n[0], q[1] + n[1]), (q[0] - n[0], q[1] - n[1]),
                      (p[0] - n[0], p[1] - n[1])])
    joints = range(len(segs)) if closed else range(1, len(segs))
    for k in joints:
        v, n1, n2 = segs[k][0], normals[k - 1], normals[k]
        for s in (1, -1):
            polys.append([v, (v[0] + s * n1[0], v[1] + s * n1[1]), (v[0] + s * n2[0], v[1] + s * n2[1])])
    out = []
    for poly in polys:
        area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(poly, poly[1:] + poly[:1]))
        if abs(area) < 1e-9:
            continue
        out.append(poly if area > 0 else poly[::-1])
    return out


# --- Style

def parse_style(el):
    style = {k: v for k, v in el.attrib.items()}
    for item in (el.get("style") or "").split(";"):
        if ":" in item:
            k, v = item.split(":", 1)
            style[k.strip()] = v.strip()
    return style


def parse_colour(text):
    text = text.strip()
    if text.startswith("#"):
        h = text[1:]
        if len(h) == 3:
            h = "".join(ch * 2 for ch in h)
        if len(h) != 6:
            raise SplashError(f"bad colour {text}")
        return tuple(int(h[k:k + 2], 16) for k in (0, 2, 4))
    m = re.match(r"rgb\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*\)", text)
    if m:
        return tuple(int(float(v)) for v in m.groups())
    if text.lower() in NAMED:
        return NAMED[text.lower()]
    raise SplashError(f"unsupported colour {text}")


def opacity(text, default=1.0):
    if text is None:
        return default
    text = text.strip()
    return max(0.0, min(1.0, float(text[:-1]) / 100 if text.endswith("%") else float(text)))


# --- The converter: SVG elements, in document order, to the scene
class Converter:
    def __init__(self, root):
        self.root = root
        self.ids = {el.get("id"): el for el in root.iter() if el.get("id")}
        vb = numbers(root.get("viewBox"))
        if len(vb) != 4:
            vb = [0, 0, float(re.sub("[a-z%]", "", root.get("width", "640"))),
                  float(re.sub("[a-z%]", "", root.get("height", "480")))]
        self.vb = vb
        s = min(W / vb[2], H / vb[3])  # the full width for a wide picture (black bars above and below)
        ox, oy = (W - vb[2] * s) / 2, (H - vb[3] * s) / 2
        self.scale = s
        self.view = (s, 0, 0, s, ox - vb[0] * s, oy - vb[1] * s)
        self.clip = (max(0, int(math.floor(ox))), max(0, int(math.floor(oy))), min(W, int(math.ceil(ox + vb[2] * s))),
                     min(H, int(math.ceil(oy + vb[3] * s))))
        self.paints, self.paint_index = [], {}
        self.layers = []  # [phases, filter, sigma16, opacity, draws]; draws: (paint, alpha, evenodd, contours)

    # Paints: ("solid", rgba) or gradients resolved to output coordinates for one element.
    def add_paint(self, key, record):
        if key not in self.paint_index:
            self.paint_index[key] = len(self.paints)
            self.paints.append(record)
        return self.paint_index[key]

    def stops(self, grad):
        out = []
        for st in grad.iter(SVG_NS + "stop"):
            style = parse_style(st)
            off = style.get("offset", "0").strip()
            off = float(off[:-1]) / 100 if off.endswith("%") else float(off)
            rgb = parse_colour(style.get("stop-color", "#000"))
            out.append((min(256, max(0, round(off * 256))), *rgb, round(255 * opacity(style.get("stop-opacity")))))
        if not out:
            raise SplashError(f"gradient {grad.get('id')} has no stops")
        for k in range(1, len(out)):  # offsets never go back
            if out[k][0] < out[k - 1][0]:
                out[k] = (out[k - 1][0], *out[k][1:])
        if len(out) > 16:
            raise SplashError(f"gradient {grad.get('id')} has more than 16 stops")
        return out

    def paint_for(self, value, ctm, bbox):
        """A paint index for a fill or stroke value, or None for none."""
        value = (value or "none").strip()
        if value == "none":
            return None
        m = re.match(r"url\(\s*#([^)\s]+)\s*\)", value)
        if not m:
            rgb = parse_colour(value)
            return self.add_paint(("solid", rgb), (0, [(0, *rgb, 255)], None))
        grad = self.ids.get(m.group(1))
        if grad is None:
            raise SplashError(f"no gradient {m.group(1)}")
        units = grad.get("gradientUnits", "objectBoundingBox")
        g = parse_transform(grad.get("gradientTransform"))
        if units == "objectBoundingBox":
            x0, y0, x1, y1 = bbox
            g = mul((x1 - x0, 0, 0, y1 - y0, x0, y0), g)
        full = mul(mul(self.view, ctm), g)  # gradient space -> output pixels
        stops = self.stops(grad)

        def coord(name, default):
            v = grad.get(name, default).strip()
            return float(v[:-1]) / 100 if v.endswith("%") else float(v)

        if grad.tag == SVG_NS + "linearGradient":
            x1, y1 = coord("x1", "0"), coord("y1", "0")
            x2, y2 = coord("x2", "1"), coord("y2", "0")
            dx, dy = x2 - x1, y2 - y1
            l2 = dx * dx + dy * dy or 1e-9
            inv = invert(full)

            def t_at(px, py):  # pixel centre px + 0.5, py + 0.5
                gx, gy = apply(inv, px + 0.5, py + 0.5)
                return ((gx - x1) * dx + (gy - y1) * dy) / l2

            t0 = t_at(0, 0)
            a, b = t_at(1, 0) - t0, t_at(0, 1) - t0
            geom = tuple(round(v * 65536) for v in (a, b, t0))
            return self.add_paint(("lin", geom, tuple(stops)), (1, stops, geom))
        cx, cy, r = coord("cx", "0.5"), coord("cy", "0.5"), coord("r", "0.5")
        px, py = apply(full, cx, cy)
        rs = r * math.sqrt(abs(full[0] * full[3] - full[1] * full[2]))
        geom = (round(px * 16), round(py * 16), max(1, round(rs * 16)))
        return self.add_paint(("rad", geom, tuple(stops)), (2, stops, geom))

    def filter_for(self, value, ctm):
        m = re.match(r"url\(\s*#([^)\s]+)\s*\)", (value or "").strip())
        if not m:
            return 0, 0
        f = self.ids.get(m.group(1))
        if f is None:
            raise SplashError(f"no filter {m.group(1)}")
        blur = next(f.iter(SVG_NS + "feGaussianBlur"), None)
        if blur is None:
            raise SplashError(f"filter {m.group(1)}: only feGaussianBlur is supported")
        # stdDeviation is in the filtered element's user space: its transforms and the view scale apply.
        sigma = numbers(blur.get("stdDeviation"))[0] * self.scale * math.sqrt(abs(ctm[0] * ctm[3] - ctm[1] * ctm[2]))
        merged = any(n.get("in") == "SourceGraphic" for n in f.iter(SVG_NS + "feMergeNode"))
        return (2 if merged else 1), max(1, round(sigma * 16))

    # Shapes of one element: (local contours [(points, closed)], local bbox)
    def shape(self, el, tag):
        def a(name, default="0"):  # a numeric attribute
            return float(el.get(name, default))

        if tag == "rect":
            if el.get("rx") or el.get("ry"):
                raise SplashError("rounded rectangles are not supported")
            x, y, w, h = a("x"), a("y"), a("width"), a("height")
            return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
        if tag == "circle":
            return [(ellipse_points(a("cx"), a("cy"), a("r"), a("r"), self.scale), True)]
        if tag == "ellipse":
            return [(ellipse_points(a("cx"), a("cy"), a("rx"), a("ry"), self.scale), True)]
        if tag in ("polygon", "polyline"):
            v = numbers(el.get("points"))
            return [(list(zip(v[0::2], v[1::2])), tag == "polygon")]
        if tag == "path":
            return path_contours(el.get("d"), self.scale)
        return []

    def emit(self, draws, phases, filt, sigma, layer_opacity):
        if not draws:
            return
        last = self.layers[-1] if self.layers else None
        if not filt and last and not last[1] and last[0] == phases and last[3] == 255:
            last[4] += draws  # consecutive plain shapes: one layer
            return
        self.layers.append([phases, filt, sigma, round(255 * layer_opacity), draws])

    def to_out(self, ctm, pts):
        m = mul(self.view, ctm)
        return [apply(m, x, y) for x, y in pts]

    def element_draws(self, el, tag, style, ctm, alpha_scale):
        contours = self.shape(el, tag)
        if not contours:
            return []
        xs = [p[0] for c, _ in contours for p in c]
        ys = [p[1] for c, _ in contours for p in c]
        bbox = (min(xs), min(ys), max(xs), max(ys))
        draws = []
        fill = self.paint_for(style.get("fill", "black"), ctm, bbox)
        if fill is not None:
            alpha = opacity(style.get("fill-opacity")) * alpha_scale
            draws.append((fill, alpha, style.get("fill-rule") == "evenodd",
                          [self.to_out(ctm, c) for c, _ in contours]))
        stroke = self.paint_for(style.get("stroke", "none"), ctm, bbox)
        width = float(re.sub("[a-z]", "", style.get("stroke-width", "1"))) * self.scale * \
            math.sqrt(abs(ctm[0] * ctm[3] - ctm[1] * ctm[2]))
        if stroke is not None and width > 0:
            alpha = opacity(style.get("stroke-opacity")) * alpha_scale
            polys = [p for c, closed in contours for p in stroke_outline(self.to_out(ctm, c), closed, width)]
            if polys:
                draws.append((stroke, alpha, False, polys))
        return draws

    def walk(self, el, ctm, inherited, phases, alpha_scale, collect=None):
        tag = el.tag.replace(SVG_NS, "")
        if tag in ("defs", "metadata", "title", "desc", "linearGradient", "radialGradient", "filter", "style"):
            return
        if tag in ("use", "image", "text", "mask", "clipPath", "pattern"):
            raise SplashError(f"<{tag}> is not supported")
        style = parse_style(el)
        mine = dict(inherited)
        for k in INHERITED:
            if k in style:
                mine[k] = style[k]
        ctm = mul(ctm, parse_transform(el.get("transform")))
        phase = el.get("data-oxos-phase")
        if phase:
            if phase not in ("start", "end"):
                raise SplashError(f'data-oxos-phase="{phase}": use "start" or "end"')
            phases &= PHASE_START if phase == "start" else PHASE_END
            if not phases:
                return
        filt, sigma = self.filter_for(style.get("filter"), ctm)
        op = opacity(style.get("opacity"))
        if filt and collect is not None:
            raise SplashError("a filter inside a filtered group is not supported")
        if filt:  # this element (and its children) become one filtered layer
            draws = []
            if tag in ("g", "svg"):
                for child in el:
                    self.walk(child, ctm, mine, phases, alpha_scale, draws)
            else:
                draws = self.element_draws(el, tag, mine, ctm, alpha_scale)
            self.emit(draws, phases, filt, sigma, op)
            return
        if tag in ("g", "svg"):
            for child in el:
                self.walk(child, ctm, mine, phases, alpha_scale * op, collect)
            return
        draws = self.element_draws(el, tag, mine, ctm, alpha_scale * op)
        if collect is not None:
            collect += draws
        else:
            self.emit(draws, phases, 0, 0, 1.0)

    def convert(self):
        self.walk(self.root, IDENTITY, {}, PHASE_START | PHASE_END, 1.0)
        if not self.layers:
            raise SplashError("the SVG draws nothing")
        return self.encode()

    def encode(self):
        out = bytearray(struct.pack("<4H", *self.clip))
        out += struct.pack("<H", len(self.paints))
        for kind, stops, geom in self.paints:
            out += struct.pack("<BB", kind, len(stops))
            if kind:
                out += struct.pack("<3i", *geom)
            for off, r, g, b, a in stops:
                out += struct.pack("<H4B", off, r, g, b, a)
        out += struct.pack("<H", len(self.layers))
        for phases, filt, sigma, op, draws in self.layers:
            out += struct.pack("<BBHBBH", phases, filt, sigma, op, 0, len(draws))
            for paint, alpha, evenodd, contours in draws:
                cs = []
                for c in contours:
                    pts = [(round(x * 16), round(y * 16)) for x, y in c]
                    pts = [p for k, p in enumerate(pts) if k == 0 or p != pts[k - 1]]
                    if len(pts) > 1 and pts[0] == pts[-1]:
                        pts.pop()
                    if len(pts) >= 2:
                        cs.append(pts)
                if not cs:
                    continue
                out += struct.pack("<HBBH", paint, max(0, min(255, round(alpha * 255))), 1 if evenodd else 0, len(cs))
                for pts in cs:
                    out += struct.pack("<H", len(pts))
                    for x, y in pts:
                        if not (-8192 <= x <= 16383 and -8192 <= y <= 16383):
                            raise SplashError("a shape lies too far outside the picture")
                        out += struct.pack("<2h", x, y)
        return bytes(out)


# --- Entry points: convert() for mkbios.py, main() for the command line
def convert(path):
    """The scene for an SVG file (bytes)."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as e:
        raise SplashError(f"{path}: {e}")
    if root.tag != SVG_NS + "svg":
        raise SplashError(f"{path}: not an SVG")
    return Converter(root).convert()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        scene = convert(sys.argv[1])
    except SplashError as e:
        sys.exit(f"svg_splash: {e}")
    open(sys.argv[2], "wb").write(scene)
    print(f"{sys.argv[2]}: {len(scene)} bytes")


if __name__ == "__main__":
    main()
