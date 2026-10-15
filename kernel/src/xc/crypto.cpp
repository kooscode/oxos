// Xc crypto services: SHA-1, HMAC-SHA1, RC4 (public algorithms; the caller-visible contexts are opaque buffers:
// 116 bytes for SHA, 258 for RC4).
#include <xk/rtl.h>

struct XC_SHA_CONTEXT {
    ULONG State[5];
    ULONG CountLow, CountHigh; // bytes
    UCHAR Buffer[64];
};
static_assert(sizeof(XC_SHA_CONTEXT) <= 116);

static ULONG Rol(ULONG x, int n) { return (x << n) | (x >> (32 - n)); }

static void XcpShaBlock(ULONG *s, const UCHAR *p) {
    ULONG w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 | (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    ULONG a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        ULONG f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        ULONG t = Rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = Rol(b, 30), b = a, a = t;
    }
    s[0] += a, s[1] += b, s[2] += c, s[3] += d, s[4] += e;
}

XBAPI void NTAPI XcSHAInit(PUCHAR pbSHAContext) {
    auto c = (XC_SHA_CONTEXT *)pbSHAContext;
    static const ULONG init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    for (int i = 0; i < 5; i++)
        c->State[i] = init[i];
    c->CountLow = c->CountHigh = 0;
}

XBAPI void NTAPI XcSHAUpdate(PUCHAR pbSHAContext, const UCHAR *pbInput, ULONG dwInputLength) {
    auto c = (XC_SHA_CONTEXT *)pbSHAContext;
    ULONG used = c->CountLow & 63;
    ULONG low = c->CountLow + dwInputLength;
    if (low < c->CountLow)
        c->CountHigh++;
    c->CountLow = low;
    while (dwInputLength) {
        ULONG n = 64 - used < dwInputLength ? 64 - used : dwInputLength;
        RtlCopyMemory(c->Buffer + used, pbInput, n);
        used += n, pbInput += n, dwInputLength -= n;
        if (used == 64) {
            XcpShaBlock(c->State, c->Buffer);
            used = 0;
        }
    }
}

XBAPI void NTAPI XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest) {
    auto c = (XC_SHA_CONTEXT *)pbSHAContext;
    ULONG bitsHigh = (c->CountHigh << 3) | (c->CountLow >> 29), bitsLow = c->CountLow << 3;
    static const UCHAR pad[64] = {0x80};
    ULONG used = c->CountLow & 63;
    XcSHAUpdate(pbSHAContext, pad, used < 56 ? 56 - used : 120 - used);
    UCHAR len[8] = {(UCHAR)(bitsHigh >> 24), (UCHAR)(bitsHigh >> 16), (UCHAR)(bitsHigh >> 8), (UCHAR)bitsHigh,
                    (UCHAR)(bitsLow >> 24),  (UCHAR)(bitsLow >> 16),  (UCHAR)(bitsLow >> 8),  (UCHAR)bitsLow};
    XcSHAUpdate(pbSHAContext, len, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            pbDigest[4 * i + j] = (UCHAR)(c->State[i] >> (24 - 8 * j));
}

// HMAC-SHA1 over the concatenation of two inputs (either may be empty).
XBAPI void NTAPI XcHMAC(const UCHAR *pbKeyMaterial, ULONG cbKeyMaterial, const UCHAR *pbData, ULONG cbData,
                        const UCHAR *pbData2, ULONG cbData2, PUCHAR HmacData) {
    UCHAR key[64] = {}, ctx[116], inner[20];
    if (cbKeyMaterial > 64) {
        XcSHAInit(ctx);
        XcSHAUpdate(ctx, pbKeyMaterial, cbKeyMaterial);
        XcSHAFinal(ctx, key);
    } else {
        RtlCopyMemory(key, pbKeyMaterial, cbKeyMaterial);
    }
    UCHAR pad[64];
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x36;
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, pad, 64);
    if (cbData)
        XcSHAUpdate(ctx, pbData, cbData);
    if (cbData2)
        XcSHAUpdate(ctx, pbData2, cbData2);
    XcSHAFinal(ctx, inner);
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x5C;
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, pad, 64);
    XcSHAUpdate(ctx, inner, 20);
    XcSHAFinal(ctx, HmacData);
}

struct XC_RC4_STATE {
    UCHAR S[256];
    UCHAR I, J;
};

XBAPI void NTAPI XcRC4Key(PUCHAR pbKeyStruct, ULONG dwKeyLength, const UCHAR *pbKey) {
    auto r = (XC_RC4_STATE *)pbKeyStruct;
    for (int i = 0; i < 256; i++)
        r->S[i] = (UCHAR)i;
    UCHAR j = 0;
    for (int i = 0; i < 256; i++) {
        j = (UCHAR)(j + r->S[i] + pbKey[i % dwKeyLength]);
        UCHAR t = r->S[i];
        r->S[i] = r->S[j];
        r->S[j] = t;
    }
    r->I = r->J = 0;
}

XBAPI void NTAPI XcRC4Crypt(PUCHAR pbKeyStruct, ULONG dwInputLength, PUCHAR pbInput) {
    auto r = (XC_RC4_STATE *)pbKeyStruct;
    UCHAR i = r->I, j = r->J;
    for (ULONG n = 0; n < dwInputLength; n++) {
        i = (UCHAR)(i + 1);
        j = (UCHAR)(j + r->S[i]);
        UCHAR t = r->S[i];
        r->S[i] = r->S[j];
        r->S[j] = t;
        pbInput[n] ^= r->S[(UCHAR)(r->S[i] + r->S[j])];
    }
    r->I = i, r->J = j;
}
