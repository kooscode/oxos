// Runtime library subset.
#pragma once
#include <xk/types.h>

XBAPI void *memset(void *dst, int c, SIZE_T n);
XBAPI void *memcpy(void *dst, const void *src, SIZE_T n);
XBAPI void *memmove(void *dst, const void *src, SIZE_T n);

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length);
XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill);
XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length);
XBAPI SIZE_T NTAPI RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length);

static inline void RtlCopyMemory(PVOID dst, const VOID *src, SIZE_T n) { memcpy(dst, src, n); }

typedef struct _TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS, *PTIME_FIELDS;
XBAPI BOOLEAN NTAPI RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time);
XBAPI void NTAPI RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields);
