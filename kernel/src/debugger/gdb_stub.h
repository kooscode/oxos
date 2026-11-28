// A GDB=1 kernel's own debugger (gdb_stub.cpp, spec/oxos_internal/51 §5): the shared GDB stub (shared/gdb_stub) on the debug serial port, attached to the
// debug port from the boot on, so gdb debugs the kernel's start too.
#pragma once
#include <core/core.h>

#if XK_GDB
namespace xk::debugger {
void InitGdbStub(bool QuickBoot); // after the IDT: the stub starts (a cold boot with a new session), attaches; a cold boot waits for gdb with GDB_WAIT=1
void StartGdbLauncher();          // once threads can start: the thread that starts the XBEs gdb asks for (vRun, `monitor launch`)
} // namespace xk::debugger
#endif
