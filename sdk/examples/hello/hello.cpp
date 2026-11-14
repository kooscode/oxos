// SPDX-License-Identifier: MIT
//
// hello.xos: the smallest complete OXOS kernel extension, written to be read. It shows every part an extension can have (spec/oxos_internal/34):
//
//   Load      runs before each title starts: read settings, use the state page, hook kernel functions, register
//             for events. It must return quickly; the title waits for it.
//   RunLoop   called again and again while the title runs (about every millisecond, on the kernel's loop thread); it must return quickly.
//   Unload    runs when the title starts another title or the dashboard (a quick reboot).
//   a hook    the title's calls of NtOpenFile come here first; this one counts them and passes each one on.
//   an event  called when a title has been loaded, before it runs: this one logs the title's name and ID.
//
// What it prints goes to the debug serial port and to C:\OXOS\log\extensions.log, e.g.:
//   [hello] Hello from OXOS 0.4.7! Launch 2 since the cold boot.
//   [hello] The last title opened 127 files.
//   [hello] Title 4D530004 "Halo" is starting.
//
// Remember that the kernel reloads every extension from the disk at each launch: global variables start again from their initial values every time. Only the
// state page (4 KB) keeps its contents from one launch to the next.
#include <oxos_sdk.h>

namespace {

// --- What this extension keeps from one launch to the next, in its state page

constexpr ULONG HELLO_MAGIC = 0x4F4C4548; // "HELO": marks a state page this extension has set up

struct HELLO_STATE {
    ULONG Magic;     // HELLO_MAGIC once the page is set up
    ULONG Launches;  // launches since the cold boot
    ULONG OpenCalls; // NtOpenFile calls of the running title (reset at each launch)
};

// --- Globals: set in Load at every launch

HELLO_STATE *State;     // the state page, as this extension's own structure
const char *Greeting;   // the "greeting" setting from oxos_extensions.ini
ULONG HeartbeatSeconds; // the "heartbeat" setting: RunLoop logs every so many seconds (0: never)
ULONG LastHeartbeat;    // KeTickCount (milliseconds) at Load or at the last heartbeat
ULONG SecondsRunning;   // seconds of this title so far, in heartbeats

// The kernel's NtOpenFile, or the next extension's hook of it: filled in by the kernel once every extension's Load has returned (the extensions listed after
// this one in oxos_extensions.ini hook below it).
decltype(&oxos::NtOpenFile) NextNtOpenFile;

// --- A hook: the title's NtOpenFile calls come here first

// It has exactly the kernel function's arguments, result and calling convention (NTAPI = stdcall). It may run on several of the title's threads at once, so the
// count is updated atomically.
NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                              ULONG ShareAccess, ULONG OpenOptions) {
    __atomic_add_fetch(&State->OpenCalls, 1, __ATOMIC_RELAXED);
    // Pass the call on unchanged: the title gets the kernel's answer. A hook could also change the arguments or the result, or answer the call itself without
    // passing it on.
    return NextNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
}

// --- An event: a title has been loaded and is about to run

void NTAPI OnTitleLoaded(OXOS_CONTEXT *Context, PVOID CallbackContext, PVOID Data) {
    const auto *title = static_cast<const OXOS_TITLE_LOADED *>(Data);
    // The name in the title's certificate is UTF-16; this keeps the plain ASCII characters.
    char name[41];
    ULONG length = 0;
    for (; length < 40 && title->TitleName[length]; length++) {
        USHORT c = title->TitleName[length];
        name[length] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    name[length] = 0;
    oxos_log(Context, "Title %08lX \"%s\" is starting.", title->TitleId, name);
}

// --- The lifecycle

// Before each title. Context describes this launch and this extension; Context->Api is only valid here.
NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    // The state page: zeroed after a cold boot (StateIsNew), else as the last launch left it. Check the magic too: an older build of this extension may have
    // written the page with another layout.
    State = static_cast<HELLO_STATE *>(Context->State);
    if (!State)
        return STATUS_INSUFFICIENT_RESOURCES; // no memory for it: this extension is unloaded, the others go on
    if (Context->StateIsNew || State->Magic != HELLO_MAGIC)
        *State = {HELLO_MAGIC, 0, 0};
    State->Launches++;

    // Where oxos_log writes: both C:\OXOS\log\extensions.log and the debug serial port (which only debug kernels have) unless an extension chooses one of them;
    // this one keeps both.
    oxos_set_log_target(Context, OXOS_LOG_SERIAL | OXOS_LOG_FILE);

    // Settings: the other keys of this extension's section in oxos_extensions.ini, as text (or null when missing).
    Greeting = oxos_get_setting(Context, "greeting");
    if (!Greeting)
        Greeting = "Hello";
    HeartbeatSeconds = 0;
    const char *heartbeat = oxos_get_setting(Context, "heartbeat");
    for (; heartbeat && *heartbeat >= '0' && *heartbeat <= '9'; heartbeat++)
        HeartbeatSeconds = HeartbeatSeconds * 10 + static_cast<ULONG>(*heartbeat - '0');

    oxos_log(Context, "%s from OXOS %s! Launch %lu since the cold boot.", Greeting, Context->KernelVersion, State->Launches);
    if (State->Launches > 1)
        oxos_log(Context, "The last title opened %lu files.", State->OpenCalls);
    State->OpenCalls = 0;
    LastHeartbeat = oxos::KeTickCount;
    SecondsRunning = 0;

    // Be called when a title has been loaded.
    NTSTATUS status = Context->Api->RegisterEvent(Context, OXOS_EVENT_TITLE_LOADED, reinterpret_cast<PVOID>(OnTitleLoaded), nullptr);
    if (!NT_SUCCESS(status))
        return status;
    // Hook NtOpenFile for the title. NextNtOpenFile is filled in after every extension's Load has returned.
    return Context->Api->HookExport(Context, OXOS_ORDINAL_NtOpenFile, reinterpret_cast<PVOID>(HookNtOpenFile), reinterpret_cast<PVOID *>(&NextNtOpenFile));
}

// Called about every millisecond while the title runs, after the RunLoop of each extension listed before this one: the place for work Load must not do, a
// little at a time. It must return quickly: every extension's RunLoop waits for it, and one that takes over 500 ms is logged in C:\OXOS\log\extensions.log.
// Here: a heartbeat every HeartbeatSeconds, timed with the kernel's millisecond count (it never waits itself).
void NTAPI RunLoop(OXOS_CONTEXT *Context) {
    if (!HeartbeatSeconds || oxos::KeTickCount - LastHeartbeat < HeartbeatSeconds * 1000)
        return;
    LastHeartbeat += HeartbeatSeconds * 1000;
    SecondsRunning += HeartbeatSeconds;
    oxos_log(Context, "%lu s into this title, %lu files opened.", SecondsRunning, State->OpenCalls);
}

// The title is starting another title or the dashboard: close files or connections here. The state page needs no saving, it is kept as it is.
void NTAPI Unload(OXOS_CONTEXT *Context) {
    oxos_log(Context, "Goodbye: %lu files opened by this title.", State->OpenCalls);
}

} // namespace

// --- The descriptor: what the kernel reads first

// Name is this extension's own name: it decides which state page it gets, so keep it the same across versions. Version is its own version, shown in the
// kernel's log when it loads.
OXOS_DEFINE_EXTENSION(.Name = "hello", .Version = "1.0.0", .Load = Load, .RunLoop = RunLoop, .Unload = Unload);
