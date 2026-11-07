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
  as (record, frame, context, &dispatcherContext), **under a guard frame** (0x80022934): for the call, the
  dispatcher links a registration {Next = FS:[0], guard handler 0x8002297E, the frame being handled} on top of the
  chain, and unlinks the top registration afterwards (FS:[0] ← FS:[0]→Next).
  - The **exception guard** answers ContinueSearch to an unwind (flags & 6) and NestedException to an exception,
    with dispatcherContext = the guarded frame: an exception raised inside a handler finds it.
  - ContinueExecution resumes with the (possibly edited) context. A noncontinuable record →
    STATUS_NONCONTINUABLE_EXCEPTION.
  - ContinueSearch moves on. NestedException sets EXCEPTION_NESTED_CALL (0x10) and remembers the highest
    dispatcherContext frame; when the walk has called that frame's handler, the flag is cleared again.
  - anything else → STATUS_INVALID_DISPOSITION.
  - Titles depend on the guard frame: a C++ runtime that unwinds from inside its handler (MSVC's nested-frame
    unwind) saves FS:[0] before RtlUnwind and links it back on top afterwards, with Next = the unwind's target. The
    saved registration is the dispatcher's guard; without one it is the target frame itself, which then points to
    itself, and the next exception walks that loop for ever (a test title stuck on a loading screen,
    spec/oxos_internal/25 §6).
- **RtlRaiseException(record):** dispatch with the caller's context (Eip = return address); a continued
  exception returns to the caller. RtlRaiseStatus / ExRaiseStatus raise {status, NONCONTINUABLE};
  ExRaiseException = RtlRaiseException.
- **RtlUnwind(targetFrame, targetIp, record, returnValue):**
  - call each handler above targetFrame with EXCEPTION_UNWINDING (plus EXIT_UNWIND when targetFrame is NULL),
    unlinking it (FS:[0] ← frame→Next) after the call. Each call runs under an **unwind guard** (0x8002293C, guard
    handler 0x800229A5): it answers ContinueSearch to an exception and CollidedUnwind to an unwind, with
    dispatcherContext = the guarded frame; on CollidedUnwind (an unwind started inside an unwind handler has
    already run the handlers up to there) the walk carries on from that frame.
  - then continue in the caller's frame at targetIp (or the return address) with Eax = returnValue. A NULL record
    gets STATUS_UNWIND.
- **RtlCaptureContext:** the state at the caller's return point.
- **RtlWalkFrameChain / RtlCaptureStackBackTrace / RtlGetCallersAddress:** follow the EBP chain inside the current
  thread's stack (the kernel is built with frame pointers).

## Implementation notes
- A context with a different stack pointer is resumed by building {eax, ebx, eip, cs, eflags} below the target
  ESP and executing `iret` (ring 0, no stack switch).
- Exception depth is capped at 8 (bug check beyond). The reference's stack-limit checks on each frame
  (STATUS_BAD_STACK) and STATUS_INVALID_UNWIND_TARGET for a target below the top frame are not implemented: a
  misaligned or low frame ends the walk.
