// Kernel extensions (spec/oxos_internal/34): .xos DLLs from C:\OXOS\extensions\, loaded in the order of
// C:\OXOS\oxos_extensions.ini before each title. What the title launcher and the rest of the kernel call, and the state
// the loader (extensions/extension_loader.cpp) shares with what runs while the title runs
// (extensions/extension_runtime.cpp).
#pragma once
#include <xk/core.h>
#include <xk/types.h>
#include <oxos/oxos_interfaces.h>

// --- Called by the rest of the kernel

namespace xk::extensions {
// Runs the extension loader (§8.1) for this launch: it loads the extensions of oxos_extensions.ini, calls
// each one's Load and starts their Main threads (§3.2, §4.3). Called once per launch by the title launcher, before the
// title is chosen; nothing in safe mode or without the list.
void LoadExtensions();
// The address a title's import of Ordinal is resolved to: the first extension's hook, else Target (§7.1).
ULONG ResolveTitleImport(ULONG Ordinal, ULONG Target);
// OXOS_EVENT_TITLE_LOADING for the XBE the kernel is about to load (§7.2); Dashboard as titles::LoadImage's.
void TitleLoading(PCSZ Path, BOOLEAN Dashboard);
// OXOS_EVENT_TITLE_LOADED for the XBE now at 0x00010000, before its entry point runs.
void TitleLoaded();
// The title quick-reboots (HalReturnToFirmware(2)): OXOS_EVENT_TITLE_EXITING, then each extension's StopEvent, its Main
// thread's end and Unload (§4.3).
void TitleExiting();

// Safe mode (B held at the cold boot's splash, §3.3): no extension loads until the next cold boot.
extern XK_PERSIST BOOLEAN SafeMode;
} // namespace xk::extensions

// --- The loader (§8.1): linked on its own at XK_EXTENSION_LOADER_BASE against this kernel's addresses
// (tools/link_extension_loader.py) and appended to the kernel image by tools/mkbios.py, in the boot-only pages. A cold
// boot writes it to C:\OXOS\extensions\oxos_extension_loader.cache; every launch runs it from that file.

constexpr ULONG XK_EXTENSION_LOADER_BASE = 0xB0000000;   // its link address, mapped only while it runs
constexpr ULONG XK_EXTENSION_LOADER_MAX_BYTES = 0x10000; // its largest image
constexpr ULONG XK_EXTENSION_LOADER_MAGIC = 0x4C564F58;  // "XOVL"
constexpr ULONG XK_EXTENSION_LOADER_HEADER_FIELD = 0x38; // in the kernel's DOS header: the header's RVA (0: none)

// In front of the loader's image, in the kernel image and in the cache file.
struct XK_EXTENSION_LOADER_HEADER {
    ULONG Magic;
    ULONG Stamp; // the kernel's PE TimeDateStamp it was linked against
    ULONG Base;  // XK_EXTENSION_LOADER_BASE
    ULONG Size;  // bytes of the image after this header (its SizeOfImage)
    ULONG Entry; // the entry point's RVA: XkExtensionLoaderEntry
};
XK_SIZE(XK_EXTENSION_LOADER_HEADER, 20);

// The loader's entry point (src/extensions/extension_loader.cpp).
extern "C" void XkExtensionLoaderEntry();
namespace xk::extensions {
// Cold boot, boot-only: writes the loader from the kernel image to the cache file, when the extensions folder exists.
void WriteLoaderCache();
} // namespace xk::extensions

// --- Resident state and code the loader uses (extensions/extension_runtime.cpp)

namespace xk::extensions {
extern XK_PERSIST PVOID StateIndexPage; // the state pages' index (§6), kept across quick reboots
extern const char KernelVersionText[];  // OXOS_CONTEXT::KernelVersion
// A Main thread's routine: the extension's Main (StartRoutine) with its context, then the thread ends.
void NTAPI MainThreadRoutine(PVOID Main, PVOID Context);
} // namespace xk::extensions

// --- Shared by the loader and the runtime

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
