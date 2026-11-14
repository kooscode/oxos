// What the extension loader (extension_loader.cpp) and the runtime (extension_runtime.cpp) share (spec/oxos_internal/34): the loaded extensions and what they
// registered, and the resident code and state the loader uses.
#pragma once
#include <core/core.h>
#include <runtime/types.h>
#include <oxos/oxos_interfaces.h>

// --- The loaded extensions

constexpr ULONG XK_EXTENSIONS_MAX = 8;  // extensions loaded at one launch
constexpr ULONG XK_EXTENSION_HOOKS_MAX = 256; // hooks of all extensions together
constexpr ULONG XK_EXTENSION_EVENTS_MAX = 32; // event registrations of all extensions together

// One loaded extension.
struct XK_EXTENSION {
    OXOS_CONTEXT Context;
    const OXOS_EXTENSION *Descriptor;
    KEVENT StopEvent;
    HANDLE MainThread; // its Main thread, or null
};
// tests/boot_test.py reads the state pages through these offsets.
XK_SIZE(OXOS_CONTEXT, 64);
XK_OFFSET(OXOS_CONTEXT, State, 40);
XK_SIZE(XK_EXTENSION, 88);

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
    PVOID CallbackContext;
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
};
namespace xk::extensions {
extern XK_EXTENSIONS_STATE *LoadedExtensions; // null when no extension is loaded
} // namespace xk::extensions

// --- Resident state and code the loader uses (extension_runtime.cpp)

namespace xk::extensions {
extern XK_PERSIST PVOID StateIndexPage; // the state pages' index (§6), kept across quick reboots
extern const char KernelVersionText[];  // OXOS_CONTEXT::KernelVersion
// A Main thread's routine: the extension's Main (StartRoutine) with its context, then the thread ends.
void NTAPI MainThreadRoutine(PVOID Main, PVOID Context);
} // namespace xk::extensions
