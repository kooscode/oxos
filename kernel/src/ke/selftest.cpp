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

// ---------------------------------------------------------------- milestone 2b: threads and waits

volatile ULONG KiTestPingA, KiTestPingB;      // expect 1000 each
volatile ULONG KiTestSleepMs;                 // expect ~500 (10 x 50 ms)
volatile ULONG KiTestSpinA, KiTestSpinB;      // both advance (round robin at equal priority)
volatile ULONG KiTestSemaphoreConsumed;       // expect 100
volatile ULONG KiTestMutantOk;                // expect 1
volatile ULONG KiTestWaitAnyIndex = 0xFF;     // expect 1 (timer signals first)
volatile ULONG KiTestWaitAllStatus = 0xFF;    // expect 0
volatile ULONG KiTestTimeoutStatus;           // expect 0x102
volatile ULONG KiTestJoinOk;                  // expect 1
volatile ULONG KiTestFpuOk;                   // expect 2 (both threads kept their x87 state)
volatile ULONG KiTestThreadsDone;             // expect 1 once the controller finished

static KEVENT PingEventA, PingEventB;

static void Sleep(LONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static void NTAPI PingThreadA(PVOID) {
    for (int i = 0; i < 1000; i++) {
        KeWaitForSingleObject(&PingEventA, Executive, KernelMode, FALSE, nullptr);
        KiTestPingA = KiTestPingA + 1;
        KeSetEvent(&PingEventB, 0, FALSE);
    }
}

static void NTAPI PingThreadB(PVOID) {
    for (int i = 0; i < 1000; i++) {
        KeWaitForSingleObject(&PingEventB, Executive, KernelMode, FALSE, nullptr);
        KiTestPingB = KiTestPingB + 1;
        KeSetEvent(&PingEventA, 0, FALSE);
    }
}

static void NTAPI SleepThread(PVOID) {
    ULONG start = KeTickCount;
    for (int i = 0; i < 10; i++)
        Sleep(50);
    KiTestSleepMs = KeTickCount - start;
}

static void NTAPI SpinThread(PVOID counter) {
    for (;;)
        *(volatile ULONG *)counter = *(volatile ULONG *)counter + 1;
}

// x87 state must survive context switches: each thread keeps its own value in st(0) while sleeping.
static void NTAPI FpuThread(PVOID value) {
    LONG v = (LONG)(ULONG)value, out = 0;
    __asm__ volatile("fninit\n\tfildl %0" ::"m"(v));
    for (int i = 0; i < 50; i++)
        Sleep(1);
    __asm__ volatile("fistpl %0" : "=m"(out));
    if (out == v)
        KiTestFpuOk = KiTestFpuOk + 1;
}

static KSEMAPHORE TestSemaphore;
static void NTAPI SemaphoreProducer(PVOID) {
    for (int i = 0; i < 100; i++) {
        KeReleaseSemaphore(&TestSemaphore, 0, 1, FALSE);
        if (i % 10 == 0)
            Sleep(1);
    }
}

static KMUTANT TestMutant;
static KEVENT MutantTaken;
static volatile ULONG MutantWaiterGotIt;
static void NTAPI MutantWaiter(PVOID) {
    KeWaitForSingleObject(&TestMutant, Executive, KernelMode, FALSE, nullptr);
    MutantWaiterGotIt = TestMutant.OwnerThread == KeGetCurrentThread();
    KeReleaseMutant(&TestMutant, 0, FALSE, FALSE);
    KeSetEvent(&MutantTaken, 0, FALSE);
}

static void NTAPI ShortLivedThread(PVOID) { Sleep(20); }

// Runs the sequential checks at priority 9 so it isn't starved by the spinners.
static void NTAPI ControllerThread(PVOID) {
    // Semaphore: consume 100 releases from a producer thread.
    KeInitializeSemaphore(&TestSemaphore, 0, 100);
    KiCreateSystemThread(SemaphoreProducer, nullptr, 0, 8);
    for (int i = 0; i < 100; i++) {
        KeWaitForSingleObject(&TestSemaphore, Executive, KernelMode, FALSE, nullptr);
        KiTestSemaphoreConsumed = KiTestSemaphoreConsumed + 1;
    }

    // Mutant: recursive acquire, then a waiter gets it after the final release.
    KeInitializeMutant(&TestMutant, FALSE);
    KeInitializeEvent(&MutantTaken, NotificationEvent, FALSE);
    KeWaitForSingleObject(&TestMutant, Executive, KernelMode, FALSE, nullptr);
    KeWaitForSingleObject(&TestMutant, Executive, KernelMode, FALSE, nullptr);
    bool recursive = TestMutant.Header.SignalState == -1 && TestMutant.OwnerThread == KeGetCurrentThread();
    KiCreateSystemThread(MutantWaiter, nullptr, 0, 8);
    Sleep(10);
    KeReleaseMutant(&TestMutant, 0, FALSE, FALSE);
    bool stillOwned = TestMutant.OwnerThread == KeGetCurrentThread();
    KeReleaseMutant(&TestMutant, 0, FALSE, FALSE);
    KeWaitForSingleObject(&MutantTaken, Executive, KernelMode, FALSE, nullptr);
    KiTestMutantOk = recursive && stillOwned && MutantWaiterGotIt;

    // WaitAny: never-signalled event vs. a 20 ms timer -> index 1.
    KEVENT never, e1, e2;
    KTIMER timer;
    KeInitializeEvent(&never, NotificationEvent, FALSE);
    KeInitializeTimerEx(&timer, NotificationTimer);
    LARGE_INTEGER due;
    due.QuadPart = -20 * 10000;
    KeSetTimer(&timer, due, nullptr);
    PVOID anyObjects[2] = {&never, &timer};
    KiTestWaitAnyIndex = KeWaitForMultipleObjects(2, anyObjects, WaitAny, Executive, KernelMode, FALSE, nullptr, nullptr);

    // WaitAll on two signalled notification events -> STATUS_WAIT_0.
    KeInitializeEvent(&e1, NotificationEvent, TRUE);
    KeInitializeEvent(&e2, NotificationEvent, TRUE);
    PVOID allObjects[2] = {&e1, &e2};
    KiTestWaitAllStatus = KeWaitForMultipleObjects(2, allObjects, WaitAll, Executive, KernelMode, FALSE, nullptr, nullptr);

    // Timeout on a never-signalled event.
    due.QuadPart = -30 * 10000;
    KiTestTimeoutStatus = KeWaitForSingleObject(&never, Executive, KernelMode, FALSE, &due);

    // Join: a terminated thread's object is signalled.
    PKTHREAD child = KiCreateSystemThread(ShortLivedThread, nullptr, 0, 8);
    KiTestJoinOk = KeWaitForSingleObject(child, Executive, KernelMode, FALSE, nullptr) == STATUS_WAIT_0 &&
                   child->State == Terminated;

    KiTestThreadsDone = 1;
}

static void KiStartThreadSelfTests() {
    KeInitializeEvent(&PingEventA, SynchronizationEvent, TRUE);
    KeInitializeEvent(&PingEventB, SynchronizationEvent, FALSE);
    KiCreateSystemThread(PingThreadA, nullptr, 0, 8);
    KiCreateSystemThread(PingThreadB, nullptr, 0, 8);
    KiCreateSystemThread(SleepThread, nullptr, 0, 10);
    KiCreateSystemThread(FpuThread, (PVOID)12345, 0, 8);
    KiCreateSystemThread(FpuThread, (PVOID)67890, 0, 8);
    KiCreateSystemThread(ControllerThread, nullptr, 0, 9);
    KiCreateSystemThread(SpinThread, (PVOID)&KiTestSpinA, 0, 1);
    KiCreateSystemThread(SpinThread, (PVOID)&KiTestSpinB, 0, 1);
}

void KiRunBootSelfTests() {
    KiStartThreadSelfTests();

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
