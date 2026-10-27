// Open BIOS image: the 2BL's C part, running from RAM at 0x400000 (spec/stock/01 §5). It does what the reference 2BL
// does for the kernel, as observed at the kernel's entry point:
//   - the kernel image, decompressed to physical 0x10000 (it runs at 0x80010000);
//   - a page directory at 0xF000: 0-256 MB identity-mapped and again at 0x80000000 with 4 MB pages, entry 0x300
//     mapping the directory itself, the NV2A registers (0xFD000000, 16 MB) uncached, the top 4 MB (flash);
//   - CR4 = PSE | OSFXSR | OSXMMEXCPT, CR3 = 0xF000, CR0 = PG | WP | NE | ET | PE;
//   - KernelEntry(arg0, keys) called with cdecl. arg0 = 'OXOS' marks our key block (spec/stock/01 §1): no EEPROM or
//     certificate key, but the EEPROM HMAC states taken from XboxDev/cromwell at build time.
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

static void BlPost(UCHAR code) { __asm__ volatile("outb %0, $0x80" ::"a"(code)); }

[[noreturn]] static void BlFail(UCHAR code) {
    BlPost(code);
    for (;;)
        __asm__ volatile("cli; hlt");
}

// --- SMC start-up challenge (spec/stock/01 §5). On a console the SMC (SMBus slave 0x20) holds four challenge bytes in
// registers 0x1C-0x1F at power-on and expects the boot loader to answer in registers 0x20 and 0x21 within a few
// seconds; without an answer it resets the console and flashes the LED red and green. The retail 2BL answers it, so
// ours must. Answer and SMBus sequence as in XboxDev/cromwell's boot_rom/2bPicResponseAction.c (GPL-2.0).
static inline void BlOut8(unsigned short port, UCHAR v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void BlOut16(unsigned short port, unsigned short v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
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

constexpr unsigned short SMB = 0xC000; // MCPX SMBus host: status, control +2, address +4, data +6, command +8

// One SMBus byte transfer with the SMC; returns the byte read (Read), 0 for a write, -1 on failure. Bounded, so a
// console without an answering SMC (or an emulator) still boots.
static int BlSmcTransfer(bool Read, UCHAR Register, UCHAR Value) {
    for (ULONG spin = 0; (BlIn16(SMB) & 0x0800) && spin < 1000000; spin++) // another master is using the bus
        ;
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
    for (int i = 0; i < 4; i++)
        if ((c[i] = BlSmcTransfer(true, (UCHAR)(0x1C + i), 0)) < 0)
            return;
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
static inline void BlOut32(unsigned short port, ULONG v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline ULONG BlIn32(unsigned short port) {
    ULONG v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static ULONG BlPciRead(ULONG Device, ULONG Register) {
    BlOut32(0xCF8, 0x80000000 | Device << 11 | Register);
    return BlIn32(0xCFC);
}
static void BlPciWrite(ULONG Device, ULONG Register, ULONG Value) {
    BlOut32(0xCF8, 0x80000000 | Device << 11 | Register);
    BlOut32(0xCFC, Value);
}

// The memory size is left to the X-codes: an OXOS probe that then wrote the 64 MB values of spec 22 §6.3 (PFB 0x100204 =
// 0x11338000, host 0x84 = 0x03FFFFFF) stopped a 1.4 console in the memory manager (2026-10-22), so that reading of the
// X-codes is not right for every console; the X-codes' own values work.

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
    // Not written here, as in the retail 2BLs (spec/stock/21 §2, spec/stock/22 §6.3): the NV2A memory configuration
    // PFB 0x100200 / 0x100204 and the host bridge's top of memory 0x84. The X-codes set them from their memory-size
    // probe (0x100204 = 0x11338000 on a 64 MB console); overwriting them with cromwell's 128 MB values (as earlier
    // OXOS images did) gave the GPU the wrong memory layout: a scrambled picture on 1.4 and 1.x consoles.
}

// --- NV2A memory pad calibration (spec/stock/21 §3.7a): the drive/slew/delay compensation of the memory pads,
// measured and set before the kernel is decompressed, exactly as Cerbios does it on NV2A revisions below D3
// (consoles 1.0-1.5): Cerbios's row-set selection, the reference 4627 measurement and arithmetic (all 32-bit,
// including the clamp of 1/2^20 above the top breakpoint and the zero store described below). The table, in
// Cerbios's layout, is the one the flash header's 0x7C points at (through the 16 MB flash window): breakpoints
// at bytes 2-11, row sets at +0x18, +0x77 and +0xD6 (boot/padcal_table.inc, placed by boot/xcodes.S). A table
// whose breakpoints do not increase (the reference would divide by zero) leaves the X-codes' values. NV2A D3 and
// later (1.6) take Cerbios's EEPROM method, not implemented: the X-codes' values stay there too.
static volatile ULONG &BlNv(ULONG Offset) { return *(volatile ULONG *)(0xFD000000 + Offset); }

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

// Four readings of 0x121C, selector First..First+3 in 0x1228, each after a 10 000-tick wait; their average.
static ULONG BlPadMeasure(ULONG First) {
    ULONG sum = 0;
    for (ULONG i = 0; i < 4; i++) {
        BlNv(0x1228) = First + i;
        BlTscWait(10000);
        sum += BlNv(0x121C);
    }
    return sum >> 2;
}

static ULONG BlPadRound(ULONG x) { return (x + 0x80000) >> 20 & 0xF; }

// The row pair and weight for a measurement M against breakpoints B[0], B[2], B[4], B[6], B[8] (every other byte):
// Hi is weighted by F, Lo by 0x100000 - F.
static void BlPadSelect(ULONG M, const volatile UCHAR *B, const volatile UCHAR *Set, const volatile UCHAR **Hi,
                        const volatile UCHAR **Lo, ULONG *F) {
    const volatile UCHAR *row[5] = {Set, Set + 0x13, Set + 0x26, Set + 0x39, Set + 0x4C};
    ULONG b0 = B[0], b1 = B[2], b2 = B[4], b3 = B[6], b4 = B[8], f;
    if (M <= b1) { // the lowest segment: a negative quotient (sign bit set) becomes 0
        f = ((M - b0) << 20) / (b1 - b0);
        if (f & 0x80000000)
            f = 0;
        *Hi = row[3], *Lo = row[4];
    } else if (M >= b3) { // the highest: a weight above 1.0 becomes 1/2^20
        f = ((M - b3) << 20) / (b4 - b3);
        if (f > 0x100000)
            f = 1;
        *Hi = row[0], *Lo = row[1];
    } else if (M < b2) {
        f = ((M - b1) << 20) / (b2 - b1);
        *Hi = row[2], *Lo = row[3];
    } else {
        f = ((M - b2) << 20) / (b3 - b2);
        *Hi = row[1], *Lo = row[2];
    }
    *F = f;
}

static ULONG BlPadNibbles(ULONG Even, ULONG Odd) { // bytes Odd << 4 | Even, as the reference builds them
    ULONG v = Odd;
    for (int i = 0; i < 7; i++)
        v = v << 4 | (i & 1 ? Odd : Even);
    return v;
}

// What the calibration measured and wrote, for the kernel's debug log (physical 0x2300, a free page at that point).
struct BL_PAD_RESULT {
    ULONG Magic; // 'PADC' when the calibration ran, 'PADN' when the image has no table
    ULONG A, B, Strap, Values[9];
};

[[gnu::noinline]] static void BlPadCalibrate(bool Upgraded) {
    auto result = (volatile BL_PAD_RESULT *)0x2300;
    auto table = (const volatile UCHAR *)(*(volatile ULONG *)0xFF00007C - 0x1000000);
    result->Magic = 0x4E444150; // 'PADN'
    for (int i = 2; i < 10; i++) // a missing table (all zero) would divide by zero below
        if (table[i] >= table[i + 2])
            return;
    ULONG strap = BlNv(0x101000);
    auto set = table + (Upgraded ? 0xD6 : (strap & 0xC0000) ? 0x18 : 0x77);
    BlNv(0x1220) = 0x000F0F0F;
    ULONG a = BlPadMeasure(0x40004), b = BlPadMeasure(0x40000);
    BlNv(0x1220) = 0;

    const volatile UCHAR *hi, *lo;
    ULONG f, o[16];
    BlPadSelect(a, table + 2, set, &hi, &lo, &f);
    for (int k = 0; k < 16; k++)
        o[k] = BlPadRound(hi[k] * f + lo[k] * (0x100000 - f));
    BlPadSelect(b, table + 3, set, &hi, &lo, &f);
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
    ULONG reg[9] = {0x122C, 0x123C, 0x1230, 0x1240, 0x1234, 0x1244, 0x1238, 0x1248, 0x1214};
    ULONG val[9] = {BlPadNibbles(o[0], o[1]),   BlPadNibbles(o[2], o[3]),   BlPadNibbles(o[4], o[5]),
                    BlPadNibbles(o[6], o[7]),   BlPadNibbles(o[8], o[9]),   BlPadNibbles(o[10], o[11]),
                    BlPadNibbles(o[12], o[13]), BlPadNibbles(o[14], o[15]), v1214};
    __asm__ volatile("wbinvd" ::: "memory");
    // A register whose value is 0 is not written; the reference then stores that 0 at NV2A offset 0 instead (its
    // read-only identification register), which this keeps.
    for (int i = 0; i < 9; i++)
        BlNv(val[i] ? reg[i] : 0) = val[i];
    result->A = a;
    result->B = b;
    result->Strap = strap;
    for (int i = 0; i < 9; i++)
        result->Values[i] = val[i];
    result->Magic = 0x43444150; // 'PADC'
}

#if XK_NETBOOT
#include "netboot.h"
#endif

extern "C" [[noreturn]] void BlMain() {
    BlPost(0x20);
    // The reference 2BL's order (spec/stock/21 §2): device 1 register 0x80 byte 0 = 0x02 (a byte cycle), the SMC
    // challenge and SMC 0x01 = 0, device 1 register 0x8C = 0x40000000, the pad calibration, then the AGP/host steps.
    BlOut32(0xCF8, 0x80000000 | 1 << 11 | 0x80);
    BlOut8(0xCFC, 0x02);
    BlAnswerSmcChallenge();
    BlPciWrite(1, 0x8C, 0x40000000);
    // Cerbios's choice of method (spec/stock/21 §3.7a): "upgraded" memory = top of memory (host 0x84) 256 MB, or
    // 128 MB with PCI 0:2:0 and 0:3:0 register 0x3C low bytes 0x0F and 0xF0. NV2A D3 and later without that: the
    // EEPROM method (not implemented). Otherwise the table, with the third row set when upgraded.
    {
        ULONG top = BlPciRead(0, 0x84);
        UCHAR irq2 = (UCHAR)BlPciRead(2, 0x3C), irq3 = (UCHAR)BlPciRead(3, 0x3C);
        bool upgraded128 = irq2 == 0x0F && irq3 == 0xF0 && top == 0x07FFFFFF;
        bool eepromMethod = *(volatile UCHAR *)0xFD000000 >= 0xD3 && !upgraded128; // a byte read, as there
        if (!eepromMethod)
            BlPadCalibrate(top == 0x0FFFFFFF || upgraded128);
    }
    BlSetUpChipset();
    // Boot status on the front LED (spec/oxos-internal/03-debug-serial.md §6): blinking orange while the 2BL runs.
    BlSmcTransfer(false, 0x08, 0xAA);
    BlSmcTransfer(false, 0x07, 1);
    BlPost(0x23);
    ULONG bank = BANK; // where the kernel and key block come from: this flash bank, or a network download
#if XK_NETBOOT
    BlSmcTransfer(false, 0x08, 0xFF); // solid orange while it asks the network (spec/oxos-internal/09-netboot.md)
    BlSmcTransfer(false, 0x07, 1);
    if (const UCHAR *download = netboot::Boot())
        bank = (ULONG)download;
    BlSmcTransfer(false, 0x08, 0xAA);
    BlSmcTransfer(false, 0x07, 1);
#endif
    auto payload = (const volatile XK_BOOT_PAYLOAD *)(bank + (PAYLOAD - BANK));
    if (payload->Magic != PAYLOAD_MAGIC || payload->KeysSize > sizeof(BlKeys))
        BlFail(0xE0);

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
