// 8259 PIC, 8254 PIT and CMOS RTC set-up (spec/02-executive-init.md §3.3-3.6).
#include <xk/arch.h>
#include <xk/hal.h>

constexpr USHORT PIC1_CMD = 0x20, PIC1_DATA = 0x21, PIC2_CMD = 0xA0, PIC2_DATA = 0xA1;
constexpr UCHAR PIC1_VECTOR_BASE = 0x30, PIC2_VECTOR_BASE = 0x38;

static UCHAR HalpPicMask[2] = {0xFF, 0xFF};

void HalpInitPic() {
    ULONG flags = save_flags_cli();
    struct { USHORT cmd, data; UCHAR base, icw3; } pics[] = {
        {PIC1_CMD, PIC1_DATA, PIC1_VECTOR_BASE, 0x04}, // slave on IRQ2
        {PIC2_CMD, PIC2_DATA, PIC2_VECTOR_BASE, 0x02}, // cascade identity
    };
    for (auto &p : pics) {
        outb(p.cmd, 0x11);     // ICW1: edge, cascade, ICW4 needed
        outb(p.data, p.base);  // ICW2
        outb(p.data, p.icw3);  // ICW3
        outb(p.data, 0x01);    // ICW4: 8086 mode
        outb(p.data, 0xFF);    // OCW1: mask all
    }
    HalpPicMask[0] = HalpPicMask[1] = 0xFF;
    // The reference returns with interrupts enabled; our caller decides when to enable.
    restore_flags(flags);
}

void HalpUnmaskIrq(ULONG irq) {
    if (irq < 8) {
        HalpPicMask[0] &= ~(1 << irq);
        outb(PIC1_DATA, HalpPicMask[0]);
    } else {
        HalpPicMask[1] &= ~(1 << (irq - 8));
        HalpPicMask[0] &= ~(1 << 2);
        outb(PIC2_DATA, HalpPicMask[1]);
        outb(PIC1_DATA, HalpPicMask[0]);
    }
}

void HalpEoi(ULONG irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// --- CMOS RTC: register A = 0x2D, register B keeps DST bit and selects 24-hour BCD.

constexpr USHORT CMOS_INDEX = 0x70, CMOS_DATA = 0x71;

static UCHAR CmosRead(UCHAR reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void CmosWrite(UCHAR reg, UCHAR v) {
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, v);
}

static bool HalpRtcInitialized;

void HalpInitRtc() {
    ULONG flags = save_flags_cli();
    CmosWrite(0x0A, 0x2D);
    CmosWrite(0x0B, (CmosRead(0x0B) & 0x01) | 0x02);
    CmosRead(0x0C);
    CmosRead(0x0D);
    restore_flags(flags);
}

void HalpInitRtcLate() {
    ULONG flags = save_flags_cli();
    CmosWrite(0x0B, (CmosRead(0x0B) & 0x01) | 0x02);
    CmosRead(0x0C);
    HalpRtcInitialized = true;
    restore_flags(flags);
}

// --- PIT channel 0: mode 2, count 1125 => 1 kHz from the Xbox's 1.125 MHz clock.

void HalpInitPit() {
    constexpr USHORT PIT_COUNT = 1125;
    outb(0x43, 0x34);
    outb(0x40, PIT_COUNT & 0xFF);
    outb(0x40, PIT_COUNT >> 8);
}
