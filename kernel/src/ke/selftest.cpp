// Boot self-tests for milestone checks. Results are plain globals that tests/boot_test.py reads
// over the emulator monitor; nothing here is part of the kernel ABI.
#include <xk/hal.h>
#include <xk/ke.h>

volatile ULONG KiTestDpcCount;        // expect 1
volatile ULONG KiTestTimerTicks;      // expect ~100/s (10 ms periodic timer)
volatile ULONG KiTestOneShotFired;    // expect 1 after 300 ms
volatile ULONG KiTestEventState;      // expect 1 (event set by the one-shot DPC)
volatile ULONG KiTestRtcCount;        // expect 16 (RTC IRQ8 through a KINTERRUPT)
volatile ULONG KiTestRtcDisconnected; // expect 1
volatile ULONG KiTestRtcIrqlOk;       // expect 1 (ISR ran at the vector's IRQL)

static KDPC TestDpc, PeriodicDpc, OneShotDpc, RtcDoneDpc;
static KTIMER PeriodicTimer, OneShotTimer;
static KEVENT TestEvent;
static KINTERRUPT RtcInterrupt;
static KIRQL RtcIrql;

static void NTAPI OnTestDpc(PKDPC, PVOID, PVOID, PVOID) { KiTestDpcCount = KiTestDpcCount + 1; }

static void NTAPI OnPeriodic(PKDPC, PVOID, PVOID, PVOID) { KiTestTimerTicks = KiTestTimerTicks + 1; }

static void NTAPI OnOneShot(PKDPC, PVOID, PVOID, PVOID) {
    KiTestOneShotFired = KiTestOneShotFired + 1;
    KeSetEvent(&TestEvent, 0, FALSE);
    KiTestEventState = (ULONG)TestEvent.Header.SignalState;
}

static void NTAPI OnRtcDone(PKDPC, PVOID, PVOID, PVOID) {
    HalpCmosWrite(0x0B, HalpCmosRead(0x0B) & ~0x40); // periodic interrupt off
    KiTestRtcDisconnected = KeDisconnectInterrupt(&RtcInterrupt);
}

static BOOLEAN NTAPI OnRtcInterrupt(PKINTERRUPT, PVOID) {
    HalpCmosRead(0x0C); // acknowledge
    if (KeGetCurrentIrql() == RtcIrql)
        KiTestRtcIrqlOk = 1;
    KiTestRtcCount = KiTestRtcCount + 1;
    if (KiTestRtcCount == 16)
        KeInsertQueueDpc(&RtcDoneDpc, nullptr, nullptr);
    return TRUE;
}

void KiRunBootSelfTests() {
    KeInitializeDpc(&TestDpc, OnTestDpc, nullptr);
    KeInsertQueueDpc(&TestDpc, nullptr, nullptr);

    KeInitializeDpc(&PeriodicDpc, OnPeriodic, nullptr);
    KeInitializeTimerEx(&PeriodicTimer, NotificationTimer);
    LARGE_INTEGER due;
    due.QuadPart = -10 * 10000; // 10 ms
    KeSetTimerEx(&PeriodicTimer, due, 10, &PeriodicDpc);

    KeInitializeEvent(&TestEvent, NotificationEvent, FALSE);
    KeInitializeDpc(&OneShotDpc, OnOneShot, nullptr);
    KeInitializeTimerEx(&OneShotTimer, SynchronizationTimer);
    due.QuadPart = -300 * 10000; // 300 ms
    KeSetTimer(&OneShotTimer, due, &OneShotDpc);

    // RTC periodic interrupt (IRQ8, 8 Hz with register A = 0x2D) through a real interrupt object.
    KeInitializeDpc(&RtcDoneDpc, OnRtcDone, nullptr);
    ULONG vector = HalGetInterruptVector(8, &RtcIrql);
    KeInitializeInterrupt(&RtcInterrupt, OnRtcInterrupt, nullptr, vector, RtcIrql, Latched, FALSE);
    if (KeConnectInterrupt(&RtcInterrupt)) {
        HalpCmosRead(0x0C);
        HalpCmosWrite(0x0B, HalpCmosRead(0x0B) | 0x40);
    }
}
