// PNG decoder for the boot splash (spec/17 §7): 8-bit palette/RGB/RGBA, non-interlaced, one IDAT chunk
// (tools/mkbios.py writes a 256-colour palette image). Inflate follows RFC 1951 with canonical Huffman decoding one bit at a time: the splash is
// decoded once per cold boot, so table speed-ups are not worth the code.
#include <xk/arch.h>

namespace {

struct Huffman {
    USHORT Count[16];   // codes of each length
    USHORT Symbol[288]; // symbols ordered by code
};

struct Inflater {
    const UCHAR *In;
    ULONG InSize, InPos, Bits, BitCount;
    UCHAR *Out;
    ULONG OutSize, OutPos;
    bool Error;

    ULONG Need(ULONG n) {
        while (BitCount < n) {
            if (InPos >= InSize) {
                Error = true;
                return 0;
            }
            Bits |= (ULONG)In[InPos++] << BitCount;
            BitCount += 8;
        }
        ULONG v = Bits & ((1u << n) - 1);
        Bits >>= n;
        BitCount -= n;
        return v;
    }

    // Walk the canonical code one bit at a time: codes of length L are consecutive, starting at `first`.
    int Decode(const Huffman &h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)Need(1);
            int count = h.Count[len];
            if (code - first < count)
                return h.Symbol[index + code - first];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
            if (Error)
                break;
        }
        Error = true;
        return -1;
    }
};

bool BuildHuffman(Huffman &h, const UCHAR *lengths, int n) {
    USHORT offset[16];
    for (int i = 0; i < 16; i++)
        h.Count[i] = 0;
    for (int i = 0; i < n; i++)
        h.Count[lengths[i]]++;
    h.Count[0] = 0;
    int left = 1; // reject over-subscribed sets; incomplete ones are legal (e.g. a single distance code)
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h.Count[len];
        if (left < 0)
            return false;
    }
    offset[1] = 0;
    for (int len = 1; len < 15; len++)
        offset[len + 1] = offset[len] + h.Count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h.Symbol[offset[lengths[i]]++] = (USHORT)i;
    return true;
}

// RFC 1951 §3.2.5 length and distance bases.
const USHORT LengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const UCHAR LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const USHORT DistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const UCHAR DistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const UCHAR CodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool InflateCodes(Inflater &s, const Huffman &lit, const Huffman &dist) {
    for (;;) {
        int sym = s.Decode(lit);
        if (sym < 0)
            return false;
        if (sym < 256) {
            if (s.OutPos >= s.OutSize)
                return false;
            s.Out[s.OutPos++] = (UCHAR)sym;
            continue;
        }
        if (sym == 256)
            return true;
        sym -= 257;
        if (sym >= 29)
            return false;
        ULONG len = LengthBase[sym] + s.Need(LengthExtra[sym]);
        int dsym = s.Decode(dist);
        if (dsym < 0 || dsym >= 30)
            return false;
        ULONG d = DistBase[dsym] + s.Need(DistExtra[dsym]);
        if (s.Error || d > s.OutPos || len > s.OutSize - s.OutPos)
            return false;
        for (; len; len--, s.OutPos++)
            s.Out[s.OutPos] = s.Out[s.OutPos - d];
    }
}

bool Inflate(Inflater &s) {
    Huffman lit, dist;
    UCHAR lengths[320];
    for (;;) {
        ULONG last = s.Need(1), type = s.Need(2);
        if (s.Error)
            return false;
        if (type == 0) { // stored: the rest of the current byte is padding
            s.Bits = s.BitCount = 0;
            if (s.InSize - s.InPos < 4)
                return false;
            ULONG len = s.In[s.InPos] | s.In[s.InPos + 1] << 8, nlen = s.In[s.InPos + 2] | s.In[s.InPos + 3] << 8;
            s.InPos += 4;
            if ((len ^ 0xFFFF) != nlen || len > s.InSize - s.InPos || len > s.OutSize - s.OutPos)
                return false;
            for (; len; len--)
                s.Out[s.OutPos++] = s.In[s.InPos++];
        } else if (type == 1) { // fixed codes (RFC 1951 §3.2.6)
            for (int i = 0; i < 288; i++)
                lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            BuildHuffman(lit, lengths, 288);
            for (int i = 0; i < 30; i++)
                lengths[i] = 5;
            BuildHuffman(dist, lengths, 30);
            if (!InflateCodes(s, lit, dist))
                return false;
        } else if (type == 2) { // dynamic codes (§3.2.7)
            ULONG nlit = s.Need(5) + 257, ndist = s.Need(5) + 1, ncode = s.Need(4) + 4;
            if (nlit > 286 || ndist > 30)
                return false;
            for (int i = 0; i < 19; i++)
                lengths[CodeLengthOrder[i]] = i < (int)ncode ? (UCHAR)s.Need(3) : 0;
            Huffman lencode;
            if (s.Error || !BuildHuffman(lencode, lengths, 19))
                return false;
            for (ULONG i = 0; i < nlit + ndist;) {
                int sym = s.Decode(lencode);
                if (sym < 0)
                    return false;
                if (sym < 16) {
                    lengths[i++] = (UCHAR)sym;
                    continue;
                }
                UCHAR value = 0;
                ULONG repeat;
                if (sym == 16) {
                    if (!i)
                        return false;
                    value = lengths[i - 1];
                    repeat = 3 + s.Need(2);
                } else if (sym == 17) {
                    repeat = 3 + s.Need(3);
                } else {
                    repeat = 11 + s.Need(7);
                }
                if (s.Error || i + repeat > nlit + ndist)
                    return false;
                while (repeat--)
                    lengths[i++] = value;
            }
            if (!lengths[256] || !BuildHuffman(lit, lengths, (int)nlit) || !BuildHuffman(dist, lengths + nlit, (int)ndist))
                return false;
            if (!InflateCodes(s, lit, dist))
                return false;
        } else {
            return false;
        }
        if (last)
            return !s.Error;
    }
}

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
