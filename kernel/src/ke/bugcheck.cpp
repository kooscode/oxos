// Bug checks: record the code and halt. (The reference also reports through the fatal-error path;
// see spec/stock/05 §6 — to be added with the video/SMC work.)
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/serial.h>

#if XK_TRACE_IMPORTS
extern "C" volatile ULONG (*XkTraceRing)[15]; // the tracer (src/xe/trace.cpp)
extern "C" volatile ULONG XkTraceCount;
#endif
volatile ULONG KiBugCheckCode;
volatile ULONG KiBugCheckParameters[4];
XBAPI ULONG KiBugCheckData[5]; // exported: code and the four parameters
ULONG KiBugCheckData[5];

extern "C" void KiSaveBugCheckLastWords(ULONG Code, ULONG P1, ULONG Caller); // ke/startup.cpp
#ifdef XK_HOST_TEST
// Host unit tests: a test can expect a bug check (the harness returns to it); any other one ends the run.
extern "C" void HostBugCheck(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4);
#endif

XBAPI void NTAPI KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
#ifdef XK_HOST_TEST
    HostBugCheck(BugCheckCode, P1, P2, P3, P4);
#endif
    cli();
    // CMOS first (needs no log; reported at the next boot): a release build otherwise halts without a trace.
    KiSaveBugCheckLastWords(BugCheckCode, P1, (ULONG)__builtin_return_address(0));
    XK_SERIAL("*** bug check %#lx (%#lx, %#lx, %#lx, %#lx)\n", BugCheckCode, P1, P2, P3, P4);
#if XK_TRACE_IMPORTS
    // Trace builds: the title's last kernel calls (ordinal, return value, caller, arguments), oldest first.
    XK_SERIAL("    trace ring %p, %lu calls\n", (void *)XkTraceRing, XkTraceCount);
    if (XkTraceRing) {
        ULONG count = XkTraceCount;
        for (ULONG k = count > 30 ? count - 30 : 0; k < count; k++) {
            volatile ULONG *e = XkTraceRing[k % 0x8000];
            XK_SERIAL("    call %lu: ordinal %lu -> %#lx, from %#lx args %#lx %#lx %#lx\n", k, e[0], e[6], e[1], e[3],
                      e[4], e[5]);
        }
    }
#endif
    KiBugCheckParameters[0] = P1;
    KiBugCheckParameters[1] = P2;
    KiBugCheckParameters[2] = P3;
    KiBugCheckParameters[3] = P4;
    KiBugCheckCode = BugCheckCode;
    KiBugCheckData[0] = BugCheckCode;
    for (int i = 0; i < 4; i++)
        KiBugCheckData[i + 1] = KiBugCheckParameters[i];
    post(0xBC);
    for (;;)
        halt();
}

XBAPI void NTAPI KeBugCheck(ULONG BugCheckCode) { KeBugCheckEx(BugCheckCode, 0, 0, 0, 0); }

// A thread's system routine returned instead of terminating the thread (reference: bugcheck 0x0E).
extern "C" void KiThreadReturned() { KeBugCheck(0x0E); }
