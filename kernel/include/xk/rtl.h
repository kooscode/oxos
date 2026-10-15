// Runtime library subset.
#pragma once
#include <xk/types.h>

XBAPI void *memset(void *dst, int c, SIZE_T n);
XBAPI void *memcpy(void *dst, const void *src, SIZE_T n);
XBAPI void *memmove(void *dst, const void *src, SIZE_T n);

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length);
XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill);
XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length);

static inline void RtlCopyMemory(PVOID dst, const VOID *src, SIZE_T n) { memcpy(dst, src, n); }
