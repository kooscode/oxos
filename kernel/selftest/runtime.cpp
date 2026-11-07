// Boot self-tests for the runtime exports (milestone 5a).
#include <xk/selftest.h>
#include <xk/io.h>
#include <xk/runtime.h>
#include <xk/hardware.h>
#include <xk/objects.h>
#include <xk/core.h>

volatile ULONG KiTestRtlSprintfOk, KiTestRtlInterlockedOk, KiTestRtlCritSecOk, KiTestRtlRwLockOk, KiTestRtlStringsOk,
    KiTestRtlMathOk, KiTestRtlRefurbOk, KiTestRtlTimeOk, KiTestRtlDbgOk, KiTestRtlBoostOk, KiTestRtlFpuOk,
    KiTestRtlDone;
volatile char KiTestRtlSprintfOut[64];

static void Sleep(LONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static bool StrEq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// Critical section contention: two threads add to a counter 1000 times each, with a deliberate yield inside.
static RTL_CRITICAL_SECTION CritSec;
static volatile ULONG CritCounter, CritInside, CritViolation;
static void NTAPI CritWorker(PVOID) {
    for (int i = 0; i < 1000; i++) {
        RtlEnterCriticalSection(&CritSec);
        RtlEnterCriticalSection(&CritSec); // recursion
        if (CritInside)
            CritViolation = 1;
        CritInside = 1;
        ULONG v = CritCounter;
        if (!(i % 100))
            Sleep(1);
        CritCounter = v + 1;
        CritInside = 0;
        RtlLeaveCriticalSection(&CritSec);
        RtlLeaveCriticalSection(&CritSec);
    }
}

// Read/write lock: readers share, writers exclude everybody.
static ERWLOCK RwLock;
static volatile LONG Readers, Writers, RwViolation, RwDone;
static void NTAPI RwReader(PVOID) {
    for (int i = 0; i < 200; i++) {
        ExAcquireReadWriteLockShared(&RwLock);
        InterlockedIncrement(&Readers);
        if (Writers)
            RwViolation = 1;
        if (!(i % 50))
            Sleep(1);
        InterlockedExchangeAdd(&Readers, -1);
        ExReleaseReadWriteLock(&RwLock);
    }
    InterlockedIncrement(&RwDone);
}

static void NTAPI RwWriter(PVOID) {
    for (int i = 0; i < 100; i++) {
        ExAcquireReadWriteLockExclusive(&RwLock);
        if (InterlockedIncrement(&Writers) != 1 || Readers)
            RwViolation = 1;
        if (!(i % 25))
            Sleep(1);
        InterlockedExchangeAdd(&Writers, -1);
        ExReleaseReadWriteLock(&RwLock);
    }
    InterlockedIncrement(&RwDone);
}

static KEVENT BoostEvent;
static volatile ULONG BoostWoken;
static void NTAPI BoostWaiter(PVOID) {
    KeWaitForSingleObject(&BoostEvent, Executive, KernelMode, FALSE, nullptr);
    BoostWoken = 1;
}

static void NTAPI RtlControllerThread(PVOID) {
    while (!KiTestHalDone)
        Sleep(10);

    // sprintf
    char b[64];
    RtlSprintf(b, "%d|%5s|%-4x|%08X|%c|%I64u", -42, "ab", 0x1F, 0xBEEF, 'z', 12345678901234ULL);
    for (int i = 0; i < 63; i++)
        KiTestRtlSprintfOut[i] = b[i];
    bool ok = StrEq(b, "-42|   ab|1f  |0000BEEF|z|12345678901234");
    ok &= RtlSnprintf(b, 4, "%s", "abcdef") == -1 && b[0] == 'a' && b[3] == 'd';
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, (PCWSTR)L"wide");
    RtlSprintf(b, "%S %wZ %.2s %%", L"wz", &us, "xyz");
    ok &= StrEq(b, "wz wide xy %");
    KiTestRtlSprintfOk = ok;

    // Interlocked + S-list
    volatile LONG v = 5;
    ok = InterlockedIncrement(&v) == 6 && InterlockedCompareExchange(&v, 9, 6) == 6 && v == 9 &&
         InterlockedCompareExchange(&v, 1, 6) == 9 && InterlockedExchangeAdd(&v, 3) == 9 && v == 12;
    SLIST_HEADER head = {};
    SINGLE_LIST_ENTRY items[3];
    for (auto &it : items)
        InterlockedPushEntrySList(&head, &it);
    ok &= head.Depth == 3 && InterlockedPopEntrySList(&head) == &items[2] &&
          InterlockedFlushSList(&head) == &items[1] && InterlockedPopEntrySList(&head) == nullptr;
    KiTestRtlInterlockedOk = ok;

    // Critical sections
    RtlInitializeCriticalSection(&CritSec);
    KiCreateSystemThread(CritWorker, nullptr, 0, 9);
    KiCreateSystemThread(CritWorker, nullptr, 0, 9);
    for (int i = 0; i < 300 && CritCounter < 2000; i++)
        Sleep(10);
    ok = CritCounter == 2000 && !CritViolation && RtlTryEnterCriticalSection(&CritSec);
    RtlLeaveCriticalSection(&CritSec);
    KiTestRtlCritSecOk = ok;

    // Read/write lock
    ExInitializeReadWriteLock(&RwLock);
    for (int i = 0; i < 3; i++)
        KiCreateSystemThread(RwReader, nullptr, 0, 9);
    KiCreateSystemThread(RwWriter, nullptr, 0, 9);
    KiCreateSystemThread(RwWriter, nullptr, 0, 9);
    for (int i = 0; i < 300 && RwDone < 5; i++)
        Sleep(10);
    KiTestRtlRwLockOk = RwDone == 5 && !RwViolation && RwLock.LockCount == -1;

    // Strings and conversions
    ULONG n = 0;
    ok = RtlCharToInteger("  -0x1F", 0, &n) == 0 && n == (ULONG)-31;
    ok &= RtlCharToInteger("777", 8, &n) == 0 && n == 511;
    ok &= RtlIntegerToChar(255, 16, sizeof(b), b) == 0 && StrEq(b, "FF");
    UNICODE_STRING num;
    RtlInitUnicodeString(&num, (PCWSTR)L"1234");
    ok &= RtlUnicodeStringToInteger(&num, 10, &n) == 0 && n == 1234;
    WCHAR upbuf[8];
    UNICODE_STRING up = {0, sizeof(upbuf), upbuf};
    ok &= RtlUpcaseUnicodeString(&up, &us, FALSE) == 0 && upbuf[0] == 'W' && upbuf[3] == 'E';
    ULONG got;
    RtlUnicodeToMultiByteN(b, sizeof(b), &got, upbuf, up.Length);
    ok &= got == 4 && b[0] == 'W';
    KiTestRtlStringsOk = ok;

    // Math
    LARGE_INTEGER dividend, magic, q;
    dividend.QuadPart = 1000000000000LL;
    magic.QuadPart = (LONGLONG)0xCCCCCCCCCCCCCCCDULL; // /10 with shift 3
    q = RtlExtendedMagicDivide(dividend, magic, 3);
    ULONG rem;
    LARGE_INTEGER q2 = RtlExtendedLargeIntegerDivide(dividend, 7, &rem);
    ok = q.QuadPart == 100000000000LL && q2.QuadPart == 142857142857LL && rem == 1;
    ok &= RtlCompareMemory("abcdef", "abcxef", 6) == 3 && RtlUlongByteSwap(0x11223344) == 0x44332211;
    KiTestRtlMathOk = ok;

    // Refurb record (sector 3): read it; a disk without a valid one (an invalid signature reads as zeros) gets one
    // written first, as on a console's first boot (the tests only ever use a copy of the disk image).
    ULONG refurb[4] = {};
    bool read = ExReadWriteRefurbInfo(refurb, 16, FALSE) == 0;
    if (read && refurb[0] != 0x52465242) {
        ULONG fresh[4] = {0x52465242, 0, 0, 0};
        read = ExReadWriteRefurbInfo(fresh, 16, TRUE) == 0 && ExReadWriteRefurbInfo(refurb, 16, FALSE) == 0;
    }
    KiTestRtlRefurbOk = read && refurb[0] == 0x52465242 &&
                        ExReadWriteRefurbInfo(refurb, 8, FALSE) == (NTSTATUS)0xC000000D;

    // System time comes from the RTC; setting it updates the RTC.
    LARGE_INTEGER now, old, set, after;
    KeQuerySystemTime(&now);
    set.QuadPart = now.QuadPart + 10LL * 365 * 86400 * 10000000; // ten years on
    ok = now.QuadPart > 0x01C0000000000000LL; // after 2001
    ok &= NtSetSystemTime(&set, &old) == 0;
    KeQuerySystemTime(&after);
    ok &= after.QuadPart - set.QuadPart < 10000000 && after.QuadPart >= set.QuadPart;
    NtSetSystemTime(&now, nullptr);
    KiTestRtlTimeOk = ok;

    // DbgPrint lands in the log
    DbgPrint("xk-log %d %s\n", 77, "marker");
    ok = false;
    for (ULONG i = 0; i + 14 < sizeof(XkDebugLog) && !ok; i++)
        ok = XkDebugLog[i] == 'x' && StrEq(XkDebugLog + i, "xk-log 77 marker\n");
    KiTestRtlDbgOk = ok;

    // KeSetEventBoostPriority hands the event to the waiter
    KeInitializeEvent(&BoostEvent, SynchronizationEvent, FALSE);
    KiCreateSystemThread(BoostWaiter, nullptr, 0, 8);
    Sleep(10);
    PKTHREAD woken = nullptr;
    KeSetEventBoostPriority(&BoostEvent, &woken);
    Sleep(10);
    KiTestRtlBoostOk = BoostWoken && woken && BoostEvent.Header.SignalState == 0;

    // FP state save/restore around FPU use
    alignas(16) KFLOATING_SAVE_ save;
    ok = KeSaveFloatingPointState(&save) == 0;
    volatile ULONGLONG x = 0x3FF8000000000000ULL; // 1.5
    __asm__ volatile("fldl %0; fmul %%st(0), %%st(0); fstpl %0" : "+m"(x));
    ok &= x == 0x4002000000000000ULL && KeRestoreFloatingPointState(&save) == 0; // 2.25
    KiTestRtlFpuOk = ok;

    KiTestRtlDone = 1;
}

void KiStartRtlSelfTests() {
    KiCreateSystemThread(RtlControllerThread, nullptr, 0, 9);
}
