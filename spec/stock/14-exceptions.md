# Spec 14 — Structured exception handling

Behaviour follows the documented x86 NT SEH model, which titles' compiler runtimes (`_except_handler3`, C++
`__CxxFrameHandler`) depend on. Xbox layouts: CONTEXT (0x238) = ContextFlags, FLOATING_SAVE_AREA (0x204, FXSAVE
format + Cr0NpxState), Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs; EXCEPTION_RECORD (0x50).
**Status: implemented (milestone 5b).**

## Trap translation
| Vector | Code |
|---|---|
| 0 | STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094 |
| 1 | STATUS_SINGLE_STEP 0x80000004 |
| 3 | STATUS_BREAKPOINT 0x80000003 (address = the int3, Eip backed up by one) |
| 4 | STATUS_INTEGER_OVERFLOW 0xC0000095 |
| 5 | STATUS_ARRAY_BOUNDS_EXCEEDED 0xC000008C |
| 6 | STATUS_ILLEGAL_INSTRUCTION 0xC000001D |
| 13 | STATUS_ACCESS_VIOLATION, parameters {0, 0xFFFFFFFF} |
| 14 | STATUS_ACCESS_VIOLATION, parameters {write ? 1 : 0, CR2} |
| 16 | x87: from the status word (stack check, invalid, divide by zero, denormal, overflow, underflow, inexact) |
| 17 | STATUS_DATATYPE_MISALIGNMENT 0x80000002 |
| 19 | STATUS_FLOAT_MULTIPLE_TRAPS 0xC00002B5 |
| 0x2D | debug service (0x8001B424): STATUS_BREAKPOINT at the int3 that follows `int 2Dh`, parameters {EAX (service), ECX, EDX} |

**Debug services** (`mov eax, service; int 2Dh; int 3`, the way titles' DbgPrint / OutputDebugString reach the
kernel). With no kernel debugger the breakpoint goes first to 0x80018B06, which takes it when parameter 0 is 1, 3,
4, 7, 8 or 0xB: a print (1, and 0xB) hands parameter 1, a counted STRING, to the serial-output routine 0x80018AC0
and sets the context's Eax to 0; in every taken case Eip is moved past the int3 and execution continues. Any other
service, and a plain int3 (vector 3, parameter 0 = 0), is dispatched as an ordinary exception: unhandled, it ends
in bug check 0x1E.
Handlers run with interrupts enabled if the faulting code had them enabled. NMI and double fault are task gates
(halt). An exception that no handler takes → **bug check 0x1E** (KMODE_EXCEPTION_NOT_HANDLED: code, address,
parameters).

## Dispatch
- **Walk the registration chain at FS:[0]:** {Next, Handler}, ending at 0xFFFFFFFF. Each handler is called cdecl
  as (record, frame, context, &dispatcherContext).
  - ContinueExecution resumes with the (possibly edited) context. A noncontinuable record →
    STATUS_NONCONTINUABLE_EXCEPTION.
  - ContinueSearch moves on; NestedException marks the record nested.
  - anything else → STATUS_INVALID_DISPOSITION.
- **RtlRaiseException(record):** dispatch with the caller's context (Eip = return address); a continued
  exception returns to the caller. RtlRaiseStatus / ExRaiseStatus raise {status, NONCONTINUABLE};
  ExRaiseException = RtlRaiseException.
- **RtlUnwind(targetFrame, targetIp, record, returnValue):**
  - call each handler above targetFrame with EXCEPTION_UNWINDING (plus EXIT_UNWIND when targetFrame is NULL),
    unlinking it
  - then continue in the caller's frame at targetIp (or the return address) with Eax = returnValue. A NULL record
    gets STATUS_UNWIND.
- **RtlCaptureContext:** the state at the caller's return point.
- **RtlWalkFrameChain / RtlCaptureStackBackTrace / RtlGetCallersAddress:** follow the EBP chain inside the current
  thread's stack (the kernel is built with frame pointers).

## Implementation notes
- A context with a different stack pointer is resumed by building {eax, ebx, eip, cs, eflags} below the target
  ESP and executing `iret` (ring 0, no stack switch).
- Nested exception records and the dispatcher's own guard frame are simplified: exception depth is capped at 8
  (bug check beyond).
