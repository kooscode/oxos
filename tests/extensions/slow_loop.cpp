// Test extension (spec/oxos_internal/34 §9.3), listed last: its first RunLoop call after the cold boot takes 600 ms, so the kernel must write "extension
// slow_loop delayed extension loop by N ms" to C:\OXOS\log\extensions.log, and the loop must go on calling every RunLoop afterwards. The test titles may
// quick-reboot sooner than 600 ms: the kernel then logs the call still running when the title ends (after its 500 ms wait), so there is one line either way.
#include <oxos_sdk.h>

namespace {

constexpr ULONG SAVED_MAGIC = 0x574F4C53; // "SLOW"
constexpr LONGLONG SLOW_CALL_MS = 600;

// The state page as boot_test.py reads it (words, in this order).
struct SAVED {
    ULONG Magic;
    ULONG Launches;  // Load calls since the cold boot
    ULONG SlowCalls; // RunLoop calls that took 600 ms: one, in the cold boot's launch
    ULONG Calls;     // RunLoop calls, all launches
};

SAVED *Saved;
bool SlowCallDone; // this launch's slow call (globals start afresh at each launch)

NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    Saved = static_cast<SAVED *>(Context->State);
    if (!Saved)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Context->StateIsNew || Saved->Magic != SAVED_MAGIC)
        *Saved = {SAVED_MAGIC, 0, 0, 0};
    Saved->Launches++;
    return STATUS_SUCCESS;
}

void NTAPI RunLoop(OXOS_CONTEXT *Context) {
    Saved->Calls++;
    if (SlowCallDone || Context->Generation != 1)
        return;
    SlowCallDone = true;
    LARGE_INTEGER wait;
    wait.QuadPart = -SLOW_CALL_MS * 10000;
    oxos::KeDelayExecutionThread(KernelMode, FALSE, &wait);
    Saved->SlowCalls++;
}

} // namespace

OXOS_DEFINE_EXTENSION(.Name = "slow_loop", .Version = "1.0.0", .Load = Load, .RunLoop = RunLoop);
