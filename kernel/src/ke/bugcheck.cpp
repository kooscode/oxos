// Bug checks: record the code and halt. (The reference also reports through the fatal-error path;
// see spec/stock/05 §6 — to be added with the video/SMC work.)
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/serial.h>

volatile ULONG KiBugCheckCode;
volatile ULONG KiBugCheckParameters[4];
XBAPI ULONG KiBugCheckData[5]; // exported: code and the four parameters
ULONG KiBugCheckData[5];

XBAPI void NTAPI KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4) {
    cli();
    XK_SERIAL("*** bug check %#lx (%#lx, %#lx, %#lx, %#lx)\n", BugCheckCode, P1, P2, P3, P4);
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
