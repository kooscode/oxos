// Clock interrupt and time keeping. PIT channel 0 ticks every 1 ms (spec/02 §3.5).
#include <xk/arch.h>
#include <xk/ke.h>

constexpr ULONG CLOCK_INCREMENT = 10000; // 1 ms in 100 ns units

volatile ULONG KeTickCount;
volatile KSYSTEM_TIME KeInterruptTime;
volatile KSYSTEM_TIME KeSystemTime;
ULONG KeTimeIncrement = CLOCK_INCREMENT;

// KSYSTEM_TIME is read lock-free: High1Time == High2Time means a consistent snapshot.
static void AdvanceSystemTime(volatile KSYSTEM_TIME &t, ULONG increment) {
    ULONGLONG v = ((ULONGLONG)(ULONG)t.High1Time << 32 | t.LowPart) + increment;
    t.High2Time = (LONG)(v >> 32);
    t.LowPart = (ULONG)v;
    t.High1Time = (LONG)(v >> 32);
}

static LONGLONG ReadSystemTime(const volatile KSYSTEM_TIME &t) {
    for (;;) {
        LONG high = t.High1Time;
        ULONG low = t.LowPart;
        if (high == t.High2Time)
            return (LONGLONG)high << 32 | low;
    }
}

void KiUpdateRunTime(KIRQL interruptedIrql);

// IRQ0 at CLOCK_LEVEL (see KiDispatchHardwareInterrupt).
void KiClockInterrupt(KIRQL interruptedIrql) {
    AdvanceSystemTime(KeInterruptTime, CLOCK_INCREMENT);
    AdvanceSystemTime(KeSystemTime, CLOCK_INCREMENT);
    KeTickCount = KeTickCount + 1;
    KiCheckTimers();
    KiUpdateRunTime(interruptedIrql);
}

XBAPI ULONGLONG NTAPI KeQueryInterruptTime() { return ReadSystemTime(KeInterruptTime); }

XBAPI void NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime) {
    CurrentTime->QuadPart = ReadSystemTime(KeSystemTime);
}

// --- Performance counter and short delays: the MCPX ACPI PM timer, 32 bits at 3.375 MHz (27 MHz / 8), I/O 0x8008
// (spec/06 §7). Bits 63-31 of the counter are kept in software: the SCI reports every toggle of timer bit 31
// (PM1 status bit 0), and a reader corrects for a toggle not handled yet.

constexpr USHORT ACPI_PM_TIMER = 0x8008;
constexpr ULONG ACPI_PM_FREQUENCY = 3375000;

static volatile ULONG KiPerfBaseLow, KiPerfHigh, KiPerfHigh2; // base (bit 31 + high part), written high2/low/high

XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter() {
    ULONG high, low;
    do {
        high = KiPerfHigh;
        low = KiPerfBaseLow;
    } while (high != KiPerfHigh2);
    ULONG now = inl(ACPI_PM_TIMER);
    ULONGLONG value = ((ULONGLONG)high << 32) | ((now & 0x7FFFFFFF) | (low & 0x80000000));
    value += (now ^ low) & 0x80000000; // bit 31 toggled since the last overflow event
    LARGE_INTEGER r;
    r.QuadPart = (LONGLONG)value;
    return r;
}

// From the SCI handler: timer bit 31 toggled.
void KiPerfCounterOverflow() {
    ULONG now = inl(ACPI_PM_TIMER);
    ULONGLONG base = ((ULONGLONG)KiPerfHigh << 32 | KiPerfBaseLow) + 0x80000000;
    base += (now ^ (ULONG)base) & 0x80000000;
    KiPerfHigh2 = (ULONG)(base >> 32);
    KiPerfBaseLow = (ULONG)base;
    KiPerfHigh = (ULONG)(base >> 32);
}

XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceFrequency() {
    LARGE_INTEGER r;
    r.QuadPart = ACPI_PM_FREQUENCY;
    return r;
}

// A fixed busy loop, as the reference: cpuid, then MicroSeconds x 367 iterations (its constant at 0x8003AFF0, sized
// for the 733 MHz CPU; the product is 32-bit). Not timed against a clock: an emulator that runs the loop faster
// than the console also shortens the stalls, which titles and xemu are used to (e.g. PhyInitialize's 3.5 s of
// waits with no link, spec/16, pass in a fraction of a second in xemu).
XBAPI void NTAPI KeStallExecutionProcessor(ULONG MicroSeconds) {
    ULONG a = 0, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    ULONG loops = MicroSeconds * 367;
    if (loops)
        __asm__ volatile("1: subl $1, %0\n\tjnz 1b" : "+r"(loops));
}

// --- System time: initialised from the RTC, changed by NtSetSystemTime (which also sets the RTC).
struct HAL_TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
};
void HalpQueryRealTimeClock(HAL_TIME_FIELDS *t);
void HalpSetRealTimeClock(const HAL_TIME_FIELDS *t);
XBAPI BOOLEAN NTAPI RtlTimeFieldsToTime(HAL_TIME_FIELDS *TimeFields, PLARGE_INTEGER Time);
XBAPI void NTAPI RtlTimeToTimeFields(PLARGE_INTEGER Time, HAL_TIME_FIELDS *TimeFields);

static void KiSetSystemTime(LONGLONG t) {
    ULONG flags = save_flags_cli();
    KeSystemTime.High2Time = (LONG)(t >> 32);
    KeSystemTime.LowPart = (ULONG)t;
    KeSystemTime.High1Time = (LONG)(t >> 32);
    restore_flags(flags);
}

void KiInitializeSystemTime() {
    HAL_TIME_FIELDS f;
    HalpQueryRealTimeClock(&f);
    LARGE_INTEGER t;
    if (RtlTimeFieldsToTime(&f, &t))
        KiSetSystemTime(t.QuadPart);
}

void HalpWriteCmosSignature();

XBAPI NTSTATUS NTAPI NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime) {
    LARGE_INTEGER t = *SystemTime;
    if (t.HighPart < 0 || t.HighPart > 0x20000000)
        return (NTSTATUS)0xC000000D;
    if (PreviousTime)
        KeQuerySystemTime(PreviousTime);
    KiSetSystemTime(t.QuadPart);
    HAL_TIME_FIELDS f;
    RtlTimeToTimeFields(&t, &f);
    HalpSetRealTimeClock(&f);
    HalpWriteCmosSignature();
    return STATUS_SUCCESS;
}
