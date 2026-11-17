// Runtime library subset (src/runtime/): memory, strings, numbers, time, formatting, interlocked operations, and the locks titles embed.
#pragma once
#include <runtime/types.h>
#include <core/core.h>

#include <runtime/c_library.h> // memcpy, strlen, snprintf, ...: what OXOS's own code uses

// --- The Xbox kernel's Rtl* exports, for titles only: OXOS's own code uses the C library (c_library.h)

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length);
XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill);
XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length);
XBAPI SIZE_T NTAPI RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length);

typedef struct _TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS, *PTIME_FIELDS;
XBAPI BOOLEAN NTAPI RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time);
XBAPI void NTAPI RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields);

// Formatting (runtime/sprintf.cpp) and status codes (runtime/status.cpp).
typedef __builtin_va_list va_list;
XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);
XBAPI int __cdecl RtlSprintf(char *Buffer, const char *Format, ...);
XBAPI int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args);
XBAPI ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);

// Numbers and memory (runtime/runtime.cpp).
XBAPI NTSTATUS NTAPI RtlIntegerToChar(ULONG Value, ULONG Base, LONG OutputLength, PCHAR String);
XBAPI LARGE_INTEGER NTAPI RtlExtendedMagicDivide(LARGE_INTEGER Dividend, LARGE_INTEGER MagicDivisor, CCHAR ShiftCount);
XBAPI LARGE_INTEGER NTAPI RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend, ULONG Divisor, PULONG Remainder);
XBAPI LARGE_INTEGER NTAPI RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand, LONG Multiplier);
XBAPI ULONG FASTCALL RtlUlongByteSwap(ULONG Source);
XBAPI USHORT FASTCALL RtlUshortByteSwap(USHORT Source);
XBAPI SIZE_T NTAPI RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern);
XBAPI void NTAPI RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern);

// Interlocked operations (runtime/runtime.cpp).
XBAPI LONG FASTCALL InterlockedIncrement(volatile LONG *Addend);
XBAPI LONG FASTCALL InterlockedDecrement(volatile LONG *Addend);
XBAPI LONG FASTCALL InterlockedExchange(volatile LONG *Target, LONG Value);
XBAPI LONG FASTCALL InterlockedExchangeAdd(volatile LONG *Addend, LONG Value);
XBAPI LONG FASTCALL InterlockedCompareExchange(volatile LONG *Destination, LONG Exchange, LONG Comparand);
XBAPI LONGLONG FASTCALL ExInterlockedCompareExchange64(volatile LONGLONG *Destination, PLONGLONG Exchange, PLONGLONG Comparand);
XBAPI LARGE_INTEGER NTAPI ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend, LARGE_INTEGER Increment, PVOID /*Lock*/);
XBAPI void FASTCALL ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend, ULONG Increment);

// S-lists: lock-free stacks. The 8-byte header: {Next, Depth:16, Sequence:16}.
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
// A page fault at Eip (with interrupts on at the fault, InterruptsOn) that is the pop's read of the first entry's Next: Eip moves back to the pop's header read
// and true is returned, so the trap returns and the pop starts over (spec/stock/14, "Page faults"). The trap handler, and the host tests' fault handler, ask.
XBAPI bool KiRestartSListPop(ULONG &Eip, bool InterruptsOn);

// --- Strings (string.cpp, runtime.cpp). Object names are ANSI counted strings on the Xbox.

typedef WCHAR *PWSTR;
typedef const WCHAR *PCWSTR;

typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} STRING, ANSI_STRING, OBJECT_STRING, *PSTRING, *PANSI_STRING, *POBJECT_STRING;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

XBAPI void NTAPI RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString);
XBAPI void NTAPI RtlInitUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString);
XBAPI BOOLEAN NTAPI RtlEqualString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI LONG NTAPI RtlCompareString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI CHAR NTAPI RtlUpperChar(CHAR Character);
XBAPI NTSTATUS NTAPI RtlUnicodeStringToInteger(const UNICODE_STRING *String, ULONG Base, PULONG Value);
XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString, BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString, PULONG BytesInMultiByteString, const WCHAR *UnicodeString,
                                            ULONG BytesInUnicodeString);

XBAPI NTSTATUS NTAPI RtlCharToInteger(PCSZ String, ULONG Base, PULONG Value);
// Access masks (runtime.cpp): the generic rights an object type maps to its specific ones.
struct GENERIC_MAPPING {
    ULONG GenericRead, GenericWrite, GenericExecute, GenericAll;
};
XBAPI void NTAPI RtlMapGenericMask(PULONG AccessMask, const GENERIC_MAPPING *GenericMapping);

XBAPI CHAR NTAPI RtlLowerChar(CHAR Character);
XBAPI LONG NTAPI RtlCompareUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI void NTAPI RtlCopyString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString);
XBAPI void NTAPI RtlUpperString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString);
XBAPI NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString, const ANSI_STRING *SourceString, BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PANSI_STRING DestinationString, const UNICODE_STRING *SourceString, BOOLEAN AllocateDestinationString);
XBAPI void NTAPI RtlFreeAnsiString(PANSI_STRING AnsiString);
XBAPI void NTAPI RtlFreeUnicodeString(PUNICODE_STRING UnicodeString);
XBAPI NTSTATUS NTAPI RtlAppendStringToString(PSTRING Destination, const STRING *Source);
XBAPI NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING Destination, PCWSTR Source);
XBAPI BOOLEAN NTAPI RtlCreateUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString);
XBAPI void NTAPI RtlCopyUnicodeString(PUNICODE_STRING Destination, const UNICODE_STRING *Source);
XBAPI WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR SourceCharacter);
XBAPI WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR SourceCharacter);
XBAPI NTSTATUS NTAPI RtlDowncaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString, BOOLEAN AllocateDestinationString);
XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR UnicodeString, ULONG MaxBytesInUnicodeString, PULONG BytesInUnicodeString, const CHAR *MultiByteString,
                                            ULONG BytesInMultiByteString);
XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString, const CHAR *,
                                               ULONG BytesInMultiByteString);
XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString, const WCHAR *, ULONG BytesInUnicodeString);
XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString, PULONG BytesInMultiByteString,
                                                  const WCHAR *UnicodeString, ULONG BytesInUnicodeString);
XBAPI NTSTATUS NTAPI RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PUNICODE_STRING String);

// --- Locks and interlocked lists (exported)

// Critical sections and read/write locks (sync.cpp). Layouts are the Xbox ABI: titles embed both.
typedef struct _RTL_CRITICAL_SECTION {
    DISPATCHER_HEADER Synchronization; // a synchronization event (Type 1, Size 4)
    LONG LockCount;                    // -1 = free
    LONG RecursionCount;
    PKTHREAD OwningThread;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
XK_SIZE(RTL_CRITICAL_SECTION, 0x1C);
XBAPI void NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection);
XBAPI void NTAPI RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection);
typedef struct _ERWLOCK {
    LONG LockCount;
    ULONG WritersWaitingCount;
    ULONG ReadersWaitingCount;
    ULONG ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK, *PERWLOCK;
XK_SIZE(ERWLOCK, 0x34);
XBAPI void NTAPI ExInitializeReadWriteLock(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExAcquireReadWriteLockExclusive(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExAcquireReadWriteLockShared(PERWLOCK ReadWriteLock);
XBAPI void NTAPI ExReleaseReadWriteLock(PERWLOCK ReadWriteLock);

// Interlocked list operations (runtime.cpp).
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry);
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry);
XBAPI PLIST_ENTRY FASTCALL ExfInterlockedRemoveHeadList(PLIST_ENTRY ListHead);
