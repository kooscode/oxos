// oxos_gdb.xos: the GDB debugger as a kernel extension (spec/oxos_internal/51 §3): gdb on a PC debugs the running title, and the kernel under it, on any
// OXOS kernel with the debug port. The GDB stub (shared/gdb_stub, the one GDB=1 kernels build in) attaches to the kernel's debug port before every title and
// talks over the transport another extension registered with the port (a network one), else over the console's serial port. Its session (gdb attached,
// breakpoints, watchpoints) is in the state page, so gdb follows the console from one title into the next; its run loop starts the XBEs gdb asks for.
#include <oxos_sdk.h>
#include <gdb_stub/gdb_stub.h>
#include <gdb_stub/serial_transport.h>

using namespace oxos;

static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
static void ExtensionTerminate(void);

OXOS_DEFINE_EXTENSION(.Name = "oxos_gdb", .Version = "1.0.0", .Start = ExtensionStart, .RunLoop = ExtensionRunLoop, .Terminate = ExtensionTerminate);

static_assert(sizeof(gdb_stub::SESSION) <= 4096, "the session lives in the state page");

static const OXOS_DEBUG_PORT *Port; // the kernel's debug port, from this launch's start

// Whether the setting Key says yes.
static bool IsSettingOn(const char *Key) {
    const char *value = oxos_setting(Key);
    return value && strcasecmp(value, "yes") == 0;
}

static BOOLEAN ExtensionStart(void) {
    Port = oxos_debug_port();
    if (!Port || Port->Size < sizeof(OXOS_DEBUG_PORT)) {
        oxos_log("This kernel's debug port is too old.");
        return FALSE;
    }
    auto &session = *static_cast<gdb_stub::SESSION *>(oxos_state());
    gdb_stub::serial_transport::Start();
    gdb_stub::Start(session, *Port, gdb_stub::serial_transport::Transport());
    session.WaitAtTitle = IsSettingOn("wait_for_gdb");
    const char *title = oxos_setting("title");
    if (title && title[0] && !session.TitleFilter[0])
        strlcpy(session.TitleFilter, title, sizeof(session.TitleFilter));
    const char *symbols = oxos_setting("title_symbols");
    if (symbols && symbols[0] && !session.TitleSymbols[0])
        strlcpy(session.TitleSymbols, symbols, sizeof(session.TitleSymbols));
    if (!NT_SUCCESS(Port->AttachDebugger(&gdb_stub::Debugger()))) {
        gdb_stub::PrepareForUnload();
        oxos_log("Another debugger is attached (a GDB=1 kernel's own).");
        return FALSE;
    }
    gdb_stub::AnnounceReady();
    const OXOS_DEBUG_TRANSPORT *transport = Port->Transport();
    oxos_log("Attached; gdb talks over the %s transport.", transport ? transport->Name : "serial");
    return TRUE;
}

// While the title runs: the XBE gdb asked for (vRun, `monitor launch`) starts from here, at passive level.
static void ExtensionRunLoop(void) {
    const char *path = gdb_stub::TakeLaunchRequest();
    if (path) {
        NTSTATUS status = oxos_title_launch(path); // returns only when the launch failed
        oxos_log("gdb asked for %s; the launch failed: %#lx", path, (unsigned long)status);
    }
}

// The title ends: the stub's int3s and debug registers come out of memory before its code goes away; the session keeps them for the next title.
static void ExtensionTerminate(void) {
    Port->DetachDebugger(&gdb_stub::Debugger());
    gdb_stub::PrepareForUnload();
}
