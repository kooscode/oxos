// Xc public-key and block ciphers: BSAFE-format RSA keys, modular exponentiation, PKCS#1 signature check, DES/3DES
// (FIPS 46-3), and the replaceable crypto vector (spec/15). Numbers are little-endian arrays of 32-bit words.
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

// --- Multiprecision helpers

static int BnCompare(const ULONG *a, const ULONG *b, ULONG n) {
    for (ULONG i = n; i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static ULONG BnSub(ULONG *r, const ULONG *a, const ULONG *b, ULONG n) {
    ULONG borrow = 0;
    for (ULONG i = 0; i < n; i++) {
        ULONGLONG d = (ULONGLONG)a[i] - b[i] - borrow;
        r[i] = (ULONG)d;
        borrow = (ULONG)(d >> 63);
    }
    return borrow;
}

// Montgomery multiplication: r = a * b * R^-1 mod m (R = 2^(32n)); t has n + 2 words.
static void BnMontMul(ULONG *r, const ULONG *a, const ULONG *b, const ULONG *m, ULONG minv, ULONG n, ULONG *t) {
    for (ULONG i = 0; i < n + 2; i++)
        t[i] = 0;
    for (ULONG i = 0; i < n; i++) {
        ULONGLONG carry = 0;
        for (ULONG j = 0; j < n; j++) {
            ULONGLONG v = (ULONGLONG)a[j] * b[i] + t[j] + carry;
            t[j] = (ULONG)v;
            carry = v >> 32;
        }
        ULONGLONG v = (ULONGLONG)t[n] + carry;
        t[n] = (ULONG)v;
        t[n + 1] = (ULONG)(v >> 32);
        ULONG u = t[0] * minv;
        carry = ((ULONGLONG)u * m[0] + t[0]) >> 32;
        for (ULONG j = 1; j < n; j++) {
            v = (ULONGLONG)u * m[j] + t[j] + carry;
            t[j - 1] = (ULONG)v;
            carry = v >> 32;
        }
        v = (ULONGLONG)t[n] + carry;
        t[n - 1] = (ULONG)v;
        t[n] = t[n + 1] + (ULONG)(v >> 32);
    }
    if (t[n] || BnCompare(t, m, n) >= 0)
        BnSub(r, t, m, n);
    else
        for (ULONG i = 0; i < n; i++)
            r[i] = t[i];
}

// x = x * 2^32 mod m, by shifting a word in and reducing bit by bit (used to build R^2 mod m).
static void BnShiftModOnce(ULONG *x, const ULONG *m, ULONG n) {
    for (int bit = 0; bit < 32; bit++) {
        ULONG top = x[n - 1] >> 31;
        for (ULONG i = n; i-- > 1;)
            x[i] = (x[i] << 1) | (x[i - 1] >> 31);
        x[0] <<= 1;
        if (top || BnCompare(x, m, n) >= 0)
            BnSub(x, x, m, n);
    }
}

// a = b^c mod d (n words). Returns FALSE for d == 0.
static BOOLEAN BnModExp(ULONG *a, const ULONG *b, const ULONG *c, const ULONG *d, ULONG n) {
    ULONG top = n;
    while (top && !d[top - 1])
        top--;
    if (!top)
        return FALSE;
    auto mem = (ULONG *)ExAllocatePoolWithTag((5 * n + 2) * 4, 'xpmB');
    if (!mem)
        return FALSE;
    ULONG *base = mem, *acc = mem + n, *rr = mem + 2 * n, *tmp = mem + 3 * n, *t = mem + 4 * n;
    // Reduce b below d (b may be as large as d's word count).
    for (ULONG i = 0; i < n; i++)
        base[i] = b[i];
    for (ULONG i = 0; i < n; i++)
        acc[i] = 0;
    if (d[0] & 1) {
        ULONG minv = 1; // -d^-1 mod 2^32 by Newton iteration
        for (int i = 0; i < 5; i++)
            minv *= 2 - d[0] * minv;
        minv = 0 - minv;
        // rr = R^2 mod d
        for (ULONG i = 0; i < n; i++)
            rr[i] = 0;
        rr[0] = 1;
        for (ULONG i = 0; i < 2 * n; i++)
            BnShiftModOnce(rr, d, n);
        while (BnCompare(base, d, n) >= 0)
            BnSub(base, base, d, n);
        BnMontMul(base, base, rr, d, minv, n, t); // to Montgomery form
        for (ULONG i = 0; i < n; i++)
            tmp[i] = 0;
        tmp[0] = 1;
        BnMontMul(acc, tmp, rr, d, minv, n, t); // 1 in Montgomery form
        for (ULONG i = n * 32; i-- > 0;) {
            BnMontMul(acc, acc, acc, d, minv, n, t);
            if ((c[i / 32] >> (i % 32)) & 1)
                BnMontMul(acc, acc, base, d, minv, n, t);
        }
        BnMontMul(a, acc, tmp, d, minv, n, t); // back from Montgomery form (tmp = 1)
    } else {
        // Even modulus: square-and-multiply with bit-serial reduction (slow, rarely used).
        auto mulmod = [&](ULONG *r, const ULONG *x, const ULONG *y) {
            for (ULONG i = 0; i < n; i++)
                tmp[i] = 0;
            for (ULONG i = n * 32; i-- > 0;) {
                ULONG overflow = tmp[n - 1] >> 31; // tmp = 2 * tmp mod d
                for (ULONG k = n; k-- > 1;)
                    tmp[k] = (tmp[k] << 1) | (tmp[k - 1] >> 31);
                tmp[0] <<= 1;
                if (overflow || BnCompare(tmp, d, n) >= 0)
                    BnSub(tmp, tmp, d, n);
                if ((y[i / 32] >> (i % 32)) & 1) { // tmp += x mod d
                    ULONGLONG carry = 0;
                    for (ULONG k = 0; k < n; k++) {
                        ULONGLONG s = (ULONGLONG)tmp[k] + x[k] + carry;
                        tmp[k] = (ULONG)s;
                        carry = s >> 32;
                    }
                    if (carry || BnCompare(tmp, d, n) >= 0)
                        BnSub(tmp, tmp, d, n);
                }
            }
            for (ULONG k = 0; k < n; k++)
                r[k] = tmp[k];
        };
        while (BnCompare(base, d, n) >= 0)
            BnSub(base, base, d, n);
        acc[0] = 1;
        for (ULONG i = n * 32; i-- > 0;) {
            mulmod(acc, acc, acc);
            if ((c[i / 32] >> (i % 32)) & 1)
                mulmod(acc, acc, base);
        }
        for (ULONG i = 0; i < n; i++)
            a[i] = acc[i];
    }
    ExFreePool(mem);
    return TRUE;
}

// --- BSAFE keys: {magic 'RSA1'/'RSA2', keylen, bitlen, datalen, pubexp, modulus[keylen], private parts...}
struct BSAFE_KEY {
    ULONG Magic, KeyLength, BitLength, DataLength, PublicExponent;
    UCHAR Modulus[1];
};
constexpr ULONG RSA1 = 0x31415352, RSA2 = 0x32415352;

static ULONG XcpKeyWords(const BSAFE_KEY *k) {
    ULONG q = k->BitLength >> 6; // 64-bit units, plus one (or two when the length is not a multiple of 64)
    return ((k->BitLength >> 1) & 0x1F ? q + 2 : q + 1) * 2;
}

static ULONG NTAPI XcpPKEncPublic(PUCHAR pbPubKey, PUCHAR pbInput, PUCHAR pbOutput) {
    auto k = (const BSAFE_KEY *)pbPubKey;
    if (k->Magic != RSA1)
        return 0;
    ULONG n = XcpKeyWords(k);
    if (k->PublicExponent == 1) {
        RtlCopyMemory(pbOutput, pbInput, n * 4);
        return 1;
    }
    if (BnCompare((const ULONG *)pbInput, (const ULONG *)k->Modulus, n) >= 0)
        return 0;
    auto e = (ULONG *)ExAllocatePoolWithTag(n * 4, 'xpcX');
    if (!e)
        return 0;
    RtlZeroMemory(e, n * 4);
    e[0] = k->PublicExponent;
    BOOLEAN ok = BnModExp((ULONG *)pbOutput, (const ULONG *)pbInput, e, (const ULONG *)k->Modulus, n);
    ExFreePool(e);
    return ok;
}

static ULONG NTAPI XcpPKDecPrivate(PUCHAR pbPrvKey, PUCHAR pbInput, PUCHAR pbOutput) {
    auto k = (const BSAFE_KEY *)pbPrvKey;
    if (k->Magic != RSA2)
        return 0;
    ULONG n = XcpKeyWords(k);
    if (k->PublicExponent == 1) {
        RtlCopyMemory(pbOutput, pbInput, n * 4);
        return 1;
    }
    // modulus, prime1, prime2, exponent1, exponent2, coefficient (keylen/2 each), private exponent.
    const UCHAR *privateExponent = k->Modulus + k->KeyLength + 5 * (k->KeyLength / 2);
    return BnModExp((ULONG *)pbOutput, (const ULONG *)pbInput, (const ULONG *)privateExponent,
                    (const ULONG *)k->Modulus, n);
}

static ULONG NTAPI XcpPKGetKeyLen(PUCHAR pbPubKey) { return ((const BSAFE_KEY *)pbPubKey)->KeyLength; }

// The decrypted block (little-endian) is: digest reversed | [DigestInfo] | 00 | FF.. | 01 | 00.
static BOOLEAN NTAPI XcpVerifyPKCS1Signature(PUCHAR pbSig, PUCHAR pbPubKey, PUCHAR pbDigest) {
    auto k = (const BSAFE_KEY *)pbPubKey;
    if (k->Magic != RSA1)
        return FALSE;
    auto buf = (PUCHAR)ExAllocatePoolWithTag(2 * k->KeyLength + 8, 'xpcX');
    if (!buf)
        return FALSE;
    PUCHAR in = buf + k->KeyLength + 8;
    RtlZeroMemory(in, k->KeyLength);
    RtlCopyMemory(in, pbSig, (k->BitLength + 7) / 8);
    BOOLEAN ok = (BOOLEAN)XcpPKEncPublic(pbPubKey, in, buf);
    for (ULONG i = 0; ok && i < 20; i++)
        ok = buf[i] == pbDigest[19 - i];
    ULONG pos = 20;
    static const UCHAR sha1Prefix[] = {0x14, 0x04, 0x00, 0x05, 0x1A, 0x02, 0x03, 0x0E, // DigestInfo for SHA-1,
                                       0x2B, 0x05, 0x06, 0x09, 0x30, 0x21, 0x30};       // reversed
    if (ok && RtlCompareMemory(buf + 20, sha1Prefix, sizeof(sha1Prefix)) == sizeof(sha1Prefix))
        pos += sizeof(sha1Prefix);
    ULONG last = k->DataLength;
    ok = ok && buf[pos] == 0 && buf[last] == 0 && buf[last - 1] == 1;
    for (ULONG i = pos + 1; ok && i < last - 1; i++)
        ok = buf[i] == 0xFF;
    ExFreePool(buf);
    return ok;
}

static ULONG NTAPI XcpModExp(PULONG pA, PULONG pB, PULONG pC, PULONG pD, ULONG dwN) {
    return BnModExp(pA, pB, pC, pD, dwN);
}

// --- DES / triple DES (FIPS 46-3). Key table: 16 rounds x 2 words (DES, 128 bytes); 3 tables for 3DES.

static const UCHAR DesPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                 35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                 46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const UCHAR DesPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                 26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const UCHAR DesShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const UCHAR DesIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const UCHAR DesFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
static const UCHAR DesE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                               12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                               22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const UCHAR DesP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                               2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const UCHAR DesS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0,  15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11,
     9,  5,  3,  8, 4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9, 7,  3,  10, 5,  0,  15, 12, 8,  2,  4,  9, 1,  7,
     5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0,  5,  10, 3,  13, 4,  7, 15, 2,  8, 14, 12, 0, 1,  10,
     6,  9,  11, 5,  0,  14, 7,  11, 10, 4, 13, 1,  5,  8,  12, 6,  9,  3,  2,  15, 13, 8, 10, 1,  3, 15, 4,  2,
     11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,  6, 10, 2,  8, 5,  14,
     12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,  1,  10, 13, 0,  6, 9,  8,  7,
     4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15, 0, 3,  4,  7, 2,  12,
     1,  10, 14, 9,  10, 6,  9,  0,  12, 11, 7, 13, 15, 1,  3,  14, 5,  2,  8,  4,  3,  15, 0,  6,  10, 1, 13, 8,
     9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,  13, 1, 5,  0, 15, 10,
     3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3,  0,  14, 11, 8,  12, 7, 1,  14, 2,  13,
     6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9, 5,  6,  1, 13, 14,
     0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9, 5,  15, 10,
     11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,  1, 10, 14, 3, 5,  12,
     2,  15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,  0,  5,  9,  2,  6,  11, 13, 8, 1,  4, 10, 7,
     9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,  7, 4,  12, 5, 6,  11,
     0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,  2,  1,  14, 7,  4, 10, 8,  13,
     15, 12, 9,  0,  3,  5,  6,  11}};

static ULONGLONG DesPermute(ULONGLONG in, const UCHAR *table, int n, int inBits) {
    ULONGLONG out = 0;
    for (int i = 0; i < n; i++)
        out = (out << 1) | ((in >> (inBits - table[i])) & 1);
    return out;
}

struct DES_TABLE {
    ULONGLONG SubKey[16]; // 48-bit subkeys
};
static_assert(sizeof(DES_TABLE) == 128);

static void DesKeySchedule(DES_TABLE *t, const UCHAR *key) {
    ULONGLONG k = 0;
    for (int i = 0; i < 8; i++)
        k = (k << 8) | key[i];
    ULONGLONG cd = DesPermute(k, DesPC1, 56, 64);
    ULONG c = (ULONG)(cd >> 28) & 0xFFFFFFF, d = (ULONG)cd & 0xFFFFFFF;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < DesShifts[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFF;
        }
        t->SubKey[r] = DesPermute(((ULONGLONG)c << 28) | d, DesPC2, 48, 56);
    }
}

static void DesBlock(const DES_TABLE *t, UCHAR *out, const UCHAR *in, bool encrypt) {
    ULONGLONG v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | in[i];
    v = DesPermute(v, DesIP, 64, 64);
    ULONG l = (ULONG)(v >> 32), r = (ULONG)v;
    for (int round = 0; round < 16; round++) {
        ULONGLONG x = DesPermute(r, DesE, 48, 32) ^ t->SubKey[encrypt ? round : 15 - round];
        ULONG f = 0;
        for (int s = 0; s < 8; s++) {
            ULONG six = (ULONG)(x >> (42 - 6 * s)) & 63;
            f = (f << 4) | DesS[s][((six & 0x20) | ((six & 1) << 4)) | ((six >> 1) & 15)];
        }
        f = (ULONG)DesPermute(f, DesP, 32, 32);
        ULONG nl = r;
        r = l ^ f;
        l = nl;
    }
    v = DesPermute(((ULONGLONG)r << 32) | l, DesFP, 64, 64);
    for (int i = 7; i >= 0; i--, v >>= 8)
        out[i] = (UCHAR)v;
}

static void NTAPI XcpDESKeyParity(PUCHAR pbKey, ULONG dwKeyLength) {
    for (ULONG i = 0; i < dwKeyLength; i++)
        if (!(__builtin_popcount(pbKey[i]) & 1))
            pbKey[i] ^= 1;
}

static void NTAPI XcpKeyTable(ULONG dwCipher, PUCHAR pbKeyTable, PUCHAR pbKey) {
    auto t = (DES_TABLE *)pbKeyTable;
    DesKeySchedule(&t[0], pbKey);
    if (dwCipher) { // triple DES: three keys
        DesKeySchedule(&t[1], pbKey + 8);
        DesKeySchedule(&t[2], pbKey + 16);
    }
}

// dwOp: 1 encrypt, 0 decrypt. 3DES is encrypt-decrypt-encrypt.
static void NTAPI XcpBlockCrypt(ULONG dwCipher, PUCHAR pbOutput, PUCHAR pbInput, PUCHAR pbKeyTable, ULONG dwOp) {
    auto t = (const DES_TABLE *)pbKeyTable;
    if (!dwCipher) {
        DesBlock(&t[0], pbOutput, pbInput, dwOp != 0);
        return;
    }
    UCHAR a[8], b[8];
    if (dwOp) {
        DesBlock(&t[0], a, pbInput, true);
        DesBlock(&t[1], b, a, false);
        DesBlock(&t[2], pbOutput, b, true);
    } else {
        DesBlock(&t[2], a, pbInput, false);
        DesBlock(&t[1], b, a, true);
        DesBlock(&t[0], pbOutput, b, false);
    }
}

static void NTAPI XcpBlockCryptCBC(ULONG dwCipher, ULONG dwInputLength, PUCHAR pbOutput, PUCHAR pbInput,
                                   PUCHAR pbKeyTable, ULONG dwOp, PUCHAR pbFeedback) {
    for (ULONG off = 0; off + 8 <= dwInputLength; off += 8) {
        UCHAR block[8];
        if (dwOp) {
            for (int i = 0; i < 8; i++)
                block[i] = pbInput[off + i] ^ pbFeedback[i];
            XcpBlockCrypt(dwCipher, pbOutput + off, block, pbKeyTable, 1);
            RtlCopyMemory(pbFeedback, pbOutput + off, 8);
        } else {
            UCHAR saved[8];
            RtlCopyMemory(saved, pbInput + off, 8);
            XcpBlockCrypt(dwCipher, block, pbInput + off, pbKeyTable, 0);
            for (int i = 0; i < 8; i++)
                pbOutput[off + i] = block[i] ^ pbFeedback[i];
            RtlCopyMemory(pbFeedback, saved, 8);
        }
    }
}

static ULONG NTAPI XcpCryptService(ULONG, PVOID) { return 0; }

// --- The crypto vector: exported Xc* functions call through it; XcUpdateCrypto replaces entries.
XBAPI void NTAPI XcpSHAInit(PUCHAR);
XBAPI void NTAPI XcpSHAUpdate(PUCHAR, const UCHAR *, ULONG);
XBAPI void NTAPI XcpSHAFinal(PUCHAR, PUCHAR);
XBAPI void NTAPI XcpRC4Key(PUCHAR, ULONG, const UCHAR *);
XBAPI void NTAPI XcpRC4Crypt(PUCHAR, ULONG, PUCHAR);
XBAPI void NTAPI XcpHMAC(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);

struct CRYPTO_VECTOR {
    void(NTAPI *SHAInit)(PUCHAR);
    void(NTAPI *SHAUpdate)(PUCHAR, const UCHAR *, ULONG);
    void(NTAPI *SHAFinal)(PUCHAR, PUCHAR);
    void(NTAPI *RC4Key)(PUCHAR, ULONG, const UCHAR *);
    void(NTAPI *RC4Crypt)(PUCHAR, ULONG, PUCHAR);
    void(NTAPI *HMAC)(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);
    ULONG(NTAPI *PKEncPublic)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *PKDecPrivate)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *PKGetKeyLen)(PUCHAR);
    BOOLEAN(NTAPI *VerifyPKCS1Signature)(PUCHAR, PUCHAR, PUCHAR);
    ULONG(NTAPI *ModExp)(PULONG, PULONG, PULONG, PULONG, ULONG);
    void(NTAPI *DESKeyParity)(PUCHAR, ULONG);
    void(NTAPI *KeyTable)(ULONG, PUCHAR, PUCHAR);
    void(NTAPI *BlockCrypt)(ULONG, PUCHAR, PUCHAR, PUCHAR, ULONG);
    void(NTAPI *BlockCryptCBC)(ULONG, ULONG, PUCHAR, PUCHAR, PUCHAR, ULONG, PUCHAR);
    ULONG(NTAPI *CryptService)(ULONG, PVOID);
};
static_assert(sizeof(CRYPTO_VECTOR) == 64);

static CRYPTO_VECTOR XcpVector = {XcpSHAInit,      XcpSHAUpdate,        XcpSHAFinal,       XcpRC4Key,
                                  XcpRC4Crypt,     XcpHMAC,             XcpPKEncPublic,    XcpPKDecPrivate,
                                  XcpPKGetKeyLen,  XcpVerifyPKCS1Signature, XcpModExp,     XcpDESKeyParity,
                                  XcpKeyTable,     XcpBlockCrypt,       XcpBlockCryptCBC,  XcpCryptService};

XBAPI void NTAPI XcSHAInit(PUCHAR c) { XcpVector.SHAInit(c); }
XBAPI void NTAPI XcSHAUpdate(PUCHAR c, const UCHAR *p, ULONG n) { XcpVector.SHAUpdate(c, p, n); }
XBAPI void NTAPI XcSHAFinal(PUCHAR c, PUCHAR d) { XcpVector.SHAFinal(c, d); }
XBAPI void NTAPI XcRC4Key(PUCHAR s, ULONG n, const UCHAR *k) { XcpVector.RC4Key(s, n, k); }
XBAPI void NTAPI XcRC4Crypt(PUCHAR s, ULONG n, PUCHAR p) { XcpVector.RC4Crypt(s, n, p); }
XBAPI void NTAPI XcHMAC(const UCHAR *k, ULONG kl, const UCHAR *d1, ULONG l1, const UCHAR *d2, ULONG l2, PUCHAR out) {
    XcpVector.HMAC(k, kl, d1, l1, d2, l2, out);
}
XBAPI ULONG NTAPI XcPKEncPublic(PUCHAR k, PUCHAR in, PUCHAR out) { return XcpVector.PKEncPublic(k, in, out); }
XBAPI ULONG NTAPI XcPKDecPrivate(PUCHAR k, PUCHAR in, PUCHAR out) { return XcpVector.PKDecPrivate(k, in, out); }
XBAPI ULONG NTAPI XcPKGetKeyLen(PUCHAR k) { return XcpVector.PKGetKeyLen(k); }
XBAPI BOOLEAN NTAPI XcVerifyPKCS1Signature(PUCHAR s, PUCHAR k, PUCHAR d) {
    return XcpVector.VerifyPKCS1Signature(s, k, d);
}
XBAPI ULONG NTAPI XcModExp(PULONG a, PULONG b, PULONG c, PULONG d, ULONG n) { return XcpVector.ModExp(a, b, c, d, n); }
XBAPI void NTAPI XcDESKeyParity(PUCHAR k, ULONG n) { XcpVector.DESKeyParity(k, n); }
XBAPI void NTAPI XcKeyTable(ULONG c, PUCHAR t, PUCHAR k) { XcpVector.KeyTable(c, t, k); }
XBAPI void NTAPI XcBlockCrypt(ULONG c, PUCHAR o, PUCHAR i, PUCHAR t, ULONG op) { XcpVector.BlockCrypt(c, o, i, t, op); }
XBAPI void NTAPI XcBlockCryptCBC(ULONG c, ULONG n, PUCHAR o, PUCHAR i, PUCHAR t, ULONG op, PUCHAR fb) {
    XcpVector.BlockCryptCBC(c, n, o, i, t, op, fb);
}
XBAPI ULONG NTAPI XcCryptService(ULONG op, PVOID args) { return XcpVector.CryptService(op, args); }

// Replace the non-NULL entries; the reference also returns a 64-byte "ROM hash" (not reproduced: zeros).
XBAPI void NTAPI XcUpdateCrypto(CRYPTO_VECTOR *pNewVector, PUCHAR pROMHash) {
    auto dst = (PVOID *)&XcpVector;
    auto src = (PVOID *)pNewVector;
    for (int i = 0; pNewVector && i < 16; i++)
        if (src[i])
            dst[i] = src[i];
    if (pROMHash)
        RtlZeroMemory(pROMHash, 64);
}
