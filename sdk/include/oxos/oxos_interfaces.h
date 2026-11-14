// SPDX-License-Identifier: MIT
// OXOS kernel extensions: the structures the kernel and a .xos extension exchange (spec/oxos_internal/34 §4-§7): the descriptor, the context, the loader's
// functions and the events. The kernel and the SDK both include this file, so the layout has one definition. Plain C, also valid C++. It needs the NT base
// types (ULONG, PVOID, NTSTATUS, NTAPI, ...) declared before it: the kernel's runtime/types.h or the SDK's oxos/oxos_types.h.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// --- Versions and names (§4.1, §4.2)

#define OXOS_EXTENSION_MAGIC 0x58534F58UL         // "XOSX" in memory
#define OXOS_API_VERSION ((1UL << 16) | 0)         // major << 16 | minor
#define OXOS_EXTENSION_EXPORT_NAME "oxos_extension" // the descriptor's export name

// --- The context the kernel gives each extension (§5)

typedef struct OXOS_CONTEXT OXOS_CONTEXT;

// One setting from the extension's section in oxos_extensions.ini (§3.1).
typedef struct OXOS_SETTING {
    const char *Key;
    const char *Value;
} OXOS_SETTING;

// The loader's functions (§5.1): only during Load; the kernel clears OXOS_CONTEXT::Api afterwards.
typedef struct OXOS_API {
    ULONG Size; // sizeof(OXOS_API) of the kernel that filled it
    // Hooks a kernel function (Ordinal, as in xboxkrnl.exe) for the title's calls (§7.1). *Next receives the function the replacement passes calls on to, once
    // every extension's Load has returned.
    NTSTATUS(NTAPI *HookExport)(OXOS_CONTEXT *Context, ULONG Ordinal, PVOID Replacement, PVOID *Next);
    // Registers Callback (an OXOS_EVENT_CALLBACK) for Event (OXOS_EVENT_*, §7.2).
    NTSTATUS(NTAPI *RegisterEvent)(OXOS_CONTEXT *Context, ULONG Event, PVOID Callback, PVOID CallbackContext);
} OXOS_API;

// Read-only facts about the console and the running title (§5.2).
typedef struct OXOS_KERNEL_INFO {
    ULONG Size;            // sizeof(OXOS_KERNEL_INFO) of the kernel that filled it
    ULONG MemoryPages;     // the console's RAM in 4 KB pages: 16384 (64 MB) or 32768 (128 MB)
    ULONG TitleId;         // the running title's ID, from OXOS_EVENT_TITLE_LOADED on (0 before)
    const char *TitlePath; // the running title's XBE (\Device\...), from OXOS_EVENT_TITLE_LOADED on (null before)
} OXOS_KERNEL_INFO;

#define OXOS_BOOT_COLD 1  // OXOS_CONTEXT::BootFlags: this launch is the cold boot's
#define OXOS_BOOT_QUICK 2 // this launch follows a quick reboot (a title started another)

struct OXOS_CONTEXT {
    ULONG Size;                // sizeof(OXOS_CONTEXT) of the kernel that filled it
    ULONG ApiVersion;          // the kernel's OXOS_API_VERSION
    const char *KernelVersion; // e.g. "0.4.7"
    ULONG Generation;          // launches since the cold boot: 1 = the cold boot's title
    ULONG BootFlags;           // OXOS_BOOT_*

    const char *SectionName;     // its section in oxos_extensions.ini
    const OXOS_SETTING *Settings; // the section's other keys
    ULONG SettingCount;
    PVOID ImageBase;             // where it is loaded this launch
    ULONG ImageSize;
    PVOID State;                 // its state page (§6)
    ULONG StateSize;             // 4096
    BOOLEAN StateIsNew;          // the page was just created (zeroed)

    const OXOS_API *Api;            // only during Load, null afterwards
    const OXOS_KERNEL_INFO *Kernel; // facts about the console and the title
    ULONG LogTarget;                // where oxos_log writes: OXOS_LOG_* (oxos_set_log_target); the kernel starts it at both
};

// --- Where oxos_log writes (OXOS_CONTEXT::LogTarget), one or both

#define OXOS_LOG_SERIAL 1 // the kernel's debug log: the debug serial port (debug kernels only: a release kernel keeps nothing of it)
#define OXOS_LOG_FILE 2   // C:\OXOS\log\extensions.log (spec/oxos_internal/35), at PASSIVE_LEVEL only

// --- The descriptor each extension exports as oxos_extension (§4.1)

typedef struct OXOS_EXTENSION {
    ULONG Magic;      // OXOS_EXTENSION_MAGIC
    ULONG ApiVersion; // OXOS_API_VERSION of the SDK it was built with
    const char *Name; // its state page's key (§6)
    const char *Version; // the extension's own version, for the log
    ULONG Flags;         // none defined: 0
    NTSTATUS(NTAPI *Load)(OXOS_CONTEXT *Context);
    // Called again and again while the title runs, on the kernel's one loop thread: about every millisecond, after the RunLoop of each extension before it in
    // oxos_extensions.ini (§4.3). It must return quickly; one that takes over 500 ms is logged in C:\OXOS\log\extensions.log.
    void(NTAPI *RunLoop)(OXOS_CONTEXT *Context);
    void(NTAPI *Unload)(OXOS_CONTEXT *Context);
} OXOS_EXTENSION;

// --- Events (§7.2)

#define OXOS_EVENT_TITLE_LOADING 1 // data: OXOS_TITLE_LOADING
#define OXOS_EVENT_TITLE_LOADED 2  // data: OXOS_TITLE_LOADED
#define OXOS_EVENT_TITLE_EXITING 3 // data: OXOS_TITLE_EXITING
#define OXOS_EVENT_COUNT 4         // one more than the highest event

typedef void(NTAPI *OXOS_EVENT_CALLBACK)(OXOS_CONTEXT *Context, PVOID CallbackContext, PVOID Data);

#define OXOS_LAUNCH_COLD_BOOT 1 // OXOS_TITLE_LOADING::LaunchKind: the cold boot's disc, title or dashboard
#define OXOS_LAUNCH_TITLE 2     // after a quick reboot: a title (named in the launch data page, or the disc)
#define OXOS_LAUNCH_DASHBOARD 3 // after a quick reboot: the dashboard

// The kernel is about to load an XBE: each one it tries (a dashboard that does not load is followed by the next).
typedef struct OXOS_TITLE_LOADING {
    ULONG Size;
    const char *Path;     // \Device\...\name.xbe
    ULONG LaunchKind;     // OXOS_LAUNCH_*
    PVOID LaunchDataPage; // the launch data page, or null
} OXOS_TITLE_LOADING;

// The XBE is loaded and its kernel imports resolved; its entry point has not run.
typedef struct OXOS_TITLE_LOADED {
    ULONG Size;
    const char *Path;        // the XBE that was loaded
    ULONG TitleId;
    ULONG TitleVersion;      // the certificate's version
    const USHORT *TitleName; // the certificate's name, UTF-16, up to 40 characters
    PVOID XbeHeader;         // the image header at 0x00010000
    PVOID Certificate;
} OXOS_TITLE_LOADED;

// The title asked for a quick reboot (HalReturnToFirmware(2)) to start another XBE or the dashboard; it is still in memory.
typedef struct OXOS_TITLE_EXITING {
    ULONG Size;
    PVOID LaunchDataPage; // what the title wrote, or null
} OXOS_TITLE_EXITING;

#ifdef __cplusplus
}
#endif
