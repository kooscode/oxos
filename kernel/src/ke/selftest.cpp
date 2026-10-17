// Boot self-tests for milestone checks. Results are plain globals that tests/boot_test.py reads
// over the emulator monitor; nothing here is part of the kernel ABI.
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>

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

// ---------------------------------------------------------------- milestone 2b+: APCs, alerts, suspend, queues

volatile ULONG KiTestApcKernelRan, KiTestApcNormalAtPassive, KiTestApcRewaitOk;
volatile ULONG KiTestSpecialApcAtApcLevel;
volatile ULONG KiTestUserApcRan, KiTestUserApcStatus;
volatile ULONG KiTestAlertStatus;
volatile ULONG KiTestCriticalRegionOk;
volatile ULONG KiTestSuspendOk;
volatile ULONG KiTestQueueProcessed, KiTestQueueTimeout;
volatile ULONG KiTestDeviceQueueOk;
volatile ULONG KiTestBoostPriority;
volatile ULONG KiTestApcSuiteDone;

static KEVENT ApcWaitEvent, ApcTargetDone;
static KAPC TestKernelApc, TestSpecialApc, TestUserApc, CriticalApc;

static void NTAPI ApcKernelRoutine(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {
    KiTestApcKernelRan = KiTestApcKernelRan + 1;
}
static void NTAPI ApcNormalRoutine(PVOID, PVOID, PVOID) {
    KiTestApcNormalAtPassive = KeGetCurrentIrql() == PASSIVE_LEVEL;
}
static void NTAPI SpecialApcRoutine(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {
    KiTestSpecialApcAtApcLevel = KeGetCurrentIrql() == APC_LEVEL;
}
static void NTAPI UserApcNormal(PVOID, PVOID, PVOID) { KiTestUserApcRan = 1; }

// Waits non-alertably; a kernel APC must run and the wait must resume until the event is set.
static void NTAPI ApcTargetThread(PVOID) {
    NTSTATUS s = KeWaitForSingleObject(&ApcWaitEvent, Executive, KernelMode, FALSE, nullptr);
    KiTestApcRewaitOk = s == STATUS_WAIT_0 && KiTestApcKernelRan == 1;
    KeSetEvent(&ApcTargetDone, 0, FALSE);
}

static KEVENT UserApcReady;
static void NTAPI UserApcThread(PVOID) {
    KEVENT never;
    KeInitializeEvent(&never, NotificationEvent, FALSE);
    KeSetEvent(&UserApcReady, 0, FALSE);
    KiTestUserApcStatus = KeWaitForSingleObject(&never, UserRequest, UserMode, TRUE, nullptr);
}

static void NTAPI AlertThreadBody(PVOID) {
    LARGE_INTEGER t;
    t.QuadPart = -5000 * 10000LL; // 5 s; the alert arrives long before
    KiTestAlertStatus = KeDelayExecutionThread(KernelMode, TRUE, &t);
}

static volatile ULONG CriticalApcRan, CriticalRegionLeft;
static void NTAPI CriticalApcNormal(PVOID, PVOID, PVOID) { CriticalApcRan = CriticalRegionLeft ? 2 : 1; }
static void NTAPI CriticalRegionThread(PVOID) {
    KeEnterCriticalRegion();
    Sleep(30); // the APC is queued meanwhile and must wait
    CriticalRegionLeft = 1;
    KeLeaveCriticalRegion(); // delivered here
    Sleep(1);
}

static volatile ULONG SuspendCounter;
static void NTAPI SuspendSpinner(PVOID) {
    for (;;)
        SuspendCounter = SuspendCounter + 1;
}

static KQUEUE TestQueue;
static LIST_ENTRY QueueItems[10];
static void NTAPI QueueWorker(PVOID) {
    for (;;) {
        PLIST_ENTRY e = KeRemoveQueue(&TestQueue, KernelMode, nullptr);
        if ((ULONG)e >= 0x80000000)
            KiTestQueueProcessed = KiTestQueueProcessed + 1;
    }
}

static KEVENT BoostEvent;
static void NTAPI BoostWaiter(PVOID) {
    KeWaitForSingleObject(&BoostEvent, Executive, KernelMode, FALSE, nullptr);
    KiTestBoostPriority = (ULONG)KeGetCurrentThread()->Priority;
}

static void NTAPI ApcControllerThread(PVOID) {
    // Kernel APC interrupts a non-alertable wait, which then resumes.
    KeInitializeEvent(&ApcWaitEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ApcTargetDone, NotificationEvent, FALSE);
    PKTHREAD target = KiCreateSystemThread(ApcTargetThread, nullptr, 0, 8);
    Sleep(10);
    KeInitializeApc(&TestKernelApc, target, ApcKernelRoutine, nullptr, ApcNormalRoutine, KernelMode, nullptr);
    KeInsertQueueApc(&TestKernelApc, nullptr, nullptr, 0);
    Sleep(10);
    KeSetEvent(&ApcWaitEvent, 0, FALSE);
    KeWaitForSingleObject(&ApcTargetDone, Executive, KernelMode, FALSE, nullptr);

    // Special APC to ourselves runs at APC_LEVEL when our IRQL drops.
    KeInitializeApc(&TestSpecialApc, KeGetCurrentThread(), SpecialApcRoutine, nullptr, nullptr, KernelMode, nullptr);
    KeInsertQueueApc(&TestSpecialApc, nullptr, nullptr, 0);

    // User APC ends an alertable user-mode wait after running.
    KeInitializeEvent(&UserApcReady, NotificationEvent, FALSE);
    PKTHREAD userThread = KiCreateSystemThread(UserApcThread, nullptr, 0, 8);
    KeWaitForSingleObject(&UserApcReady, Executive, KernelMode, FALSE, nullptr);
    Sleep(5);
    KeInitializeApc(&TestUserApc, userThread, ApcKernelRoutine, nullptr, UserApcNormal, UserMode, nullptr);
    KeInsertQueueApc(&TestUserApc, nullptr, nullptr, 0);
    KeWaitForSingleObject(userThread, Executive, KernelMode, FALSE, nullptr);

    // Alert ends an alertable delay.
    PKTHREAD alertThread = KiCreateSystemThread(AlertThreadBody, nullptr, 0, 8);
    Sleep(10);
    KeAlertThread(alertThread, KernelMode);
    KeWaitForSingleObject(alertThread, Executive, KernelMode, FALSE, nullptr);

    // Critical region defers a normal kernel APC.
    PKTHREAD crit = KiCreateSystemThread(CriticalRegionThread, nullptr, 0, 8);
    Sleep(10);
    KeInitializeApc(&CriticalApc, crit, ApcKernelRoutine, nullptr, CriticalApcNormal, KernelMode, nullptr);
    KeInsertQueueApc(&CriticalApc, nullptr, nullptr, 0);
    KeWaitForSingleObject(crit, Executive, KernelMode, FALSE, nullptr);
    KiTestCriticalRegionOk = CriticalApcRan == 2;

    // Suspend stops a spinning thread; resume restarts it.
    PKTHREAD spinner = KiCreateSystemThread(SuspendSpinner, nullptr, 0, 2); // above the 2b spinners
    Sleep(20);
    KeSuspendThread(spinner);
    Sleep(5);
    ULONG frozen = SuspendCounter;
    Sleep(30);
    bool stopped = SuspendCounter == frozen;
    KeResumeThread(spinner);
    Sleep(30);
    KiTestSuspendOk = stopped && SuspendCounter != frozen;
    KeSuspendThread(spinner); // leave it parked

    // Queue with concurrency 1 feeds two workers.
    KeInitializeQueue(&TestQueue, 1);
    KiCreateSystemThread(QueueWorker, nullptr, 0, 8);
    KiCreateSystemThread(QueueWorker, nullptr, 0, 8);
    Sleep(5);
    for (auto &item : QueueItems)
        KeInsertQueue(&TestQueue, &item);
    Sleep(20);
    KQUEUE emptyQueue;
    KeInitializeQueue(&emptyQueue, 1);
    LARGE_INTEGER t;
    t.QuadPart = -10 * 10000;
    KiTestQueueTimeout = (ULONG)KeRemoveQueue(&emptyQueue, KernelMode, &t);

    // Device queue: idle insert starts the request, later ones queue; by-key removal order.
    KDEVICE_QUEUE dq;
    KDEVICE_QUEUE_ENTRY e1, e2, e3;
    KeInitializeDeviceQueue(&dq);
    bool ok = !KeInsertDeviceQueue(&dq, &e1) && dq.Busy;
    ok &= KeInsertByKeyDeviceQueue(&dq, &e2, 50) && KeInsertByKeyDeviceQueue(&dq, &e3, 20);
    ok &= KeRemoveByKeyDeviceQueue(&dq, 30) == &e2; // first key >= 30
    ok &= KeRemoveDeviceQueue(&dq) == &e3;
    ok &= KeRemoveDeviceQueue(&dq) == nullptr && !dq.Busy;
    KiTestDeviceQueueOk = ok;

    // Wake-up boost: base 8 + increment 2.
    KeInitializeEvent(&BoostEvent, SynchronizationEvent, FALSE);
    PKTHREAD boosted = KiCreateSystemThread(BoostWaiter, nullptr, 0, 8);
    Sleep(5);
    KeSetEvent(&BoostEvent, 2, FALSE);
    KeWaitForSingleObject(boosted, Executive, KernelMode, FALSE, nullptr);

    KiTestApcSuiteDone = 1;
}

// ---------------------------------------------------------------- milestone 2c: memory manager

volatile ULONG KiTestMmContiguousOk;   // VA = 0x80000000|PA, top-down, size query, data
volatile ULONG KiTestMmAlignedOk;      // Ex: below 16 MB, 64 KB aligned, write-combine protect
volatile ULONG KiTestMmReuseOk;        // free + reallocate returns the same block, page count restored
volatile ULONG KiTestMmRelocateOk;     // a contiguous request moves an unlocked page in use, not a locked one
volatile ULONG KiTestMmSystemOk;       // system memory in the PTE range, unmapped after free
volatile ULONG KiTestMmIoSpaceOk;      // MmMapIoSpace alias + MMIO pass-through
volatile ULONG KiTestMmPoolOk;         // 500 small blocks + a large block
volatile ULONG KiTestMmLayoutOk;       // no user mappings, read-only kernel text, stack guard page
volatile ULONG KiTestMmThreadChurnPages; // pages lost after 150 thread lifetimes (one unreaped stack at most)
volatile ULONG KiTestMmContiguousVa;
volatile ULONG KiTestMmDone;
volatile ULONG KiTestVmReserveCommitOk;  // reserve 1 MB, commit 64 KB inside, zeroed + writable, query states
volatile ULONG KiTestVmImageOk;          // explicit base 0x10000 reserve+commit like an XBE; conflict detected
volatile ULONG KiTestVmTopDownOk;        // MEM_TOP_DOWN lands high
volatile ULONG KiTestVmFreeRulesOk;      // release at non-base -> 0xC000009F; partial decommit; release
volatile ULONG KiTestVmProtectOk;        // NtProtectVirtualMemory RW -> RO
volatile ULONG KiTestVmPagesRestored;    // available pages identical before/after the VM tests
volatile ULONG KiTestVmStatsOk;          // MmQueryStatistics

static void NTAPI ShortThread(PVOID) {}

static void NTAPI MmControllerThread(PVOID) {
    // Contiguous memory.
    auto a = (ULONG *)MmAllocateContiguousMemory(0x3000);
    KiTestMmContiguousVa = (ULONG)a;
    bool ok = a && ((ULONG)a & 0xFFF) == 0 && (ULONG)a >= 0x83000000 &&
              MmGetPhysicalAddress(a) == ((ULONG)a & 0x7FFFFFFF) && MmQueryAllocationSize(a) == 0x3000;
    for (ULONG i = 0; ok && i < 0x3000 / 4; i++)
        a[i] = i * 2654435761u;
    for (ULONG i = 0; ok && i < 0x3000 / 4; i++)
        ok = a[i] == i * 2654435761u;
    KiTestMmContiguousOk = ok;

    auto b = MmAllocateContiguousMemoryEx(0x1000, 0, 0x00FFFFFF, 0x10000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    PHYSICAL_ADDRESS pb = MmGetPhysicalAddress(b);
    KiTestMmAlignedOk = b && pb < 0x01000000 && (pb & 0xFFFF) == 0 &&
                        MmQueryAddressProtect(b) == (PAGE_READWRITE | PAGE_WRITECOMBINE);
    MmFreeContiguousMemory(b);

    ULONG before = MmAvailablePages;
    MmFreeContiguousMemory(a);
    ULONG afterFree = MmAvailablePages;
    auto a2 = MmAllocateContiguousMemory(0x3000);
    KiTestMmReuseOk = a2 == a && afterFree == before + 3;

    // A contiguous request over a page in use: the page moves (contents kept, same VA); a locked page does not.
    auto sp = (ULONG *)MmAllocateSystemMemory(PAGE_SIZE, PAGE_READWRITE);
    ok = sp != nullptr;
    if (ok) {
        sp[0] = 0x12345678, sp[1023] = 0x9ABCDEF0;
        PHYSICAL_ADDRESS old = MmGetPhysicalAddress(sp);
        MmLockUnlockBufferPages(sp, PAGE_SIZE, FALSE);
        ok = !MmAllocateContiguousMemoryEx(PAGE_SIZE, old, old + PAGE_SIZE - 1, 0, PAGE_READWRITE);
        MmLockUnlockBufferPages(sp, PAGE_SIZE, TRUE);
        PVOID c = MmAllocateContiguousMemoryEx(PAGE_SIZE, old, old + PAGE_SIZE - 1, 0, PAGE_READWRITE);
        ok = ok && c && MmGetPhysicalAddress(c) == old && MmGetPhysicalAddress(sp) != old && sp[0] == 0x12345678 &&
             sp[1023] == 0x9ABCDEF0;
        if (c)
            MmFreeContiguousMemory(c);
        MmFreeSystemMemory(sp, PAGE_SIZE);
    }
    KiTestMmRelocateOk = ok;

    // System memory.
    auto s = (ULONG *)MmAllocateSystemMemory(0x5000, PAGE_READWRITE);
    ok = s && (ULONG)s >= MM_SYSTEM_PTE_BASE && (ULONG)s < MM_SYSTEM_PTE_END && MmQueryAllocationSize(s) == 0x5000 &&
         MmGetPhysicalAddress(s) != 0;
    for (ULONG i = 0; ok && i < 0x5000 / 4; i++)
        s[i] = ~i;
    for (ULONG i = 0; ok && i < 0x5000 / 4; i++)
        ok = s[i] == ~i;
    MmFreeSystemMemory(s, 0x5000);
    KiTestMmSystemOk = ok && !MmIsAddressValid(s);

    // I/O space: alias a contiguous page through system PTEs; MMIO pass-through.
    auto alias = (volatile ULONG *)MmMapIoSpace(MmGetPhysicalAddress(a2), 0x1000, PAGE_READWRITE);
    ok = alias && (ULONG)alias >= MM_SYSTEM_PTE_BASE;
    if (ok) {
        alias[7] = 0x5A5AA5A5;
        ok = ((volatile ULONG *)a2)[7] == 0x5A5AA5A5;
        MmUnmapIoSpace((PVOID)alias, 0x1000);
        ok &= !MmIsAddressValid((PVOID)alias);
    }
    KiTestMmIoSpaceOk = ok && MmMapIoSpace(0xFD000000, 0x1000, PAGE_READWRITE | PAGE_NOCACHE) == (PVOID)0xFD000000;
    MmFreeContiguousMemory(a2);

    // Pool.
    static UCHAR *blocks[500];
    ok = true;
    for (ULONG i = 0; i < 500; i++) {
        ULONG size = (i * 37) % 4000 + 1;
        blocks[i] = (UCHAR *)ExAllocatePoolWithTag(size, 'tseT');
        ULONG q = blocks[i] ? ExQueryPoolBlockSize(blocks[i]) : 0;
        ok &= blocks[i] && q >= size && (q + 8) % 32 == 0 && ((ULONG)blocks[i] & 7) == 0;
        if (blocks[i])
            for (ULONG j = 0; j < size; j++)
                blocks[i][j] = (UCHAR)i;
    }
    for (ULONG i = 0; ok && i < 500; i++) {
        ULONG size = (i * 37) % 4000 + 1;
        for (ULONG j = 0; j < size && ok; j++)
            ok = blocks[i][j] == (UCHAR)i;
    }
    for (auto p : blocks)
        if (p)
            ExFreePool(p);
    auto big = ExAllocatePoolWithTag(0x2000, 'giB ');
    ok &= big && ((ULONG)big & 0xFFF) == 0 && ExQueryPoolBlockSize(big) == 0x2000;
    ExFreePool(big);
    KiTestMmPoolOk = ok;

    // Layout: no user mappings, read-only kernel code, guard page under our stack.
    PKTHREAD self = KeGetCurrentThread();
    KiTestMmLayoutOk = !MmIsAddressValid((PVOID)0x10000) && MmQueryAddressProtect((PVOID)&KiRunBootSelfTests) == PAGE_READONLY &&
                       !MmIsAddressValid((UCHAR *)self->StackLimit - 1) && MmIsAddressValid(self->StackLimit);

    // Thread churn through the real lifecycle: create, wait on the handle, close. Stacks and objects return.
    ULONG baseline = MmAvailablePages;
    for (int i = 0; i < 150; i++) {
        HANDLE h;
        if (NT_SUCCESS(PsCreateSystemThread(&h, nullptr, (PVOID)ShortThread, nullptr, FALSE))) {
            NtWaitForSingleObject(h, FALSE, nullptr);
            NtClose(h);
        }
    }
    Sleep(5);
    KiTestMmThreadChurnPages = baseline - MmAvailablePages;

    // Virtual memory.
    ULONG vmBaseline = MmAvailablePages;
    PVOID base = nullptr;
    SIZE_T size = 0x100000;
    NTSTATUS st = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ok = st == 0 && base && ((ULONG)base & 0xFFFF) == 0 && (ULONG)base >= 0x10000 && size == 0x100000;
    PVOID commit = (UCHAR *)base + 0x20000;
    SIZE_T commitSize = 0x10000;
    ok &= NtAllocateVirtualMemory(&commit, 0, &commitSize, MEM_COMMIT, PAGE_READWRITE) == 0;
    auto w = (ULONG *)commit;
    for (ULONG i = 0; ok && i < 0x10000 / 4; i++)
        ok = w[i] == 0;
    for (ULONG i = 0; ok && i < 0x10000 / 4; i++)
        w[i] = i;
    MEMORY_BASIC_INFORMATION mbi;
    NtQueryVirtualMemory(base, &mbi);
    ok &= mbi.State == MEM_RESERVE && mbi.RegionSize == 0x20000 && mbi.AllocationBase == base;
    NtQueryVirtualMemory(commit, &mbi);
    ok &= mbi.State == MEM_COMMIT && mbi.RegionSize == 0x10000 && mbi.Protect == PAGE_READWRITE && mbi.Type == MEM_PRIVATE;
    KiTestVmReserveCommitOk = ok;

    // Protect RW -> RO.
    PVOID pbase = commit;
    SIZE_T psize = 0x1000;
    ULONG oldProtect = 0;
    ok = NtProtectVirtualMemory(&pbase, &psize, PAGE_READONLY, &oldProtect) == 0 && oldProtect == PAGE_READWRITE;
    NtQueryVirtualMemory(commit, &mbi);
    KiTestVmProtectOk = ok && mbi.Protect == PAGE_READONLY && mbi.RegionSize == 0x1000 && w[5] == 5;

    // Free rules: release at a non-base address fails; decommit part; release all.
    PVOID fbase = commit;
    SIZE_T fsize = 0;
    ok = NtFreeVirtualMemory(&fbase, &fsize, MEM_RELEASE) == (NTSTATUS)0xC000009F;
    fbase = commit;
    fsize = 0x8000;
    ok &= NtFreeVirtualMemory(&fbase, &fsize, MEM_DECOMMIT) == 0;
    NtQueryVirtualMemory(commit, &mbi);
    ok &= mbi.State == MEM_RESERVE;
    fbase = base;
    fsize = 0;
    ok &= NtFreeVirtualMemory(&fbase, &fsize, MEM_RELEASE) == 0 && fsize == 0x100000;
    NtQueryVirtualMemory(base, &mbi);
    KiTestVmFreeRulesOk = ok && mbi.State == MEM_FREE;

    // XBE-style image: reserve + commit at 0x10000; a second reservation over it conflicts.
    PVOID img = (PVOID)0x10000;
    SIZE_T imgSize = 0x30000;
    ok = NtAllocateVirtualMemory(&img, 0, &imgSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) == 0 &&
         img == (PVOID)0x10000 && MmIsAddressValid((PVOID)0x3F000);
    *(volatile ULONG *)0x10000 = 0x48454258; // 'XBEH'
    PVOID img2 = (PVOID)0x20000;
    SIZE_T img2Size = 0x1000;
    ok &= NtAllocateVirtualMemory(&img2, 0, &img2Size, MEM_RESERVE, PAGE_READWRITE) == (NTSTATUS)0xC0000018;
    fbase = img;
    fsize = 0;
    ok &= NtFreeVirtualMemory(&fbase, &fsize, MEM_RELEASE) == 0 && !MmIsAddressValid((PVOID)0x10000);
    KiTestVmImageOk = ok;

    // Top-down reservation.
    PVOID top = nullptr;
    SIZE_T topSize = 0x10000;
    ok = NtAllocateVirtualMemory(&top, 0, &topSize, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE) == 0 &&
         (ULONG)top >= 0x7FF00000 && (ULONG)top + topSize <= 0x7FFE0000;
    fbase = top;
    fsize = 0;
    NtFreeVirtualMemory(&fbase, &fsize, MEM_RELEASE);
    KiTestVmTopDownOk = ok;

    KiTestVmPagesRestored = MmAvailablePages == vmBaseline;
    MM_STATISTICS stats;
    stats.Length = sizeof(stats);
    KiTestVmStatsOk = MmQueryStatistics(&stats) == 0 && stats.TotalPhysicalPages == 0x4000 &&
                      stats.AvailablePages == MmAvailablePages && stats.StackPagesCommitted > 0 &&
                      stats.PoolPagesCommitted > 0 && stats.VirtualMemoryBytesReserved == 0;
    KiTestMmDone = 1;
}

// ---------------------------------------------------------------- milestone 3a: object manager, Nt objects, Ps

volatile ULONG KiTestObHandlesOk, KiTestObNamedOk, KiTestObNamespaceOk, KiTestObNameErrorsOk;
volatile ULONG KiTestPsThreadOk, KiTestPsNotifyCount, KiTestNtApcOk, KiTestNtWaitMultipleOk, KiTestObDuplicateOk;
volatile ULONG KiTestObPoolBalanced, KiTestObDone;
extern volatile ULONG ExpPoolBlocks;

static OBJECT_ATTRIBUTES ObAttrs(OBJECT_STRING *s, PCSZ text, ULONG attributes = 0, HANDLE root = nullptr) {
    RtlInitAnsiString(s, text);
    return OBJECT_ATTRIBUTES{root, s, attributes | OBJ_CASE_INSENSITIVE};
}

static void NTAPI CountNotify(PVOID, HANDLE, BOOLEAN) { KiTestPsNotifyCount = KiTestPsNotifyCount + 1; }

static volatile ULONG TlsSeenOk, ApcArgsOk;
static void NTAPI TlsThreadBody(PVOID tlsSize, PVOID) {
    PKTHREAD t = KeGetCurrentThread();
    auto tls = (UCHAR *)t->TlsData;
    bool ok = tls && tls + (ULONG)tlsSize == (UCHAR *)t->StackBase - NPX_FRAME_LENGTH;
    for (ULONG i = 0; ok && i < (ULONG)tlsSize; i++)
        ok = tls[i] == 0;
    TlsSeenOk = ok;
    PsTerminateSystemThread(0x1234);
}

static void NTAPI UserApcRoutine(PVOID a, PVOID b, PVOID c) { ApcArgsOk = a == (PVOID)1 && b == (PVOID)2 && c == (PVOID)3; }
static volatile NTSTATUS AlertableWaitStatus;
static HANDLE ApcWaitEventHandle;
static void NTAPI AlertableWaiter(PVOID) {
    AlertableWaitStatus = NtWaitForSingleObjectEx(ApcWaitEventHandle, UserMode, TRUE, nullptr);
}

static void NTAPI ObControllerThread(PVOID) {
    Sleep(50); // let the other suites create their objects first
    ULONG poolBefore = ExpPoolBlocks;

    // Handles: multiples of 4, LIFO reuse.
    HANDLE h1, h2, h3;
    NtCreateEvent(&h1, nullptr, NotificationEvent, FALSE);
    NtCreateEvent(&h2, nullptr, NotificationEvent, FALSE);
    NtClose(h1);
    NtCreateEvent(&h3, nullptr, NotificationEvent, FALSE);
    KiTestObHandlesOk = ((ULONG)h2 & 3) == 0 && (ULONG)h2 >= 4 && h3 == h1 && NtClose((HANDLE)0x7FFC) == STATUS_INVALID_HANDLE;
    NtClose(h2);
    NtClose(h3);

    // Named objects.
    OBJECT_STRING s1, s2, s3;
    OBJECT_ATTRIBUTES a = ObAttrs(&s1, "\\Win32NamedObjects\\XkEvent");
    OBJECT_ATTRIBUTES aOpenIf = ObAttrs(&s2, "\\Win32NamedObjects\\XkEvent", OBJ_OPENIF);
    OBJECT_ATTRIBUTES aRel = ObAttrs(&s3, "xkevent", 0, ObWin32NamedObjectsDirectory());
    HANDLE ha, hb, hc, hd;
    bool ok = NtCreateEvent(&ha, &a, NotificationEvent, FALSE) == 0;
    ok &= NtCreateEvent(&hb, &aOpenIf, NotificationEvent, FALSE) == STATUS_OBJECT_NAME_EXISTS;
    ok &= NtCreateEvent(&hc, &a, NotificationEvent, FALSE) == STATUS_OBJECT_NAME_COLLISION;
    ok &= ObOpenObjectByName(&aRel, &ExEventObjectType, nullptr, &hd) == 0;
    PVOID oa, ob, od;
    ObReferenceObjectByHandle(ha, nullptr, &oa);
    ObReferenceObjectByHandle(hb, nullptr, &ob);
    ObReferenceObjectByHandle(hd, nullptr, &od);
    ok &= oa == ob && ob == od;
    ObfDereferenceObject(oa);
    ObfDereferenceObject(ob);
    ObfDereferenceObject(od);
    NtSetEvent(hb, nullptr);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ok &= NtWaitForSingleObject(ha, FALSE, &zero) == 0;
    NtClose(ha);
    NtClose(hb);
    NtClose(hd);
    PVOID gone;
    ok &= ObReferenceObjectByName(&s1, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_NAME_NOT_FOUND;
    KiTestObNamedOk = ok;

    // Directory + symbolic link + queries.
    OBJECT_STRING ds, es, ls, ts, ps;
    OBJECT_ATTRIBUTES da = ObAttrs(&ds, "\\XkDir"), ea = ObAttrs(&es, "\\XkDir\\E1");
    HANDLE hdir = nullptr, he = nullptr;
    ok = NtCreateDirectoryObject(&hdir, &da) == 0 && NtCreateEvent(&he, &ea, NotificationEvent, FALSE) == 0;
    RtlInitAnsiString(&ls, "\\??\\Q:");
    RtlInitAnsiString(&ts, "\\XkDir");
    ok &= ObpCreateSymbolicLink(&ls, &ts) == 0;
    PVOID viaLink, direct;
    RtlInitAnsiString(&ps, "\\??\\Q:\\E1");
    ok &= ObReferenceObjectByName(&ps, 0, nullptr, nullptr, &viaLink) == 0;
    ObReferenceObjectByHandle(he, nullptr, &direct);
    ok &= viaLink == direct;
    ObfDereferenceObject(viaLink);
    ObfDereferenceObject(direct);
    HANDLE hl;
    OBJECT_ATTRIBUTES la = {nullptr, &ls, OBJ_CASE_INSENSITIVE};
    char target[32];
    OBJECT_STRING tgt = {0, sizeof(target), target};
    ok &= NtOpenSymbolicLinkObject(&hl, &la) == 0 && NtQuerySymbolicLinkObject(hl, &tgt, nullptr) == 0 &&
          RtlEqualString(&tgt, &ts, TRUE);
    NtClose(hl);
    ULONG ctx = 0, ret;
    UCHAR buf[64];
    ok &= NtQueryDirectoryObject(hdir, buf, sizeof(buf), TRUE, &ctx, &ret) == 0;
    OBJECT_STRING e1;
    RtlInitAnsiString(&e1, "E1");
    ok &= RtlEqualString(&((POBJECT_DIRECTORY_INFORMATION)buf)->Name, &e1, TRUE);
    ok &= NtQueryDirectoryObject(hdir, buf, sizeof(buf), FALSE, &ctx, &ret) == STATUS_NO_MORE_ENTRIES;
    KiTestObNamespaceOk = ok;
    NtClose(he);

    // Name errors.
    OBJECT_STRING n1, n2, n3;
    RtlInitAnsiString(&n1, "\\XkDir\\Missing\\X");
    RtlInitAnsiString(&n2, "\\XkDir\\Missing");
    RtlInitAnsiString(&n3, "relative");
    KiTestObNameErrorsOk = ObReferenceObjectByName(&n1, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_PATH_NOT_FOUND &&
                           ObReferenceObjectByName(&n2, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_NAME_NOT_FOUND &&
                           ObReferenceObjectByName(&n3, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_NAME_INVALID;
    NtClose(hdir);

    // Ps: TLS, extension, ids, notify routines, exit status.
    PsSetCreateThreadNotifyRoutine(CountNotify);
    HANDLE ht, tid;
    ok = PsCreateSystemThreadEx(&ht, 32, 0x4000, 64, &tid, (PVOID)64, nullptr, FALSE, FALSE,
                                (PKSYSTEM_ROUTINE)TlsThreadBody) == 0;
    ok &= NtWaitForSingleObject(ht, FALSE, nullptr) == 0;
    PVOID to;
    ObReferenceObjectByHandle(ht, &PsThreadObjectType, &to);
    ok &= *(NTSTATUS *)((UCHAR *)to + 0x120) == 0x1234 && ((ULONG)tid & 3) == 0 && TlsSeenOk;
    ObfDereferenceObject(to);
    NtClose(ht);
    KiTestPsThreadOk = ok;

    // NtQueueApcThread into an alertable user-mode wait.
    NtCreateEvent(&ApcWaitEventHandle, nullptr, NotificationEvent, FALSE);
    HANDLE hw;
    PsCreateSystemThread(&hw, nullptr, (PVOID)AlertableWaiter, nullptr, FALSE);
    Sleep(10);
    NtQueueApcThread(hw, UserApcRoutine, (PVOID)1, (PVOID)2, (PVOID)3);
    NtWaitForSingleObject(hw, FALSE, nullptr);
    KiTestNtApcOk = AlertableWaitStatus == STATUS_USER_APC && ApcArgsOk;
    NtClose(hw);

    // WaitAny over handles: unsignalled event + semaphore with count 1 -> index 1.
    HANDLE hs, waitHandles[2];
    NtCreateSemaphore(&hs, nullptr, 1, 1);
    waitHandles[0] = ApcWaitEventHandle;
    waitHandles[1] = hs;
    KiTestNtWaitMultipleOk = NtWaitForMultipleObjectsEx(2, waitHandles, WaitAny, KernelMode, FALSE, nullptr) == 1;

    // Duplicate keeps the object alive after the source closes.
    HANDLE hdup;
    ok = NtDuplicateObject(hs, &hdup, 1 /*DUPLICATE_CLOSE_SOURCE*/) == 0 && NtClose(hs) == STATUS_INVALID_HANDLE;
    ok &= NtReleaseSemaphore(hdup, 1, nullptr) == 0 && NtWaitForSingleObject(hdup, FALSE, &zero) == 0;
    KiTestObDuplicateOk = ok;
    NtClose(hdup);
    NtClose(ApcWaitEventHandle);

    // Everything created above was released (the Q: link and its directory are permanent).
    Sleep(5);
    KiTestObPoolBalanced = ExpPoolBlocks <= poolBefore + 6;
    KiTestObDone = 1;
}

void KiStartIoSelfTests();
void KiStartIdexSelfTests();
void KiStartFatxSelfTests();
void KiStartDvdSelfTests();
void KiStartHalSelfTests();
void KiStartRtlSelfTests();
void KiStartSehSelfTests();
void KiStartXcSelfTests();
void KiStartMiscSelfTests();

static void KiStartThreadSelfTests() {
    KiStartIoSelfTests();
    KiStartIdexSelfTests();
    KiStartFatxSelfTests();
    KiStartDvdSelfTests();
    KiStartHalSelfTests();
    KiStartRtlSelfTests();
    KiStartSehSelfTests();
    KiStartXcSelfTests();
    KiStartMiscSelfTests();
    KiCreateSystemThread(ObControllerThread, nullptr, 0, 9);
    KiCreateSystemThread(MmControllerThread, nullptr, 0, 9);
    KiCreateSystemThread(ApcControllerThread, nullptr, 0, 9);
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
