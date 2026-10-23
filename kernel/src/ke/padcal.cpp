// Experiment XK_EXPERIMENT_PADCAL: the NV2A memory pad calibration the retail 2BL performs (spec/stock/21 §3, Complex
// 4627 variant), done at kernel entry instead (the 2BL also runs it from RAM). Our 2BL skips it, as Cromwell does;
// this tests whether the missing calibration is what freezes real consoles. The table is Microsoft data and is read
// from an untracked file (experimental/padcal_4627.inc, generated from the 4627 BIOS image); nothing of it is
// committed.
#include <xk/arch.h>
#include <xk/serial.h>

#ifdef XK_EXPERIMENT_PADCAL
#include "../../../experimental/padcal_4627.inc"

volatile ULONG KiPadCal[16]; // diagnostics: {A, B, strap, set offset, 9 values, done}

static inline volatile ULONG &Nv(ULONG o) { return *(volatile ULONG *)(0xFD000000 + o); }

static void KiPadDelay(ULONG ticks) {
    ULONG lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    ULONGLONG end = ((ULONGLONG)hi << 32 | lo) + ticks, now;
    do {
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        now = (ULONGLONG)hi << 32 | lo;
    } while (now < end);
}

static ULONG KiPadMeasure(ULONG firstSelector) {
    ULONG sum = 0;
    for (ULONG i = 0; i < 4; i++) {
        Nv(0x1228) = firstSelector + i;
        KiPadDelay(10000);
        sum += Nv(0x121C);
    }
    return sum >> 2;
}

// Rows bracketing the measurement and the weight of the upper one (20-bit fixed point, clamped to 0..1).
static void KiPadSelect(ULONG m, const UCHAR *bp, const UCHAR *set, const UCHAR **hi, const UCHAR **lo, LONG *f) {
    ULONG b0, b1;
    if (m <= bp[2]) { // breakpoint bytes at bp[0], bp[2], ... (every other byte)
        *hi = set + 3 * 19, *lo = set + 4 * 19, b0 = bp[0], b1 = bp[2];
    } else if (m >= bp[6]) {
        *hi = set + 0 * 19, *lo = set + 1 * 19, b0 = bp[6], b1 = bp[8];
    } else if (m < bp[4]) {
        *hi = set + 2 * 19, *lo = set + 3 * 19, b0 = bp[2], b1 = bp[4];
    } else {
        *hi = set + 1 * 19, *lo = set + 2 * 19, b0 = bp[4], b1 = bp[6];
    }
    LONGLONG v = b1 > b0 ? (((LONGLONG)m - (LONGLONG)b0) << 20) / (LONGLONG)(b1 - b0) : 0;
    *f = v < 0 ? 0 : v > 0x100000 ? 0x100000 : (LONG)v;
}

static ULONG KiPadValue(const UCHAR *hi, const UCHAR *lo, LONG f, ULONG k) {
    ULONGLONG v = (ULONGLONG)hi[k] * (ULONG)f + (ULONGLONG)lo[k] * (ULONG)(0x100000 - f);
    return (ULONG)((v + 0x80000) >> 20) & 0xF;
}

static ULONG KiPadPattern(ULONG even, ULONG odd) { // nibbles alternate: odd ones from `odd`, even ones from `even`
    ULONG v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | odd << 4 | even;
    return v;
}

extern "C" void KiPadCalibrate() {
    ULONG strap = Nv(0x101000);
    const UCHAR *set = KiPadTable + ((strap & 0xC0000) ? 0x0C : 0x6B);
    Nv(0x1220) = 0x000F0F0F;
    ULONG a = KiPadMeasure(0x40004), b = KiPadMeasure(0x40000);
    Nv(0x1220) = 0;
    const UCHAR *hi, *lo;
    LONG f;
    ULONG o[16], c[3];
    KiPadSelect(a, KiPadTable + 2, set, &hi, &lo, &f);
    for (ULONG k = 0; k < 16; k++)
        o[k] = KiPadValue(hi, lo, f, k);
    KiPadSelect(b, KiPadTable + 3, set, &hi, &lo, &f);
    for (ULONG k = 0; k < 3; k++)
        c[k] = KiPadValue(hi, lo, f, 0x10 + k);
    ULONG c1214 = 0;
    for (int i = 0; i < 4; i++)
        c1214 = ((c1214 << 3 | c[2]) << 2 | c[1]) << 3 | c[0];
    const ULONG regs[9] = {0x122C, 0x123C, 0x1230, 0x1240, 0x1234, 0x1244, 0x1238, 0x1248, 0x1214};
    const ULONG vals[9] = {KiPadPattern(o[0], o[1]),   KiPadPattern(o[2], o[3]),   KiPadPattern(o[4], o[5]),
                           KiPadPattern(o[6], o[7]),   KiPadPattern(o[8], o[9]),   KiPadPattern(o[10], o[11]),
                           KiPadPattern(o[12], o[13]), KiPadPattern(o[14], o[15]), c1214};
    __asm__ volatile("wbinvd" ::: "memory");
    for (ULONG i = 0; i < 9; i++)
        if (vals[i]) // Complex 4627 skips a zero value
            Nv(regs[i]) = vals[i];
    KiPadCal[0] = a;
    KiPadCal[1] = b;
    KiPadCal[2] = strap;
    KiPadCal[3] = (ULONG)(set - KiPadTable);
    for (ULONG i = 0; i < 9; i++)
        KiPadCal[4 + i] = vals[i];
    KiPadCal[13] = 1;
}

extern "C" void KiPadReport() {
    XK_SERIAL("padcal: A %#lx B %#lx strap %08lx set +%#lx; 122C %08lx 123C %08lx 1230 %08lx 1240 %08lx 1234 %08lx "
              "1244 %08lx 1238 %08lx 1248 %08lx 1214 %08lx\n",
              KiPadCal[0], KiPadCal[1], KiPadCal[2], KiPadCal[3], KiPadCal[4], KiPadCal[5], KiPadCal[6], KiPadCal[7],
              KiPadCal[8], KiPadCal[9], KiPadCal[10], KiPadCal[11], KiPadCal[12]);
}
#endif
