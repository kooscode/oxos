// More of the kernel core: alerts, boosts and priorities, time keeping, FPU state, process-level thread APIs, the
// RTC, and the rarer memory-manager entry points.
#include "kernel_test.h"

#include <atomic>
#include <vector>

#define Prcb (KiPcr.PrcbData)

extern "C" void KiThreadReturned();
void KiPerfCounterOverflow();
void KiNpxNotAvailableForTest();
extern "C" void KiNpxNotAvailable();
struct HAL_TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
};
void HalpQueryRealTimeClock(HAL_TIME_FIELDS *t);
void HalpSetRealTimeClock(const HAL_TIME_FIELDS *t);
bool HalpCmosSignatureValid();
void HalpWriteCmosSignature();
UCHAR HalpCmosRead(UCHAR reg);
void HalpCmosWrite(UCHAR reg, UCHAR v);
void HalpInitRtc();
XBAPI BOOLEAN NTAPI RtlTimeFieldsToTime(HAL_TIME_FIELDS *TimeFields, PLARGE_INTEGER Time);
XBAPI NTSTATUS NTAPI NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime);
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter();
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceFrequency();
XBAPI void NTAPI KeSetEventBoostPriority(PKEVENT Event, PKTHREAD *Thread);
struct KFLOATING_SAVE_ {
    UCHAR Flags, Irql, Reserved[6];
    PVOID Buffer;
    ULONG Reserved2[2];
    PKTHREAD Thread;
};
XBAPI NTSTATUS NTAPI KeSaveFloatingPointState(KFLOATING_SAVE_ *FloatSave);
XBAPI NTSTATUS NTAPI KeRestoreFloatingPointState(KFLOATING_SAVE_ *FloatSave);
struct PS_STATISTICS {
    ULONG Length, ThreadCount, HandleCount;
};
XBAPI NTSTATUS NTAPI PsQueryStatistics(PS_STATISTICS *ProcessStatistics);
NTSTATUS MiCommitImageMemory(PVOID *BaseAddress, PSIZE_T RegionSize);
extern volatile ULONG MiPteListDamaged;
extern KPROCESS KiSystemProcess;

// --- Alerts and boosts

TEST(Alert, TestAlertConsumesAPendingAlert) {
    EXPECT_FALSE(KeTestAlertThread(KernelMode));
    KeAlertThread(KeGetCurrentThread(), KernelMode);
    EXPECT_TRUE(KeTestAlertThread(KernelMode));
    EXPECT_FALSE(KeTestAlertThread(KernelMode));
}

TEST(Alert, AlertResumeWakesASuspendedAlertableThread) {
    KEVENT e;
    KeInitializeEvent(&e, NotificationEvent, FALSE);
    std::atomic<NTSTATUS> result{-1};
    PKTHREAD t = Spawn([&] { result = KeWaitForSingleObject(&e, Executive, KernelMode, TRUE, nullptr); });
    SleepMs(1);
    KeSuspendThread(t);
    EXPECT_EQ(KeAlertResumeThread(t), 1u);
    Join(t);
    EXPECT_EQ(result, STATUS_ALERTED);
}

TEST(Boost, SetEventBoostPriorityWakesTheFirstWaiter) {
    KEVENT e;
    KeInitializeEvent(&e, SynchronizationEvent, FALSE);
    PKTHREAD woken = nullptr;
    KeSetEventBoostPriority(&e, &woken);
    EXPECT_EQ(e.Header.SignalState, 1) << "no waiter: the event is set";
    KeResetEvent(&e);
    PKTHREAD t = Spawn([&] { KeWaitForSingleObject(&e, Executive, KernelMode, FALSE, nullptr); });
    SleepMs(1);
    KeSetEventBoostPriority(&e, &woken);
    EXPECT_EQ(woken, t);
    EXPECT_EQ(e.Header.SignalState, 0);
    Join(t);
}

TEST(Priority, BasePriorityAndSaturation) {
    std::atomic<bool> stop{false};
    PKTHREAD t = Spawn([&] {
        while (!stop)
            SleepMs(1);
    });
    EXPECT_EQ(KeQueryBasePriorityThread(t), 0) << "system process base 8, thread base 8";
    EXPECT_EQ(KeSetBasePriorityThread(t, 2), 0);
    EXPECT_EQ(t->BasePriority, 10);
    EXPECT_EQ(KeQueryBasePriorityThread(t), 2);
    EXPECT_EQ(KeSetBasePriorityThread(t, 16), 2) << "saturation: +16 means the top of the class";
    EXPECT_EQ(t->BasePriority, 15);
    EXPECT_EQ(KeSetBasePriorityThread(t, -16), 8) << "previous reported as half the class";
    EXPECT_EQ(t->BasePriority, 1);
    KeSetBasePriorityThread(t, 0);
    EXPECT_EQ(t->BasePriority, 8);
    EXPECT_FALSE(KeSetDisableBoostThread(t, TRUE));
    EXPECT_TRUE(KeSetDisableBoostThread(t, FALSE));
    stop = true;
    Join(t);
}

TEST(Priority, BoostRaisesAVariableThreadOnce) {
    std::atomic<bool> stop{false};
    PKTHREAD t = Spawn([&] {
        while (!stop)
            SleepMs(1);
    }, 6);
    SleepMs(2);
    KeBoostPriorityThread(t, 4);
    EXPECT_EQ(t->Priority, 10);
    EXPECT_EQ(t->PriorityDecrement, 4);
    KeBoostPriorityThread(t, 6); // a decrement is outstanding: no second boost
    EXPECT_EQ(t->Priority, 10);
    stop = true;
    Join(t);
}

TEST(Priority, ProcessBasePriorityShiftsItsThreads) {
    std::atomic<bool> stop{false};
    PKTHREAD t = Spawn([&] {
        while (!stop)
            SleepMs(1);
    });
    SleepMs(1);
    KPRIORITY old = KeSetPriorityProcess(&KiSystemProcess, 10);
    EXPECT_EQ(old, 8);
    EXPECT_EQ(t->BasePriority, 10);
    EXPECT_EQ(KeGetCurrentThread()->BasePriority, 10);
    KeSetPriorityProcess(&KiSystemProcess, old);
    EXPECT_EQ(t->BasePriority, 8);
    stop = true;
    Join(t);
}

// --- Time

TEST(Time, InterruptAndSystemTimeAdvanceWithTheClock) {
    ULONGLONG i0 = KeQueryInterruptTime();
    LARGE_INTEGER s0, s1;
    KeQuerySystemTime(&s0);
    host::AdvanceTime(25);
    EXPECT_EQ(KeQueryInterruptTime() - i0, 25u * 10000);
    KeQuerySystemTime(&s1);
    EXPECT_EQ(s1.QuadPart - s0.QuadPart, 25LL * 10000);
}

TEST(Time, PerformanceCounterFollowsThePmTimerAcrossBit31) {
    EXPECT_EQ(KeQueryPerformanceFrequency().QuadPart, 3375000);
    LARGE_INTEGER a = KeQueryPerformanceCounter();
    host::SkipTime(10);
    LARGE_INTEGER b = KeQueryPerformanceCounter();
    EXPECT_NEAR((double)(b.QuadPart - a.QuadPart), 33750.0, 2.0);
    // Past a toggle of the timer's bit 31 (636 s at 3.375 MHz) before and after the SCI reports it.
    host::SkipTime(640000);
    LARGE_INTEGER c = KeQueryPerformanceCounter();
    EXPECT_NEAR((double)(c.QuadPart - b.QuadPart), 640000.0 * 3375, 4.0) << "bit 31 toggled, not yet reported";
    KiPerfCounterOverflow();
    LARGE_INTEGER d = KeQueryPerformanceCounter();
    EXPECT_NEAR((double)(d.QuadPart - c.QuadPart), 0.0, 2.0) << "reported: the same count";
    host::SkipTime(1000);
    EXPECT_GT(KeQueryPerformanceCounter().QuadPart, d.QuadPart);
}

TEST(Time, StallReturns) {
    KeStallExecutionProcessor(0);
    KeStallExecutionProcessor(50);
}

TEST(Time, SetSystemTimeUpdatesTheRtcAndItsSignature) {
    HalpInitRtc();
    HAL_TIME_FIELDS f = {2026, 10, 23, 13, 45, 30, 0, 5};
    LARGE_INTEGER t;
    ASSERT_TRUE(RtlTimeFieldsToTime(&f, &t));
    LARGE_INTEGER previous;
    ASSERT_EQ(NtSetSystemTime(&t, &previous), STATUS_SUCCESS);
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    EXPECT_EQ(now.QuadPart, t.QuadPart);
    EXPECT_TRUE(HalpCmosSignatureValid());
    HAL_TIME_FIELDS r = {};
    HalpQueryRealTimeClock(&r);
    EXPECT_EQ(r.Year, 2026);
    EXPECT_EQ(r.Month, 10);
    EXPECT_EQ(r.Day, 23);
    EXPECT_EQ(r.Hour, 13);
    EXPECT_EQ(r.Minute, 45);
    EXPECT_EQ(r.Second, 30);
    EXPECT_EQ(HalpCmosRead(0x09), 0x26) << "BCD year";
    EXPECT_EQ(HalpCmosRead(0x7F), 0x20) << "BCD century";
    LARGE_INTEGER bad;
    bad.QuadPart = -1;
    EXPECT_NE(NtSetSystemTime(&bad, nullptr), STATUS_SUCCESS);
    HalpCmosWrite(0x10, 0); // break the signature
    EXPECT_FALSE(HalpCmosSignatureValid());
    HalpWriteCmosSignature();
    EXPECT_TRUE(HalpCmosSignatureValid());
}

// --- FPU state

TEST(Fpu, SaveGivesFreshStateAndRestoreBringsTheOldBack) {
    KFLOATING_SAVE_ save = {};
    ASSERT_EQ(KeSaveFloatingPointState(&save), STATUS_SUCCESS);
    EXPECT_NE(save.Buffer, nullptr);
    EXPECT_EQ(save.Thread, KeGetCurrentThread());
    EXPECT_EQ(Prcb.NpxThread, KeGetCurrentThread());
    ASSERT_EQ(KeRestoreFloatingPointState(&save), STATUS_SUCCESS);
    EXPECT_EQ(save.Buffer, nullptr);
    EXPECT_EQ(KeRestoreFloatingPointState(&save), STATUS_SUCCESS) << "nothing to restore";
}

TEST(Fpu, DeviceNotAvailableSwitchesTheOwner) {
    // What the #NM handler does when another thread's FPU state is loaded: save it there, load ours.
    PKTHREAD self = KeGetCurrentThread();
    PKTHREAD other = Spawn([] { SleepMs(5); });
    SleepMs(1);
    Prcb.NpxThread = other;
    other->NpxState = NPX_STATE_LOADED;
    self->NpxState = NPX_STATE_NOT_LOADED;
    KiNpxNotAvailable();
    EXPECT_EQ(Prcb.NpxThread, self);
    EXPECT_EQ(self->NpxState, NPX_STATE_LOADED);
    EXPECT_EQ(other->NpxState, NPX_STATE_NOT_LOADED);
    KiNpxNotAvailable(); // already ours: nothing to do
    EXPECT_EQ(Prcb.NpxThread, self);
    Join(other);
}

// --- Threads through handles

namespace {
std::atomic<int> Notified;
void NTAPI Notify(PVOID, HANDLE, BOOLEAN create) { Notified += create ? 1 : 100; }
std::atomic<int> UserApcs;
void NTAPI UserApc(PVOID a, PVOID b, PVOID c) { UserApcs += (int)(ULONG)a + (int)(ULONG)b + (int)(ULONG)c; }
std::atomic<bool> HandleThreadStop;
void NTAPI HandleThread(PVOID) {
    while (!HandleThreadStop) {
        LARGE_INTEGER t = RelativeMs(1);
        KeDelayExecutionThread(UserMode, TRUE, &t); // alertable: user APCs run here
    }
}
} // namespace

TEST(Ps, CreateSuspendResumeQueueApcAndStatistics) {
    PS_STATISTICS st = {sizeof(st)};
    ASSERT_EQ(PsQueryStatistics(&st), STATUS_SUCCESS);
    ULONG threads = st.ThreadCount;
    ASSERT_EQ(PsSetCreateThreadNotifyRoutine(Notify), STATUS_SUCCESS);
    Notified = 0;
    HandleThreadStop = false;
    HANDLE h = nullptr, id = nullptr;
    ASSERT_EQ(PsCreateSystemThread(&h, &id, (PVOID)HandleThread, nullptr, FALSE), STATUS_SUCCESS);
    EXPECT_NE(h, nullptr);
    EXPECT_NE(id, nullptr);
    EXPECT_EQ(Notified, 1);
    PsQueryStatistics(&st);
    EXPECT_EQ(st.ThreadCount, threads + 1);
    ULONG prev = 99;
    EXPECT_EQ(NtSuspendThread(h, &prev), STATUS_SUCCESS);
    EXPECT_EQ(prev, 0u);
    EXPECT_EQ(NtResumeThread(h, &prev), STATUS_SUCCESS);
    EXPECT_EQ(prev, 1u);
    UserApcs = 0;
    EXPECT_EQ(NtQueueApcThread(h, UserApc, (PVOID)1, (PVOID)2, (PVOID)3), STATUS_SUCCESS);
    SleepMs(5);
    EXPECT_EQ(UserApcs, 6);
    EXPECT_NE(NtSuspendThread((HANDLE)0x7FFC, nullptr), STATUS_SUCCESS) << "bad handle";
    HandleThreadStop = true;
    PVOID t;
    ASSERT_EQ(ObReferenceObjectByHandle(h, nullptr, &t), STATUS_SUCCESS);
    NtClose(h);
    KeWaitForSingleObject(t, Executive, KernelMode, FALSE, nullptr);
    ObfDereferenceObject(t);
    EXPECT_EQ(Notified, 101);
    PsQueryStatistics(&st);
    EXPECT_EQ(st.ThreadCount, threads);
    PS_STATISTICS bad = {1};
    EXPECT_NE(PsQueryStatistics(&bad), STATUS_SUCCESS);
}

TEST(Ps, ASystemRoutineThatReturnsIsABugCheck) { EXPECT_BUGCHECK(0x0E, KiThreadReturned()); }

// --- Memory manager leftovers

TEST(MmMore, PersistMarksAndClearsContiguousPages) {
    auto p = MmAllocateContiguousMemory(2 * 4096);
    ULONG pfn = MmGetPhysicalAddress(p) >> 12;
    MmPersistContiguousMemory(p, 2 * 4096, TRUE);
    EXPECT_TRUE(PfnEntry(pfn) & PTE_PERSIST);
    EXPECT_TRUE(PfnEntry(pfn + 1) & PTE_PERSIST);
    MmPersistContiguousMemory(p, 2 * 4096, FALSE);
    EXPECT_FALSE(PfnEntry(pfn) & PTE_PERSIST);
    MmFreeContiguousMemory(p);
}

TEST(MmMore, GpuInstanceMemorySitsBelowThePageTables) {
    ULONG padding = 0;
    EXPECT_EQ(MmClaimGpuInstanceMemory(0xFFFFFFFF, &padding), (PVOID)(0x84000000 - 0x10000));
    EXPECT_EQ(padding, 0x10000u);
    ULONG available = MmAvailablePages;
    MmClaimGpuInstanceMemory(0x8000, &padding); // shrink from 64 KB to 32 KB: 8 pages come back
    EXPECT_EQ(MmAvailablePages, available + 8);
    EXPECT_EQ(CheckPfnDatabase(), "");
}

TEST(MmMore, ImageMemoryCommitsIntoAReservation) {
    PVOID base = (PVOID)0x00800000;
    SIZE_T size = 0x20000;
    ASSERT_EQ(NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS);
    MM_STATISTICS st0 = {sizeof(st0)}, st1 = {sizeof(st1)};
    MmQueryStatistics(&st0);
    size = 0x3000;
    ASSERT_EQ(MiCommitImageMemory(&base, &size), STATUS_SUCCESS);
    MmQueryStatistics(&st1);
    EXPECT_EQ(st1.ImagePagesCommitted, st0.ImagePagesCommitted + 3);
    ((volatile ULONG *)0x00800000)[0] = 1;
    size = 0;
    EXPECT_EQ(NtFreeVirtualMemory(&base, &size, MEM_RELEASE), STATUS_SUCCESS);
}

TEST(MmMore, DamagedSystemPteListIsReportedNotFollowed) {
    // An isolated 8-PTE free run (the page after it stays allocated), its link corrupted, then a request for more
    // than it holds: the search must stop at the bad link, report it and fail, not follow it.
    auto a = (uint8_t *)MmAllocateSystemMemory(8 * 4096, PAGE_READWRITE);
    auto b = (uint8_t *)MmAllocateSystemMemory(4096, PAGE_READWRITE);
    if (b != a + 8 * 4096) {
        MmFreeSystemMemory(a, 8 * 4096);
        MmFreeSystemMemory(b, 4096);
        GTEST_SKIP() << "no adjacent placement";
    }
    MmFreeSystemMemory(a, 8 * 4096);
    uint32_t *run = (uint32_t *)MiGetPteAddress((ULONG)a);
    if ((run[0] & 2) || run[1] >> 2 != 8) { // merged with a free run before it
        MmFreeSystemMemory(b, 4096);
        GTEST_SKIP() << "the run merged with a neighbour";
    }
    uint32_t saved = run[0];
    ULONG damaged = MiPteListDamaged;
    run[0] = 0x12345670; // a link that points nowhere sane
    PVOID got = MiReserveSystemPtes(20);
    run[0] = saved; // repair before anything else allocates
    EXPECT_EQ(got, nullptr);
    EXPECT_EQ(MiPteListDamaged, damaged + 1);
    PVOID ok = MiReserveSystemPtes(20);
    EXPECT_NE(ok, nullptr);
    MiReleaseSystemPtes(ok, 20);
    MmFreeSystemMemory(b, 4096);
}
