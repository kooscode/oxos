// Run-time library: memory, strings and conversions, formatting, time, critical sections and read/write locks,
// interlocked operations, integer conversions.
#include "kernel_test.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <xk/rtl.h>

XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);
XBAPI int __cdecl RtlSprintf(char *Buffer, const char *Format, ...);
XBAPI CHAR NTAPI RtlLowerChar(CHAR Character);
XBAPI LONG NTAPI RtlCompareUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2,
                                         BOOLEAN CaseInSensitive);
XBAPI BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2,
                                          BOOLEAN CaseInSensitive);
XBAPI void NTAPI RtlCopyString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString);
XBAPI void NTAPI RtlUpperString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString);
XBAPI NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString, const ANSI_STRING *SourceString,
                                                  BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PANSI_STRING DestinationString, const UNICODE_STRING *SourceString,
                                                  BOOLEAN AllocateDestinationString);
XBAPI void NTAPI RtlFreeAnsiString(PANSI_STRING AnsiString);
XBAPI void NTAPI RtlFreeUnicodeString(PUNICODE_STRING UnicodeString);
XBAPI NTSTATUS NTAPI RtlAppendStringToString(PSTRING Destination, const STRING *Source);
XBAPI NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING Destination, PCWSTR Source);
XBAPI BOOLEAN NTAPI RtlCreateUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString);
XBAPI WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR SourceCharacter);
XBAPI WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR SourceCharacter);
XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString,
                                            BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR UnicodeString, ULONG MaxBytesInUnicodeString,
                                            PULONG BytesInUnicodeString, const CHAR *MultiByteString,
                                            ULONG BytesInMultiByteString);
XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
                                            PULONG BytesInMultiByteString, const WCHAR *UnicodeString,
                                            ULONG BytesInUnicodeString);
XBAPI NTSTATUS NTAPI RtlCharToInteger(PCSZ String, ULONG Base, PULONG Value);
XBAPI NTSTATUS NTAPI RtlIntegerToChar(ULONG Value, ULONG Base, LONG OutputLength, PCHAR String);
XBAPI ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);
XBAPI SIZE_T NTAPI RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern);
XBAPI void NTAPI RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern);
XBAPI LARGE_INTEGER NTAPI RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand, LONG Multiplier);
XBAPI LARGE_INTEGER NTAPI RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend, ULONG Divisor, PULONG Remainder);
XBAPI LONG FASTCALL InterlockedIncrement(volatile LONG *Addend);
XBAPI LONG FASTCALL InterlockedDecrement(volatile LONG *Addend);
XBAPI LONG FASTCALL InterlockedExchange(volatile LONG *Target, LONG Value);
XBAPI LONG FASTCALL InterlockedExchangeAdd(volatile LONG *Addend, LONG Value);
XBAPI LONG FASTCALL InterlockedCompareExchange(volatile LONG *Destination, LONG Exchange, LONG Comparand);
XBAPI LONGLONG FASTCALL ExInterlockedCompareExchange64(volatile LONGLONG *Destination, PLONGLONG Exchange,
                                                       PLONGLONG Comparand);
XBAPI ULONG FASTCALL RtlUlongByteSwap(ULONG Source);
XBAPI USHORT FASTCALL RtlUshortByteSwap(USHORT Source);

typedef struct _SINGLE_LIST_ENTRY {
    struct _SINGLE_LIST_ENTRY *Next;
} SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;
typedef union _SLIST_HEADER {
    ULONGLONG Alignment;
    struct {
        SINGLE_LIST_ENTRY Next;
        USHORT Depth, Sequence;
    };
} SLIST_HEADER, *PSLIST_HEADER;
XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSINGLE_LIST_ENTRY ListEntry);
XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedPopEntrySList(PSLIST_HEADER ListHead);
XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedFlushSList(PSLIST_HEADER ListHead);

typedef struct _RTL_CRITICAL_SECTION {
    DISPATCHER_HEADER Synchronization;
    LONG LockCount, RecursionCount;
    PKTHREAD OwningThread;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
XBAPI void NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection);
typedef struct _ERWLOCK {
    LONG LockCount;
    ULONG WritersWaitingCount, ReadersWaitingCount, ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK, *PERWLOCK;
XBAPI void NTAPI ExInitializeReadWriteLock(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExAcquireReadWriteLockExclusive(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExAcquireReadWriteLockShared(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExReleaseReadWriteLock(PERWLOCK ReadWriteLock);

// --- Memory

TEST(RtlMemory, FillCompareMove) {
    uint8_t a[64], b[64];
    RtlFillMemory(a, sizeof(a), 0x5A);
    RtlFillMemory(b, sizeof(b), 0x5A);
    EXPECT_EQ(RtlCompareMemory(a, b, 64), 64u);
    b[17] = 0;
    EXPECT_EQ(RtlCompareMemory(a, b, 64), 17u);
    RtlZeroMemory(a, 10);
    EXPECT_EQ(a[9], 0);
    EXPECT_EQ(a[10], 0x5A);
    for (int i = 0; i < 64; i++)
        a[i] = (uint8_t)i;
    RtlMoveMemory(a + 4, a, 32); // overlapping, destination above: backwards copy
    for (int i = 0; i < 32; i++)
        ASSERT_EQ(a[4 + i], i) << i;
    for (int i = 0; i < 64; i++)
        a[i] = (uint8_t)i;
    RtlMoveMemory(a, a + 4, 32); // overlapping, destination below
    for (int i = 0; i < 32; i++)
        ASSERT_EQ(a[i], i + 4) << i;
    ULONG w[8];
    RtlFillMemoryUlong(w, sizeof(w), 0xDEADBEEF);
    EXPECT_EQ(RtlCompareMemoryUlong(w, sizeof(w), 0xDEADBEEF), sizeof(w));
    w[3] = 0;
    EXPECT_EQ(RtlCompareMemoryUlong(w, sizeof(w), 0xDEADBEEF), 12u);
}

// --- Strings

TEST(RtlString, AnsiCompareCopyUpperAppend) {
    ANSI_STRING a, b;
    RtlInitAnsiString(&a, "Hello");
    RtlInitAnsiString(&b, "hello");
    EXPECT_EQ(a.Length, 5);
    EXPECT_EQ(a.MaximumLength, 6);
    EXPECT_FALSE(RtlEqualString(&a, &b, FALSE));
    EXPECT_TRUE(RtlEqualString(&a, &b, TRUE));
    EXPECT_LT(RtlCompareString(&a, &b, FALSE), 0);
    EXPECT_EQ(RtlCompareString(&a, &b, TRUE), 0);
    ANSI_STRING shorter;
    RtlInitAnsiString(&shorter, "Hell");
    EXPECT_GT(RtlCompareString(&a, &shorter, FALSE), 0);
    RtlInitAnsiString(&a, nullptr);
    EXPECT_EQ(a.Length, 0);
    EXPECT_EQ(a.Buffer, nullptr);
    char buf[8];
    ANSI_STRING dst = {0, sizeof(buf), buf}, src;
    RtlInitAnsiString(&src, "abcdefghij");
    RtlCopyString(&dst, &src);
    EXPECT_EQ(dst.Length, 8) << "truncated to the destination";
    EXPECT_EQ(std::string(buf, 8), "abcdefgh");
    RtlInitAnsiString(&src, "mIxEd");
    RtlUpperString(&dst, &src);
    EXPECT_EQ(std::string(dst.Buffer, dst.Length), "MIXED");
    char big[16] = "ab";
    STRING s = {2, sizeof(big), big}, more;
    RtlInitAnsiString(&more, "cdef");
    EXPECT_EQ(RtlAppendStringToString(&s, &more), STATUS_SUCCESS);
    EXPECT_EQ(std::string(s.Buffer, s.Length), "abcdef");
    STRING tiny = {2, 3, big};
    EXPECT_NE(RtlAppendStringToString(&tiny, &more), STATUS_SUCCESS) << "too small";
    EXPECT_EQ(RtlUpperChar('a'), 'A');
    EXPECT_EQ(RtlUpperChar('Z'), 'Z');
    EXPECT_EQ(RtlLowerChar('Q'), 'q');
    EXPECT_EQ(RtlUpperChar('1'), '1');
}

TEST(RtlString, UnicodeConversionsRoundTrip) {
    ANSI_STRING a;
    RtlInitAnsiString(&a, "Xbox 1.0");
    UNICODE_STRING u;
    ASSERT_EQ(RtlAnsiStringToUnicodeString(&u, &a, TRUE), STATUS_SUCCESS);
    EXPECT_EQ(u.Length, 16);
    EXPECT_EQ(u.Buffer[0], L'X');
    UNICODE_STRING up;
    ASSERT_EQ(RtlUpcaseUnicodeString(&up, &u, TRUE), STATUS_SUCCESS);
    EXPECT_EQ(up.Buffer[1], L'B');
    EXPECT_TRUE(RtlEqualUnicodeString(&u, &up, TRUE));
    EXPECT_FALSE(RtlEqualUnicodeString(&u, &up, FALSE));
    EXPECT_NE(RtlCompareUnicodeString(&u, &up, FALSE), 0);
    EXPECT_EQ(RtlCompareUnicodeString(&u, &up, TRUE), 0);
    ANSI_STRING back;
    ASSERT_EQ(RtlUnicodeStringToAnsiString(&back, &up, TRUE), STATUS_SUCCESS);
    EXPECT_EQ(std::string(back.Buffer, back.Length), "XBOX 1.0");
    RtlFreeAnsiString(&back);
    RtlFreeUnicodeString(&up);
    RtlFreeUnicodeString(&u);
    EXPECT_EQ(u.Buffer, nullptr);
    EXPECT_EQ(RtlUpcaseUnicodeChar(L'q'), L'Q');
    EXPECT_EQ(RtlDowncaseUnicodeChar(L'Q'), L'q');
    UNICODE_STRING c;
    const WCHAR name[] = {'a', 'b', 0};
    ASSERT_TRUE(RtlCreateUnicodeString(&c, name));
    EXPECT_EQ(c.Length, 4);
    WCHAR room[8] = {};
    UNICODE_STRING d = {0, sizeof(room), room};
    EXPECT_EQ(RtlAppendUnicodeToString(&d, name), STATUS_SUCCESS);
    EXPECT_EQ(d.Length, 4);
    RtlFreeUnicodeString(&c);
    WCHAR w[4];
    ULONG n = 0;
    EXPECT_EQ(RtlMultiByteToUnicodeN(w, sizeof(w), &n, "abcdef", 6), STATUS_SUCCESS);
    EXPECT_EQ(n, sizeof(w)) << "limited by the output";
    char m[8];
    EXPECT_EQ(RtlUnicodeToMultiByteN(m, sizeof(m), &n, w, sizeof(w)), STATUS_SUCCESS);
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(std::string(m, 4), "abcd");
}

TEST(RtlString, IntegerConversions) {
    ULONG v = 0;
    EXPECT_EQ(RtlCharToInteger("  1234", 10, &v), STATUS_SUCCESS);
    EXPECT_EQ(v, 1234u);
    EXPECT_EQ(RtlCharToInteger("0x1F", 0, &v), STATUS_SUCCESS);
    EXPECT_EQ(v, 0x1Fu);
    EXPECT_EQ(RtlCharToInteger("-5", 10, &v), STATUS_SUCCESS);
    EXPECT_EQ(v, (ULONG)-5);
    EXPECT_EQ(RtlCharToInteger("101", 2, &v), STATUS_SUCCESS);
    EXPECT_EQ(v, 5u);
    EXPECT_NE(RtlCharToInteger("1", 7, &v), STATUS_SUCCESS) << "bad base";
    char out[16];
    EXPECT_EQ(RtlIntegerToChar(255, 16, sizeof(out), out), STATUS_SUCCESS);
    EXPECT_STREQ(out, "FF");
    EXPECT_EQ(RtlIntegerToChar(1000, 10, sizeof(out), out), STATUS_SUCCESS);
    EXPECT_STREQ(out, "1000");
    EXPECT_NE(RtlIntegerToChar(1000, 10, 2, out), STATUS_SUCCESS) << "too small";
}

// --- Formatting: the standard conversions match the C library; truncation follows _snprintf.

TEST(RtlFormat, MatchesTheCLibraryForTheStandardConversions) {
    struct Case {
        const char *format;
        unsigned long a;
    } cases[] = {{"%d", 42},        {"%d", (unsigned long)-42}, {"%5d|", 7},     {"%-5d|", 7},     {"%05d", 7},
                 {"%+d", 3},        {"% d", 3},                 {"%u", 4000000000ul}, {"%x", 0xBEEF}, {"%X", 0xBEEF},
                 {"%08lx", 0x1234}, {"%#x", 255},               {"%o", 8},       {"%#o", 8},       {"%.3d", 5},
                 {"%c", 'Q'},       {"%%%d", 1},                {"%10.4x|", 0xA}, {"%lu", 17},     {"%i", 9}};
    for (auto &c : cases) {
        char mine[64], theirs[64];
        int n1 = RtlSnprintf(mine, sizeof(mine), c.format, c.a);
        int n2 = snprintf(theirs, sizeof(theirs), c.format, c.a);
        EXPECT_STREQ(mine, theirs) << c.format;
        EXPECT_EQ(n1, n2) << c.format;
    }
    char mine[64], theirs[64];
    RtlSnprintf(mine, sizeof(mine), "[%s|%-6s|%6s|%.2s|%.*s]", "abc", "ab", "ab", "abcdef", 3, "xyzzy");
    snprintf(theirs, sizeof(theirs), "[%s|%-6s|%6s|%.2s|%.*s]", "abc", "ab", "ab", "abcdef", 3, "xyzzy");
    EXPECT_STREQ(mine, theirs);
    RtlSnprintf(mine, sizeof(mine), "%lld %llx", -5LL, 0x123456789ABCLL);
    snprintf(theirs, sizeof(theirs), "%lld %llx", -5LL, 0x123456789ABCLL);
    EXPECT_STREQ(mine, theirs);
}

TEST(RtlFormat, TruncationAndTheMicrosoftExtensions) {
    char buf[8];
    memset(buf, '#', sizeof(buf));
    EXPECT_EQ(RtlSnprintf(buf, 4, "%s", "abcdef"), -1) << "_snprintf: -1 when truncated";
    EXPECT_EQ(std::string(buf, 4), "abcd") << "no terminator written when full";
    EXPECT_EQ(RtlSnprintf(buf, sizeof(buf), "%s", "abc"), 3);
    EXPECT_STREQ(buf, "abc");
    char big[64];
    EXPECT_EQ(RtlSprintf(big, "%s=%d", "x", 5), 3);
    EXPECT_STREQ(big, "x=5");
    const WCHAR wide[] = {'w', 'i', 'd', 'e', 0};
    RtlSnprintf(big, sizeof(big), "%S|%C", wide, (WCHAR)'Z');
    EXPECT_STREQ(big, "wide|Z");
    ANSI_STRING as;
    RtlInitAnsiString(&as, "counted");
    RtlSnprintf(big, sizeof(big), "%Z", &as);
    EXPECT_STREQ(big, "counted");
    RtlSnprintf(big, sizeof(big), "%s", (const char *)nullptr);
    EXPECT_STREQ(big, "(null)");
    int count = 0;
    RtlSnprintf(big, sizeof(big), "abc%n", &count);
    EXPECT_EQ(count, 3);
}

// --- Time

TEST(RtlTime, FieldsRoundTripAndValidate) {
    TIME_FIELDS f = {2026, 10, 23, 4, 5, 6, 789, 0};
    LARGE_INTEGER t;
    ASSERT_TRUE(RtlTimeFieldsToTime(&f, &t));
    TIME_FIELDS g = {};
    RtlTimeToTimeFields(&t, &g);
    EXPECT_EQ(g.Year, 2026);
    EXPECT_EQ(g.Month, 10);
    EXPECT_EQ(g.Day, 23);
    EXPECT_EQ(g.Hour, 4);
    EXPECT_EQ(g.Minute, 5);
    EXPECT_EQ(g.Second, 6);
    EXPECT_EQ(g.Milliseconds, 789);
    EXPECT_EQ(g.Weekday, 5) << "a Friday";
    TIME_FIELDS epoch = {1601, 1, 1, 0, 0, 0, 0, 0};
    ASSERT_TRUE(RtlTimeFieldsToTime(&epoch, &t));
    EXPECT_EQ(t.QuadPart, 0);
    TIME_FIELDS leap = {2024, 2, 29, 0, 0, 0, 0, 0}, notLeap = {2023, 2, 29, 0, 0, 0, 0, 0},
                century = {1900, 2, 29, 0, 0, 0, 0, 0}, y2k = {2000, 2, 29, 0, 0, 0, 0, 0},
                badMonth = {2024, 13, 1, 0, 0, 0, 0, 0}, badHour = {2024, 1, 1, 24, 0, 0, 0, 0};
    EXPECT_TRUE(RtlTimeFieldsToTime(&leap, &t));
    EXPECT_FALSE(RtlTimeFieldsToTime(&notLeap, &t));
    EXPECT_FALSE(RtlTimeFieldsToTime(&century, &t));
    EXPECT_TRUE(RtlTimeFieldsToTime(&y2k, &t));
    EXPECT_FALSE(RtlTimeFieldsToTime(&badMonth, &t));
    EXPECT_FALSE(RtlTimeFieldsToTime(&badHour, &t));
    // Every day across four years round-trips.
    TIME_FIELDS d = {2023, 1, 1, 12, 0, 0, 0, 0};
    ASSERT_TRUE(RtlTimeFieldsToTime(&d, &t));
    for (int day = 0; day < 4 * 366; day++) {
        LARGE_INTEGER u;
        u.QuadPart = t.QuadPart + (LONGLONG)day * 864000000000LL;
        TIME_FIELDS x;
        RtlTimeToTimeFields(&u, &x);
        LARGE_INTEGER back;
        ASSERT_TRUE(RtlTimeFieldsToTime(&x, &back)) << day;
        ASSERT_EQ(back.QuadPart, u.QuadPart) << day;
    }
}

// --- Critical sections and read/write locks

TEST(RtlSync, CriticalSectionIsRecursiveAndExcludesOthers) {
    RTL_CRITICAL_SECTION cs;
    RtlInitializeCriticalSection(&cs);
    RtlEnterCriticalSection(&cs);
    RtlEnterCriticalSection(&cs);
    EXPECT_EQ(cs.RecursionCount, 2);
    EXPECT_EQ(cs.OwningThread, KeGetCurrentThread());
    std::atomic<int> state{0};
    PKTHREAD t = Spawn([&] {
        state = 1;
        EXPECT_FALSE(RtlTryEnterCriticalSection(&cs));
        RtlEnterCriticalSection(&cs);
        state = 2;
        RtlLeaveCriticalSection(&cs);
    }, 9);
    EXPECT_EQ(state, 1) << "blocked on the section";
    RtlLeaveCriticalSection(&cs);
    EXPECT_EQ(state, 1) << "still held once";
    RtlLeaveCriticalSection(&cs);
    Join(t);
    EXPECT_EQ(state, 2);
    EXPECT_EQ(cs.LockCount, -1);
    EXPECT_TRUE(RtlTryEnterCriticalSection(&cs));
    EXPECT_TRUE(RtlTryEnterCriticalSection(&cs)) << "recursive try";
    RtlLeaveCriticalSection(&cs);
    RtlLeaveCriticalSection(&cs);
    RtlEnterCriticalSectionAndRegion(&cs);
    EXPECT_LT((LONG)KeGetCurrentThread()->KernelApcDisable, 0);
    RtlLeaveCriticalSectionAndRegion(&cs);
    EXPECT_EQ(KeGetCurrentThread()->KernelApcDisable, 0);
}

TEST(RtlSync, ReadWriteLockReadersShareWritersExclude) {
    ERWLOCK l;
    ExInitializeReadWriteLock(&l);
    ExAcquireReadWriteLockShared(&l);
    ExAcquireReadWriteLockShared(&l); // a second reader gets in
    std::atomic<int> writer{0};
    PKTHREAD w = Spawn([&] {
        writer = 1;
        ExAcquireReadWriteLockExclusive(&l);
        writer = 2;
        SleepMs(5);
        ExReleaseReadWriteLock(&l);
    }, 9);
    EXPECT_EQ(writer, 1) << "waits for the readers";
    std::atomic<int> reader{0};
    PKTHREAD r = Spawn([&] {
        ExAcquireReadWriteLockShared(&l); // a writer is waiting: readers queue behind it
        reader = writer.load();
        ExReleaseReadWriteLock(&l);
    }, 9);
    EXPECT_EQ(reader, 0);
    ExReleaseReadWriteLock(&l);
    EXPECT_EQ(writer, 1);
    ExReleaseReadWriteLock(&l); // the last reader leaves: the writer runs
    Join(w);
    Join(r);
    EXPECT_EQ(writer, 2);
    EXPECT_EQ(reader, 2) << "the queued reader ran after the writer";
    EXPECT_EQ(l.LockCount, -1);
}

// --- Interlocked operations, S-lists, arithmetic

TEST(RtlRuntime, InterlockedAndSLists) {
    volatile LONG v = 5;
    EXPECT_EQ(InterlockedIncrement(&v), 6);
    EXPECT_EQ(InterlockedDecrement(&v), 5);
    EXPECT_EQ(InterlockedExchange(&v, 9), 5);
    EXPECT_EQ(InterlockedExchangeAdd(&v, 3), 9);
    EXPECT_EQ(InterlockedCompareExchange(&v, 1, 12), 12);
    EXPECT_EQ(v, 1);
    EXPECT_EQ(InterlockedCompareExchange(&v, 7, 99), 1) << "no exchange";
    EXPECT_EQ(v, 1);
    volatile LONGLONG q = 10;
    LONGLONG x = 20, c = 10;
    EXPECT_EQ(ExInterlockedCompareExchange64(&q, &x, &c), 10);
    EXPECT_EQ(q, 20);
    SLIST_HEADER h = {};
    SINGLE_LIST_ENTRY e[3];
    EXPECT_EQ(InterlockedPushEntrySList(&h, &e[0]), nullptr);
    EXPECT_EQ(InterlockedPushEntrySList(&h, &e[1]), &e[0]);
    InterlockedPushEntrySList(&h, &e[2]);
    EXPECT_EQ(h.Depth, 3);
    EXPECT_EQ(InterlockedPopEntrySList(&h), &e[2]);
    EXPECT_EQ(InterlockedFlushSList(&h), &e[1]);
    EXPECT_EQ(h.Depth, 0);
    EXPECT_EQ(InterlockedPopEntrySList(&h), nullptr);
    EXPECT_EQ(RtlUlongByteSwap(0x11223344), 0x44332211u);
    EXPECT_EQ(RtlUshortByteSwap(0x1122), 0x2211);
    LARGE_INTEGER a;
    a.QuadPart = 0x123456789LL;
    EXPECT_EQ(RtlExtendedIntegerMultiply(a, -3).QuadPart, -3 * 0x123456789LL);
    ULONG rem = 0;
    EXPECT_EQ(RtlExtendedLargeIntegerDivide(a, 1000, &rem).QuadPart, 0x123456789LL / 1000);
    EXPECT_EQ(rem, (ULONG)(0x123456789LL % 1000));
}

TEST(RtlRuntime, StatusToDosError) {
    EXPECT_EQ(RtlNtStatusToDosError(STATUS_SUCCESS), 0u);
    EXPECT_EQ(RtlNtStatusToDosError((NTSTATUS)0xC0000034), 2u) << "file not found";
    EXPECT_EQ(RtlNtStatusToDosError((NTSTATUS)0xC000003A), 3u) << "path not found";
    EXPECT_EQ(RtlNtStatusToDosError((NTSTATUS)0xC0000022), 5u) << "access denied";
    EXPECT_EQ(RtlNtStatusToDosError(STATUS_TIMEOUT), 258u) << "a success code: WAIT_TIMEOUT";
}
