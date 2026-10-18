// Inflate (RFC 1951) for the boot splash PNG and the open BIOS image's compressed kernel (spec/stock/01 §5). Canonical
// Huffman decoding one bit at a time: each user decodes once per boot, so table speed-ups are not worth the code.
// Header-only with plain C++ types, so the 2BL (built without the kernel headers) can include it.
#pragma once

namespace xk_inflate {
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;


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

inline bool BuildHuffman(Huffman &h, const UCHAR *lengths, int n) {
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

inline bool InflateCodes(Inflater &s, const Huffman &lit, const Huffman &dist) {
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

inline bool Inflate(Inflater &s) {
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

} // namespace xk_inflate
