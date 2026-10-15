// 64-bit division for the MSVC-style compiler helpers (src/arch/int64.S). Only 32-bit divides and
// constant shifts are used here so the compiler cannot call back into the helpers.
#include <xk/types.h>

extern "C" ULONGLONG XkUDivMod64(ULONGLONG n, ULONGLONG d, ULONGLONG *rem) {
    if (!(d >> 32)) {
        ULONG dl = (ULONG)d, nh = (ULONG)(n >> 32), nl = (ULONG)n;
        ULONG qh = nh / dl, rh = nh % dl, ql, rl;
        __asm__("divl %4" : "=a"(ql), "=d"(rl) : "a"(nl), "d"(rh), "rm"(dl));
        *rem = rl;
        return ((ULONGLONG)qh << 32) | ql;
    }
    ULONGLONG q = 0, r = 0;
    for (int i = 0; i < 64; i++) {
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    *rem = r;
    return q;
}

extern "C" LONGLONG XkSDivMod64(LONGLONG n, LONGLONG d, LONGLONG *rem) {
    bool nn = n < 0, dn = d < 0;
    ULONGLONG r, q = XkUDivMod64(nn ? 0 - (ULONGLONG)n : (ULONGLONG)n, dn ? 0 - (ULONGLONG)d : (ULONGLONG)d, &r);
    *rem = nn ? -(LONGLONG)r : (LONGLONG)r; // remainder takes the dividend's sign
    return nn != dn ? -(LONGLONG)q : (LONGLONG)q;
}
