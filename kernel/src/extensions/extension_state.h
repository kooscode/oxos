// What the extension loader (extension_loader.cpp) and the runtime (extension_runtime.cpp) share (spec/oxos_internal/34): the loaded extensions and what they
// registered, and the resident code and state the loader uses.
#pragma once
#include <core/core.h>
#include <runtime/types.h>
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#include <oxos_sdk.h>
#include <oxos_extension_context.h>

// --- The loaded extensions

constexpr ULONG XK_EXTENSIONS_MAX = 16; // extensions loaded at one launch
constexpr ULONG XK_EXTENSION_HOOKS_MAX = 256; // hooks of all extensions together
constexpr ULONG XK_EXTENSION_EVENTS_MAX = 32; // event registrations of all extensions together

// One loaded extension.
struct XK_EXTENSION {
    OXOS_CONTEXT Context;
    const OXOS_EXTENSION *Descriptor;
};
// tests/boot_test.py reads the state pages through these offsets.
XK_SIZE(OXOS_CONTEXT, 60);
XK_OFFSET(OXOS_CONTEXT, State, 40);
XK_SIZE(XK_EXTENSION, 64);

constexpr ULONG XK_NO_EXTENSION = 0xFFFFFFFF; // XK_EXTENSIONS_STATE::LoopExtension between RunLoop calls

// One hook: Extension's Replacement for the title's calls of Ordinal; *Next is written when the chains are linked.
struct XK_EXTENSION_HOOK {
    ULONG Ordinal;
    ULONG Extension; // index in XkExtensions
    PVOID Replacement;
    PVOID *Next;
};

// One event registration.
struct XK_EXTENSION_EVENT {
    ULONG Event; // OXOS_EVENT_*
    ULONG Extension;
    OXOS_EVENT_CALLBACK Callback;
    PVOID UserData;
};

// The loaded extensions and what they registered, for this launch (pool memory; a quick reboot clears it all).
struct XK_EXTENSIONS_STATE {
    ULONG Count;
    XK_EXTENSION Extensions[XK_EXTENSIONS_MAX]; // at +4 (tests/boot_test.py)
    ULONG HookCount;
    XK_EXTENSION_HOOK Hooks[XK_EXTENSION_HOOKS_MAX];
    ULONG EventCount;
    XK_EXTENSION_EVENT Events[XK_EXTENSION_EVENTS_MAX];
    OXOS_KERNEL_INFO Kernel;
    // The run loop (§4.3): its thread, the request that stops it, and the RunLoop call in progress (for the warning when one does not return in time).
    HANDLE LoopThread; // null when no extension has a RunLoop
    KEVENT LoopStop;
    volatile ULONG LoopExtension; // the index of the extension whose RunLoop is running, or XK_NO_EXTENSION
    volatile ULONG LoopCallStart; // KeTickCount when that RunLoop was called
};
namespace xk::extensions {
extern XK_EXTENSIONS_STATE *LoadedExtensions; // null when no extension is loaded
} // namespace xk::extensions

// --- Resident state and code the loader uses (extension_runtime.cpp)

namespace xk::extensions {
extern XK_PERSIST PVOID StateIndexPage; // the state pages' index (§6), kept across quick reboots
extern const char KernelVersionText[];  // OXOS_CONTEXT::KernelVersion
// The run loop thread's routine (§4.3): until LoopStop, waits 1 ms, then calls each extension's RunLoop in oxos_extensions.ini order.
void NTAPI RunLoopThread(PVOID StartRoutine, PVOID StartContext);
} // namespace xk::extensions
