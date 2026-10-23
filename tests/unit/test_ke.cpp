// Kernel dispatcher: IRQL and software interrupts, DPCs, timers, threads and scheduling, waits and dispatcher
// objects, APCs, suspend/resume, queues, interrupt objects.
#include "kernel_test.h"

#include <atomic>
#include <string>
#include <vector>

#define Prcb (KiPcr.PrcbData)

// --- IRQL and software interrupts

TEST(Irql, RaiseAndLower) {
    EXPECT_EQ(KeGetCurrentIrql(), PASSIVE_LEVEL);
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    EXPECT_EQ(old, PASSIVE_LEVEL);
    EXPECT_EQ(KeGetCurrentIrql(), DISPATCH_LEVEL);
    EXPECT_EQ(KfRaiseIrql(APC_LEVEL), DISPATCH_LEVEL) << "never lowers";
    EXPECT_EQ(KeGetCurrentIrql(), DISPATCH_LEVEL);
    KIRQL old2 = KeRaiseIrqlToSynchLevel();
    EXPECT_EQ(old2, DISPATCH_LEVEL);
    KfLowerIrql(old2);
    KfLowerIrql(old);
    EXPECT_EQ(KeGetCurrentIrql(), PASSIVE_LEVEL);
}

TEST(Irql, DevicesAboveTheCurrentLevelStayUnmaskedOthersMasked) {
    // At DISPATCH_LEVEL the clock (IRQ 0, level 28) is still taken; at 28 it is masked.
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ULONG t0 = KeTickCount;
    host::AdvanceTime(3);
    EXPECT_EQ(KeTickCount - t0, 3u);
    KfRaiseIrql(28);
    host::AdvanceTime(3);
    EXPECT_EQ(KeTickCount - t0, 3u) << "clock masked at CLOCK_LEVEL";
    KfLowerIrql(old); // the pending tick is delivered now
    EXPECT_GE(KeTickCount - t0, 4u);
}

// --- DPCs

namespace {
std::vector<std::string> Events;
KDPC DpcA, DpcB;
void NTAPI RecordDpc(PKDPC, PVOID ctx, PVOID a1, PVOID) {
    Events.push_back(std::string((const char *)ctx) + "@" + std::to_string(KeGetCurrentIrql()) +
                     (KeIsExecutingDpc() ? "+dpc" : "") + (a1 ? "+arg" : ""));
}
} // namespace

TEST(Dpc, RunsAtDispatchLevelOnceIrqlDrops) {
    Events.clear();
    KeInitializeDpc(&DpcA, RecordDpc, (PVOID) "A");
    KeInitializeDpc(&DpcB, RecordDpc, (PVOID) "B");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    EXPECT_TRUE(KeInsertQueueDpc(&DpcA, (PVOID)1, nullptr));
    EXPECT_FALSE(KeInsertQueueDpc(&DpcA, nullptr, nullptr)) << "already queued";
    EXPECT_TRUE(KeInsertQueueDpc(&DpcB, nullptr, nullptr));
    EXPECT_TRUE(Events.empty()) << "not at DISPATCH_LEVEL";
    KfLowerIrql(old);
    ASSERT_EQ(Events.size(), 2u);
    EXPECT_EQ(Events[0], "A@2+dpc+arg");
    EXPECT_EQ(Events[1], "B@2+dpc");
}

TEST(Dpc, RemovedDpcDoesNotRun) {
    Events.clear();
    KeInitializeDpc(&DpcA, RecordDpc, (PVOID) "A");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KeInsertQueueDpc(&DpcA, nullptr, nullptr);
    EXPECT_TRUE(KeRemoveQueueDpc(&DpcA));
    EXPECT_FALSE(KeRemoveQueueDpc(&DpcA));
    KfLowerIrql(old);
    EXPECT_TRUE(Events.empty());
}

TEST(Dpc, AfterARemovedDpcTheNextOneStillRequestsItsInterrupt) {
    // Regression (2026-10-23): the DISPATCH interrupt of a DPC that was removed again found an empty list and left
    // DpcInterruptRequested set, so the next DPC never asked for its interrupt.
    Events.clear();
    KeInitializeDpc(&DpcA, RecordDpc, (PVOID) "A");
    KeInitializeDpc(&DpcB, RecordDpc, (PVOID) "B");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KeInsertQueueDpc(&DpcA, nullptr, nullptr);
    KeRemoveQueueDpc(&DpcA);
    KfLowerIrql(old); // the DISPATCH interrupt runs with nothing queued
    EXPECT_FALSE(Prcb.DpcInterruptRequested);
    KeInsertQueueDpc(&DpcB, nullptr, nullptr); // at PASSIVE_LEVEL: runs at once
    ASSERT_EQ(Events.size(), 1u);
    EXPECT_EQ(Events[0], "B@2+dpc");
}

// --- Timers

namespace {
volatile int TimerDpcs;
KDPC TimerDpc;
void NTAPI CountDpc(PKDPC, PVOID, PVOID, PVOID) { TimerDpcs = TimerDpcs + 1; }
} // namespace

TEST(Timer, RelativeTimerSignalsAndQueuesItsDpcWhenDue) {
    KTIMER t;
    KeInitializeTimerEx(&t, NotificationTimer);
    KeInitializeDpc(&TimerDpc, CountDpc, nullptr);
    TimerDpcs = 0;
    EXPECT_FALSE(KeSetTimer(&t, RelativeMs(10), &TimerDpc));
    host::AdvanceTime(9);
    EXPECT_EQ(t.Header.SignalState, 0);
    EXPECT_EQ(TimerDpcs, 0);
    host::AdvanceTime(2);
    EXPECT_EQ(t.Header.SignalState, 1);
    EXPECT_EQ(TimerDpcs, 1);
    EXPECT_FALSE(KeCancelTimer(&t)) << "already expired";
}

TEST(Timer, CancelledTimerNeverFires) {
    KTIMER t;
    KeInitializeTimerEx(&t, NotificationTimer);
    KeInitializeDpc(&TimerDpc, CountDpc, nullptr);
    TimerDpcs = 0;
    KeSetTimer(&t, RelativeMs(5), &TimerDpc);
    EXPECT_TRUE(KeSetTimer(&t, RelativeMs(5), &TimerDpc)) << "re-arming reports it was set";
    EXPECT_TRUE(KeCancelTimer(&t));
    host::AdvanceTime(20);
    EXPECT_EQ(TimerDpcs, 0);
    EXPECT_EQ(t.Header.SignalState, 0);
}

TEST(Timer, PeriodicTimerKeepsFiring) {
    KTIMER t;
    KeInitializeTimerEx(&t, SynchronizationTimer);
    KeInitializeDpc(&TimerDpc, CountDpc, nullptr);
    TimerDpcs = 0;
    KeSetTimerEx(&t, RelativeMs(3), 3, &TimerDpc);
    host::AdvanceTime(31);
    EXPECT_GE(TimerDpcs, 9);
    EXPECT_LE(TimerDpcs, 11);
    KeCancelTimer(&t);
    int n = TimerDpcs;
    host::AdvanceTime(10);
    EXPECT_EQ(TimerDpcs, n);
}

TEST(Timer, TimersFireInDueOrderAndPastDueTimesFireAtOnce) {
    KTIMER a, b, c;
    KeInitializeTimerEx(&a, NotificationTimer);
    KeInitializeTimerEx(&b, NotificationTimer);
    KeInitializeTimerEx(&c, NotificationTimer);
    KeSetTimer(&a, RelativeMs(8), nullptr);
    KeSetTimer(&b, RelativeMs(4), nullptr);
    host::AdvanceTime(5);
    EXPECT_EQ(a.Header.SignalState, 0);
    EXPECT_EQ(b.Header.SignalState, 1);
    LARGE_INTEGER past;
    past.QuadPart = 1; // an absolute time long gone
    KeSetTimer(&c, past, nullptr);
    EXPECT_EQ(c.Header.SignalState, 1);
    host::AdvanceTime(5);
    EXPECT_EQ(a.Header.SignalState, 1);
}

// --- Threads, priorities, waits

TEST(Thread, RunsAndCanBeJoined) {
    std::atomic<int> ran{0};
    PKTHREAD t = Spawn([&] { ran = KeGetCurrentIrql() == PASSIVE_LEVEL ? 1 : 2; });
    ASSERT_NE(t, nullptr);
    Join(t);
    EXPECT_EQ(ran, 1);
}

TEST(Thread, HigherPriorityThreadPreemptsAtOnce) {
    KEVENT go;
    KeInitializeEvent(&go, SynchronizationEvent, FALSE);
    std::vector<int> order;
    PKTHREAD high = Spawn([&] {
        KeWaitForSingleObject(&go, Executive, KernelMode, FALSE, nullptr);
        order.push_back(2);
    }, 20);
    SleepMs(1); // let it reach its wait
    order.push_back(1);
    KeSetEvent(&go, 0, FALSE); // wakes the priority-20 thread: it runs before KeSetEvent returns
    order.push_back(3);
    Join(high);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(Thread, LowerPriorityThreadWaitsForTheCpu) {
    std::vector<int> order;
    PKTHREAD low = Spawn([&] { order.push_back(2); }, 4);
    order.push_back(1); // we (priority 8) keep running until we wait
    Join(low);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(Thread, EqualPriorityThreadsShareTheCpuByQuantum) {
    std::atomic<bool> stop{false};
    std::atomic<int> otherRan{0};
    PKTHREAD other = Spawn([&] {
        while (!stop) {
            otherRan++;
            host::AdvanceTime(1); // clock ticks inside the busy loop: quantum end
        }
    }, 8);
    for (int i = 0; i < 200 && !otherRan; i++)
        host::AdvanceTime(1); // our quantum runs out; the other thread gets the CPU
    EXPECT_GT(otherRan, 0);
    stop = true;
    Join(other);
}

TEST(Thread, PriorityChangesTakeEffect) {
    PKTHREAD self = KeGetCurrentThread();
    LONG old = KeSetPriorityThread(self, 12);
    EXPECT_EQ(self->Priority, 12);
    std::vector<int> order;
    PKTHREAD t = Spawn([&] { order.push_back(2); }, 10);
    order.push_back(1); // 12 > 10: still ours
    KeSetPriorityThread(self, old);
    // Now 8 < 10: the other thread runs as soon as we lowered ourselves.
    order.push_back(3);
    Join(t);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(Wait, PollAndTimeout) {
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    uint64_t t0 = host::NowMs();
    LARGE_INTEGER t = RelativeMs(20);
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &t), STATUS_TIMEOUT);
    EXPECT_GE(host::NowMs() - t0, 20u);
    EXPECT_LE(host::NowMs() - t0, 22u);
}

TEST(Wait, NotificationEventReleasesEveryWaiterAndStaysSignalled) {
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    std::atomic<int> woke{0};
    std::vector<PKTHREAD> ts;
    for (int i = 0; i < 3; i++)
        ts.push_back(Spawn([&] {
            KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, nullptr);
            woke++;
        }));
    SleepMs(1);
    EXPECT_EQ(woke, 0);
    EXPECT_EQ(KeSetEvent(&e, 0, FALSE), 0) << "previous state";
    for (auto t : ts)
        Join(t);
    EXPECT_EQ(woke, 3);
    EXPECT_EQ(e.Header.SignalState, 1);
    EXPECT_EQ(KeResetEvent(&e), 1);
    EXPECT_EQ(e.Header.SignalState, 0);
}

TEST(Wait, SynchronizationEventReleasesOneWaiterAtATime) {
    KEVENT e;
    KeInitializeEvent(&e, SynchronizationEvent, FALSE);
    std::atomic<int> woke{0};
    std::vector<PKTHREAD> ts;
    for (int i = 0; i < 3; i++)
        ts.push_back(Spawn([&] {
            KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, nullptr);
            woke++;
        }));
    SleepMs(1);
    KeSetEvent(&e, 0, FALSE);
    SleepMs(1);
    EXPECT_EQ(woke, 1);
    EXPECT_EQ(e.Header.SignalState, 0) << "consumed by the waiter";
    KeSetEvent(&e, 0, FALSE);
    KeSetEvent(&e, 0, FALSE);
    for (auto t : ts)
        Join(t);
    EXPECT_EQ(woke, 3);
}

TEST(Wait, PulseWakesWaitersAndLeavesTheEventClear) {
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    std::atomic<int> woke{0};
    PKTHREAD t = Spawn([&] {
        KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, nullptr);
        woke++;
    });
    SleepMs(1);
    KePulseEvent(&e, 0, FALSE);
    EXPECT_EQ(e.Header.SignalState, 0);
    Join(t);
    EXPECT_EQ(woke, 1);
}

TEST(Wait, WaitAnyReturnsTheIndexWaitAllNeedsEveryObject) {
    KEVENT a, b;
    KeInitializeEvent(&a, NotificationEvent, FALSE);
    KeInitializeEvent(&b, NotificationEvent, FALSE);
    PVOID objs[2] = {&a, &b};
    KeSetEvent(&b, 0, FALSE);
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, nullptr, nullptr), 1);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAll, Executive, KernelMode, FALSE, &zero, nullptr), STATUS_TIMEOUT);
    std::atomic<NTSTATUS> result{-1};
    PKTHREAD t = Spawn([&] {
        result = KeWaitForMultipleObjects(2, objs, WaitAll, Executive, KernelMode, FALSE, nullptr, nullptr);
    });
    SleepMs(1);
    EXPECT_EQ(result, -1);
    KeSetEvent(&a, 0, FALSE);
    Join(t);
    EXPECT_EQ(result, STATUS_SUCCESS);
}

TEST(Wait, MultipleObjectsWithATimeout) {
    KEVENT a, b;
    KeInitializeEvent(&a, SynchronizationEvent, FALSE);
    KeInitializeEvent(&b, SynchronizationEvent, FALSE);
    PVOID objs[2] = {&a, &b};
    LARGE_INTEGER t = RelativeMs(5);
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &t, nullptr), STATUS_TIMEOUT);
    // The timed-out wait left nothing behind: a later signal is still there for the next waiter.
    KeSetEvent(&a, 0, FALSE);
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &t, nullptr), 0);
    EXPECT_EQ(a.Header.SignalState, 0);
}

TEST(Wait, TooManyObjectsWithoutWaitBlocksIsABugCheck) {
    KEVENT e[4];
    PVOID objs[4];
    for (int i = 0; i < 4; i++) {
        KeInitializeEvent(&e[i], NotificationEvent, TRUE);
        objs[i] = &e[i];
    }
    EXPECT_BUGCHECK(0x0C, KeWaitForMultipleObjects(4, objs, WaitAny, Executive, KernelMode, FALSE, nullptr, nullptr));
}

TEST(Semaphore, CountsAndLimit) {
    KSEMAPHORE s;
    KeInitializeSemaphore(&s, 0, 2);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    EXPECT_EQ(KeReleaseSemaphore(&s, 0, 2, FALSE), 0);
    EXPECT_EQ(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    EXPECT_EQ(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    EXPECT_EQ(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    KeReleaseSemaphore(&s, 0, 2, FALSE);
    EXPECT_BUGCHECK(0x1E, KeReleaseSemaphore(&s, 0, 1, FALSE)); // above the limit
}

TEST(Mutant, RecursiveOwnershipAndRelease) {
    KMUTANT m;
    KeInitializeMutant(&m, FALSE);
    EXPECT_EQ(KeWaitForSingleObject(&m, Executive, KernelMode, FALSE, nullptr), STATUS_SUCCESS);
    EXPECT_EQ(KeWaitForSingleObject(&m, Executive, KernelMode, FALSE, nullptr), STATUS_SUCCESS) << "recursive";
    std::atomic<int> got{0};
    PKTHREAD t = Spawn([&] {
        KeWaitForSingleObject(&m, Executive, KernelMode, FALSE, nullptr);
        got = 1;
        KeReleaseMutant(&m, 0, FALSE, FALSE);
    });
    SleepMs(1);
    KeReleaseMutant(&m, 0, FALSE, FALSE);
    SleepMs(1);
    EXPECT_EQ(got, 0) << "still owned once";
    KeReleaseMutant(&m, 0, FALSE, FALSE);
    Join(t);
    EXPECT_EQ(got, 1);
    EXPECT_EQ(m.Header.SignalState, 1);
}

TEST(Mutant, ReleaseByANonOwnerIsABugCheck) {
    KMUTANT m;
    KeInitializeMutant(&m, FALSE);
    PKTHREAD t = Spawn([&] { KeWaitForSingleObject(&m, Executive, KernelMode, FALSE, nullptr); });
    Join(t);
    EXPECT_BUGCHECK(0x1E, KeReleaseMutant(&m, 0, FALSE, FALSE));
}

TEST(Mutant, AbandonedMutantReportsItToTheNextOwner) {
    KMUTANT m;
    KeInitializeMutant(&m, TRUE);
    KeReleaseMutant(&m, 0, TRUE, FALSE); // abandoned
    EXPECT_EQ(KeWaitForSingleObject(&m, Executive, KernelMode, FALSE, nullptr), STATUS_ABANDONED);
    KeReleaseMutant(&m, 0, FALSE, FALSE);
}

TEST(Thread, WaitingOnAThreadWaitsForItsEnd) {
    std::atomic<int> state{0};
    PKTHREAD t = Spawn([&] {
        SleepMs(30);
        state = 1;
    });
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(KeWaitForSingleObject(t, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    Join(t);
    EXPECT_EQ(state, 1);
}

TEST(Thread, DelayOfZeroYieldsToAnEqualPriorityThread) {
    std::atomic<int> ran{0};
    PKTHREAD t = Spawn([&] { ran = 1; }, 8);
    EXPECT_EQ(ran, 0);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    KeDelayExecutionThread(KernelMode, FALSE, &zero);
    EXPECT_EQ(ran, 1);
    Join(t);
}

// --- APCs, alerts, suspend/resume

namespace {
std::vector<std::string> ApcLog;
void NTAPI ApcKernel(PKAPC, PKNORMAL_ROUTINE *, PVOID *ctx, PVOID *, PVOID *) {
    ApcLog.push_back("kernel:" + std::string(*ctx ? (const char *)*ctx : "special") + "@" +
                     std::to_string(KeGetCurrentIrql()));
}
void NTAPI ApcNormal(PVOID ctx, PVOID, PVOID) {
    ApcLog.push_back("normal:" + std::string((const char *)ctx) + "@" + std::to_string(KeGetCurrentIrql()));
}
} // namespace

TEST(Apc, KernelApcToTheCurrentThreadRunsWhenIrqlDrops) {
    ApcLog.clear();
    KAPC apc;
    KeInitializeApc(&apc, KeGetCurrentThread(), ApcKernel, nullptr, ApcNormal, KernelMode, (PVOID) "x");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    EXPECT_TRUE(KeInsertQueueApc(&apc, nullptr, nullptr, 0));
    EXPECT_FALSE(KeInsertQueueApc(&apc, nullptr, nullptr, 0)) << "already queued";
    EXPECT_TRUE(ApcLog.empty());
    KfLowerIrql(old);
    EXPECT_EQ(ApcLog, (std::vector<std::string>{"kernel:x@1", "normal:x@0"}));
}

TEST(Apc, CriticalRegionHoldsNormalApcsButNotSpecialOnes) {
    ApcLog.clear();
    KAPC normal, special;
    KeInitializeApc(&normal, KeGetCurrentThread(), ApcKernel, nullptr, ApcNormal, KernelMode, (PVOID) "n");
    KeInitializeApc(&special, KeGetCurrentThread(), ApcKernel, nullptr, nullptr, KernelMode, (PVOID) "s");
    KeEnterCriticalRegion();
    KeInsertQueueApc(&normal, nullptr, nullptr, 0);
    KeInsertQueueApc(&special, nullptr, nullptr, 0);
    // The special APC (no normal routine) ran; the normal one waits for the region to end. (A special APC's
    // context argument is ignored: nullptr.)
    ASSERT_EQ(ApcLog.size(), 1u);
    KeLeaveCriticalRegion();
    ASSERT_EQ(ApcLog.size(), 3u);
    EXPECT_EQ(ApcLog[1], "kernel:n@1");
    EXPECT_EQ(ApcLog[2], "normal:n@0");
}

TEST(Apc, KernelApcInterruptsAWaitWhichThenContinues) {
    ApcLog.clear();
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    std::atomic<NTSTATUS> result{-1};
    PKTHREAD t = Spawn([&] { result = KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, nullptr); });
    SleepMs(1);
    KAPC apc;
    KeInitializeApc(&apc, t, ApcKernel, nullptr, ApcNormal, KernelMode, (PVOID) "w");
    KeInsertQueueApc(&apc, nullptr, nullptr, 0);
    SleepMs(1);
    EXPECT_EQ(ApcLog.size(), 2u) << "delivered to the waiting thread";
    EXPECT_EQ(result, -1) << "the wait resumed";
    KeSetEvent(&e, 0, FALSE);
    Join(t);
    EXPECT_EQ(result, STATUS_SUCCESS);
}

TEST(Apc, AlertableUserWaitRunsUserApcs) {
    ApcLog.clear();
    std::atomic<NTSTATUS> result{-1};
    PKTHREAD t = Spawn([&] {
        LARGE_INTEGER forever = RelativeMs(100000);
        result = KeDelayExecutionThread(UserMode, TRUE, &forever);
    });
    SleepMs(1);
    KAPC apc;
    KeInitializeApc(&apc, t, ApcKernel, nullptr, ApcNormal, UserMode, (PVOID) "u");
    KeInsertQueueApc(&apc, nullptr, nullptr, 0);
    Join(t);
    EXPECT_EQ(result, STATUS_USER_APC);
    EXPECT_EQ(ApcLog, (std::vector<std::string>{"kernel:u@0", "normal:u@0"}));
}

TEST(Apc, AlertEndsAnAlertableWait) {
    std::atomic<NTSTATUS> result{-1};
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    PKTHREAD t = Spawn([&] { result = KeWaitForSingleObject(&e, Executive, KernelMode, TRUE, nullptr); });
    SleepMs(1);
    EXPECT_FALSE(KeAlertThread(t, KernelMode));
    Join(t);
    EXPECT_EQ(result, STATUS_ALERTED);
    // An alert to a thread that is not waiting is kept for its next alertable wait.
    EXPECT_FALSE(KeAlertThread(KeGetCurrentThread(), KernelMode));
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, KernelMode, TRUE, nullptr), STATUS_ALERTED);
}

TEST(Suspend, SuspendedThreadDoesNotRunUntilResumed) {
    std::atomic<int> count{0};
    std::atomic<bool> stop{false};
    PKTHREAD t = Spawn([&] {
        while (!stop) {
            count++;
            SleepMs(1);
        }
    });
    SleepMs(5);
    EXPECT_EQ(KeSuspendThread(t), 0u);
    EXPECT_EQ(KeSuspendThread(t), 1u);
    SleepMs(2);
    int at = count;
    SleepMs(10);
    EXPECT_EQ(count, at) << "suspended";
    EXPECT_EQ(KeResumeThread(t), 2u);
    SleepMs(5);
    EXPECT_EQ(count, at) << "one suspension left";
    EXPECT_EQ(KeResumeThread(t), 1u);
    SleepMs(5);
    EXPECT_GT(count, at);
    stop = true;
    Join(t);
}

// --- Queues

TEST(Queue, EntriesInOrderAndWaitersWoken) {
    KQUEUE q;
    KeInitializeQueue(&q, 0);
    LIST_ENTRY e1, e2, e3;
    EXPECT_EQ(KeInsertQueue(&q, &e1), 0);
    EXPECT_EQ(KeInsertQueue(&q, &e2), 1);
    EXPECT_EQ(KeInsertHeadQueue(&q, &e3), 2);
    EXPECT_EQ(KeRemoveQueue(&q, KernelMode, nullptr), &e3);
    EXPECT_EQ(KeRemoveQueue(&q, KernelMode, nullptr), &e1);
    EXPECT_EQ(KeRemoveQueue(&q, KernelMode, nullptr), &e2);
    LARGE_INTEGER t = RelativeMs(3);
    EXPECT_EQ(KeRemoveQueue(&q, KernelMode, &t), (PLIST_ENTRY)STATUS_TIMEOUT);
    std::atomic<PLIST_ENTRY> got{nullptr};
    PKTHREAD w = Spawn([&] { got = KeRemoveQueue(&q, KernelMode, nullptr); });
    SleepMs(1);
    KeInsertQueue(&q, &e1);
    Join(w);
    EXPECT_EQ(got, &e1);
    EXPECT_EQ(KeRundownQueue(&q), nullptr);
}

TEST(DeviceQueue, BusyQueueKeepsEntriesInKeyOrder) {
    KDEVICE_QUEUE dq;
    KeInitializeDeviceQueue(&dq);
    KDEVICE_QUEUE_ENTRY a, b, c;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    EXPECT_FALSE(KeInsertDeviceQueue(&dq, &a)) << "idle queue: start at once";
    EXPECT_TRUE(KeInsertByKeyDeviceQueue(&dq, &b, 20));
    EXPECT_TRUE(KeInsertByKeyDeviceQueue(&dq, &c, 10));
    EXPECT_EQ(KeRemoveDeviceQueue(&dq), &c);
    EXPECT_TRUE(KeRemoveEntryDeviceQueue(&dq, &b));
    EXPECT_EQ(KeRemoveDeviceQueue(&dq), nullptr) << "empty: the queue goes idle";
    EXPECT_FALSE(KeInsertDeviceQueue(&dq, &a));
    KfLowerIrql(old);
}

// --- Interrupt objects

namespace {
std::atomic<int> IsrRuns;
KIRQL IsrIrql;
BOOLEAN NTAPI CountingIsr(PKINTERRUPT, PVOID) {
    IsrRuns++;
    IsrIrql = KeGetCurrentIrql();
    return TRUE;
}
BOOLEAN NTAPI SyncRoutine(PVOID ctx) {
    *(KIRQL *)ctx = KeGetCurrentIrql();
    return TRUE;
}
} // namespace

TEST(Interrupt, ConnectedServiceRoutineRunsAtItsLevel) {
    KINTERRUPT intr;
    KIRQL irql;
    ULONG vector = HalGetInterruptVector(6, &irql);
    EXPECT_EQ(vector, 0x36u);
    KeInitializeInterrupt(&intr, CountingIsr, nullptr, vector, irql, LevelSensitive, FALSE);
    ASSERT_TRUE(KeConnectInterrupt(&intr));
    EXPECT_FALSE(KeConnectInterrupt(&intr)) << "already connected";
    IsrRuns = 0;
    host::RaiseIrq(6);
    host::AdvanceTime(1);
    EXPECT_EQ(IsrRuns, 1);
    EXPECT_EQ(IsrIrql, irql);
    // At or above its level it waits.
    KIRQL old = KfRaiseIrql(irql);
    host::RaiseIrq(6);
    host::AdvanceTime(1);
    EXPECT_EQ(IsrRuns, 1);
    KfLowerIrql(old);
    EXPECT_EQ(IsrRuns, 2);
    KIRQL seen = 0;
    EXPECT_TRUE(KeSynchronizeExecution(&intr, SyncRoutine, &seen));
    EXPECT_EQ(seen, irql);
    EXPECT_TRUE(KeDisconnectInterrupt(&intr));
    host::RaiseIrq(6);
    host::AdvanceTime(1);
    EXPECT_EQ(IsrRuns, 2) << "disconnected: masked";
    EXPECT_FALSE(KeDisconnectInterrupt(&intr));
}
