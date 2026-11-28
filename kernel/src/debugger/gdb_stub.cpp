// A GDB=1 kernel's own debugger (gdb_stub.h, spec/oxos_internal/51 §5): the shared GDB stub (shared/gdb_stub/gdb_stub.cpp) with the shared serial transport,
// its session kept across quick reboots, attached to the kernel's debug port for good, and a thread that starts the XBEs gdb asks for.
#include <debugger/gdb_stub.h>

#if XK_GDB
#include <debugger/debug_port.h>
#include <diagnostics/diagnostics.h>
#include <gdb_stub/gdb_stub.h>
#include <gdb_stub/serial_transport.h>
#include <runtime/runtime.h>
#include <titles/titles.h>

namespace xk::debugger {

XK_PERSIST static oxos::gdb_stub::SESSION Session; // breakpoints and watchpoints, kept across quick reboots so gdb follows the console into the next title

void InitGdbStub(bool QuickBoot) {
    if (!QuickBoot)
        memset(&Session, 0, sizeof(Session));
    const OXOS_DEBUG_PORT &port = *GetDebugPort();
    oxos::gdb_stub::serial_transport::Start();
    oxos::gdb_stub::Start(Session, port, oxos::gdb_stub::serial_transport::Transport());
    AttachKernelDebugger(&oxos::gdb_stub::Debugger());
    XK_SERIAL("gdb: stub ready\n"); // the bridge forwards gdb's bytes from here on
    if (XK_GDB_WAIT && !QuickBoot) {
        XK_SERIAL("gdb: waiting for gdb\n");
        oxos::gdb_stub::WaitForDebugger(nullptr);
    }
}

// Every 20 ms, at passive level: the XBE gdb asked for, started as an extension's oxos_title_launch would.
static void NTAPI RunGdbLauncher(PVOID) {
    LARGE_INTEGER interval;
    interval.QuadPart = -20 * 10000;
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        const char *path = oxos::gdb_stub::TakeLaunchRequest();
        if (path)
            XK_SERIAL("gdb: launching %s: %#lx\n", path, titles::SwitchTitle(path)); // returns only when the launch failed
    }
}

void StartGdbLauncher() {
    core::CreateSystemThread(RunGdbLauncher, nullptr, 0, 8);
}

} // namespace xk::debugger
#endif
