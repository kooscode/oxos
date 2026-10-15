// Freestanding memory primitives (the compiler may also emit calls to memset/memcpy).
#include <xk/rtl.h>

extern "C" void *memset(void *dst, int c, SIZE_T n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

extern "C" void *memcpy(void *dst, const void *src, SIZE_T n) {
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n)::"memory");
    return dst;
}

extern "C" void *memmove(void *dst, const void *src, SIZE_T n) {
    if (dst <= src || (const UCHAR *)dst >= (const UCHAR *)src + n)
        return memcpy(dst, src, n);
    // Overlapping with dst above src: copy backwards.
    UCHAR *d = (UCHAR *)dst + n - 1;
    const UCHAR *s = (const UCHAR *)src + n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n)::"memory");
    return dst;
}

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length) { memset(Destination, 0, Length); }

XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill) { memset(Destination, Fill, Length); }

XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length) {
    memmove(Destination, Source, Length);
}
