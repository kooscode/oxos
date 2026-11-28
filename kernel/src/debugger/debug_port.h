// The kernel's debug port (debug_port.cpp, spec/oxos_internal/51 §2): one debugger at a time gets the traps, the clock's break-in poll, the bug check, the
// title's start and the titles' debug text; a debug transport an extension registers carries its bytes. Resident in every kernel: the hooks below are the
// only places the kernel calls a debugger from.
#pragma once
#include <core/core.h>
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#include <oxos_sdk.h>

namespace xk::debugger {

// The attached debugger (an extension's, or a GDB=1 kernel's own), or null.
extern const OXOS_DEBUGGER *volatile AttachedDebugger;

// The port's table: exported as oxos_debug_port, and what a GDB=1 kernel's own stub uses.
const OXOS_DEBUG_PORT *GetDebugPort();
// A GDB=1 kernel's own stub (gdb_stub.cpp): attached for good, so the extensions' end does not detach it.
void AttachKernelDebugger(const OXOS_DEBUGGER *Debugger);
// The extensions end (the title's quick reboot, extensions/extension_runtime.cpp): their debugger and transport are forgotten, as their code goes away.
void ForgetExtensionDebugger();

// --- The hooks: each returns at once while no debugger is attached

// KiDispatchTrap's first step: true when the trap was the debugger's (the frame resumes).
inline bool HandleTrap(core::KTRAP_FRAME_XK *Frame) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    return debugger && debugger->HandleTrap(reinterpret_cast<OXOS_TRAP_FRAME *>(Frame));
}

// An exception nothing handled: true when the debugger resumes the frame (else the bug check follows).
inline bool StopOnUnhandledException(core::KTRAP_FRAME_XK *Frame) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    return debugger && debugger->StopOnUnhandledException(reinterpret_cast<OXOS_TRAP_FRAME *>(Frame));
}

// KeBugCheckEx, before its screen.
inline void StopOnBugCheck(ULONG Code) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    if (debugger)
        debugger->StopOnBugCheck(Code);
}

// After the bug-check screen, for ever: the debugger serves its host (attach after a crash), or the processor halts.
[[noreturn]] void WaitAfterBugCheck();

// The clock interrupt: the interrupted eip, cs, eflags.
inline void PollBreakIn(PULONG InterruptedFrame) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    if (debugger)
        debugger->PollBreakIn(InterruptedFrame);
}

// The title's XBE (Path) is in memory, before its entry point.
inline void TitleLoaded(const char *Path) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    if (debugger)
        debugger->TitleLoaded(Path);
}

// Debug text (DbgPrint, OutputDebugString, a GDB=1 kernel's log): true when the debugger took it.
inline bool PrintDebugText(const char *Text, ULONG Length) {
    const OXOS_DEBUGGER *debugger = AttachedDebugger;
    return debugger && debugger->PrintDebugText(Text, Length);
}

} // namespace xk::debugger
