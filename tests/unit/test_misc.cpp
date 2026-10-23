// The remaining entry points: Nt query calls, timer APCs, path links through a parse procedure, link deletion, and
// the rarer run-time helpers.
#include "kernel_test.h"

#include <atomic>
#include <string>

#include <xk/rtl.h>

NTSTATUS ObpCreateSymbolicLink(POBJECT_STRING LinkName, POBJECT_STRING TargetName);
XBAPI NTSTATUS NTAPI NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES ObjectAttributes, TIMER_TYPE TimerType);
typedef void(NTAPI *PTIMER_APC_ROUTINE)(PVOID TimerContext, ULONG TimerLowValue, LONG TimerHighValue);
XBAPI NTSTATUS NTAPI NtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE TimerApcRoutine,
                                  KPROCESSOR_MODE ApcMode, PVOID TimerContext, BOOLEAN WakeTimer, LONG Period,
                                  PBOOLEAN PreviousState);
struct EVENT_BASIC_INFORMATION {
    EVENT_TYPE EventType;
    LONG EventState;
};
struct MUTANT_BASIC_INFORMATION {
    LONG CurrentCount;
    BOOLEAN OwnedByCaller, AbandonedState;
};
struct SEMAPHORE_BASIC_INFORMATION {
    LONG CurrentCount, MaximumCount;
};
struct TIMER_BASIC_INFORMATION {
    LARGE_INTEGER RemainingTime;
    BOOLEAN TimerState;
};
XBAPI NTSTATUS NTAPI NtQueryEvent(HANDLE EventHandle, EVENT_BASIC_INFORMATION *EventInformation);
XBAPI NTSTATUS NTAPI NtQueryMutant(HANDLE MutantHandle, MUTANT_BASIC_INFORMATION *MutantInformation);
XBAPI NTSTATUS NTAPI NtQuerySemaphore(HANDLE SemaphoreHandle, SEMAPHORE_BASIC_INFORMATION *SemaphoreInformation);
XBAPI NTSTATUS NTAPI NtQueryTimer(HANDLE TimerHandle, TIMER_BASIC_INFORMATION *TimerInformation);
XBAPI NTSTATUS NTAPI NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);

XBAPI LARGE_INTEGER NTAPI ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend, LARGE_INTEGER Increment, PVOID Lock);
XBAPI void FASTCALL ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend, ULONG Increment);
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry);
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry);
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedRemoveHeadList(PLIST_ENTRY ListHead);
XBAPI LARGE_INTEGER NTAPI RtlExtendedMagicDivide(LARGE_INTEGER Dividend, LARGE_INTEGER MagicDivisor, CCHAR ShiftCount);
struct GENERIC_MAPPING {
    ULONG GenericRead, GenericWrite, GenericExecute, GenericAll;
};
XBAPI void NTAPI RtlMapGenericMask(PULONG AccessMask, const GENERIC_MAPPING *GenericMapping);
XBAPI void NTAPI READ_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);
XBAPI void NTAPI RtlCopyUnicodeString(PUNICODE_STRING Destination, const UNICODE_STRING *Source);
XBAPI void NTAPI RtlFreeUnicodeString(PUNICODE_STRING UnicodeString);
XBAPI NTSTATUS NTAPI RtlDowncaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString,
                                              BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString, const CHAR *, ULONG BytesInMultiByteString);
XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString, const WCHAR *, ULONG BytesInUnicodeString);
XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
                                                  PULONG BytesInMultiByteString, const WCHAR *UnicodeString,
                                                  ULONG BytesInUnicodeString);
XBAPI NTSTATUS NTAPI RtlUnicodeStringToInteger(const UNICODE_STRING *String, ULONG Base, PULONG Value);
XBAPI NTSTATUS NTAPI RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PUNICODE_STRING String);
XBAPI PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, ULONG SortKey);
void HalpInitRtcLate();
void KiInitializeSystemTime();

TEST(Query, EventMutantSemaphoreTimer) {
    HANDLE e, m, s, t;
    ASSERT_EQ(NtCreateEvent(&e, nullptr, SynchronizationEvent, TRUE), STATUS_SUCCESS);
    EVENT_BASIC_INFORMATION ei;
    ASSERT_EQ(NtQueryEvent(e, &ei), STATUS_SUCCESS);
    EXPECT_EQ(ei.EventType, SynchronizationEvent);
    EXPECT_EQ(ei.EventState, 1);
    ASSERT_EQ(NtCreateMutant(&m, nullptr, TRUE), STATUS_SUCCESS);
    MUTANT_BASIC_INFORMATION mi;
    ASSERT_EQ(NtQueryMutant(m, &mi), STATUS_SUCCESS);
    EXPECT_EQ(mi.CurrentCount, 0);
    EXPECT_TRUE(mi.OwnedByCaller);
    EXPECT_FALSE(mi.AbandonedState);
    LONG prev;
    NtReleaseMutant(m, &prev);
    ASSERT_EQ(NtCreateSemaphore(&s, nullptr, 2, 5), STATUS_SUCCESS);
    SEMAPHORE_BASIC_INFORMATION si;
    ASSERT_EQ(NtQuerySemaphore(s, &si), STATUS_SUCCESS);
    EXPECT_EQ(si.CurrentCount, 2);
    EXPECT_EQ(si.MaximumCount, 5);
    ASSERT_EQ(NtCreateTimer(&t, nullptr, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = RelativeMs(50);
    NtSetTimerEx(t, &due, nullptr, KernelMode, nullptr, FALSE, 0, nullptr);
    TIMER_BASIC_INFORMATION ti;
    ASSERT_EQ(NtQueryTimer(t, &ti), STATUS_SUCCESS);
    EXPECT_FALSE(ti.TimerState);
    EXPECT_GT(ti.RemainingTime.QuadPart, 0);
    EXPECT_LE(ti.RemainingTime.QuadPart, 50LL * 10000);
    EXPECT_NE(NtQueryEvent(s, &ei), STATUS_SUCCESS) << "wrong object type";
    for (HANDLE h : {e, m, s, t})
        NtClose(h);
}

namespace {
std::atomic<int> TimerApcs;
PVOID TimerApcContext;
void NTAPI OnTimerApc(PVOID context, ULONG, LONG) {
    TimerApcs++;
    TimerApcContext = context;
}
} // namespace

TEST(Query, TimerApcRunsInTheSettingThread) {
    HANDLE t;
    ASSERT_EQ(NtCreateTimer(&t, nullptr, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = RelativeMs(5);
    TimerApcs = 0;
    ASSERT_EQ(NtSetTimerEx(t, &due, OnTimerApc, KernelMode, (PVOID)0x1234, FALSE, 0, nullptr), STATUS_SUCCESS);
    SleepMs(10); // the kernel-mode APC runs in this thread when the timer's DPC queues it
    EXPECT_EQ(TimerApcs, 1);
    EXPECT_EQ(TimerApcContext, (PVOID)0x1234);
    EXPECT_NE(NtSetTimerEx(t, &due, nullptr, KernelMode, nullptr, FALSE, -1, nullptr), STATUS_SUCCESS)
        << "negative period";
    NtClose(t);
}

// --- Path links through an object with a parse procedure (as \Device\Harddisk0 parses "PartitionN\...").

namespace {
std::string Parsed;
PVOID ParseResult;
NTSTATUS NTAPI RecordingParse(PVOID, POBJECT_TYPE, ULONG, POBJECT_STRING, POBJECT_STRING Remaining, PVOID,
                              PVOID *Object) {
    Parsed.assign(Remaining->Buffer, Remaining->Length);
    ObfReferenceObject(ParseResult);
    *Object = ParseResult;
    return STATUS_SUCCESS;
}
OBJECT_TYPE ParsingType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr, RecordingParse, nullptr, 'sraP'};
} // namespace

TEST(ObLinks, PathLinksReparseThroughTheParseProcedureAndLinksCanBeDeleted) {
    HANDLE ev;
    ASSERT_EQ(NtCreateEvent(&ev, nullptr, NotificationEvent, FALSE), STATUS_SUCCESS);
    ObReferenceObjectByHandle(ev, nullptr, &ParseResult);
    OBJECT_STRING devName;
    RtlInitAnsiString(&devName, "\\Device\\ParsingThing");
    OBJECT_ATTRIBUTES a = {nullptr, &devName, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT};
    PVOID dev;
    ASSERT_EQ(ObCreateObject(&ParsingType, &a, 16, &dev), STATUS_SUCCESS);
    HANDLE dh;
    ASSERT_EQ(ObInsertObject(dev, &a, 0, &dh), STATUS_SUCCESS);
    NtClose(dh);

    OBJECT_STRING link, target;
    RtlInitAnsiString(&link, "\\??\\P:");
    RtlInitAnsiString(&target, "\\Device\\ParsingThing\\Sub");
    ASSERT_EQ(ObpCreateSymbolicLink(&link, &target), STATUS_SUCCESS);
    OBJECT_STRING path;
    RtlInitAnsiString(&path, "\\??\\P:\\dir\\file.xbe");
    PVOID o = nullptr;
    ASSERT_EQ(ObReferenceObjectByName(&path, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &o), STATUS_SUCCESS);
    EXPECT_EQ(o, ParseResult);
    EXPECT_EQ(Parsed, "\\Sub\\dir\\file.xbe") << "link target + rest, handed to the parse procedure";
    ObfDereferenceObject(o);
    RtlInitAnsiString(&path, "\\??\\P:\\");
    ASSERT_EQ(ObReferenceObjectByName(&path, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &o), STATUS_SUCCESS);
    EXPECT_EQ(Parsed, "\\Sub\\") << "a trailing separator is kept: the root, not the link target itself";
    ObfDereferenceObject(o);

    // Delete the link: open it, make it temporary, close the handle.
    OBJECT_ATTRIBUTES la = {nullptr, &link, OBJ_CASE_INSENSITIVE};
    HANDLE lh;
    ASSERT_EQ(NtOpenSymbolicLinkObject(&lh, &la), STATUS_SUCCESS);
    PVOID lo;
    ObReferenceObjectByHandle(lh, nullptr, &lo);
    ObMakeTemporaryObject(lo);
    ObfDereferenceObject(lo);
    NtClose(lh);
    RtlInitAnsiString(&path, "\\??\\P:\\dir");
    EXPECT_NE(ObReferenceObjectByName(&path, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &o), STATUS_SUCCESS);
    ObMakeTemporaryObject(dev);
    ObfDereferenceObject(ParseResult);
    NtClose(ev);
}

// --- Run-time helpers

TEST(RtlMore, InterlockedListsAndLargeIntegers) {
    LIST_ENTRY head, a, b;
    InitializeListHead(&head);
    EXPECT_EQ(ExfInterlockedInsertHeadList(&head, &a), nullptr) << "was empty";
    EXPECT_EQ(ExfInterlockedInsertTailList(&head, &b), &a) << "returns the old first entry";
    EXPECT_EQ(ExfInterlockedRemoveHeadList(&head), &a);
    EXPECT_EQ(ExfInterlockedRemoveHeadList(&head), &b);
    EXPECT_EQ(ExfInterlockedRemoveHeadList(&head), nullptr);
    LARGE_INTEGER x, inc;
    x.QuadPart = 0xFFFFFFFF;
    inc.QuadPart = 1;
    EXPECT_EQ(ExInterlockedAddLargeInteger(&x, inc, nullptr).QuadPart, 0xFFFFFFFFLL);
    EXPECT_EQ(x.QuadPart, 0x100000000LL);
    ExInterlockedAddLargeStatistic(&x, 0xFFFFFFFF);
    EXPECT_EQ(x.QuadPart, 0x1FFFFFFFFLL);
    // Magic divide by 10 (the classic constant): q = (n * ceil(2^67 / 10)) >> 64 >> 3.
    LARGE_INTEGER n, magic;
    n.QuadPart = 1234567890123LL;
    magic.QuadPart = (LONGLONG)0xCCCCCCCCCCCCCCCDULL;
    EXPECT_EQ(RtlExtendedMagicDivide(n, magic, 3).QuadPart, 123456789012LL);
    n.QuadPart = -1234567890123LL;
    EXPECT_EQ(RtlExtendedMagicDivide(n, magic, 3).QuadPart, -123456789012LL);
    GENERIC_MAPPING map = {1, 2, 4, 8};
    ULONG mask = 0x80000000 | 0x40000000 | 0x100;
    RtlMapGenericMask(&mask, &map);
    EXPECT_EQ(mask, 0x100u | 1 | 2);
}

TEST(RtlMore, PortBuffers) {
    // Port 0x71 is the CMOS data port: the fake CMOS keeps what is written at the selected index.
    UCHAR in[4] = {1, 2, 3, 4}, out[4] = {};
    __asm__ volatile("outb %0, $0x70" ::"a"((UCHAR)0x50));
    WRITE_PORT_BUFFER_UCHAR((PUCHAR)0x71, in, 4);
    READ_PORT_BUFFER_UCHAR((PUCHAR)0x71, out, 4);
    EXPECT_EQ(out[0], 4) << "the last value written to the same register";
    USHORT w[2];
    ULONG d[2];
    READ_PORT_BUFFER_USHORT((PUSHORT)0x1234, w, 2); // nothing there
    READ_PORT_BUFFER_ULONG((PULONG)0x1234, d, 2);
    EXPECT_EQ(w[1], 0xFFFF);
    EXPECT_EQ(d[1], 0xFFFFFFFFu);
    WRITE_PORT_BUFFER_USHORT((PUSHORT)0x1234, w, 2);
    WRITE_PORT_BUFFER_ULONG((PULONG)0x1234, d, 2);
}

TEST(RtlMore, UnicodeLeftovers) {
    const WCHAR src[] = {'M', 'i', 'X', 0};
    UNICODE_STRING s;
    RtlInitUnicodeString(&s, src);
    WCHAR room[8];
    UNICODE_STRING d = {0, sizeof(room), room};
    RtlCopyUnicodeString(&d, &s);
    EXPECT_EQ(d.Length, 6);
    UNICODE_STRING lower;
    ASSERT_EQ(RtlDowncaseUnicodeString(&lower, &s, TRUE), STATUS_SUCCESS);
    EXPECT_EQ(lower.Buffer[2], L'x');
    RtlFreeUnicodeString(&lower);
    ULONG n;
    RtlMultiByteToUnicodeSize(&n, "abc", 3);
    EXPECT_EQ(n, 6u);
    RtlUnicodeToMultiByteSize(&n, src, 6);
    EXPECT_EQ(n, 3u);
    char up[4];
    RtlUpcaseUnicodeToMultiByteN(up, sizeof(up), &n, src, 6);
    EXPECT_EQ(std::string(up, n), "MIX");
    const WCHAR digits[] = {'0', 'x', '4', '2', 0};
    UNICODE_STRING ds;
    RtlInitUnicodeString(&ds, digits);
    ULONG v;
    ASSERT_EQ(RtlUnicodeStringToInteger(&ds, 0, &v), STATUS_SUCCESS);
    EXPECT_EQ(v, 0x42u);
    WCHAR obuf[12];
    UNICODE_STRING os = {0, sizeof(obuf), obuf};
    ASSERT_EQ(RtlIntegerToUnicodeString(255, 16, &os), STATUS_SUCCESS);
    EXPECT_EQ(os.Length, 4);
    EXPECT_EQ(obuf[0], L'F');
}

TEST(Misc, ByKeyDeviceQueueRemovalAndClockInitialisation) {
    KDEVICE_QUEUE dq;
    KeInitializeDeviceQueue(&dq);
    KDEVICE_QUEUE_ENTRY a, b, c;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KeInsertDeviceQueue(&dq, &a);
    KeInsertByKeyDeviceQueue(&dq, &b, 10);
    KeInsertByKeyDeviceQueue(&dq, &c, 30);
    EXPECT_EQ(KeRemoveByKeyDeviceQueue(&dq, 20), &c) << "the first entry at or after the key";
    EXPECT_EQ(KeRemoveByKeyDeviceQueue(&dq, 20), &b) << "none after it: the first one";
    EXPECT_EQ(KeRemoveByKeyDeviceQueue(&dq, 20), nullptr);
    KfLowerIrql(old);
    HalpInitRtcLate();
    KiInitializeSystemTime(); // from the RTC set by the time tests
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    EXPECT_GT(now.QuadPart, 0);
}
