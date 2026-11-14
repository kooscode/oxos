// 8259 PIC, interrupt enable/disable, 8254 PIT and CMOS RTC (spec/stock/02 §3.3-3.6, spec/stock/06 §1, §3).
#include <hardware/chipset/chipset.h>
#include <arch/arch.h>
#include <hardware/chipset/serial.h>

namespace xk::hardware::chipset {

// --- 8259 PICs and the interrupt lines (spec/stock/06 §1)

constexpr USHORT PIC1_CMD = 0x20, PIC1_DATA = 0x21, PIC2_CMD = 0xA0, PIC2_DATA = 0xA1;
constexpr USHORT ELCR1 = 0x4D0, ELCR2 = 0x4D1;
constexpr UCHAR PIC1_VECTOR_BASE = 0x30, PIC2_VECTOR_BASE = 0x38;
constexpr ULONG CASCADE_IRQ = 2;

static USHORT DisabledMask = 0xFFFF; // IRQs not enabled through HalEnableSystemInterrupt
static USHORT PicMask = 0xFFFF;      // value currently programmed into both PICs
static USHORT Elcr;                  // bit set = level-sensitive

// Dispatch IRQL per IRQ. Defaults reproduce the reference per-IRQL PIC mask table (spec/stock/07):
// IRQ0 (clock) 28, IRQ8 26, IRQ12 27, the cascade never, every other IRQ n at 26 - n.
static constexpr KIRQL DefaultIrqLevel[16] = {28, 25, 0xFF, 23, 22, 21, 20, 19,
                                                  26, 17, 16, 15, 27, 13, 12, 11};
// The current table: the defaults, except where a connected KINTERRUPT set its own IRQL.
static KIRQL IrqLevel[16] = {28, 25, 0xFF, 23, 22, 21, 20, 19, 26, 17, 16, 15, 27, 13, 12, 11};

static void WritePicMask(USHORT mask) {
    if (mask == PicMask)
        return;
    PicMask = mask;
    outb(PIC1_DATA, (UCHAR)mask);
    outb(PIC2_DATA, (UCHAR)(mask >> 8));
}

KIRQL GetIrqLevel(ULONG irq) {
    return IrqLevel[irq];
}

void SetIrqLevel(ULONG irq, KIRQL irql) {
    IrqLevel[irq] = irql;
}

void ResetIrqLevel(ULONG irq) {
    IrqLevel[irq] = DefaultIrqLevel[irq];
}

// IRQs dispatched at or below `irql` must not interrupt it.
static USHORT LevelMask(KIRQL irql) {
    USHORT mask = 0;
    for (ULONG irq = 0; irq < 16; irq++) {
        if (IrqLevel[irq] <= irql)
            mask |= 1u << irq;
    }
    return mask;
}

void SetIrqlMask(KIRQL irql) {
    USHORT mask = DisabledMask | LevelMask(irql);
    if ((mask & 0xFF00) != 0xFF00)
        mask &= ~(1u << CASCADE_IRQ); // keep the cascade open while any slave IRQ is live
    WritePicMask(mask);
}

void InitPic() {
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
    PicMask = 0xFFFF;
    DisabledMask = 0xFFFF;
    Elcr = (USHORT)(inb(ELCR1) | inb(ELCR2) << 8);
    // As the reference (0x80013F98): returns with interrupts enabled whatever the caller had. Every line is masked
    // here, so nothing arrives until a driver enables its line.
    (void)flags;
    sti();
}

void SendEoi(ULONG irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// IRQ7/IRQ15 may be spurious: the in-service bit is then clear.
bool IsSpuriousIrq(ULONG irq) {
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
        Elcr |= bit;
    else
        Elcr &= ~bit;
    outb(ELCR1, (UCHAR)Elcr);
    outb(ELCR2, (UCHAR)(Elcr >> 8));
    DisabledMask &= ~bit;
    SetIrqlMask(KiPcr.Irql);
    restore_flags(flags);
}

XBAPI void NTAPI HalDisableSystemInterrupt(ULONG BusInterruptLevel) {
    ULONG flags = save_flags_cli();
    DisabledMask |= 1u << BusInterruptLevel;
    SetIrqlMask(KiPcr.Irql);
    restore_flags(flags);
}

// --- CMOS RTC: register A = 0x2D, register B keeps DST bit and selects 24-hour BCD.

constexpr USHORT CMOS_INDEX = 0x70, CMOS_DATA = 0x71;

UCHAR ReadCmos(UCHAR reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

void WriteCmos(UCHAR reg, UCHAR v) {
    outb(CMOS_INDEX, reg);
    outb(CMOS_DATA, v);
}

void InitRtc() {
    ULONG flags = save_flags_cli();
    WriteCmos(0x0A, 0x2D);
    WriteCmos(0x0B, (ReadCmos(0x0B) & 0x01) | 0x02);
    ReadCmos(0x0C);
    ReadCmos(0x0D);
    restore_flags(flags);
}

void InitRtcLate() {
    ULONG flags = save_flags_cli();
    WriteCmos(0x0B, (ReadCmos(0x0B) & 0x01) | 0x02);
    ReadCmos(0x0C);
    restore_flags(flags);
}

// --- PIT channel 0: mode 2, count 1125 => 1 kHz from the Xbox's 1.125 MHz clock.

void InitPit() {
    constexpr USHORT PIT_COUNT = 1125;
    outb(0x43, 0x34);
    outb(0x40, PIT_COUNT & 0xFF);
    outb(0x40, PIT_COUNT >> 8);
}

// --- Real-time clock (BCD; century in register 0x7F). Fields: year, month, day, hour, minute, second, ms, weekday.

static UCHAR FromBcd(UCHAR v) {
    return (UCHAR)((v >> 4) * 10 + (v & 15));
}

static UCHAR ToBcd(ULONG v) {
    return (UCHAR)(((v / 10) << 4) | (v % 10));
}

static void WaitRtcStable() {
    for (int i = 0; i < 100000 && (ReadCmos(0x0A) & 0x80); i++) {
        // Wait while the RTC's update is in progress (bounded).
    }
}

void QueryRealTimeClock(TIME_FIELDS *t) {
    ULONG flags = save_flags_cli();
    WaitRtcStable();
    t->Milliseconds = 500;
    t->Second = FromBcd(ReadCmos(0x00));
    t->Minute = FromBcd(ReadCmos(0x02));
    t->Hour = FromBcd(ReadCmos(0x04));
    t->Weekday = FromBcd(ReadCmos(0x06));
    t->Day = FromBcd(ReadCmos(0x07));
    t->Month = FromBcd(ReadCmos(0x08));
    USHORT year = FromBcd(ReadCmos(0x09)) + FromBcd(ReadCmos(0x7F)) * 100;
    if (year > 1899 && year < 1920)
        year += 100;
    t->Year = (SHORT)year;
    restore_flags(flags);
}

void SetRealTimeClock(const TIME_FIELDS *t) {
    ULONG flags = save_flags_cli();
    WaitRtcStable();
    WriteCmos(0x00, ToBcd(t->Second));
    WriteCmos(0x02, ToBcd(t->Minute));
    WriteCmos(0x04, ToBcd(t->Hour));
    WriteCmos(0x06, ToBcd(t->Weekday));
    WriteCmos(0x07, ToBcd(t->Day));
    WriteCmos(0x08, ToBcd(t->Month));
    ULONG year = t->Year > 9999 ? 9999 : t->Year;
    WriteCmos(0x7F, ToBcd(year / 100));
    WriteCmos(0x09, ToBcd(year % 100));
    restore_flags(flags);
}

// The "clock has been set" signature in the second CMOS bank (spec/stock/05 §1): bytes 0x10-0x6F and 0x80-0xFF hold
// 0x55, 0xAA, 0x55, ... Written by NtSetSystemTime; checked before the first title launch. As the reference does it:
// the index goes to port 0x72, the data through port 0x71.
static bool CmosPattern(UCHAR first, ULONG count, bool write) {
    ULONG flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    UCHAR expect = 0x55;
    bool ok = true;
    for (ULONG i = 0; i < count && ok; i++, expect = (UCHAR)~expect) {
        __asm__ volatile("outb %0, $0x72" ::"a"((UCHAR)(first + i)));
        if (write) {
            __asm__ volatile("outb %0, $0x71" ::"a"(expect));
        } else {
            UCHAR v;
            __asm__ volatile("inb $0x71, %0" : "=a"(v));
            ok = v == expect;
        }
    }
    __asm__ volatile("pushl %0; popfl" ::"r"(flags) : "cc");
    return ok;
}

void WriteCmosSignature() {
    CmosPattern(0x10, 0x60, true);
    CmosPattern(0x80, 0x80, true);
}

bool CmosSignatureValid() {
    return CmosPattern(0x10, 0x60, false) && CmosPattern(0x80, 0x80, false);
}

} // namespace xk::hardware::chipset
