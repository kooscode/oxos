// The GDB remote stub (spec/oxos_internal/51): gdb debugs the kernel and the running title through the kernel's debug port (oxos_sdk.h §9). One target,
// one session, all-stop on the one processor: a stop runs inside the trap with interrupts off and serves gdb's packets until gdb resumes or detaches. Built
// into GDB=1 kernels (kernel/src/debugger/gdb_stub.cpp) and into oxos_gdb.xos (kernel/extensions/oxos_gdb), the same code: everything it needs from the
// kernel goes through OXOS_DEBUG_PORT, and every byte through an OXOS_DEBUG_TRANSPORT.
#pragma once
#if defined(OXOS_KERNEL)
#include <runtime/types.h> // first: its NULL is the kernel's
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#endif
#include <c_library.h>
#include <oxos_sdk.h>

namespace oxos::gdb_stub {

constexpr uint32_t MAX_BREAKPOINTS = 64;       // software breakpoints gdb can have set at once
constexpr uint32_t SESSION_MAGIC = 0x32424447; // "GDB2": SESSION::Magic of a session that has started (with this layout)
constexpr uint32_t PATH_LENGTH = 360;          // an XBE's \Device\ path, as the title loader takes it

// A software breakpoint: an int3 in place of the byte at Address.
struct BREAKPOINT {
    uint32_t Address; // 0 = a free slot
    uint8_t Saved;    // the byte the int3 replaced
    bool Inserted;    // the int3 is in memory (false: pending, the page or the title is not there yet)
};

// One of the processor's debug registers DR0-DR3, for a hardware breakpoint or a part of a watchpoint (gdb's Z1-Z4): a watchpoint the processor cannot
// cover with one register (unaligned, or longer than 4 bytes) takes several, each an aligned 1, 2 or 4 bytes of it.
struct WATCHPOINT {
    uint32_t Address;      // what this register watches; 0 = free
    uint8_t Kind;          // gdb's Z kind: 1 execute, 2 write, 3 read, 4 access
    uint8_t Length;        // 1, 2 or 4 bytes
    uint32_t WatchAddress; // the watchpoint gdb set, which this register is part of: its address and length
    uint32_t WatchLength;
};

// What lasts from one launch to the next, so gdb follows the console from one title into the next: the caller keeps it (a GDB=1 kernel in its persistent
// data, oxos_gdb.xos in its state page). All zero is a new session.
struct SESSION {
    uint32_t Magic;       // SESSION_MAGIC once the stub has started with it
    bool Attached;        // gdb has talked to the stub and not detached
    bool WaitingForStop;  // gdb resumed the target and waits for a stop reply
    bool Stepping;        // the last resume was a single step: the next single-step trap ends it
    bool WaitAtTitle;     // TitleLoaded stops (as at a breakpoint) until gdb has attached: the title's first instruction can be debugged
    bool LaunchRequested; // gdb asked for LaunchPath to start (vRun, `monitor launch`): TakeLaunchRequest hands it on once the target runs
    BREAKPOINT Breakpoints[MAX_BREAKPOINTS];
    WATCHPOINT Watchpoints[4];
    char TitleFilter[64];         // `monitor title NAME`: the title gdb debugs (part of its path); its breakpoints go only into it
    char TitleSymbols[64];        // `monitor symbols FILE`: the title's symbol file as the library list names it (empty: the title is gdb's own program)
    char TitlePath[PATH_LENGTH];  // the title gdb debugs, as the kernel loaded it (TitleLoaded): what an empty vRun starts again
    char LaunchPath[PATH_LENGTH]; // the XBE gdb asked for
};

// Starts the stub on Session, with the kernel's Port; OwnTransport carries the bytes while no transport is registered with the port. Breakpoints in titles
// wait for the next title (TitleLoaded); the others and the debug registers go in now. Call it before attaching Debugger().
void Start(SESSION &Session, const OXOS_DEBUG_PORT &Port, const OXOS_DEBUG_TRANSPORT &OwnTransport);
// The stub's functions, for the port's AttachDebugger.
const OXOS_DEBUGGER &Debugger();
// Stops here, as at a breakpoint, until gdb resumes the target: a cold boot waiting for gdb (GDB_WAIT=1). Announcement goes to the host as plain text first
// when gdb is not attached (the bridge shows it as a log line), unless null.
void WaitForDebugger(const char *Announcement);
// Tells the host the stub now reads its transport ("gdb: stub ready", the line tools/gdb_bridge.py waits for before it passes gdb's bytes on); nothing
// while gdb is attached.
void AnnounceReady();
// The XBE gdb asked to start (vRun, `monitor launch`), once; null when there is none. The caller starts it at passive level, while the target runs
// (oxos_gdb.xos's run loop, a GDB=1 kernel's own thread): titles::SwitchTitle, oxos_title_launch.
const char *TakeLaunchRequest();
// Before the stub's code goes away (the extension's end): every int3 out of memory and the debug registers off; the session keeps them, pending, for the
// next Start.
void PrepareForUnload();

} // namespace oxos::gdb_stub
