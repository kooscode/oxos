// Behaviour on the less common paths: standby displacement, quantum decay and DisableQuantum, Wait=TRUE hand-off,
// waits with held-back APCs, queue concurrency and thread association, and title VM corner cases.
#include "kernel_test.h"

#include <atomic>
#include <vector>

#define Prcb (KiPcr.PrcbData)

extern KPROCESS KiSystemProcess;
extern volatile ULONG MiCommitFailures;

// --- Scheduler

TEST(Sched, AHigherPriorityThreadDisplacesTheStandbyThread) {
    KEVENT e10, e12;
    KeInitializeEvent(&e10, SynchronizationEvent, FALSE);
    KeInitializeEvent(&e12, SynchronizationEvent, FALSE);
    std::vector<int> order;
    PKTHREAD t10 = Spawn([&] {
        KeWaitForSingleObject(&e10, Executive, KernelMode, FALSE, nullptr);
        order.push_back(10);
    }, 10);
    PKTHREAD t12 = Spawn([&] {
        KeWaitForSingleObject(&e12, Executive, KernelMode, FALSE, nullptr);
        order.push_back(12);
    }, 12);
    SleepMs(1);
    KIRQL old = KeRaiseIrqlToDpcLevel(); // no switch until we lower
    KeSetEvent(&e10, 0, FALSE);
    EXPECT_EQ(Prcb.NextThread, t10) << "standby";
    KeSetEvent(&e12, 0, FALSE);
    EXPECT_EQ(Prcb.NextThread, t12) << "displaced by the higher priority";
    KfLowerIrql(old);
    Join(t12);
    Join(t10);
    EXPECT_EQ(order, (std::vector<int>{12, 10}));
}

TEST(Sched, ABoostedBusyThreadDecaysAtItsQuantumEnd) {
    std::atomic<bool> stop{false};
    std::atomic<int> highest{0};
    PKTHREAD t = Spawn([&] {
        while (!stop) {
            int p = KeGetCurrentThread()->Priority;
            if (p > highest)
                highest = p;
            host::AdvanceTime(1); // busy: its quantum runs out
        }
    }, 6);
    KeBoostPriorityThread(t, 4); // 10 > 8: it preempts us at once, burns its quantum and decays below us
    int after = t->Priority;
    stop = true;
    Join(t);
    EXPECT_EQ(highest, 10) << "it ran boosted";
    EXPECT_LT(after, 8) << "decayed below us, so we got the CPU back";
    EXPECT_GE(after, 6) << "never below its base";
}

TEST(Sched, DisableQuantumKeepsRealTimeThreadsRunning) {
    KiSystemProcess.DisableQuantum = TRUE;
    std::atomic<bool> stop{false};
    std::atomic<LONG> quantum{0};
    PKTHREAD t = Spawn([&] {
        host::AdvanceTime(100);
        quantum = KeGetCurrentThread()->Quantum;
        stop = true;
    }, 16);
    Join(t);
    KiSystemProcess.DisableQuantum = FALSE;
    EXPECT_GT(quantum, 0x10000000) << "refilled to 0x7FFFFFFF at its first quantum end";
}

// --- Waits

TEST(WaitPaths, SetWithWaitTrueHandsTheDispatcherToTheNextWait) {
    KEVENT a, b;
    KeInitializeEvent(&a, NotificationEvent, FALSE);
    KeInitializeEvent(&b, NotificationEvent, FALSE);
    std::atomic<int> seen{0};
    PKTHREAD t = Spawn([&] {
        KeWaitForSingleObject(&a, Executive, KernelMode, FALSE, nullptr);
        seen = 1;
        KeSetEvent(&b, 0, FALSE);
    }, 6); // lower than us: it only runs when we block
    SleepMs(1);
    KeSetEvent(&a, 0, TRUE); // returns at DISPATCH_LEVEL...
    EXPECT_EQ(KeGetCurrentIrql(), DISPATCH_LEVEL);
    EXPECT_EQ(seen, 0);
    KeWaitForSingleObject(&b, Executive, KernelMode, FALSE, nullptr); // ...and this wait takes it over
    EXPECT_EQ(KeGetCurrentIrql(), PASSIVE_LEVEL);
    EXPECT_EQ(seen, 1);
    Join(t);
    KSEMAPHORE s;
    KeInitializeSemaphore(&s, 0, 1);
    KeReleaseSemaphore(&s, 0, 1, TRUE);
    EXPECT_EQ(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, nullptr), STATUS_SUCCESS);
    EXPECT_EQ(KeGetCurrentIrql(), PASSIVE_LEVEL);
}

namespace {
std::atomic<int> HeldApcRan;
void NTAPI HeldKernel(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {}
void NTAPI HeldNormal(PVOID, PVOID, PVOID) { HeldApcRan++; }
} // namespace

TEST(WaitPaths, ANormalApcHeldByACriticalRegionWaitsForItsEnd) {
    HeldApcRan = 0;
    KAPC apc;
    KeInitializeApc(&apc, KeGetCurrentThread(), HeldKernel, nullptr, HeldNormal, KernelMode, nullptr);
    KeEnterCriticalRegion();
    KeInsertQueueApc(&apc, nullptr, nullptr, 0);
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    LARGE_INTEGER t = RelativeMs(5);
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, &t), STATUS_TIMEOUT);
    EXPECT_EQ(HeldApcRan, 0);
    KeLeaveCriticalRegion();
    EXPECT_EQ(HeldApcRan, 1);
}

TEST(WaitPaths, UserApcPendingEndsAUserWaitAtOnce) {
    PKTHREAD self = KeGetCurrentThread();
    self->ApcState.UserApcPending = TRUE;
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, UserMode, FALSE, nullptr), STATUS_USER_APC);
    self->ApcState.UserApcPending = FALSE;
    // A kernel-mode alert seen by a user-mode alertable wait.
    KeAlertThread(self, KernelMode);
    EXPECT_EQ(KeWaitForSingleObject(&e, Executive, UserMode, TRUE, nullptr), STATUS_ALERTED);
}

TEST(WaitPaths, MultipleWaitAlertAndAbsoluteTimeout) {
    KEVENT a, b;
    KeInitializeEvent(&a, NotificationEvent, FALSE);
    KeInitializeEvent(&b, NotificationEvent, FALSE);
    PVOID objs[2] = {&a, &b};
    KeAlertThread(KeGetCurrentThread(), KernelMode);
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, TRUE, nullptr, nullptr),
              STATUS_ALERTED);
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    LARGE_INTEGER at;
    at.QuadPart = now.QuadPart + 3 * 10000; // absolute: 3 ms from now
    EXPECT_EQ(KeWaitForSingleObject(&a, Executive, KernelMode, FALSE, &at), STATUS_TIMEOUT);
    at.QuadPart = now.QuadPart - 1; // already past
    EXPECT_EQ(KeWaitForMultipleObjects(2, objs, WaitAll, Executive, KernelMode, FALSE, &at, nullptr), STATUS_TIMEOUT);
}

// --- Queues

TEST(QueuePaths, ConcurrencyLimitAndThreadAssociation) {
    KQUEUE q1, q2;
    KeInitializeQueue(&q1, 1); // one active thread at a time
    KeInitializeQueue(&q2, 0);
    LIST_ENTRY items[4];
    std::atomic<int> got{0};
    KEVENT block;
    KeInitializeEvent(&block, NotificationEvent, FALSE);
    PKTHREAD a = Spawn([&] {
        KeRemoveQueue(&q1, KernelMode, nullptr);
        got++;
        // Blocking on something else frees the concurrency slot for another queue thread.
        KeWaitForSingleObject(&block, Executive, KernelMode, FALSE, nullptr);
        KeRemoveQueue(&q2, KernelMode, nullptr); // moves to another queue
        got++;
    });
    PKTHREAD b = Spawn([&] {
        KeRemoveQueue(&q1, KernelMode, nullptr);
        got++;
    });
    SleepMs(1);
    KeInsertQueue(&q1, &items[0]);
    KeInsertQueue(&q1, &items[1]);
    SleepMs(2);
    EXPECT_EQ(got, 2) << "a blocked on the event, so b could take the second item";
    KeInsertQueue(&q2, &items[2]);
    KeSetEvent(&block, 0, FALSE);
    Join(a);
    Join(b);
    EXPECT_EQ(got, 3);
    LARGE_INTEGER t = RelativeMs(2);
    EXPECT_EQ(KeRemoveQueue(&q1, KernelMode, &t), (PLIST_ENTRY)STATUS_TIMEOUT);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(KeRemoveQueue(&q1, KernelMode, &zero), (PLIST_ENTRY)STATUS_TIMEOUT);
    // We are associated with q1 now: run it down before it goes out of scope (a queue must outlive its threads).
    KeRundownQueue(&q1);
    KeRundownQueue(&q2);
    EXPECT_EQ(KeGetCurrentThread()->Queue, nullptr);
}

// --- Title VM corner cases

namespace {
NTSTATUS Alloc(ULONG *base, ULONG size, ULONG type, ULONG protect) {
    PVOID b = (PVOID)*base;
    SIZE_T s = size;
    NTSTATUS st = NtAllocateVirtualMemory(&b, 0, &s, type, protect);
    *base = (ULONG)b;
    return st;
}
NTSTATUS Release(ULONG base, ULONG size) {
    PVOID b = (PVOID)base;
    SIZE_T s = size;
    return NtFreeVirtualMemory(&b, &s, MEM_RELEASE);
}
ULONG Protect(ULONG va) {
    MEMORY_BASIC_INFORMATION m;
    NtQueryVirtualMemory((PVOID)va, &m);
    return m.Protect;
}
} // namespace

TEST(VmPaths, ProtectionKindsAndRecommit) {
    ULONG base = 0x00900000;
    ASSERT_EQ(Alloc(&base, 0x10000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    ULONG p = 0x00900000;
    ASSERT_EQ(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_NOACCESS), STATUS_SUCCESS);
    EXPECT_EQ(Protect(0x00900000), PAGE_NOACCESS);
    p = 0x00901000;
    ASSERT_EQ(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE), STATUS_SUCCESS);
    EXPECT_EQ(Protect(0x00901000), PAGE_READWRITE | PAGE_NOCACHE);
    p = 0x00902000;
    ASSERT_EQ(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READ | PAGE_WRITECOMBINE), STATUS_SUCCESS);
    EXPECT_EQ(Protect(0x00902000), PAGE_READONLY | PAGE_WRITECOMBINE);
    p = 0x00903000;
    EXPECT_NE(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE | PAGE_WRITECOMBINE), STATUS_SUCCESS);
    EXPECT_NE(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_NOACCESS | PAGE_NOCACHE), STATUS_SUCCESS);
    // Recommit a committed page with a new protection: same frame, new attributes, contents kept.
    p = 0x00901000;
    ((volatile ULONG *)p)[0] = 0xABCD;
    ASSERT_EQ(Alloc(&p, 0x1000, MEM_COMMIT, PAGE_READONLY), STATUS_SUCCESS);
    EXPECT_EQ(Protect(0x00901000), PAGE_READONLY);
    EXPECT_EQ(((volatile ULONG *)0x00901000)[0], 0xABCDu);
    // Protect on an uncommitted page / outside any VAD.
    PVOID b = (PVOID)0x00904000;
    SIZE_T s = 0x1000;
    ULONG old;
    EXPECT_EQ(NtProtectVirtualMemory(&b, &s, PAGE_READONLY, &old), (NTSTATUS)0xC000002D) << "not committed";
    b = (PVOID)0x00A00000;
    EXPECT_NE(NtProtectVirtualMemory(&b, &s, PAGE_READONLY, &old), STATUS_SUCCESS);
    EXPECT_NE(NtProtectVirtualMemory(&b, &s, 0x777, &old), STATUS_SUCCESS);
    EXPECT_EQ(Release(0x00900000, 0), STATUS_SUCCESS);
}

TEST(VmPaths, ReleasingTheStartOrEndOfAReservation) {
    ULONG base = 0x00B00000;
    ASSERT_EQ(Alloc(&base, 0x30000, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_EQ(Release(0x00B00000, 0x10000), STATUS_SUCCESS); // the start
    EXPECT_EQ(Release(0x00B20000, 0x10000), STATUS_SUCCESS); // the end
    MEMORY_BASIC_INFORMATION m;
    NtQueryVirtualMemory((PVOID)0x00B10000, &m);
    EXPECT_EQ(m.State, MEM_RESERVE);
    EXPECT_EQ(m.AllocationBase, (PVOID)0x00B10000) << "the trimmed VAD starts after the released part";
    EXPECT_EQ(m.RegionSize, 0x10000u);
    EXPECT_EQ(Release(0x00B10000, 0x20000), (NTSTATUS)0xC000001A) << "STATUS_UNABLE_TO_FREE_VM: runs past the VAD";
    EXPECT_EQ(Release(0x00B10000, 0x10000), STATUS_SUCCESS);
    PVOID bad = (PVOID)0x7FFF0000;
    SIZE_T s = 0x1000;
    EXPECT_NE(NtFreeVirtualMemory(&bad, &s, MEM_RELEASE), STATUS_SUCCESS);
    EXPECT_NE(NtFreeVirtualMemory(&bad, &s, 0x1234), STATUS_SUCCESS);
    MEMORY_BASIC_INFORMATION q;
    EXPECT_NE(NtQueryVirtualMemory((PVOID)0x80000000, &q), STATUS_SUCCESS);
}

TEST(VmPaths, CommitBeyondTheFreePagesFails) {
    ULONG base = 0x01000000;
    ULONG size = (MmAvailablePages + 16) << 12;
    ASSERT_EQ(Alloc(&base, size, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    ULONG failures = MiCommitFailures;
    ULONG p = base;
    EXPECT_EQ(Alloc(&p, size, MEM_COMMIT, PAGE_READWRITE), (NTSTATUS)0xC0000017) << "STATUS_NO_MEMORY";
    EXPECT_EQ(MiCommitFailures, failures + 1);
    EXPECT_EQ(Release(base, 0), STATUS_SUCCESS);
    EXPECT_EQ(CheckPfnDatabase(), "");
}

TEST(VmPaths, TopDownSkipsAnOccupiedTop) {
    ULONG first = 0, second = 0;
    ASSERT_EQ(Alloc(&first, 0x1000, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE), STATUS_SUCCESS);
    ASSERT_EQ(Alloc(&second, 0x1000, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_EQ(second, first - 0x10000) << "the next 64 KB down";
    Release(first, 0);
    Release(second, 0);
}

TEST(QueuePaths, ATerminatedThreadLeavesItsQueueAndFreesItsSlot) {
    // Regression (2026-10-23): KeTerminateThread did not take the thread off its queue (spec/stock/07). Its slot
    // stayed taken (a one-at-a-time queue never served anyone again) and the freed thread stayed on the queue's list.
    KQUEUE q;
    KeInitializeQueue(&q, 1);
    LIST_ENTRY first, second;
    KeInsertQueue(&q, &first);
    PKTHREAD a = Spawn([&] { KeRemoveQueue(&q, KernelMode, nullptr); });
    Join(a);
    EXPECT_TRUE(IsListEmpty(&q.ThreadListHead)) << "the dead thread is still on the queue";
    EXPECT_EQ(q.CurrentCount, 0) << "its concurrency slot was not released";
    KeInsertQueue(&q, &second);
    std::atomic<PLIST_ENTRY> got{nullptr};
    PKTHREAD b = Spawn([&] {
        LARGE_INTEGER t = RelativeMs(50);
        got = KeRemoveQueue(&q, KernelMode, &t);
    });
    Join(b);
    EXPECT_EQ(got, &second);
}

TEST(VmPaths, APageTableIsReleasedWithItsLastPage) {
    // spec/stock/09 §6: "a page table is released when its last PTE goes".
    ULONG before = MmAvailablePages;
    ULONG base = 0x02400000; // a 4 MB region no other test uses
    ASSERT_EQ(Alloc(&base, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
    EXPECT_EQ(MmAvailablePages, before - 16 - 1) << "16 pages and their page table";
    EXPECT_EQ(Release(base, 0), STATUS_SUCCESS);
    EXPECT_EQ(MmAvailablePages, before) << "the page table came back too";
    EXPECT_FALSE(*MiGetPdeAddress(base) & PTE_PRESENT);
}
