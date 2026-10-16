// PNG decoder for the boot splash (spec/17 §7): 8-bit palette/RGB/RGBA, non-interlaced, one IDAT chunk
// (tools/mkbios.py writes a 256-colour palette image). Inflate is shared with the open-image 2BL (xk/inflate.h).
#include <xk/arch.h>

#include <xk/inflate.h>

namespace {
using xk_inflate::Inflate;
using xk_inflate::Inflater;

ULONG BigEndian32(const UCHAR *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }

LONG Abs(LONG v) { return v < 0 ? -v : v; }

} // namespace

ULONG HalpPngScratchSize(ULONG Width, ULONG Height) { return Height * (1 + Width * 4); }

// Decode Png (Size bytes) into Pixels (Width*Height of 0x00RRGGBB). Scratch holds the inflated scanlines and must
// be HalpPngScratchSize(Width, Height) bytes. Returns 0 on success, else the step that failed (diagnostics).
ULONG HalpDecodePng(const UCHAR *Png, ULONG Size, ULONG *Pixels, ULONG Width, ULONG Height, UCHAR *Scratch) {
    static const UCHAR signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; i++)
        if (Size < 8 || Png[i] != signature[i])
            return 1;
    const UCHAR *idat = nullptr, *palette = nullptr;
    ULONG idatSize = 0, bpp = 0, colors = 0;
    for (ULONG o = 8; o + 12 <= Size;) {
        ULONG n = BigEndian32(Png + o);
        const UCHAR *type = Png + o + 4, *data = Png + o + 8;
        if (n > Size - o - 12)
            return 2;
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (n < 13 || BigEndian32(data) != Width || BigEndian32(data + 4) != Height || data[8] != 8 ||
                (data[9] != 2 && data[9] != 3 && data[9] != 6) || data[10] || data[11] || data[12])
                return 3;
            bpp = data[9] == 2 ? 3 : data[9] == 3 ? 1 : 4;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            palette = data, colors = n / 3;
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (idat)
                return 4; // one IDAT only
            idat = data, idatSize = n;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        o += 12 + n;
    }
    if (!bpp || !idat || idatSize < 6 || (bpp == 1 && !palette))
        return 5;
    // zlib wrapper (RFC 1950): deflate, no preset dictionary, Adler-32 of the output at the end.
    if ((idat[0] & 0x0F) != 8 || (idat[0] << 8 | idat[1]) % 31 || (idat[1] & 0x20))
        return 6;
    ULONG stride = 1 + Width * bpp;
    Inflater s = {idat + 2, idatSize - 6, 0, 0, 0, Scratch, Height * stride, 0, false};
    if (!Inflate(s) || s.OutPos != s.OutSize)
        return 7;
    ULONG a = 1, b = 0;
    for (ULONG i = 0; i < s.OutSize; i++) {
        a = (a + Scratch[i]) % 65521;
        b = (b + a) % 65521;
    }
    if ((b << 16 | a) != BigEndian32(idat + idatSize - 4))
        return 8;
    // Undo the per-scanline filters (PNG §9) in place, then pack the pixels.
    UCHAR *prior = nullptr;
    for (ULONG y = 0; y < Height; y++) {
        UCHAR *row = Scratch + y * stride, filter = row[0];
        UCHAR *x = row + 1;
        for (ULONG i = 0; i < Width * bpp; i++) {
            LONG left = i >= bpp ? x[i - bpp] : 0, up = prior ? prior[i] : 0,
                 upLeft = prior && i >= bpp ? prior[i - bpp] : 0;
            LONG pred;
            switch (filter) {
            case 0: pred = 0; break;
            case 1: pred = left; break;
            case 2: pred = up; break;
            case 3: pred = (left + up) / 2; break;
            case 4: {
                LONG p = left + up - upLeft, pa = Abs(p - left), pb = Abs(p - up), pc = Abs(p - upLeft);
                pred = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
                break;
            }
            default: return 9;
            }
            x[i] = (UCHAR)(x[i] + pred);
        }
        for (ULONG i = 0; i < Width; i++) {
            const UCHAR *rgb = x + i * bpp;
            if (bpp == 1) {
                if (*rgb >= colors)
                    return 10;
                rgb = palette + *rgb * 3;
            }
            Pixels[y * Width + i] = (ULONG)rgb[0] << 16 | (ULONG)rgb[1] << 8 | rgb[2];
        }
        prior = x;
    }
    return 0;
}
