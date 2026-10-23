// Structured exception handling: trap translation, handler dispatch, raise, unwind, stack walks (spec/stock/14).
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/ke.h>

constexpr ULONG EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4,
                EXCEPTION_NESTED_CALL = 0x10;
constexpr ULONG STATUS_BREAKPOINT_ = 0x80000003, STATUS_SINGLE_STEP_ = 0x80000004,
                STATUS_DATATYPE_MISALIGNMENT_ = 0x80000002, STATUS_ACCESS_VIOLATION_ = 0xC0000005,
                STATUS_ILLEGAL_INSTRUCTION_ = 0xC000001D, STATUS_NONCONTINUABLE_EXCEPTION_ = 0xC0000025,
                STATUS_INVALID_DISPOSITION_ = 0xC0000026, STATUS_UNWIND_ = 0xC0000027,
                STATUS_ARRAY_BOUNDS_EXCEEDED_ = 0xC000008C, STATUS_FLOAT_DENORMAL_OPERAND_ = 0xC000008D,
                STATUS_FLOAT_DIVIDE_BY_ZERO_ = 0xC000008E, STATUS_FLOAT_INEXACT_RESULT_ = 0xC000008F,
                STATUS_FLOAT_INVALID_OPERATION_ = 0xC0000090, STATUS_FLOAT_OVERFLOW_ = 0xC0000091,
                STATUS_FLOAT_STACK_CHECK_ = 0xC0000092, STATUS_FLOAT_UNDERFLOW_ = 0xC0000093,
                STATUS_INTEGER_DIVIDE_BY_ZERO_ = 0xC0000094, STATUS_INTEGER_OVERFLOW_ = 0xC0000095,
                STATUS_FLOAT_MULTIPLE_TRAPS_ = 0xC00002B5;
constexpr ULONG KMODE_EXCEPTION_NOT_HANDLED = 0x1E;

typedef struct _EXCEPTION_RECORD {
    ULONG ExceptionCode;
    ULONG ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    ULONG NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
XK_SIZE(EXCEPTION_RECORD, 0x50);

typedef struct _CONTEXT {
    ULONG ContextFlags;
    UCHAR FloatSave[0x204];
    ULONG Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs;
} CONTEXT, *PCONTEXT;
XK_SIZE(CONTEXT, 0x238);
XK_OFFSET(CONTEXT, Eip, 0x224);

struct KTRAP_FRAME_XK {
    ULONG Edi, Esi, Ebp, EspIgnored, Ebx, Edx, Ecx, Eax;
    ULONG Vector, ErrorCode;
    ULONG Eip, SegCs, EFlags; // the faulting ESP is &EFlags + 1 (no privilege change)
};

struct EXCEPTION_REGISTRATION {
    EXCEPTION_REGISTRATION *Next;
    ULONG(__cdecl *Handler)(PEXCEPTION_RECORD, PVOID, PCONTEXT, PVOID);
};
#define EXCEPTION_CHAIN_END ((EXCEPTION_REGISTRATION *)0xFFFFFFFF)
enum { ExceptionContinueExecution, ExceptionContinueSearch, ExceptionNestedException, ExceptionCollidedUnwind };

extern "C" void KiContinueContext(PCONTEXT Context) __attribute__((noreturn));
extern "C" void KiUnexpectedException(ULONG vector, ULONG errorCode, ULONG eip);

static EXCEPTION_REGISTRATION *&KiExceptionList() { return *(EXCEPTION_REGISTRATION **)&KiPcr.NtTib.ExceptionList; }

static volatile ULONG KiDispatchDepth;
volatile ULONG KiExceptionsDispatched; // diagnostics

// Walk the registration chain; TRUE when a handler asked to continue execution.
static BOOLEAN RtlpDispatchException(PEXCEPTION_RECORD Record, PCONTEXT Context) {
    KiExceptionsDispatched = KiExceptionsDispatched + 1;
    for (EXCEPTION_REGISTRATION *r = KiExceptionList(); r && r != EXCEPTION_CHAIN_END; r = r->Next) {
        if (((ULONG)r & 3) || (ULONG)r < 0x10000)
            break; // corrupt chain
        PVOID dispatcherContext = nullptr;
        ULONG disposition = r->Handler(Record, r, Context, &dispatcherContext);
        if (disposition == ExceptionContinueExecution) {
            if (Record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                EXCEPTION_RECORD nested = {STATUS_NONCONTINUABLE_EXCEPTION_, EXCEPTION_NONCONTINUABLE, Record,
                                           Record->ExceptionAddress, 0, {}};
                KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, nested.ExceptionCode, (ULONG)nested.ExceptionAddress,
                             Record->ExceptionCode, 0);
            }
            return TRUE;
        }
        if (disposition == ExceptionNestedException) {
            Record->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            continue;
        }
        if (disposition != ExceptionContinueSearch)
            KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, STATUS_INVALID_DISPOSITION_, (ULONG)r->Handler, disposition,
                         Record->ExceptionCode);
    }
    return FALSE;
}

extern volatile ULONG KiLastExceptionVector, KiLastExceptionCode, KiLastExceptionEip;

[[noreturn]] static void KiUnhandled(PEXCEPTION_RECORD Record, ULONG vector, ULONG errorCode) {
    KiLastExceptionVector = vector;
    KiLastExceptionCode = errorCode;
    KiLastExceptionEip = (ULONG)Record->ExceptionAddress;
    KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, Record->ExceptionCode, (ULONG)Record->ExceptionAddress,
                 Record->ExceptionInformation[0], Record->ExceptionInformation[1]);
}

static ULONG KiFloatStatusCode(UCHAR vector) {
    if (vector == 19)
        return STATUS_FLOAT_MULTIPLE_TRAPS_;
    USHORT sw = 0;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    __asm__ volatile("fnclex");
    if (sw & 0x40)
        return STATUS_FLOAT_STACK_CHECK_;
    if (sw & 1)
        return STATUS_FLOAT_INVALID_OPERATION_;
    if (sw & 4)
        return STATUS_FLOAT_DIVIDE_BY_ZERO_;
    if (sw & 2)
        return STATUS_FLOAT_DENORMAL_OPERAND_;
    if (sw & 8)
        return STATUS_FLOAT_OVERFLOW_;
    if (sw & 0x10)
        return STATUS_FLOAT_UNDERFLOW_;
    return STATUS_FLOAT_INEXACT_RESULT_;
}

#if XK_NETLOG
bool KiDebugTrap(ULONG *eip, ULONG *eflags, ULONG edi, ULONG ecx, ULONG ebp); // ke/dbgstep.cpp
#endif

extern "C" void KiDispatchTrap(KTRAP_FRAME_XK *Frame) {
#if XK_NETLOG
    if (Frame->Vector == 1 && KiDebugTrap(&Frame->Eip, &Frame->EFlags, Frame->Edi, Frame->Ecx, Frame->Ebp))
        return;
#endif
    EXCEPTION_RECORD record = {};
    record.ExceptionAddress = (PVOID)Frame->Eip;
    ULONG vector = Frame->Vector;
    switch (vector) {
    case 0: record.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO_; break;
    case 1: record.ExceptionCode = STATUS_SINGLE_STEP_; break;
    case 3: // int3: report the breakpoint instruction itself
        record.ExceptionCode = STATUS_BREAKPOINT_;
        Frame->Eip--;
        record.ExceptionAddress = (PVOID)Frame->Eip;
        record.NumberParameters = 1;
        break;
    case 4: record.ExceptionCode = STATUS_INTEGER_OVERFLOW_; break;
    case 5: record.ExceptionCode = STATUS_ARRAY_BOUNDS_EXCEEDED_; break;
    case 6: record.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION_; break;
    case 13: // general protection: privileged instruction or a bad access
        record.ExceptionCode = STATUS_ACCESS_VIOLATION_;
        record.NumberParameters = 2;
        record.ExceptionInformation[1] = 0xFFFFFFFF;
        break;
    case 14: {
        ULONG cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        record.ExceptionCode = STATUS_ACCESS_VIOLATION_;
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = (Frame->ErrorCode >> 1) & 1; // 1 = write
        record.ExceptionInformation[1] = cr2;
        break;
    }
    case 16:
    case 19: record.ExceptionCode = KiFloatStatusCode((UCHAR)vector); break;
    case 17: record.ExceptionCode = STATUS_DATATYPE_MISALIGNMENT_; break;
    default:
        KiUnexpectedException(vector, Frame->ErrorCode, Frame->Eip); // machine check, reserved vectors
    }
    KiDispatchDepth = KiDispatchDepth + 1;
    if (KiDispatchDepth > 8)
        KiUnhandled(&record, vector, Frame->ErrorCode);
    if (Frame->EFlags & 0x200)
        sti(); // handlers run with interrupts as the faulting code had them

    CONTEXT context;
    context.ContextFlags = 0x10007;
    context.Edi = Frame->Edi, context.Esi = Frame->Esi, context.Ebx = Frame->Ebx, context.Edx = Frame->Edx;
    context.Ecx = Frame->Ecx, context.Eax = Frame->Eax, context.Ebp = Frame->Ebp, context.Eip = Frame->Eip;
    context.SegCs = Frame->SegCs, context.EFlags = Frame->EFlags, context.Esp = (ULONG)(&Frame->EFlags + 1);
    context.SegSs = 0x10;
    BOOLEAN handled = RtlpDispatchException(&record, &context);
    KiDispatchDepth = KiDispatchDepth - 1;
    if (!handled) {
        XK_SERIAL("*** unhandled exception %#lx at %#lx (vector %lu, error %#lx, address %#lx)\n", record.ExceptionCode,
                  Frame->Eip, vector, Frame->ErrorCode, record.ExceptionInformation[1]);
        XK_SERIAL("    eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx efl %08lx\n",
                  Frame->Eax, Frame->Ebx, Frame->Ecx, Frame->Edx, Frame->Esi, Frame->Edi, Frame->Ebp, context.Esp,
                  Frame->EFlags);
        [[maybe_unused]] auto code = (const UCHAR *)Frame->Eip;
        ULONG cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        XK_SERIAL("    code %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; cr0 %08lx cr4 %08lx\n", code[0],
                  code[1], code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11],
                  cr0, cr4);
        [[maybe_unused]] auto stack = (const ULONG *)context.Esp;
        XK_SERIAL("    stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", stack[0], stack[1], stack[2], stack[3],
                  stack[4], stack[5], stack[6], stack[7]);
        KiUnhandled(&record, vector, Frame->ErrorCode);
    }
    cli();
    if (context.Esp != (ULONG)(&Frame->EFlags + 1))
        KiContinueContext(&context);
    Frame->Edi = context.Edi, Frame->Esi = context.Esi, Frame->Ebx = context.Ebx, Frame->Edx = context.Edx;
    Frame->Ecx = context.Ecx, Frame->Eax = context.Eax, Frame->Ebp = context.Ebp, Frame->Eip = context.Eip;
    Frame->EFlags = context.EFlags;
}

// Called by RtlRaiseException with the caller's context.
extern "C" [[noreturn]] void KiRaiseException(PEXCEPTION_RECORD Record, PCONTEXT Context) {
    Record->ExceptionAddress = (PVOID)Context->Eip;
    if (!RtlpDispatchException(Record, Context))
        KiUnhandled(Record, 0xFF, 0);
    KiContinueContext(Context);
}

// Called by RtlUnwind: run the handlers between the top of the chain and TargetFrame with EXCEPTION_UNWINDING,
// unlink them, then resume at TargetIp (or the RtlUnwind caller) with Eax = ReturnValue.
extern "C" [[noreturn]] void KiUnwind(EXCEPTION_REGISTRATION *TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD Record,
                                      PVOID ReturnValue, PCONTEXT Context) {
    EXCEPTION_RECORD local;
    if (!Record) {
        local = {STATUS_UNWIND_, 0, nullptr, (PVOID)Context->Eip, 0, {}};
        Record = &local;
    }
    Record->ExceptionFlags |= EXCEPTION_UNWINDING | (TargetFrame ? 0 : EXCEPTION_EXIT_UNWIND);
    EXCEPTION_REGISTRATION *r = KiExceptionList();
    while (r && r != EXCEPTION_CHAIN_END && r != TargetFrame) {
        if (((ULONG)r & 3) || (ULONG)r < 0x10000)
            break;
        PVOID dispatcherContext = nullptr;
        ULONG disposition = r->Handler(Record, r, Context, &dispatcherContext);
        if (disposition != ExceptionContinueSearch && disposition != ExceptionCollidedUnwind)
            KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, STATUS_INVALID_DISPOSITION_, (ULONG)r->Handler, disposition,
                         Record->ExceptionCode);
        r = r->Next;
        KiExceptionList() = r;
    }
    if (TargetIp)
        Context->Eip = (ULONG)TargetIp;
    Context->Eax = (ULONG)ReturnValue;
    KiContinueContext(Context);
}

XBAPI void NTAPI RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

XBAPI void NTAPI RtlRaiseStatus(NTSTATUS Status) {
    EXCEPTION_RECORD r = {(ULONG)Status, EXCEPTION_NONCONTINUABLE, nullptr, nullptr, 0, {}};
    RtlRaiseException(&r);
}

XBAPI void NTAPI ExRaiseException(PEXCEPTION_RECORD ExceptionRecord) { RtlRaiseException(ExceptionRecord); }
XBAPI void NTAPI ExRaiseStatus(NTSTATUS Status) { RtlRaiseStatus(Status); }

// --- Stack walks over the EBP chain, bounded by the current thread's stack.

static bool KiOnStack(ULONG p) {
    PKTHREAD t = KeGetCurrentThread();
    return p >= (ULONG)t->StackLimit && p + 8 <= (ULONG)t->StackBase;
}

XBAPI ULONG NTAPI RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags) {
    ULONG ebp = (ULONG)__builtin_frame_address(0), n = 0;
    while (n < Count && KiOnStack(ebp)) {
        ULONG next = *(PULONG)ebp, ret = *(PULONG)(ebp + 4);
        if (!ret)
            break;
        Callers[n++] = (PVOID)ret;
        if (next <= ebp)
            break;
        ebp = next;
    }
    return n;
}

XBAPI USHORT NTAPI RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture, PVOID *BackTrace,
                                            PULONG BackTraceHash) {
    PVOID frames[64];
    ULONG total = RtlWalkFrameChain(frames, 64, 0);
    ULONG first = FramesToSkip + 1; // not this function's caller frame of the walker itself
    ULONG n = 0, hash = 0;
    for (ULONG i = first; i < total && n < FramesToCapture; i++, n++) {
        BackTrace[n] = frames[i];
        hash += (ULONG)frames[i];
    }
    if (BackTraceHash)
        *BackTraceHash = hash;
    return (USHORT)n;
}

XBAPI void NTAPI RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller) {
    PVOID frames[3] = {};
    ULONG n = RtlWalkFrameChain(frames, 3, 0);
    *CallersAddress = n > 1 ? frames[1] : nullptr;
    *CallersCaller = n > 2 ? frames[2] : nullptr;
}
