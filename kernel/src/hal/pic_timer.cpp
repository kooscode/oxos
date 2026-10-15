// 8259 PIC, interrupt enable/disable, 8254 PIT and CMOS RTC (spec/02 §3.3-3.6, spec/06 §1, §3).
#include <xk/arch.h>
#include <xk/hal.h>

constexpr USHORT PIC1_CMD = 0x20, PIC1_DATA = 0x21, PIC2_CMD = 0xA0, PIC2_DATA = 0xA1;
constexpr USHORT ELCR1 = 0x4D0, ELCR2 = 0x4D1;
constexpr UCHAR PIC1_VECTOR_BASE = 0x30, PIC2_VECTOR_BASE = 0x38;
constexpr ULONG CASCADE_IRQ = 2;

static USHORT HalpDisabledMask = 0xFFFF; // IRQs not enabled through HalEnableSystemInterrupt
static USHORT HalpPicMask = 0xFFFF;      // value currently programmed into both PICs
static USHORT HalpElcr;                  // bit set = level-sensitive

static void HalpWritePicMask(USHORT mask) {
    if (mask == HalpPicMask)
        return;
    HalpPicMask = mask;
    outb(PIC1_DATA, (UCHAR)mask);
    outb(PIC2_DATA, (UCHAR)(mask >> 8));
}

// Dispatch IRQL per IRQ. Defaults reproduce the reference per-IRQL PIC mask table (spec/07):
// IRQ0 (clock) 28, IRQ8 26, IRQ12 27, the cascade never, every other IRQ n at 26 - n.
static constexpr KIRQL HalpDefaultIrqLevel[16] = {28, 25, 0xFF, 23, 22, 21, 20, 19,
                                                  26, 17, 16, 15, 27, 13, 12, 11};
static KIRQL HalpIrqLevel[16] = {28, 25, 0xFF, 23, 22, 21, 20, 19, 26, 17, 16, 15, 27, 13, 12, 11};

KIRQL HalpGetIrqLevel(ULONG irq) { return HalpIrqLevel[irq]; }
void HalpSetIrqLevel(ULONG irq, KIRQL irql) { HalpIrqLevel[irq] = irql; }
void HalpResetIrqLevel(ULONG irq) { HalpIrqLevel[irq] = HalpDefaultIrqLevel[irq]; }

// IRQs dispatched at or below `irql` must not interrupt it.
static USHORT HalpLevelMask(KIRQL irql) {
    USHORT mask = 0;
    for (ULONG irq = 0; irq < 16; irq++)
        if (HalpIrqLevel[irq] <= irql)
            mask |= 1u << irq;
    return mask;
}

void HalpSetIrqlMask(KIRQL irql) {
    USHORT mask = HalpDisabledMask | HalpLevelMask(irql);
    if ((mask & 0xFF00) != 0xFF00)
        mask &= ~(1u << CASCADE_IRQ); // keep the cascade open while any slave IRQ is live
    HalpWritePicMask(mask);
}

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
    HalpPicMask = 0xFFFF;
    HalpDisabledMask = 0xFFFF;
    HalpElcr = (USHORT)(inb(ELCR1) | inb(ELCR2) << 8);
    // The reference returns with interrupts enabled; our caller decides when to enable.
    restore_flags(flags);
}

void HalpEoi(ULONG irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// IRQ7/IRQ15 may be spurious: the in-service bit is then clear.
bool HalpIsSpuriousIrq(ULONG irq) {
    if (irq != 7 && irq != 15)
        return false;
    USHORT cmd = irq == 7 ? PIC1_CMD : PIC2_CMD;
    outb(cmd, 0x0B); // OCW3: read ISR
    bool inService = inb(cmd) & 0x80;
    if (!inService && irq == 15)
        outb(PIC1_CMD, 0x20); // the master still saw the cascade
    return !inService;
}

XBAPI ULONG NTAPI HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) {
    ULONG vector = BusInterruptLevel + HAL_IRQ_BASE_VECTOR;
    if (vector < HAL_IRQ_BASE_VECTOR || vector > 0x4A)
        return 0;
    *Irql = (KIRQL)(0x4A - vector);
    return vector;
}

XBAPI void NTAPI HalEnableSystemInterrupt(ULONG BusInterruptLevel, KINTERRUPT_MODE InterruptMode) {
    ULONG bit = 1u << BusInterruptLevel;
    ULONG flags = save_flags_cli();
    if (InterruptMode == LevelSensitive)
        HalpElcr |= bit;
    else
        HalpElcr &= ~bit;
    outb(ELCR1, (UCHAR)HalpElcr);
    outb(ELCR2, (UCHAR)(HalpElcr >> 8));
    HalpDisabledMask &= ~bit;
    HalpSetIrqlMask(KiPcr.Irql);
    restore_flags(flags);
}

XBAPI void NTAPI HalDisableSystemInterrupt(ULONG BusInterruptLevel) {
    ULONG flags = save_flags_cli();
    HalpDisabledMask |= 1u << BusInterruptLevel;
    HalpSetIrqlMask(KiPcr.Irql);
    restore_flags(flags);
}

// --- CMOS RTC: register A = 0x2D, register B keeps DST bit and selects 24-hour BCD.

constexpr USHORT CMOS_INDEX = 0x70, CMOS_DATA = 0x71;

UCHAR HalpCmosRead(UCHAR reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

void HalpCmosWrite(UCHAR reg, UCHAR v) {
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, v);
}

static bool HalpRtcInitialized;

void HalpInitRtc() {
    ULONG flags = save_flags_cli();
    HalpCmosWrite(0x0A, 0x2D);
    HalpCmosWrite(0x0B, (HalpCmosRead(0x0B) & 0x01) | 0x02);
    HalpCmosRead(0x0C);
    HalpCmosRead(0x0D);
    restore_flags(flags);
}

void HalpInitRtcLate() {
    ULONG flags = save_flags_cli();
    HalpCmosWrite(0x0B, (HalpCmosRead(0x0B) & 0x01) | 0x02);
    HalpCmosRead(0x0C);
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

// --- Real-time clock (BCD; century in register 0x7F). Fields: year, month, day, hour, minute, second, ms, weekday.
struct HAL_TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
};

static UCHAR FromBcd(UCHAR v) { return (UCHAR)((v >> 4) * 10 + (v & 15)); }
static UCHAR ToBcd(ULONG v) { return (UCHAR)(((v / 10) << 4) | (v % 10)); }

static void HalpWaitRtcStable() {
    for (int i = 0; i < 100000 && (HalpCmosRead(0x0A) & 0x80); i++) // update in progress
        ;
}

void HalpQueryRealTimeClock(HAL_TIME_FIELDS *t) {
    ULONG flags = save_flags_cli();
    HalpWaitRtcStable();
    t->Milliseconds = 500;
    t->Second = FromBcd(HalpCmosRead(0x00));
    t->Minute = FromBcd(HalpCmosRead(0x02));
    t->Hour = FromBcd(HalpCmosRead(0x04));
    t->Weekday = FromBcd(HalpCmosRead(0x06));
    t->Day = FromBcd(HalpCmosRead(0x07));
    t->Month = FromBcd(HalpCmosRead(0x08));
    USHORT year = FromBcd(HalpCmosRead(0x09)) + FromBcd(HalpCmosRead(0x7F)) * 100;
    if (year > 1899 && year < 1920)
        year += 100;
    t->Year = (SHORT)year;
    restore_flags(flags);
}

void HalpSetRealTimeClock(const HAL_TIME_FIELDS *t) {
    ULONG flags = save_flags_cli();
    HalpWaitRtcStable();
    HalpCmosWrite(0x00, ToBcd(t->Second));
    HalpCmosWrite(0x02, ToBcd(t->Minute));
    HalpCmosWrite(0x04, ToBcd(t->Hour));
    HalpCmosWrite(0x06, ToBcd(t->Weekday));
    HalpCmosWrite(0x07, ToBcd(t->Day));
    HalpCmosWrite(0x08, ToBcd(t->Month));
    ULONG year = t->Year > 9999 ? 9999 : t->Year;
    HalpCmosWrite(0x7F, ToBcd(year / 100));
    HalpCmosWrite(0x09, ToBcd(year % 100));
    restore_flags(flags);
}
