// Debugger services without a kernel debugger (spec/stock/13): DbgPrint and a title's OutputDebugString go to the kernel's debug log
// (diagnostics/serial_log.cpp: the serial port and its in-memory copy in SERIAL=1 builds; nothing in a release kernel, as the stock kernel without a debugger).
#include <core/core.h>
#include <diagnostics/diagnostics.h>

namespace xk::core {

// Exported: there is no kernel debugger (titles and the XDK check these).
XBAPI BOOLEAN KdDebuggerEnabled, KdDebuggerNotPresent;
XBAPI BOOLEAN KdDebuggerEnabled = FALSE;
XBAPI BOOLEAN KdDebuggerNotPresent = TRUE;

// Debug service 1 (int 0x2D, print): the counted string a title's OutputDebugString / DbgPrint passes (spec/stock/14); copied as it is ("%.*s": no formatting,
// no closing NUL needed).
void PrintDebugString(const char *Text, ULONG Length) {
    diagnostics::LogLine("%.*s", (int)Length, Text);
}

XBAPI ULONG __cdecl DbgPrint(const char *Format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, Format);
    diagnostics::LogLineArgs(Format, args);
    __builtin_va_end(args);
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
    DbgPrint("\n*** Assertion failed: %s%s\n***   Source File: %s, line %lu\n\n", Message ? Message : "", (const char *)FailedAssertion, (const char *)FileName,
             LineNumber);
    DbgBreakPoint();
}

XBAPI void NTAPI RtlRip(const char *ApiName, const char *Expression, const char *Message) {
    DbgPrint("%s%c %c%s%c %s\n", ApiName ? ApiName : "", ApiName ? ':' : ' ', Expression ? '(' : ' ', Expression ? Expression : "", Expression ? ')' : ' ',
             Message ? Message : "");
    DbgBreakPoint();
}

} // namespace xk::core
