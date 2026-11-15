// The C library's memory functions (c_library.h; the compiler also calls memset and memcpy itself), and the Xbox kernel's Rtl* memory exports for titles.
#include <runtime/runtime.h>

namespace xk::runtime {

extern "C" void *XK_C(memset)(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

extern "C" void *XK_C(memcpy)(void *dst, const void *src, size_t n) {
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n)::"memory");
    return dst;
}

extern "C" void *XK_C(memmove)(void *dst, const void *src, size_t n) {
    if (dst <= src || (const UCHAR *)dst >= (const UCHAR *)src + n)
        return XK_C(memcpy)(dst, src, n);
    // Overlapping with dst above src: copy backwards.
    UCHAR *d = (UCHAR *)dst + n - 1;
    const UCHAR *s = (const UCHAR *)src + n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n)::"memory");
    return dst;
}

extern "C" int XK_C(memcmp)(const void *a, const void *b, size_t n) {
    auto x = (const UCHAR *)a;
    auto y = (const UCHAR *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

extern "C" void *XK_C(memchr)(const void *Memory, int Value, size_t Count) {
    auto p = (const UCHAR *)Memory;
    for (size_t i = 0; i < Count; i++) {
        if (p[i] == (UCHAR)Value)
            return (void *)(p + i);
    }
    return nullptr;
}

// --- The Xbox kernel's, for titles

XBAPI void NTAPI RtlZeroMemory(PVOID Destination, SIZE_T Length) {
    XK_C(memset)(Destination, 0, Length);
}

XBAPI void NTAPI RtlFillMemory(PVOID Destination, ULONG Length, UCHAR Fill) {
    XK_C(memset)(Destination, Fill, Length);
}

XBAPI void NTAPI RtlMoveMemory(PVOID Destination, const VOID *Source, SIZE_T Length) {
    XK_C(memmove)(Destination, Source, Length);
}

} // namespace xk::runtime
