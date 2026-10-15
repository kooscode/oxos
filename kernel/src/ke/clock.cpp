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

// --- Performance counter and short delays use the ACPI PM timer (3.579545 MHz, 24-bit)
// at PM base 0x8000 + 8. TODO(spec): confirm against the reference KeQueryPerformance*.

constexpr USHORT ACPI_PM_TIMER = 0x8008;
constexpr ULONG ACPI_PM_FREQUENCY = 3579545;

static ULONGLONG KiPerfCounter;
static ULONG KiPerfLast;

XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter() {
    ULONG flags = save_flags_cli();
    ULONG now = inl(ACPI_PM_TIMER) & 0xFFFFFF;
    KiPerfCounter += (now - KiPerfLast) & 0xFFFFFF;
    KiPerfLast = now;
    LARGE_INTEGER r;
    r.QuadPart = (LONGLONG)KiPerfCounter;
    restore_flags(flags);
    return r;
}

XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceFrequency() {
    LARGE_INTEGER r;
    r.QuadPart = ACPI_PM_FREQUENCY;
    return r;
}

XBAPI void NTAPI KeStallExecutionProcessor(ULONG MicroSeconds) {
    // ticks = us * 3.579545, as us * 3665 / 1024 (0.01% low), avoiding a 64-bit divide.
    ULONGLONG ticks = ((ULONGLONG)MicroSeconds * 3665 >> 10) + 1;
    ULONG last = inl(ACPI_PM_TIMER) & 0xFFFFFF;
    while (ticks) {
        ULONG now = inl(ACPI_PM_TIMER) & 0xFFFFFF;
        ULONG elapsed = (now - last) & 0xFFFFFF;
        last = now;
        ticks = elapsed >= ticks ? 0 : ticks - elapsed;
    }
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
