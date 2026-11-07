// Open BIOS image: the 2BL's C part, running from RAM at 0x400000 (spec/stock/01 §5). It does what the reference 2BL
// does for the kernel, as observed at the kernel's entry point:
//   - the kernel image, decompressed to physical 0x10000 (it runs at 0x80010000);
//   - a page directory at 0xF000: 0-256 MB identity-mapped and again at 0x80000000 with 4 MB pages, entry 0x300
//     mapping the directory itself, the NV2A registers (0xFD000000, 16 MB) uncached, the top 4 MB (flash);
//   - CR4 = PSE | OSFXSR | OSXMMEXCPT, CR3 = 0xF000, CR0 = PG | WP | NE | ET | PE;
//   - KernelEntry(arg0, keys) called with cdecl. arg0 = 'OXOS' marks our key block (spec/stock/01 §1): no EEPROM key
//     but the EEPROM HMAC states taken from XboxDev/cromwell at build time, and the retail certificate key.
#include <xk/inflate.h>

using xk_inflate::Inflate;
using xk_inflate::Inflater;
typedef unsigned char UCHAR;
typedef unsigned long ULONG;

// Written by tools/mkbios.py at bank offset 0x6000.
struct XK_BOOT_PAYLOAD {
    ULONG Magic;                      // 'OXKP'
    ULONG KernelOffset, KernelPacked; // raw deflate stream, bank-relative
    ULONG KernelSize;                 // the flat kernel image
    ULONG KeysOffset, KeysSize;       // the key block handed to the kernel
    ULONG SplashOffset, SplashSize;   // the boot splash copy in this bank (0, 0 if none; read by the kernel)
};

// The boot splash, copied into RAM for the kernel (spec/oxos_internal/00_boot_splash.md): "XKSPLASH" header (20 bytes)
// + vector scene, at SPLASH_RAM, described by the record at SPLASH_RECORD (in page 2, free at this point, next to the
// pad calibration record at 0x2300). The kernel decodes it from RAM: decoding it straight from flash, byte by byte over
// LPC while the kernel runs, coincided with bits of RAM changing on a Modxo console (2026-10-28).
constexpr ULONG SPLASH_RAM = 0x00C00000, SPLASH_RAM_MAX = 0x40000, SPLASH_RECORD = 0x2380,
                SPLASH_RECORD_MAGIC = 0x5053584F;
constexpr ULONG SPLASH_BANK1 = 0xFFF40000, SPLASH_HEADER = 20;

constexpr ULONG BANK = 0xFFFC0000, PAYLOAD = BANK + 0x6000, PAYLOAD_MAGIC = 0x504B584F, BOOT_MAGIC = 0x534F584F;
constexpr ULONG KERNEL_PHYS = 0x10000, PAGE_DIRECTORY = 0xF000;

static UCHAR BlKeys[1024];

extern "C" void *memcpy(void *d, const void *s, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ((UCHAR *)d)[i] = ((const UCHAR *)s)[i];
    return d;
}

extern "C" void *memset(void *d, int c, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        ((UCHAR *)d)[i] = (UCHAR)c;
    return d;
}

// --- Helpers
static void BlPost(UCHAR code) {
    __asm__ volatile("outb %0, $0x80" ::"a"(code));
}

[[noreturn]] static void BlFail(UCHAR code) {
    BlPost(code);
    for (;;)
        __asm__ volatile("cli; hlt");
}

// Port I/O.
static inline void BlOut8(unsigned short port, UCHAR v) {
    __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port));
}

static inline void BlOut16(unsigned short port, unsigned short v) {
    __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port));
}

static inline UCHAR BlIn8(unsigned short port) {
    UCHAR v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline unsigned short BlIn16(unsigned short port) {
    unsigned short v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void BlOut32(unsigned short port, ULONG v) {
    __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port));
}

static inline ULONG BlIn32(unsigned short port) {
    ULONG v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// PCI configuration space (mechanism 1, bus 0).
static ULONG BlPciRead(ULONG Device, ULONG Register) {
    BlOut32(0xCF8, 0x80000000 | Device << 11 | Register);
    return BlIn32(0xCFC);
}

static void BlPciWrite(ULONG Device, ULONG Register, ULONG Value) {
    BlOut32(0xCF8, 0x80000000 | Device << 11 | Register);
    BlOut32(0xCFC, Value);
}

// An NV2A register (the GPU's registers at 0xFD000000).
static volatile ULONG &BlNv(ULONG Offset) {
    return *(volatile ULONG *)(0xFD000000 + Offset);
}

// The reference's TSC wait: end = start + Ticks (64-bit); stop when the high half passes the end's high half, or when
// it is not below it and the low half has reached the end's low half.
[[gnu::noinline]] static void BlTscWait(ULONG Ticks) {
    ULONG lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    ULONG endLo = lo + Ticks, endHi = hi + (endLo < lo ? 1 : 0);
    do
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    while (!(hi > endHi) && lo < endLo);
}

// --- SMC start-up challenge (spec/stock/01 §5). On a console the SMC (SMBus slave 0x20) holds four challenge bytes in
// registers 0x1C-0x1F at power-on and expects the boot loader to answer in registers 0x20 and 0x21 within a few
// seconds; without an answer it resets the console and flashes the LED red and green. The retail 2BL answers it, so
// ours must. Answer and SMBus sequence as in XboxDev/cromwell's boot_rom/2bPicResponseAction.c (GPL-2.0).
constexpr unsigned short SMB = 0xC000; // MCPX SMBus host: status, control +2, address +4, data +6, command +8

// One SMBus byte transfer with the SMC; returns the byte read (Read), 0 for a write, -1 on failure. Bounded, so a
// console without an answering SMC (or an emulator) still boots.
static int BlSmcTransfer(bool Read, UCHAR Register, UCHAR Value) {
    for (ULONG spin = 0; (BlIn16(SMB) & 0x0800) && spin < 1000000; spin++) {
        // Wait while another master is using the bus (bounded).
    }
    for (int retry = 0; retry < 400; retry++) {
        BlOut8(SMB + 4, Read ? 0x21 : 0x20);
        BlOut8(SMB + 8, Register);
        if (!Read)
            BlOut8(SMB + 6, Value);
        BlOut16(SMB, 0xFFFF); // clear earlier status
        BlOut8(SMB + 2, Read ? 0x0A : 0x1A);
        UCHAR status = 0;
        for (ULONG spin = 0; !(status & 0x36) && spin < 1000000; spin++)
            status = BlIn8(SMB);
        if (status & 0x10)
            return Read ? BlIn8(SMB + 6) : 0;
    }
    return -1;
}

static void BlAnswerSmcChallenge() {
    int c[4];
    for (int i = 0; i < 4; i++) {
        if ((c[i] = BlSmcTransfer(true, (UCHAR)(0x1C + i), 0)) < 0)
            return;
    }
    UCHAR b1 = 0x33, b2 = 0xED;
    UCHAR b3 = (UCHAR)((c[0] << 2) ^ (c[1] + 0x39) ^ (c[2] >> 2) ^ (c[3] + 0x63));
    UCHAR b4 = (UCHAR)((c[0] + 0x0B) ^ (c[1] >> 2) ^ (c[2] + 0x1B));
    for (int n = 0; n < 4; n++) {
        b1 = (UCHAR)(b1 + (b2 ^ b3));
        b2 = (UCHAR)(b2 + (b1 ^ b4));
    }
    BlSmcTransfer(false, 0x20, b1);
    BlSmcTransfer(false, 0x21, b2);
    BlSmcTransfer(false, 0x01, 0); // as the reference does after the answer
}

// --- Chipset and GPU set-up after the challenge (spec/stock/01 §5), as XboxDev/cromwell's 2BL does
// (boot_rom/2bBootStartBios.c, GPL-2.0): the X-codes leave the NV2A's second memory configuration register, the
// AGP bridge and the top of memory unset. Without them the GPU reads RAM with the wrong layout (a screen of banded
// noise on hardware); xemu ignores these registers.

// The memory size is decided before this (boot/start.S, the retail X-codes' probe, spec 22 §6.3). An earlier OXOS probe
// that wrote 0x100204 = 0x11338000 on a 1.4 console stopped it in the memory manager (2026-10-22): that row is for
// eight smaller chips, not the four 128 Mbit chips of a retail 64 MB console, which keep 0x11448000.

// Bridge chip (device 1) and host set-up in the reference 2BL's order (spec/stock/21 §2): BlMain does step 1 (device 1
// register 0x80 byte 0 = 0x02, the MCPX boot ROM off), the SMC challenge with step 5 (register 0x8C = 0x40000000: bit
// 30, which the IDE controller and the NIC need; without it both read 0xFFFFFFFF on a 1.4 console) and the pad
// calibration (BlPadCalibrate) before this.
static void BlSetUpChipset() {
    // Steps 8-11: AGP, bridge (bus 0 device 1) and host (device 0) enables, then the host's register 0x6C toggled.
    BlPciWrite(1, 0x54, BlPciRead(1, 0x54) | 0x88000000);
    BlPciWrite(0, 0x64, BlPciRead(0, 0x64) | 0x88000000);
    ULONG v = BlPciRead(0, 0x6C); // leaves 0xCF8 selecting device 0, register 0x6C
    BlOut32(0xCFC, v & ~1u);
    BlOut32(0xCFC, v);
    BlPciWrite(0, 0x80, 0x100);
    // Step 12: on MCPX revision D1 and later, device 1 register 0xC8 = 0x8F00 (the revision byte read as a byte cycle).
    BlOut32(0xCF8, 0x80000000 | 1 << 11 | 0x08);
    if (BlIn8(0xCFC) >= 0xD1)
        BlPciWrite(1, 0xC8, 0x8F00);
    // Not written here, as in the retail 2BLs (spec/stock/21 §2, spec/stock/22 §6.3): the NV2A memory configuration PFB
    // 0x100200 / 0x100204 and the host bridge's top of memory 0x84. The memory-size probe sets them (boot/start.S; a
    // retail 64 MB console: 0x03070003, 0x11448000, 64 MB); overwriting them with cromwell's 128 MB values (as earlier
    // OXOS images did) gave the GPU the wrong memory layout: a scrambled picture on 1.4 and 1.x consoles.
}

// --- The GPU core clock

// The GPU core clock: NVPLL coefficients (NV2A 0x680500) and the PLL coefficient select (0x68050C). The X-codes write
// 0x00011C01 (M 1, N 28, P 1: 233 MHz), then 0x000A0400. On some cold boots the 2BL finds other values:
// - NVPLL 0x0006F3FF (0.25 MHz) with the select right: titles crawl, no vertical-blank interrupt;
// - NVPLL 0x00012501 with the select 0x000A0000 (bit 10 clear): the core clock does not come from NVPLL, a write to
//   NVPLL alone does not change it, and titles crawl too.
// The 2BL writes both, in the X-codes' order, until they read back right (3 tries): at its start, before anything
// uses the GPU, and again before the kernel. Record at physical 0x23E0 for the kernel's log: 'NVPL', NVPLL and the
// select at 2BL start, NVPLL and the select before the kernel.
constexpr ULONG NVPLL = 0x680500, NVPLL_XCODES = 0x00011C01, PLL_SELECT = 0x68050C, PLL_SELECT_XCODES = 0x000A0400;
constexpr ULONG NVPLL_RECORD = 0x23E0, NVPLL_RECORD_MAGIC = 0x4C50564E;

static void BlCheckGpuClock(ULONG Slot) {
    auto record = (volatile ULONG *)NVPLL_RECORD;
    record[Slot] = BlNv(NVPLL);
    record[Slot + 1] = BlNv(PLL_SELECT);
    for (ULONG tries = 0; tries < 3 &&
         (BlNv(NVPLL) != NVPLL_XCODES || BlNv(PLL_SELECT) != PLL_SELECT_XCODES); tries++) {
        BlNv(NVPLL) = NVPLL_XCODES; // read back: the X-codes' own writes did not hold on those boots
        BlNv(PLL_SELECT) = PLL_SELECT_XCODES;
    }
}

// --- X-code checks (debug)

// X-code check (debug, spec/oxos_internal/26 §4): on some cold boots settings the X-codes write do not hold (the GPU
// clock, DRAM refresh). When the 2BL starts, every register in this list (the X-codes, kernel/boot/xcodes.S: the
// last value written, under a mask) is read back; the ones that differ go to a record at physical 0x2400 for the
// kernel's log: 'XCHK', the count, then {address, value read} pairs; again before the kernel, at 0x2600. Addresses:
// 0x0Fxxxxxx = NV2A register (now at 0xFD000000), 0x8xxxxxxx = PCI configuration address. Left out: the memory size
// (start.S decides it), the DRAM mode commands, the strap and bridge registers the table changes with
// read-modify-write.

struct BL_XCODE_VALUE {
    ULONG Address, Value, Mask;
};
#ifdef MCPXREVD5
// 1.6: OXOS's own program (boot/xcodes_1_6.inc, spec/stock/24 §7). Also left out: the registers it sets from EEPROM
// bytes 0xC0-0xC2 (0x10B8, 0x1214, 0x122C).
static const BL_XCODE_VALUE BlXcodeValues[] = {
    {0x0F0010B0, 0x01000010, ~0u}, {0x0F0010B4, 0, ~0u},          {0x0F0010BC, 0x00005860, ~0u},
    {0x0F0010C4, 0xAAAA0000, ~0u}, {0x0F0010C8, 0x00007D67, ~0u}, {0x0F0010CC, 0x66660000, ~0u},
    {0x0F0010D4, 0xF, ~0u},        {0x0F0010D8, 0, ~0u},          {0x0F0010DC, 0, ~0u},
    {0x0F0010E8, 0x04000000, ~0u}, {0x0F001210, 0, ~0u},          {0x0F001218, 0, ~0u},
    {0x0F001220, 0, ~0u},          {0x0F001228, 0, ~0u},          {0x0F001230, 0xFFFFFFFF, ~0u},
    {0x0F001234, 0xAAAAAAAA, ~0u}, {0x0F001238, 0xAAAAAAAA, ~0u}, {0x0F00123C, 0x8B8B8B8B, ~0u},
    {0x0F001240, 0xFFFFFFFF, ~0u}, {0x0F001244, 0x8B8B8B8B, ~0u}, {0x0F001248, 0x8B8B8B8B, ~0u},
    {0x0F00124C, 0xAA8BAA8B, ~0u}, {0x0F001250, 0x00005474, ~0u}, {0x0F001264, 0, ~0u},
    {0x0F100210, 0x80000000, 0x80000000}, {0x0F100228, 0x081205FF, ~0u}, {0x0F100328, 1, ~0u},
    {0x0F10032C, 0xFFFFCFFF, ~0u}, {0x0F100330, 0x84848888, ~0u}, {0x0F100338, 0xDF, ~0u},
    {0x0F100410, 0x11000016, ~0u}, {0x0F680500, 0x00011C01, ~0u}, {0x0F68050C, 0x000A0400, ~0u},
    {0x80000340, 0xF0F0C0C0, ~0u}, {0x80000344, 0, ~0u},          {0x8000035C, 0, ~0u},
    {0x8000036C, 0x01230801, ~0u}, {0x80000804, 3, 0xFFFF},       {0x8000084C, 0xFDDE, ~0u},
    {0x80000884, 0x8001, ~0u},     {0x8000089C, 0x871CC707, ~0u}, {0x80000904, 1, 0xFFFF},
    {0x80000914, 0xC001, ~0u},     {0x80000918, 0xC201, ~0u},     {0x8000F004, 7, 0xFFFF},
    {0x8000F018, 0x00010100, 0xFFFFFF}, {0x8000F020, 0xFDF0FD00, ~0u}, {0x8000F024, 0xF7F0F000, ~0u},
    {0x8000F04C, 1, ~0u},          {0x80010004, 7, 0xFFFF},       {0x80010010, 0xFD000000, ~0u},
    {0x80010014, 0xF0000000, ~0u},
};
#else
static const BL_XCODE_VALUE BlXcodeValues[] = {
    {0x0F0010B0, 0x07633461, ~0u}, {0x0F0010B4, 0, ~0u},          {0x0F0010B8, 0xFFFF0000, ~0u},
    {0x0F0010BC, 0x00005866, ~0u}, {0x0F0010C4, 0x0351C858, ~0u}, {0x0F0010C8, 0x30007D67, ~0u},
    {0x0F0010CC, 0x66660000, ~0u}, {0x0F0010D4, 9, ~0u},          {0x0F0010D8, 0, ~0u},
    {0x0F0010DC, 0xA0423635, ~0u}, {0x0F0010E8, 0x0C6558C6, ~0u}, {0x0F001210, 0x10, ~0u},
    {0x0F001214, 0x09090909, ~0u}, {0x0F001218, 0x00010000, ~0u}, {0x0F001220, 0, ~0u},
    {0x0F001228, 0, ~0u},          {0x0F00122C, 0xAAAAAAAA, ~0u}, {0x0F001230, 0xFFFFFFFF, ~0u},
    {0x0F001234, 0xAAAAAAAA, ~0u}, {0x0F001238, 0xAAAAAAAA, ~0u}, {0x0F00123C, 0x8B8B8B8B, ~0u},
    {0x0F001240, 0xFFFFFFFF, ~0u}, {0x0F001244, 0x8B8B8B8B, ~0u}, {0x0F001248, 0x8B8B8B8B, ~0u},
    {0x0F00124C, 0xAA8BAA8B, ~0u}, {0x0F001250, 0x0000AA8B, ~0u}, {0x0F001264, 0, ~0u},
    {0x0F100210, 0x80000000, 0x80000000}, {0x0F100228, 0x081205FF, ~0u}, {0x0F100328, 1, ~0u},
    {0x0F10032C, 0xFFFFCFFF, ~0u}, {0x0F100330, 0x84848888, ~0u}, {0x0F100338, 0xDF, ~0u},
    {0x0F100410, 0x11000016, ~0u}, {0x0F680500, 0x00011C01, ~0u}, {0x0F68050C, 0x000A0400, ~0u},
    {0x80000340, 0xF0F0C0C0, ~0u}, {0x80000344, 0x00C00000, ~0u}, {0x8000035C, 0x04070000, ~0u},
    {0x8000036C, 0x01230801, ~0u}, {0x80000804, 3, 0xFFFF},       {0x80000810, 0x8001, ~0u},
    {0x8000084C, 0xFDDE, ~0u},     {0x80000884, 0x8001, ~0u},     {0x8000089C, 0x871CC707, ~0u},
    {0x80000904, 1, 0xFFFF},       {0x80000914, 0xC001, ~0u},     {0x80000918, 0xC201, ~0u},
    {0x8000F004, 7, 0xFFFF},       {0x8000F018, 0x00010100, 0xFFFFFF}, {0x8000F020, 0xFDF0FD00, ~0u},
    {0x8000F024, 0xF7F0F000, ~0u}, {0x8000F04C, 1, ~0u},          {0x80010004, 7, 0xFFFF},
    {0x80010010, 0xFD000000, ~0u}, {0x80010014, 0xF0000000, ~0u},
};
#endif
constexpr ULONG XCODE_RECORD = 0x2400, XCODE_RECORD_END = 0x2600, XCODE_RECORD_MAGIC = 0x4B484358,
                XCODE_RECORD_MAX = 60;

static void BlCheckXcodeValues(ULONG Record) {
    auto record = (volatile ULONG *)Record;
    ULONG n = 0;
    for (const auto &x : BlXcodeValues) {
        ULONG v;
        if (x.Address & 0x80000000) {
            BlOut32(0xCF8, x.Address);
            v = BlIn32(0xCFC);
        } else {
            v = BlNv(x.Address - 0x0F000000);
        }
        if ((v & x.Mask) != (x.Value & x.Mask) && n < XCODE_RECORD_MAX) {
            record[2 + 2 * n] = x.Address;
            record[3 + 2 * n] = v;
            n++;
        }
    }
    record[1] = n;
    record[0] = XCODE_RECORD_MAGIC;
}

// --- NV2A memory pad calibration (spec/stock/21 §3): the drive/slew/delay compensation of the memory pads, measured
// and set before the kernel is decompressed. Two methods, chosen in BlMain as Cerbios chooses them:
// - NV2A revisions below D3 (consoles 1.0-1.5), the table method (§3.7a), exactly as Cerbios does it: Cerbios's
//   row-set selection, the reference 4627 measurement and arithmetic (all 32-bit, including the clamp of 1/2^20 above
//   the top breakpoint and the zero store described below).
// - D3 and later (1.6), the EEPROM method (§3.7), exactly as the 5838 2BL does it: the five rows from the EEPROM, the
//   5838 measurement (two groups per reading, the steadier kept) and arithmetic (a signed division in the lowest
//   segment, a weight clamped to 1.0 above the top breakpoint, all nine registers written).
// The table, in Cerbios's layout, is the one the flash header's 0x7C points at (through the 16 MB flash window):
// breakpoints at bytes 2-11 (the 5838 method's at 0x0E-0x17), row sets at +0x18, +0x77 and +0xD6
// (boot/padcal_table.inc, placed by boot/xcodes.S). A table whose breakpoints do not increase (the reference would
// divide by zero), or an EEPROM that does not answer, leaves the X-codes' values.

constexpr UCHAR EEPROM_SLAVE = 0xA8;
constexpr ULONG PAD_ROW = 0x13, PAD_ROWS = 5;

// Four readings of 0x121C, selector First..First+3 in 0x1228, each after a 10 000-tick wait, into R.
static void BlPadRead4(ULONG First, ULONG *R) {
    for (ULONG i = 0; i < 4; i++) {
        BlNv(0x1228) = First + i;
        BlTscWait(10000);
        R[i] = BlNv(0x121C);
    }
}

// Four readings of 0x121C, selector First..First+3; their average (4627).
static ULONG BlPadMeasure(ULONG First) {
    ULONG r[4];
    BlPadRead4(First, r);
    return (r[0] + r[1] + r[2] + r[3]) >> 2;
}

// Of two groups of four readings, the steadier one's average: the first group when the sum of its absolute
// differences between neighbours is strictly smaller (5838).
static ULONG BlPadSteadier(const ULONG *G1, const ULONG *G2) {
    int d1 = 0, d2 = 0;
    for (int i = 0; i < 3; i++) {
        int a = (int)G1[i] - (int)G1[i + 1], b = (int)G2[i] - (int)G2[i + 1];
        d1 += a < 0 ? -a : a;
        d2 += b < 0 ? -b : b;
    }
    const ULONG *g = d1 < d2 ? G1 : G2;
    return (g[0] + g[1] + g[2] + g[3]) >> 2;
}

static ULONG BlPadRound(ULONG x) {
    return (x + 0x80000) >> 20 & 0xF;
}

// The row pair and weight for a measurement M against breakpoints B[0], B[2], B[4], B[6], B[8] (every other byte):
// Hi is weighted by F, Lo by 0x100000 - F. Retail16: the 5838 arithmetic (a signed division in the lowest segment, a
// weight above 1.0 clamped to 1.0); otherwise the 4627 one (unsigned, such a weight becomes 1/2^20).
template <bool Retail16>
static void BlPadSelect(ULONG M, const volatile UCHAR *B, const volatile UCHAR *Set, const volatile UCHAR **Hi,
                        const volatile UCHAR **Lo, ULONG *F) {
    const volatile UCHAR *row[5] = {Set, Set + 0x13, Set + 0x26, Set + 0x39, Set + 0x4C};
    ULONG b0 = B[0], b1 = B[2], b2 = B[4], b3 = B[6], b4 = B[8], f;
    if (M <= b1) { // the lowest segment: a negative quotient (sign bit set) becomes 0
        if constexpr (Retail16)
            f = (ULONG)((int)((M - b0) << 20) / (int)(b1 - b0));
        else
            f = ((M - b0) << 20) / (b1 - b0);
        if (f & 0x80000000)
            f = 0;
        *Hi = row[3];
        *Lo = row[4];
    } else if (M >= b3) { // the highest segment: a weight above 1.0 is clamped
        f = ((M - b3) << 20) / (b4 - b3);
        if (f > 0x100000)
            f = Retail16 ? 0x100000 : 1;
        *Hi = row[0];
        *Lo = row[1];
    } else if (M < b2) {
        f = ((M - b1) << 20) / (b2 - b1);
        *Hi = row[2];
        *Lo = row[3];
    } else {
        f = ((M - b2) << 20) / (b3 - b2);
        *Hi = row[1];
        *Lo = row[2];
    }
    *F = f;
}

static ULONG BlPadNibbles(ULONG Even, ULONG Odd) { // bytes Odd << 4 | Even, as the reference builds them
    ULONG v = Odd;
    for (int i = 0; i < 7; i++)
        v = v << 4 | (i & 1 ? Odd : Even);
    return v;
}

// Stores to NV2A registers that change the DRAM interface (drive, slew, delay, timing): from the first to the last of
// them the CPU must not touch memory (spec/stock/21 §3.6a). Pairs is an {offset, value} array, all computed before.
// In one asm block with interrupts off: wbinvd; a dry pass that runs the same loop and loads but stores nothing,
// bringing its code and the array into the caches; the storing pass, which then runs from the caches only (no calls,
// no stack).
[[gnu::noinline]] static void BlWriteWindow(const ULONG *Pairs, ULONG Count) {
    ULONG pass, index, value, offset;
    __asm__ volatile("pushfl\n\t"
                     "cli\n\t"
                     "wbinvd\n\t"
                     "xorl %[pass], %[pass]\n"
                     "1:\n\t"
                     "xorl %[index], %[index]\n"
                     "2:\n\t"
                     "movl 4(%[pairs],%[index],8), %[value]\n\t"
                     "movl (%[pairs],%[index],8), %[offset]\n\t"
                     "testl %[pass], %[pass]\n\t"
                     "jz 3f\n\t"
                     "movl %[value], (%[nv],%[offset])\n"
                     "3:\n\t"
                     "incl %[index]\n\t"
                     "cmpl %[count], %[index]\n\t"
                     "jb 2b\n\t"
                     "incl %[pass]\n\t"
                     "cmpl $2, %[pass]\n\t"
                     "jb 1b\n\t"
                     "popfl"
                     : [pass] "=&r"(pass), [index] "=&r"(index), [value] "=&r"(value), [offset] "=&r"(offset)
                     : [pairs] "r"(Pairs), [nv] "r"(0xFD000000u), [count] "m"(Count)
                     : "memory", "cc");
}

// What the calibration measured and wrote, for the kernel's debug log (physical 0x2300, a free page at that point).
struct BL_PAD_RESULT {
    ULONG Magic; // 'PADC' table method, 'PADE' EEPROM method, 'PADN' when it did not run (no table, no EEPROM)
    ULONG A, B, Strap, Values[9];
};
constexpr ULONG PAD_TABLE = 0x43444150, PAD_EEPROM = 0x45444150, PAD_NONE = 0x4E444150; // 'PADC' 'PADE' 'PADN'

// From the measurements A and B, the rows (Set, 19 bytes each) and the breakpoints (Breaks: A at bytes 0, 2, ..., 8,
// B at 1, 3, ..., 9): the nine values, written in the write window, and the record. ZeroStore: a register whose value
// is 0 is not written, the 4627 reference storing that 0 at NV2A offset 0 instead (its read-only identification
// register); the 5838 one writes all nine.
template <bool Retail16>
static void BlPadApply(ULONG A, ULONG B, const volatile UCHAR *Breaks, const volatile UCHAR *Set, ULONG Magic,
                       ULONG Strap) {
    const volatile UCHAR *hi, *lo;
    ULONG f, o[16];
    BlPadSelect<Retail16>(A, Breaks, Set, &hi, &lo, &f);
    for (int k = 0; k < 16; k++)
        o[k] = BlPadRound(hi[k] * f + lo[k] * (0x100000 - f));
    BlPadSelect<Retail16>(B, Breaks + 1, Set, &hi, &lo, &f);
    ULONG c12 = BlPadRound(lo[0x12] * (0x100000 - f) + hi[0x12] * f);
    ULONG c10 = BlPadRound(hi[0x10] * f + lo[0x10] * (0x100000 - f));
    ULONG c11 = BlPadRound(lo[0x11] * (0x100000 - f) + hi[0x11] * f);
    ULONG v1214 = c12;
    for (int i = 0; i < 4; i++) {
        v1214 = v1214 << 2 | c11;
        v1214 = v1214 << 3 | c10;
        if (i < 3)
            v1214 = v1214 << 3 | c12;
    }
    static const ULONG reg[9] = {0x122C, 0x123C, 0x1230, 0x1240, 0x1234, 0x1244, 0x1238, 0x1248, 0x1214};
    ULONG val[9] = {BlPadNibbles(o[0], o[1]),   BlPadNibbles(o[2], o[3]),   BlPadNibbles(o[4], o[5]),
                    BlPadNibbles(o[6], o[7]),   BlPadNibbles(o[8], o[9]),   BlPadNibbles(o[10], o[11]),
                    BlPadNibbles(o[12], o[13]), BlPadNibbles(o[14], o[15]), v1214};
    alignas(32) ULONG pairs[18];
    for (int i = 0; i < 9; i++) {
        pairs[2 * i] = (Retail16 || val[i]) ? reg[i] : 0;
        pairs[2 * i + 1] = val[i];
    }
    BlWriteWindow(pairs, 9);
    auto result = (volatile BL_PAD_RESULT *)0x2300;
    result->A = A;
    result->B = B;
    result->Strap = Strap;
    for (int i = 0; i < 9; i++)
        result->Values[i] = val[i];
    result->Magic = Magic;
}

// The table that the flash header's 0x7C points at, or nullptr when its breakpoints from byte First on (five pairs) do
// not increase.
static const volatile UCHAR *BlPadTable(ULONG First) {
    auto table = (const volatile UCHAR *)(*(volatile ULONG *)0xFF00007C - 0x1000000);
    for (ULONG i = First + 2; i < First + 10; i++) {
        if (table[i] >= table[i + 2])
            return nullptr;
    }
    return table;
}

// NV2A below D3: the table method.
[[gnu::noinline]] static void BlPadCalibrate(bool Upgraded) {
    ((volatile BL_PAD_RESULT *)0x2300)->Magic = PAD_NONE;
    const volatile UCHAR *table = BlPadTable(0);
    if (!table) {
        BlPost(0x33);
        return;
    }
    ULONG strap = BlNv(0x101000);
    auto set = table + (Upgraded ? 0xD6 : (strap & 0xC0000) ? 0x18 : 0x77);
    BlNv(0x1220) = 0x000F0F0F;
    ULONG a = BlPadMeasure(0x40004), b = BlPadMeasure(0x40000);
    BlNv(0x1220) = 0;
    BlPadApply<false>(a, b, table + 2, set, PAD_TABLE, strap);
    BlPost(0x31);
}

// One SMBus word read of the EEPROM (slave 0xA8), as the 5838 2BL does it: the status cleared by writing back what it
// reads, start (word data), then wait while busy (bit 3); a collision (bit 1) starts again, an error (bit 5 or 2)
// fails, done (bit 4) gives the word. Bounded, unlike the reference: -1 when the EEPROM does not answer.
static int BlEepromReadWord(UCHAR Command) {
    for (int attempt = 0; attempt < 400; attempt++) {
        BlOut8(SMB + 4, EEPROM_SLAVE | 1);
        BlOut8(SMB + 8, Command);
        BlOut16(SMB, BlIn16(SMB));
        BlOut8(SMB + 2, 0x0B); // start, word data
        UCHAR status = BlIn8(SMB);
        for (ULONG spin = 0; (status & 0x08) && spin < 1000000; spin++)
            status = BlIn8(SMB);
        if (status & 0x02)
            continue;
        if ((status & 0x24) || !(status & 0x10))
            return -1;
        return BlIn16(SMB + 6);
    }
    return -1;
}

// NV2A D3 and later (1.6): the EEPROM method. Row r is EEPROM bytes 0xC4 + 10r .. 0xCD + 10r: each of the first nine
// gives two row bytes (its high nibble first), the tenth is row byte 0x12. Readings in the order A, B, A, B.
[[gnu::noinline]] static void BlPadCalibrateFromEeprom() {
    ((volatile BL_PAD_RESULT *)0x2300)->Magic = PAD_NONE;
    const volatile UCHAR *table = BlPadTable(0x0C);
    if (!table) {
        BlPost(0x33);
        return;
    }
    UCHAR eeprom[50];
    for (int i = 0; i < 25; i++) {
        int word = BlEepromReadWord((UCHAR)(0xC4 + 2 * i));
        if (word < 0) {
            BlPost(0x34);
            return;
        }
        eeprom[2 * i] = (UCHAR)word;
        eeprom[2 * i + 1] = (UCHAR)(word >> 8);
    }
    UCHAR rows[PAD_ROWS * PAD_ROW];
    for (ULONG r = 0; r < PAD_ROWS; r++) {
        const UCHAR *e = eeprom + 10 * r;
        for (int j = 0; j < 9; j++) {
            rows[PAD_ROW * r + 2 * j] = e[j] >> 4;
            rows[PAD_ROW * r + 2 * j + 1] = e[j] & 0xF;
        }
        rows[PAD_ROW * r + 0x12] = e[9];
    }
    ULONG g[4][4];
    BlNv(0x1220) = 0x000F0F0F;
    BlPadRead4(0x40004, g[0]);
    BlPadRead4(0x40000, g[1]);
    BlPadRead4(0x40004, g[2]);
    BlPadRead4(0x40000, g[3]);
    BlNv(0x1220) = 0;
    ULONG a = BlPadSteadier(g[0], g[2]), b = BlPadSteadier(g[1], g[3]);
    BlPadApply<true>(a, b, table + 0x0E, rows, PAD_EEPROM, BlNv(0x101000));
    BlPost(0x32);
}

// --- Network boot (NETBOOT=1 images): built on the port, PCI and timing helpers above.

#if XK_NETBOOT
#include "netboot.h"
#endif

// --- The boot splash's copy for the kernel

// A splash blob ("XKSPLASH", version 2: a vector scene; length; Room bytes available): its whole size, 0 if there is
// none.
static ULONG BlSplashSize(const volatile UCHAR *Blob, ULONG Room) {
    static const char magic[8] = {'X', 'K', 'S', 'P', 'L', 'A', 'S', 'H'};
    if (Room < SPLASH_HEADER)
        return 0;
    for (int i = 0; i < 8; i++) {
        if (Blob[i] != (UCHAR)magic[i])
            return 0;
    }
    ULONG version = *(const volatile ULONG *)(Blob + 8), length = *(const volatile ULONG *)(Blob + 12);
    return version == 2 && length <= Room - SPLASH_HEADER ? SPLASH_HEADER + length : 0;
}

// Copy the splash to SPLASH_RAM: flash bank 1 (a 1 MiB image), else the copy that the payload of Bank points to
// (the flash boot bank, or a netboot download). The record says where it is (size 0: none).
static void BlCopySplash(ULONG Bank) {
    auto record = (volatile ULONG *)SPLASH_RECORD;
    record[0] = SPLASH_RECORD_MAGIC;
    record[1] = SPLASH_RAM;
    record[2] = 0;
    auto bank1 = (const volatile UCHAR *)SPLASH_BANK1;
    const volatile UCHAR *from = bank1;
    ULONG size = BlSplashSize(bank1, 0x40000);
    if (!size) {
        auto payload = (const volatile ULONG *)(Bank + 0x6000);
        ULONG offset = payload[6], room = payload[7];
        if (payload[0] == PAYLOAD_MAGIC && offset >= 0x6040 && offset < 0x40000 && room <= 0x40000 - offset) {
            from = (const volatile UCHAR *)(Bank + offset);
            size = BlSplashSize(from, room);
        }
    }
    if (!size || size > SPLASH_RAM_MAX)
        return;
    auto to = (UCHAR *)SPLASH_RAM;
    for (ULONG i = 0; i < size; i++)
        to[i] = from[i];
    record[2] = size;
}

// --- The GPU's memory interface settings

// The GPU's memory interface settings (NV2A 0x10B0-0x10E8), as the X-codes write them, in their order. On some cold
// boots some of them read other values when the 2BL starts, and the screen is then garbled: 0x10BC 0xFF885066 with
// every line of text drawn as one glyph repeated to the screen edge, 0x10B0 / 0x10E8 with speckled textures
// (spec/oxos_internal/26 §4). When any differs, the 2BL writes them all again, through the write window.
#ifdef MCPXREVD5
// 1.6: the values boot/xcodes_1_6.inc writes, in its order (spec/stock/24 §5.3). 0x10B8 is left as it is: the
// X-codes take it from the EEPROM.
static const ULONG BlMemoryInterface[] = {
    0x10B0, 0x01000010, 0x10CC, 0x66660000, 0x10D4, 0xF,        0x10B4, 0,          0x10BC, 0x5860,
    0x10C4, 0xAAAA0000, 0x10C8, 0x00007D67, 0x10D8, 0,          0x10DC, 0,          0x10E8, 0x04000000,
};
#else
static const ULONG BlMemoryInterface[] = {
    0x10B0, 0x07633461, 0x10CC, 0x66660000, 0x10B8, 0xFFFF0000, 0x10D4, 9,          0x10B4, 0,          0x10BC, 0x5866,
    0x10C4, 0x0351C858, 0x10C8, 0x30007D67, 0x10D8, 0,          0x10DC, 0xA0423635, 0x10E8, 0x0C6558C6,
};
#endif
constexpr ULONG MEMORY_INTERFACE_COUNT = sizeof(BlMemoryInterface) / 8;

static void BlRestoreMemoryInterface() {
    bool differ = false;
    for (ULONG i = 0; i < MEMORY_INTERFACE_COUNT; i++)
        differ |= BlNv(BlMemoryInterface[2 * i]) != BlMemoryInterface[2 * i + 1];
    if (!differ)
        return;
    alignas(32) ULONG pairs[2 * MEMORY_INTERFACE_COUNT];
    for (ULONG i = 0; i < 2 * MEMORY_INTERFACE_COUNT; i++)
        pairs[i] = BlMemoryInterface[i];
    BlWriteWindow(pairs, MEMORY_INTERFACE_COUNT);
}

#if XK_XCODE_ENTRY_SNAPSHOT
// Debug (serial builds, spec/oxos_internal/28 §4 G0): the memory set-up the X-codes leave, read at the 2BL's start
// before anything changes it, and the EEPROM bytes the 1.6 X-codes take their memory parameters from (0xC0-0xC3,
// read later, after the SMC challenge). Record at physical 0x2800 for the kernel's log: 'X16S', the count, then
// {what, value} pairs. "What" is an NV2A offset, a PCI configuration address (bit 31 set), or 0xEE0000nn for the
// EEPROM word at nn.
constexpr ULONG SNAPSHOT_RECORD = 0x2800, SNAPSHOT_MAGIC = 0x53363158, SNAPSHOT_MAX = 40;

static const ULONG BlSnapshotNv[] = {
    0x101000, 0x10B0, 0x10B8, 0x10CC, 0x10D4, 0x10B4, 0x10BC, 0x10C4, 0x10C8, 0x10D8, 0x10DC, 0x10E8,
    0x1210,   0x1214, 0x1218, 0x122C, 0x1250, 0x1002C4, 0x1002CC, 0x100200, 0x100204, 0x680500, 0x68050C,
};
static const ULONG BlSnapshotPci[] = {
    0x80000084, 0x80000344, 0x8000035C, 0x8000036C, 0x8000083C, 0x80000884, 0x80000808, 0x8000103C, 0x8000183C,
};

static void BlSnapshotAdd(ULONG What, ULONG Value) {
    auto record = (volatile ULONG *)SNAPSHOT_RECORD;
    ULONG n = record[1];
    if (n >= SNAPSHOT_MAX)
        return;
    record[2 + 2 * n] = What;
    record[3 + 2 * n] = Value;
    record[1] = n + 1;
}

static void BlSnapshotRegisters() {
    auto record = (volatile ULONG *)SNAPSHOT_RECORD;
    record[0] = SNAPSHOT_MAGIC;
    record[1] = 0;
    for (ULONG offset : BlSnapshotNv)
        BlSnapshotAdd(offset, BlNv(offset));
    for (ULONG address : BlSnapshotPci) {
        BlOut32(0xCF8, address);
        BlSnapshotAdd(address, BlIn32(0xCFC));
    }
}

static void BlSnapshotEeprom() {
    for (UCHAR command = 0xC0; command < 0xC4; command += 2)
        BlSnapshotAdd(0xEE000000 | command, (ULONG)BlEepromReadWord(command));
}
#else
static void BlSnapshotRegisters() {
}

static void BlSnapshotEeprom() {
}
#endif

// --- The 2BL's main line (from boot/start.S): checks and repairs, the chipset, the kernel's payload, then the kernel

extern "C" [[noreturn]] void BlMain() {
    BlPost(0x20);
    BlSnapshotRegisters();
    BlCheckXcodeValues(XCODE_RECORD); // first: before the 2BL changes anything
    BlRestoreMemoryInterface();
    ((volatile ULONG *)NVPLL_RECORD)[0] = NVPLL_RECORD_MAGIC;
    BlCheckGpuClock(1); // before the pad calibration, like the X-codes (the clock first, then the pads)
    // The reference 2BL's order (spec/stock/21 §2): device 1 register 0x80 byte 0 = 0x02 (a byte cycle), the SMC
    // challenge and SMC 0x01 = 0, device 1 register 0x8C = 0x40000000, the pad calibration, then the AGP/host steps.
    BlOut32(0xCF8, 0x80000000 | 1 << 11 | 0x80);
    BlOut8(0xCFC, 0x02);
    BlAnswerSmcChallenge();
    BlPciWrite(1, 0x8C, 0x40000000);
    BlSnapshotEeprom();
    // Cerbios's choice of method (spec/stock/21 §3.7a): "upgraded" memory = top of memory (host 0x84) 256 MB, or
    // 128 MB with PCI 0:2:0 and 0:3:0 register 0x3C low bytes 0x0F and 0xF0. NV2A D3 and later without that: the
    // EEPROM method (1.6). Otherwise the table, with the third row set when upgraded.
    {
        ULONG top = BlPciRead(0, 0x84);
        UCHAR irq2 = (UCHAR)BlPciRead(2, 0x3C), irq3 = (UCHAR)BlPciRead(3, 0x3C);
        bool upgraded128 = irq2 == 0x0F && irq3 == 0xF0 && top == 0x07FFFFFF;
        bool eepromMethod = *(volatile UCHAR *)0xFD000000 >= 0xD3 && !upgraded128; // a byte read, as there
        if (eepromMethod)
            BlPadCalibrateFromEeprom();
        else
            BlPadCalibrate(top == 0x0FFFFFFF || upgraded128);
    }
    BlSetUpChipset();
    // Boot status on the front LED (spec/oxos_internal/03_debug_serial.md §6): slow green blink while the 2BL runs.
    BlSmcTransfer(false, 0x08, 0x03);
    BlSmcTransfer(false, 0x07, 1);
    BlPost(0x23);
    ULONG bank = BANK; // where the kernel and key block come from: this flash bank, or a network download
#if XK_NETBOOT
    BlSmcTransfer(false, 0x08, 0xFF); // solid orange while it asks the network (spec/oxos_internal/09_netboot.md)
    BlSmcTransfer(false, 0x07, 1);
    if (const UCHAR *download = netboot::Boot())
        bank = (ULONG)download;
    BlSmcTransfer(false, 0x08, 0x03);
    BlSmcTransfer(false, 0x07, 1);
#endif
    auto payload = (const volatile XK_BOOT_PAYLOAD *)(bank + (PAYLOAD - BANK));
    if (payload->Magic != PAYLOAD_MAGIC || payload->KeysSize > sizeof(BlKeys))
        BlFail(0xE0);

    BlCopySplash(bank); // before the kernel is in RAM
    Inflater s = {(const UCHAR *)(bank + payload->KernelOffset), payload->KernelPacked, 0, 0, 0,
                  (UCHAR *)KERNEL_PHYS, payload->KernelSize, 0, false};
    if (!Inflate(s) || s.OutPos != payload->KernelSize)
        BlFail(0xE1);
    BlPost(0x21);
    memcpy(BlKeys, (const void *)(bank + payload->KeysOffset), payload->KeysSize);

    auto pd = (volatile ULONG *)PAGE_DIRECTORY;
    for (ULONG i = 0; i < 1024; i++)
        pd[i] = 0;
    for (ULONG i = 0; i < 0x40; i++) {
        pd[i] = (i << 22) | 0xE3;         // present, writable, accessed, dirty, 4 MB
        pd[0x200 + i] = (i << 22) | 0xE3; // the same at 0x80000000
    }
    pd[0x300] = PAGE_DIRECTORY | 0x63;
    for (ULONG i = 0; i < 4; i++)
        pd[0x3F4 + i] = (0xFD000000 + (i << 22)) | 0xFB; // + write-through, cache-disabled
    pd[0x3FF] = 0xFFC00000 | 0xE3;

    auto image = (const UCHAR *)KERNEL_PHYS;
    ULONG nt = *(const ULONG *)(image + 0x3C);
    ULONG entry = *(const ULONG *)(image + nt + 0x34) + *(const ULONG *)(image + nt + 0x28); // ImageBase + entry RVA
    // DRAM refresh must be on when the kernel starts (spec/oxos_internal/20). boot/start.S makes sure of it after the
    // X-codes; checked again here, after everything the 2BL does, with its own record (physical 0x23D0: 'RFEN', then
    // the value found).
    if (!(BlNv(0x100210) & 0x80000000)) {
        ((volatile ULONG *)0x23D0)[1] = BlNv(0x100210);
        BlNv(0x100210) = 0x80000000;
        ((volatile ULONG *)0x23D0)[0] = 0x4E454652; // 'RFEN'
    } else {
        ((volatile ULONG *)0x23D0)[0] = 0;
    }
    BlCheckGpuClock(3);
    BlCheckXcodeValues(XCODE_RECORD_END);
    BlPost(0x22);
    __asm__ volatile("fninit\n\t"
                     "movl %%cr4, %%eax\n\t"
                     "orl $0x610, %%eax\n\t"
                     "movl %%eax, %%cr4\n\t"
                     "movl %0, %%eax\n\t"
                     "movl %%eax, %%cr3\n\t"
                     "movl $0x80010031, %%eax\n\t"
                     "movl %%eax, %%cr0\n\t"
                     "pushl %1\n\t"
                     "pushl %2\n\t"
                     "call *%3\n\t"
                     :
                     : "i"(PAGE_DIRECTORY), "r"(BlKeys), "i"(BOOT_MAGIC), "r"(entry)
                     : "eax", "memory");
    BlFail(0xE2);
}
