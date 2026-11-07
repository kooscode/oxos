// The vector boot splash (vsplash.h; the scene format is in spec/oxos-internal/00-boot-splash.md §1.1).
//   - Shapes: closed polygons (the host tool flattens curves and turns strokes into outlines), filled with the
//     non-zero or even-odd rule and anti-aliased: 8 sample rows per pixel row, exact coverage along each row.
//   - Paints: a colour, or a linear or radial gradient through up to 16 stops (a 257-entry table per shape).
//   - Layers: drawn in order; a layer with a filter is drawn into its own buffer first and then blurred (a Gaussian
//     approximated by three box blurs), either replacing it ("blur") or under it ("glow").
#include "vsplash.h"

#ifdef _WIN32 // the kernel: boot-only code, freed before the first title starts (spec/oxos-internal/18)
#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")
#endif

namespace vsplash {
namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
using i32 = int;

constexpr u32 SUB = 8; // sample rows per pixel row (each at 1/16-pixel resolution)
constexpr u32 MAX_PAINTS = 64, MAX_STOPS = 16, MAX_LAYERS = 256, MAX_DRAWS = 1024, MAX_CONTOURS = 1024,
              MAX_POINTS = 4096, MAX_EDGES = 65535, MAX_SIGMA16 = 64 * 16;
constexpr i32 COORD_MIN = -8192, COORD_MAX = 16383;                 // 1/16 pixel: products stay within 32 bits
constexpr i32 CENTRE_MIN = -16384, CENTRE_MAX = 32767;              // radial gradient centres, 1/16 pixel
constexpr i32 SLOPE_MAX = 1 << 20, OFFSET_MAX = 1 << 29;            // linear gradients: |a| X + |b| Y + |c| < 2^31

enum Filter : u8 { NONE = 0, BLUR = 1, GLOW = 2 };

struct Reader {
    const u8 *p, *end;
    bool ok = true;
    bool Need(u32 n) {
        if (u32(end - p) < n)
            ok = false;
        return ok;
    }
    u32 U8() { return Need(1) ? *p++ : 0; }
    u32 U16() {
        if (!Need(2))
            return 0;
        u32 v = p[0] | p[1] << 8;
        p += 2;
        return v;
    }
    i32 I16() { return i32(short(U16())); }
    i32 I32() {
        if (!Need(4))
            return 0;
        u32 v = p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24;
        p += 4;
        return i32(v);
    }
};

struct Paint {
    u32 type, stops;
    i32 a, b, c; // linear: t * 65536 = a X + b Y + c at pixel (X, Y); radial: centre x, y and radius (1/16 pixel)
    const u8 *stop;
};

// Reads the paint at r (already checked).
Paint ReadPaint(Reader &r) {
    Paint p{};
    p.type = r.U8();
    p.stops = r.U8();
    if (p.type) {
        p.a = r.I32();
        p.b = r.I32();
        p.c = r.I32();
    }
    p.stop = r.p;
    r.p += p.stops * 6;
    return p;
}

u32 Isqrt(u32 v) {
    u32 res = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// 257 colours (0xAARRGGBB, straight alpha) for t = 0..256.
void BuildTable(const Paint &p, u32 *lut) {
    for (u32 t = 0; t <= 256; t++) {
        const u8 *s = p.stop, *last = p.stop + (p.stops - 1) * 6;
        const u8 *from = s, *to = s;
        u32 f = 0;
        if (t <= u32(s[0] | s[1] << 8)) {
            from = to = s;
        } else if (t >= u32(last[0] | last[1] << 8)) {
            from = to = last;
        } else {
            for (u32 i = 0; i + 1 < p.stops; i++) {
                u32 o0 = s[i * 6] | s[i * 6 + 1] << 8, o1 = s[i * 6 + 6] | s[i * 6 + 7] << 8;
                if (t >= o0 && t < o1) {
                    from = s + i * 6, to = s + i * 6 + 6;
                    f = (t - o0) * 256 / (o1 - o0);
                    break;
                }
            }
        }
        u32 out = 0;
        for (u32 ch = 0; ch < 4; ch++) // r, g, b, a
            out |= ((from[2 + ch] * (256 - f) + to[2 + ch] * f) >> 8) << (ch == 3 ? 24 : 16 - 8 * ch);
        lut[t] = out;
    }
}

u32 PaintAt(const Paint &p, const u32 *lut, i32 x, i32 y) {
    if (p.type == 1) {
        i32 t = (p.a * x + p.b * y + p.c) >> 8;
        return lut[t < 0 ? 0 : t > 256 ? 256 : t];
    }
    if (p.type == 2) {
        i32 dx = x * 16 + 8 - p.a, dy = y * 16 + 8 - p.b;
        u32 d = Isqrt(u32(dx * dx) + u32(dy * dy)), t = d * 256 / u32(p.c);
        return lut[t > 256 ? 256 : t];
    }
    return lut[0];
}

struct Work {
    u16 *cov;   // W: coverage of the current pixel row, 0..SUB*16
    i32 *cross; // 2 per edge: x (1/16 pixel) and direction of the crossings on one sample row
    u32 *lut;   // 257
    u32 *line;  // max(W, H): one row or column for the blur
    u32 *layer[2]; // W x H, premultiplied 0xAARRGGBB (only when a layer has a filter)
};

struct Rect {
    i32 x0, y0, x1, y1;
};

// A draw's shape: contours at r. For every pixel it covers, Sink(x, y, coverage 1..256).
template <class Sink>
void Fill(Reader r, u32 contours, bool evenOdd, const Rect &clip, Work &s, Sink sink) {
    // The bounds, in pixels.
    i32 minY = COORD_MAX, maxY = COORD_MIN;
    {
        Reader q = r;
        for (u32 c = 0; c < contours; c++)
            for (u32 n = q.U16(); n; n--) {
                q.I16();
                i32 y = q.I16();
                minY = y < minY ? y : minY;
                maxY = y > maxY ? y : maxY;
            }
    }
    i32 y0 = minY >> 4, y1 = (maxY + 15) >> 4;
    y0 = y0 < clip.y0 ? clip.y0 : y0;
    y1 = y1 > clip.y1 ? clip.y1 : y1;
    const i32 cx0 = clip.x0 * 16, cx1 = clip.x1 * 16;
    for (i32 y = y0; y < y1; y++) {
        i32 lo = clip.x1, hi = clip.x0; // the pixels touched in this row
        for (u32 sub = 0; sub < SUB; sub++) {
            const i32 ys = y * 16 + i32(sub * 16 / SUB + 8 / SUB);
            // Crossings of this sample row: every edge whose half-open y range [min, max) holds it.
            u32 n = 0;
            Reader q = r;
            for (u32 c = 0; c < contours; c++) {
                u32 count = q.U16();
                const u8 *first = q.p;
                for (u32 i = 0; i < count; i++) {
                    i32 xa = q.I16(), ya = q.I16(), xb, yb;
                    if (i + 1 < count) {
                        Reader nx{q.p, q.end};
                        xb = nx.I16(), yb = nx.I16();
                    } else {
                        Reader nx{first, q.end};
                        xb = nx.I16(), yb = nx.I16();
                    }
                    if (ya == yb)
                        continue;
                    i32 dir = 1;
                    if (ya > yb) {
                        i32 t = xa;
                        xa = xb, xb = t, t = ya, ya = yb, yb = t, dir = -1;
                    }
                    if (ys < ya || ys >= yb)
                        continue;
                    i32 x = xa + (ys - ya) * (xb - xa) / (yb - ya);
                    // Insertion in x order.
                    u32 j = n++;
                    while (j && s.cross[2 * (j - 1)] > x) {
                        s.cross[2 * j] = s.cross[2 * (j - 1)], s.cross[2 * j + 1] = s.cross[2 * j - 1];
                        j--;
                    }
                    s.cross[2 * j] = x, s.cross[2 * j + 1] = dir;
                }
            }
            // Spans where the winding number says "inside".
            i32 winding = 0;
            for (u32 i = 0; i + 1 < n; i++) {
                winding += s.cross[2 * i + 1];
                bool inside = evenOdd ? (winding & 1) : winding != 0;
                if (!inside)
                    continue;
                i32 xa = s.cross[2 * i], xb = s.cross[2 * i + 2];
                xa = xa < cx0 ? cx0 : xa;
                xb = xb > cx1 ? cx1 : xb;
                if (xa >= xb)
                    continue;
                i32 pa = xa >> 4, pb = xb >> 4;
                lo = pa < lo ? pa : lo;
                hi = pb > hi ? pb : hi;
                if (pa == pb) {
                    s.cov[pa] = u16(s.cov[pa] + xb - xa);
                    continue;
                }
                s.cov[pa] = u16(s.cov[pa] + 16 - (xa & 15));
                for (i32 p = pa + 1; p < pb; p++)
                    s.cov[p] = u16(s.cov[p] + 16);
                if (xb & 15)
                    s.cov[pb] = u16(s.cov[pb] + (xb & 15));
            }
        }
        hi = hi >= clip.x1 ? clip.x1 - 1 : hi;
        for (i32 x = lo; x <= hi; x++)
            if (u32 c = s.cov[x]) {
                s.cov[x] = 0;
                sink(x, y, c * 256 / (SUB * 16));
            }
    }
}

// Source-over of a straight-alpha colour with coverage a (0..256) onto an opaque pixel or a premultiplied one.
inline u32 Over(u32 dst, u32 colour, u32 a, bool premultipliedDst) {
    u32 out = 0;
    for (u32 sh = 0; sh < 24; sh += 8) {
        u32 s = colour >> sh & 0xFF, d = dst >> sh & 0xFF;
        out |= ((s * a + d * (256 - a)) >> 8) << sh;
    }
    if (premultipliedDst)
        out |= ((255 * a + (dst >> 24) * (256 - a)) >> 8) << 24;
    return out;
}

// A premultiplied layer pixel onto an opaque one, at opacity op (0..256).
inline u32 LayerOver(u32 dst, u32 src, u32 op) {
    u32 ka = ((src >> 24) * op) >> 8;
    ka += ka >> 7; // 0..256
    u32 out = 0;
    for (u32 sh = 0; sh < 24; sh += 8) {
        u32 v = (((src >> sh & 0xFF) * op) >> 8) + (((dst >> sh & 0xFF) * (256 - ka)) >> 8);
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

// One box blur of radius r along a row (step 1) or a column (step W) of n pixels, zero outside.
void BoxLine(u32 *p, u32 n, u32 step, u32 r, u32 *tmp) {
    for (u32 i = 0; i < n; i++)
        tmp[i] = p[i * step];
    const u32 recip = (65536 + r) / (2 * r + 1);
    u32 sum[4] = {};
    auto add = [&](u32 v, bool remove) {
        for (u32 ch = 0; ch < 4; ch++)
            sum[ch] = remove ? sum[ch] - (v >> (8 * ch) & 0xFF) : sum[ch] + (v >> (8 * ch) & 0xFF);
    };
    for (u32 j = 0; j <= r && j < n; j++)
        add(tmp[j], false);
    for (u32 i = 0; i < n; i++) {
        u32 v = 0;
        for (u32 ch = 0; ch < 4; ch++) {
            u32 c = (sum[ch] * recip + 32768) >> 16;
            v |= (c > 255 ? 255 : c) << (8 * ch);
        }
        p[i * step] = v;
        if (i + r + 1 < n)
            add(tmp[i + r + 1], false);
        if (i >= r)
            add(tmp[i - r], true);
    }
}

// Gaussian blur (three box blurs each way) of the area a of a layer; sigma in 1/16 pixel.
void Blur(u32 *layer, const Rect &a, u32 sigma16, u32 *tmp) {
    u32 d = (sigma16 * 1880 + 8000) / 16000, r = d / 2;
    if (!r)
        r = 1;
    for (u32 pass = 0; pass < 3; pass++)
        for (i32 y = a.y0; y < a.y1; y++)
            BoxLine(layer + y * W + a.x0, a.x1 - a.x0, 1, r, tmp);
    for (u32 pass = 0; pass < 3; pass++)
        for (i32 x = a.x0; x < a.x1; x++)
            BoxLine(layer + a.y0 * W + x, a.y1 - a.y0, W, r, tmp);
}

// The blur's reach for sigma: three boxes of radius r spread 3 r pixels.
i32 BlurMargin(u32 sigma16) {
    u32 d = (sigma16 * 1880 + 8000) / 16000, r = d / 2;
    return i32(3 * (r ? r : 1) + 1);
}

} // namespace

unsigned Check(const unsigned char *Scene, unsigned Size, unsigned *ScratchBytes) {
    Reader r{Scene, Scene + Size};
    i32 x0 = r.U16(), y0 = r.U16(), x1 = r.U16(), y1 = r.U16();
    if (!r.ok || x0 >= x1 || y0 >= y1 || x1 > i32(W) || y1 > i32(H))
        return 1;
    u32 paints = r.U16();
    if (!paints || paints > MAX_PAINTS)
        return 2;
    for (u32 i = 0; i < paints; i++) {
        u32 type = r.U8(), stops = r.U8();
        if (type > 2 || !stops || stops > MAX_STOPS)
            return 3;
        if (type == 1) {
            i32 a = r.I32(), b = r.I32(), c = r.I32();
            if (a <= -SLOPE_MAX || a >= SLOPE_MAX || b <= -SLOPE_MAX || b >= SLOPE_MAX || c <= -OFFSET_MAX ||
                c >= OFFSET_MAX)
                return 4;
        } else if (type == 2) {
            i32 cx = r.I32(), cy = r.I32(), radius = r.I32();
            if (cx < CENTRE_MIN || cx > CENTRE_MAX || cy < CENTRE_MIN || cy > CENTRE_MAX || radius < 1 ||
                radius > (1 << 20))
                return 5;
        }
        u32 last = 0;
        for (u32 s = 0; s < stops; s++) {
            u32 offset = r.U16();
            r.U8(), r.U8(), r.U8(), r.U8();
            if (offset > 256 || offset < last)
                return 6;
            last = offset;
        }
        if (!r.ok)
            return 7;
    }
    u32 layers = r.U16(), maxEdges = 1;
    bool filtered = false;
    if (!layers || layers > MAX_LAYERS)
        return 8;
    for (u32 l = 0; l < layers; l++) {
        u32 phases = r.U8(), filter = r.U8(), sigma = r.U16();
        r.U8(), r.U8();
        u32 draws = r.U16();
        if (!phases || phases > 3 || filter > GLOW || (filter && (!sigma || sigma > MAX_SIGMA16)) || !draws ||
            draws > MAX_DRAWS)
            return 9;
        filtered |= filter != NONE;
        for (u32 d = 0; d < draws; d++) {
            u32 paint = r.U16();
            r.U8();
            u32 flags = r.U8(), contours = r.U16(), edges = 0;
            if (paint >= paints || flags > 1 || !contours || contours > MAX_CONTOURS)
                return 10;
            for (u32 c = 0; c < contours; c++) {
                u32 points = r.U16();
                if (points < 2 || points > MAX_POINTS)
                    return 11;
                edges += points;
                for (u32 p = 0; p < points; p++) {
                    i32 x = r.I16(), y = r.I16();
                    if (x < COORD_MIN || x > COORD_MAX || y < COORD_MIN || y > COORD_MAX)
                        return 12;
                }
                if (!r.ok || edges > MAX_EDGES)
                    return 13;
            }
            maxEdges = edges > maxEdges ? edges : maxEdges;
        }
    }
    if (!r.ok || r.p != r.end)
        return 14;
    *ScratchBytes = (W * 2 + 3) / 4 * 4 + maxEdges * 8 + 257 * 4 + (W > H ? W : H) * 4 + (filtered ? 2 * W * H * 4 : 0);
    return 0;
}

void Render(const unsigned char *Scene, unsigned Size, Phase Phase, unsigned *Out, void *Scratch) {
    Reader r{Scene, Scene + Size};
    Rect clip;
    clip.x0 = i32(r.U16()), clip.y0 = i32(r.U16()), clip.x1 = i32(r.U16()), clip.y1 = i32(r.U16());
    u32 paintCount = r.U16();
    const u8 *paintsAt = r.p;
    for (u32 i = 0; i < paintCount; i++)
        ReadPaint(r);
    // The working memory, in the order Check counted it.
    u32 maxEdges = 0;
    {
        Reader q = r;
        for (u32 l = q.U16(); l; l--) {
            q.U8(), q.U8(), q.U16(), q.U8(), q.U8();
            for (u32 d = q.U16(); d; d--) {
                q.U16(), q.U8(), q.U8();
                u32 edges = 0;
                for (u32 c = q.U16(); c; c--) {
                    u32 n = q.U16();
                    edges += n;
                    q.p += n * 4;
                }
                maxEdges = edges > maxEdges ? edges : maxEdges;
            }
        }
    }
    Work s;
    auto mem = (u8 *)Scratch;
    s.cov = (u16 *)mem, mem += (W * 2 + 3) / 4 * 4;
    s.cross = (i32 *)mem, mem += (maxEdges ? maxEdges : 1) * 8;
    s.lut = (u32 *)mem, mem += 257 * 4;
    s.line = (u32 *)mem, mem += (W > H ? W : H) * 4;
    s.layer[0] = (u32 *)mem, s.layer[1] = (u32 *)(mem + W * H * 4);
    for (u32 x = 0; x < W; x++)
        s.cov[x] = 0;
    for (u32 i = 0; i < W * H; i++)
        Out[i] = 0;

    for (u32 l = r.U16(); l; l--) {
        u32 phases = r.U8(), filter = r.U8(), sigma = r.U16(), opacity = r.U8();
        r.U8();
        u32 draws = r.U16();
        const bool shown = phases & Phase;
        // The layer's area (filtered layers): its shapes' bounds plus the blur's reach, within the clip.
        Rect area = clip;
        if (shown && filter) {
            i32 ax0 = COORD_MAX, ay0 = COORD_MAX, ax1 = COORD_MIN, ay1 = COORD_MIN;
            Reader q = r;
            for (u32 d = 0; d < draws; d++) {
                q.U16(), q.U8(), q.U8();
                for (u32 c = q.U16(); c; c--)
                    for (u32 n = q.U16(); n; n--) {
                        i32 x = q.I16(), y = q.I16();
                        ax0 = x < ax0 ? x : ax0, ax1 = x > ax1 ? x : ax1;
                        ay0 = y < ay0 ? y : ay0, ay1 = y > ay1 ? y : ay1;
                    }
            }
            i32 m = BlurMargin(sigma);
            area.x0 = (ax0 >> 4) - m, area.y0 = (ay0 >> 4) - m, area.x1 = ((ax1 + 15) >> 4) + m,
            area.y1 = ((ay1 + 15) >> 4) + m;
            area.x0 = area.x0 < clip.x0 ? clip.x0 : area.x0, area.y0 = area.y0 < clip.y0 ? clip.y0 : area.y0;
            area.x1 = area.x1 > clip.x1 ? clip.x1 : area.x1, area.y1 = area.y1 > clip.y1 ? clip.y1 : area.y1;
            if (area.x0 >= area.x1 || area.y0 >= area.y1)
                area = clip;
            for (i32 y = area.y0; y < area.y1; y++)
                for (i32 x = area.x0; x < area.x1; x++)
                    s.layer[0][y * W + x] = 0;
        }
        u32 *target = filter ? s.layer[0] : Out;
        for (u32 d = 0; d < draws; d++) {
            u32 paintIndex = r.U16(), alpha = r.U8(), flags = r.U8(), contours = r.U16();
            Reader shape = r;
            for (u32 c = 0; c < contours; c++)
                r.p += r.U16() * 4;
            if (!shown)
                continue;
            Reader pr{paintsAt, r.end};
            Paint paint{};
            for (u32 i = 0; i <= paintIndex; i++)
                paint = ReadPaint(pr);
            BuildTable(paint, s.lut);
            if (!filter)
                alpha = alpha * opacity / 255; // a plain layer's opacity applies to each shape
            Fill(shape, contours, flags & 1, clip, s, [&](i32 x, i32 y, u32 cov) {
                u32 colour = PaintAt(paint, s.lut, x, y);
                u32 a = ((colour >> 24) * alpha + 127) / 255;
                a = (a * cov + 127) / 255;
                if (a)
                    target[y * W + x] = Over(target[y * W + x], colour, a, filter != NONE);
            });
        }
        if (!shown || !filter)
            continue;
        const u32 op = opacity + (opacity >> 7);
        if (filter == GLOW)
            for (i32 y = area.y0; y < area.y1; y++)
                for (i32 x = area.x0; x < area.x1; x++)
                    s.layer[1][y * W + x] = s.layer[0][y * W + x];
        u32 *blurred = filter == GLOW ? s.layer[1] : s.layer[0];
        Blur(blurred, area, sigma, s.line);
        for (i32 y = area.y0; y < area.y1; y++)
            for (i32 x = area.x0; x < area.x1; x++) {
                u32 &o = Out[y * W + x];
                o = LayerOver(o, blurred[y * W + x], op);
                if (filter == GLOW)
                    o = LayerOver(o, s.layer[0][y * W + x], op);
            }
    }
}

} // namespace vsplash
