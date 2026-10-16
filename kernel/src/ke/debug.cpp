// Debugger services without a kernel debugger: DbgPrint goes to an in-memory log (spec/13).
#include <xk/ke.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
XBAPI int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args);

XBAPI BOOLEAN KdDebuggerEnabled, KdDebuggerNotPresent;
BOOLEAN KdDebuggerEnabled = FALSE;
BOOLEAN KdDebuggerNotPresent = TRUE;

// Ring buffer of debug output, readable from the emulator monitor (XkDebugLog / XkDebugLogPosition).
char XkDebugLog[XK_BOOT_SELFTEST ? 8192 : 1024]; // smaller in release builds: it stays resident
volatile ULONG XkDebugLogPosition;

static void XkLogWrite(const char *s, ULONG n) {
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    for (ULONG i = 0; i < n; i++)
        XkDebugLog[(XkDebugLogPosition + i) % sizeof(XkDebugLog)] = s[i];
    XkDebugLogPosition = XkDebugLogPosition + n;
    __builtin_ia32_writeeflags_u32(flags);
}

XBAPI ULONG __cdecl DbgPrint(const char *Format, ...) {
    char line[512];
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(line, sizeof(line), Format, ap);
    va_end(ap);
    XkLogWrite(line, n < 0 ? sizeof(line) : (ULONG)n);
    return STATUS_SUCCESS;
}

XBAPI ULONG NTAPI DbgPrompt(const char *Prompt, char *Response, ULONG MaximumResponseLength) {
    DbgPrint("%s", Prompt);
    if (MaximumResponseLength)
        Response[0] = 0;
    return 0; // nothing typed: there is no debugger
}

XBAPI void NTAPI DbgBreakPoint() {
    if (KdDebuggerEnabled)
        __asm__ volatile("int3");
}

XBAPI void NTAPI DbgBreakPointWithStatus(ULONG Status) {
    if (KdDebuggerEnabled)
        __asm__ volatile("int3" ::"a"(Status));
}

XBAPI void NTAPI RtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber, const char *Message) {
    DbgPrint("\n*** Assertion failed: %s%s\n***   Source File: %s, line %lu\n\n", Message ? Message : "",
             (const char *)FailedAssertion, (const char *)FileName, LineNumber);
    DbgBreakPoint();
}

XBAPI void NTAPI RtlRip(const char *ApiName, const char *Expression, const char *Message) {
    DbgPrint("%s%c %c%s%c %s\n", ApiName ? ApiName : "", ApiName ? ':' : ' ', Expression ? '(' : ' ',
             Expression ? Expression : "", Expression ? ')' : ' ', Message ? Message : "");
    DbgBreakPoint();
}
