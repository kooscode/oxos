// Boot self-tests for structured exception handling (milestone 5b). Registration frames are built by hand, the
// way compiler-generated SEH prologues do: {Next, Handler} pushed on the stack and linked at FS:[0].
#include <xk/ke.h>

struct XK_RECORD {
    ULONG ExceptionCode, ExceptionFlags;
    PVOID Record, ExceptionAddress;
    ULONG NumberParameters;
    ULONG_PTR Info[15];
};
struct XK_CONTEXT {
    ULONG ContextFlags;
    UCHAR FloatSave[0x204];
    ULONG Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs;
};
XBAPI void NTAPI RtlRaiseException(XK_RECORD *);
XBAPI void NTAPI RtlRaiseStatus(NTSTATUS);
XBAPI void NTAPI RtlUnwind(PVOID, PVOID, XK_RECORD *, PVOID);
XBAPI void NTAPI RtlCaptureContext(XK_CONTEXT *);
XBAPI USHORT NTAPI RtlCaptureStackBackTrace(ULONG, ULONG, PVOID *, PULONG);
extern volatile ULONG KiTestRtlDone;

volatile ULONG KiTestSehDivideOk, KiTestSehPageFaultOk, KiTestSehRaiseOk, KiTestSehUnwindOk, KiTestSehNestedOk,
    KiTestSehCaptureOk, KiTestSehDone;

struct REGISTRATION {
    REGISTRATION *Next;
    PVOID Handler;
};
static REGISTRATION *&Head() { return *(REGISTRATION **)&KiPcr.NtTib.ExceptionList; }

static volatile ULONG LastCode, LastParam0, LastParam1, LastFlags, UnwindCalls;

// Handler: record the exception, skip the faulting 2-byte instruction and continue with Eax = 77.
static ULONG __cdecl SkipHandler(XK_RECORD *r, PVOID, XK_CONTEXT *c, PVOID) {
    if (r->ExceptionFlags & 6)
        return 1;
    LastCode = r->ExceptionCode;
    LastParam0 = r->Info[0];
    LastParam1 = r->Info[1];
    c->Eip += 2;
    c->Eax = 77;
    return 0; // ExceptionContinueExecution
}

static ULONG __cdecl ContinueRaiseHandler(XK_RECORD *r, PVOID, XK_CONTEXT *, PVOID) {
    if (r->ExceptionFlags & 6)
        return 1;
    LastCode = r->ExceptionCode;
    return r->ExceptionFlags & 1 ? 1 : 0; // continuable ones continue
}

// Like compiler SEH: unwind the inner frames (TargetIp 0 returns here), then jump back into the guarded function.
static void *JumpBuffer[5];
static ULONG __cdecl UnwindHandler(XK_RECORD *r, PVOID frame, XK_CONTEXT *, PVOID) {
    if (r->ExceptionFlags & 6)
        return 1;
    LastCode = r->ExceptionCode;
    RtlUnwind(frame, nullptr, r, nullptr);
    __builtin_longjmp(JumpBuffer, 1);
}

static ULONG __cdecl InnerSearchHandler(XK_RECORD *r, PVOID, XK_CONTEXT *, PVOID) {
    if (r->ExceptionFlags & 2) {
        UnwindCalls = UnwindCalls + 1;
        LastFlags = r->ExceptionFlags;
    }
    return 1; // ExceptionContinueSearch
}

static void NTAPI SehControllerThread(PVOID) {
    while (!KiTestRtlDone) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    // 1. Integer divide by zero, continued past the faulting "div ecx".
    REGISTRATION reg = {Head(), (PVOID)SkipHandler};
    Head() = &reg;
    ULONG result;
    __asm__ volatile("xorl %%ecx, %%ecx\n\t"
                     "movl $10, %%eax\n\t"
                     "xorl %%edx, %%edx\n\t"
                     "divl %%ecx\n\t" // f7 f1: 2 bytes
                     : "=a"(result)
                     :
                     : "ecx", "edx");
    KiTestSehDivideOk = result == 77 && LastCode == 0xC0000094;

    // 2. Page fault on a read of address 0 ("movl (%ecx), %eax" = 8b 01).
    __asm__ volatile("xorl %%ecx, %%ecx\n\t"
                     "movl (%%ecx), %%eax\n\t"
                     : "=a"(result)
                     :
                     : "ecx");
    KiTestSehPageFaultOk = result == 77 && LastCode == 0xC0000005 && LastParam0 == 0 && LastParam1 == 0;
    Head() = reg.Next;

    // 3. A continuable RtlRaiseException returns to its caller.
    REGISTRATION reg2 = {Head(), (PVOID)ContinueRaiseHandler};
    Head() = &reg2;
    XK_RECORD rec = {0xE0001234, 0, nullptr, nullptr, 0, {}};
    LastCode = 0;
    RtlRaiseException(&rec);
    KiTestSehRaiseOk = LastCode == 0xE0001234;
    Head() = reg2.Next;

    // 4. RtlRaiseStatus is noncontinuable: the outer handler unwinds (the inner frame sees the unwind pass, 5.)
    //    and jumps back here.
    REGISTRATION outer = {Head(), (PVOID)UnwindHandler};
    REGISTRATION inner = {&outer, (PVOID)InnerSearchHandler};
    volatile ULONG reached = 0;
    Head() = &inner;
    if (!__builtin_setjmp(JumpBuffer)) {
        RtlRaiseStatus((NTSTATUS)0xC0004321);
        reached = 99; // never: the status cannot be continued
    }
    bool unwound = Head() == &outer;
    Head() = outer.Next;
    KiTestSehUnwindOk = reached == 0 && LastCode == 0xC0004321 && unwound;
    KiTestSehNestedOk = UnwindCalls == 1 && (LastFlags & 2);

    // 5b. RtlUnwind with a target IP in the same function resumes there with Eax = ReturnValue.
    REGISTRATION top = {Head(), (PVOID)InnerSearchHandler};
    REGISTRATION above = {&top, (PVOID)InnerSearchHandler};
    Head() = &above;
    ULONG value;
    __asm__ volatile("pushl $0x5678\n\t"
                     "pushl $0\n\t"
                     "pushl $2f\n\t"
                     "pushl %1\n\t"
                     "call _RtlUnwind@16\n\t"
                     "movl $1, %%eax\n" // skipped
                     "2:\n\t"
                     : "=a"(value)
                     : "r"(&top)
                     : "ecx", "edx", "memory");
    KiTestSehUnwindOk = KiTestSehUnwindOk && value == 0x5678 && Head() == &top && UnwindCalls == 2;
    Head() = top.Next;

    // 6. Context capture and stack walk.
    XK_CONTEXT ctx;
    RtlCaptureContext(&ctx);
    PVOID frames[8];
    USHORT n = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
    KiTestSehCaptureOk = ctx.Eip >= 0x80010000 && ctx.Esp <= (ULONG)&ctx && ctx.Esp + 0x1000 > (ULONG)&ctx &&
                         ctx.SegCs == 8 && n >= 1 && (ULONG)frames[0] >= 0x80010000;
    KiTestSehDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartSehSelfTests() { KiCreateSystemThread(SehControllerThread, nullptr, 0, 9); }
