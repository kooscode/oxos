// SPDX-License-Identifier: MIT
// What the kernel gives each extension at each launch, and the loader's functions (spec/oxos_internal/34 §5): shared by the kernel and the SDK's library
// (src/oxos_sdk.cpp), not shipped. Extension code reaches all of it through oxos_sdk.h's functions. Include it after oxos_sdk.h.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define OXOS_EVENT_TITLE_LOADING 1 // events (§7.2): their data is oxos_sdk.h's OXOS_TITLE_LOADING ...
#define OXOS_EVENT_TITLE_LOADED 2
#define OXOS_EVENT_TITLE_EXITING 3
#define OXOS_EVENT_PAD_CHANGED 4 // its data is oxos_sdk.h's OXOS_PAD_STATE (spec/oxos_internal/34 §7.3)
#define OXOS_EVENT_TOAST 5 // its data is an OXOS_TOAST_NOTICE, for the extension that queued the toast only (spec/oxos_internal/36 §5)
#define OXOS_EVENT_COUNT 6 // one more than the highest event

// The data of OXOS_EVENT_TOAST: one of the extension's toasts shown, hidden or dropped.
typedef struct OXOS_TOAST_NOTICE {
    ULONG ToastId;
    OXOS_TOAST_EVENT Event;
} OXOS_TOAST_NOTICE;

// An event's callback, as the kernel calls it: the event's data and what the extension passed when it registered.
typedef void (*OXOS_EVENT_CALLBACK)(const void *Data, void *UserData);

#define OXOS_BOOT_COLD 1  // OXOS_CONTEXT::BootFlags: this launch is the cold boot's
#define OXOS_BOOT_QUICK 2 // this launch follows a quick reboot

// One setting (§3.1): a key at the top of the extension's own C:\OXOS\extensions\NAME.ini, or of its section in oxos_extensions.ini. The kernel owns the
// strings; they stay valid until the title ends.
typedef struct OXOS_SETTING {
    const char *Key;
    const char *Value;
} OXOS_SETTING;

// The loader's functions (§5.1): only during the extension's Start; the kernel clears OXOS_CONTEXT::Api afterwards.
typedef struct OXOS_API {
    ULONG Size; // sizeof(OXOS_API) of the kernel that filled it
    // Hooks the kernel function at Function (the extension's import of it, still the kernel's own during Start) for the title's calls (§7.1).
    NTSTATUS(NTAPI *HookFunction)(OXOS_CONTEXT *Context, PVOID Function, PVOID Hook);
    // Registers Callback (an OXOS_EVENT_CALLBACK) for Event (OXOS_EVENT_*, §7.2).
    NTSTATUS(NTAPI *RegisterEvent)(OXOS_CONTEXT *Context, ULONG Event, PVOID Callback, PVOID UserData);
} OXOS_API;

// Read-only facts about the console and the running title (§5.2).
typedef struct OXOS_KERNEL_INFO {
    ULONG Size;            // sizeof(OXOS_KERNEL_INFO) of the kernel that filled it
    ULONG MemoryPages;     // the console's RAM in 4 KB pages: 16384 (64 MB) or 32768 (128 MB)
    ULONG TitleId;         // the running title's ID, from the title-loaded event on (0 before)
    const char *TitlePath; // the running title's XBE (\Device\...), from the title-loaded event on (null before)
} OXOS_KERNEL_INFO;

// One extension at one launch, filled by the kernel before Start.
struct OXOS_CONTEXT {
    ULONG Size;                // sizeof(OXOS_CONTEXT) of the kernel that filled it
    ULONG ApiVersion;          // the kernel's OXOS_API_VERSION
    const char *KernelVersion; // e.g. "0.4.7"
    ULONG Generation;          // launches since the cold boot: 1 = the cold boot's title
    ULONG BootFlags;           // OXOS_BOOT_*

    const char *SectionName;      // its section in oxos_extensions.ini
    const OXOS_SETTING *Settings; // its settings: the top of its own NAME.ini, then its section's other keys
    ULONG SettingCount;
    PVOID ImageBase; // where it is loaded this launch
    ULONG ImageSize;
    PVOID State;     // its state page (§6): 4096 bytes, never null

    const OXOS_API *Api;            // only during Start, null afterwards
    const OXOS_KERNEL_INFO *Kernel; // facts about the console and the title
    ULONG LogTarget;                // where oxos_log writes: OXOS_LOG_*; the kernel starts it at both
    const char *INIPath;            // its own C:\OXOS\extensions\NAME.ini (\Device\... path), for oxos_ini_section; null from kernels before 0.4.9
};

#ifdef __cplusplus
}
#endif
