// Runtime library subset.
#pragma once
#include <xk/types.h>

#ifdef XK_HOST_TEST // host unit tests: the C library's (its size_t)
XBAPI void *memset(void *dst, int c, __SIZE_TYPE__ n);
XBAPI void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n);
XBAPI void *memmove(void *dst, const void *src, __SIZE_TYPE__ n);
#else
XBAPI void *memset(void *dst, int c, SIZE_T n);
XBAPI void *memcpy(void *dst, const void *src, SIZE_T n);
XBAPI void *memmove(void *dst, const void *src, SIZE_T n);
#endif

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length);
XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill);
XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length);
XBAPI SIZE_T NTAPI RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length);

static inline void RtlCopyMemory(PVOID dst, const VOID *src, SIZE_T n) {
    memcpy(dst, src, n);
}

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
XBAPI LONGLONG FASTCALL ExInterlockedCompareExchange64(volatile LONGLONG *Destination, PLONGLONG Exchange,
                                                       PLONGLONG Comparand);
XBAPI LARGE_INTEGER NTAPI ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend, LARGE_INTEGER Increment,
                                                       PVOID /*Lock*/);
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

// Exceptions raised from C++, and stack walks (core/exceptions.cpp).
XBAPI void NTAPI RtlRaiseStatus(NTSTATUS Status);
XBAPI USHORT NTAPI RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture, PVOID *BackTrace,
                                            PULONG BackTraceHash);
