// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK: the header an extension includes, in C or C++ (spec/oxos_internal/34). Examples: examples/example_run_loop, example_kernel_hook,
// example_title_event.
//
// An extension is a small DLL (a .xos file in C:\OXOS\extensions\) that the OXOS kernel loads before every title, as C:\OXOS\oxos_extensions.ini lists it.
// It has up to three functions of its own:
//
//   ExtensionStart      before each title: read settings, hook kernel functions, ask for title events. Return TRUE to stay, FALSE to be unloaded.
//                       It must return quickly: the title waits for it.
//   ExtensionRunLoop    optional: called again and again while the title runs, about every millisecond, on one kernel thread that every extension
//                       shares. Do a little work and return.
//   ExtensionTerminate  optional: the title is starting another title or the dashboard. Close files here.
//
// The kernel loads every extension afresh at each launch: global variables start from their initial values every time, and only the state page
// (oxos_state) keeps its contents from one launch to the next.
#pragma once
#ifndef OXOS_SDK_IN_KERNEL
#include <oxos_kernel.h> // the Xbox kernel: every function an extension can call or hook, with its types
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- 1. Naming the extension and its functions, after declaring them:
//
//   static BOOLEAN ExtensionStart(void);
//   OXOS_DEFINE_EXTENSION(.Name = "my_extension", .Version = "1.0.0", .Start = ExtensionStart);
//
// The fields, in this order: Name (the extension's own name, which picks its state page: keep it across versions), Version (for the log), Start, and the
// optional RunLoop and Terminate.

#define OXOS_DEFINE_EXTENSION(...) OXOS_DESCRIPTOR(__VA_ARGS__)

// --- 2. Logging

// One line, as printf formats it, after "[name] " (the extension's section in oxos_extensions.ini). It goes to C:\OXOS\log\extensions.log and to the debug
// serial port, which only debug kernels have.
void oxos_log(const char *Format, ...);

#define OXOS_LOG_SERIAL 1
#define OXOS_LOG_FILE 2
void oxos_set_log_target(ULONG Target); // OXOS_LOG_FILE, OXOS_LOG_SERIAL, or both (the default); at any time

// --- 3. Settings, the launch, the state page

// The value of Key in the extension's section of oxos_extensions.ini (any case), or null. The kernel owns the text; the extension never frees it.
const char *oxos_setting(const char *Key);

// The value of Key as a whole number ("interval = 10" gives 10), or Default when the setting is missing or does not start with a digit.
ULONG oxos_setting_number(const char *Key, ULONG Default);

ULONG oxos_launch_number(void);        // launches since the cold boot: 1 = the cold boot's title
const char *oxos_kernel_version(void); // e.g. "0.4.7"

// The state page: 4 KB that keep their contents from one launch to the next, until the next cold boot. All zero at the first launch, and again when the
// extension's Version changes (a new build may keep other data in it). Never null.
void *oxos_state(void);

// --- 4. Files: a whole file of at most MaxSize bytes (a \Device\... path: C:\ is \Device\Harddisk0\Partition2\), or null. Free it with oxos_free_file.

char *oxos_read_file(const char *Path, ULONG MaxSize, ULONG *Size);
void oxos_free_file(char *Text);

// --- 5. Hooks, in ExtensionStart: the title's calls of a kernel function go to the extension's function first
//
//   static NTSTATUS NTAPI My_NtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
//       InterlockedIncrement(&Opens);
//       return oxos_next(NtOpenFile)(File, Access, Name, Iosb, Share, Options);
//   }
//   ... in ExtensionStart: oxos_hook(NtOpenFile, My_NtOpenFile);
//
// The hook has exactly the kernel function's arguments, result and calling convention, as oxos_kernel.h declares it; anything else does not compile.
// oxos_next(Function) passes a call on: to the next extension's hook, or to the kernel (an extension's own calls of a kernel function never come back to its
// hooks, so oxos_next(NtOpenFile) is NtOpenFile itself). A hook may also change the arguments or the result, or answer the call itself. It runs on the
// title's threads, possibly several at once: count with InterlockedIncrement, not ++. oxos_hook returns FALSE when the kernel refuses (a function that
// cannot be hooked, such as KeGetCurrentIrql, or one hooked twice).

#define oxos_hook(Function, Hook) oxos_hook_function((PVOID)&Function, (PVOID)OXOS_SAME_TYPE(Function, Hook))
#define oxos_next(Function) Function

// --- 6. Title events, in ExtensionStart: the kernel calls these at each launch, with UserData as the extension passed it

#define OXOS_LAUNCH_COLD_BOOT 1 // OXOS_TITLE_LOADING::LaunchKind: the cold boot's disc, title or dashboard
#define OXOS_LAUNCH_TITLE 2     // after a quick reboot: a title
#define OXOS_LAUNCH_DASHBOARD 3 // after a quick reboot: the dashboard

typedef struct OXOS_TITLE_LOADING { // the kernel is about to load an XBE (each one it tries)
    ULONG Size;
    const char *Path;     // \Device\...\name.xbe
    ULONG LaunchKind;     // OXOS_LAUNCH_*
    PVOID LaunchDataPage; // the launch data page, or null
} OXOS_TITLE_LOADING;

typedef struct OXOS_TITLE_LOADED { // the XBE is loaded; it has not started yet
    ULONG Size;
    const char *Path;
    ULONG TitleId;
    ULONG TitleVersion;      // the certificate's version
    const char *TitleName;   // the certificate's name as plain text: 41 bytes, up to 40 characters and a 0 (a character outside ASCII is '?')
    PVOID XbeHeader;         // the image header at 0x00010000
    PVOID Certificate;
} OXOS_TITLE_LOADED;

typedef struct OXOS_TITLE_EXITING { // the title is starting another title or the dashboard; it is still in memory
    ULONG Size;
    PVOID LaunchDataPage; // what the title wrote, or null
} OXOS_TITLE_EXITING;

BOOLEAN oxos_on_title_loading(void (*Callback)(const OXOS_TITLE_LOADING *Title, void *UserData), void *UserData);
BOOLEAN oxos_on_title_loaded(void (*Callback)(const OXOS_TITLE_LOADED *Title, void *UserData), void *UserData);
BOOLEAN oxos_on_title_exiting(void (*Callback)(const OXOS_TITLE_EXITING *Title, void *UserData), void *UserData);

// --- How the macros above work: nothing here to use

#define OXOS_EXTENSION_MAGIC 0x58534F58UL         // "XOSX"
#define OXOS_API_VERSION ((1UL << 16) | 0)         // major << 16 | minor
#define OXOS_EXTENSION_EXPORT_NAME "oxos_extension" // the descriptor's export

typedef struct OXOS_CONTEXT OXOS_CONTEXT; // this launch's context: the kernel fills it, the SDK's functions read it

typedef struct OXOS_EXTENSION { // the descriptor OXOS_DEFINE_EXTENSION writes and the kernel reads
    ULONG Magic;
    ULONG ApiVersion;
    OXOS_CONTEXT **Context; // the kernel puts this launch's context here before Start
    const char *Name;
    const char *Version;
    ULONG Flags;
    BOOLEAN (*Start)(void);
    void (*RunLoop)(void);
    void (*Terminate)(void);
} OXOS_EXTENSION;

extern OXOS_CONTEXT *oxos_context;
BOOLEAN oxos_hook_function(PVOID Function, PVOID Hook);

#ifdef __cplusplus
}
#define OXOS_DESCRIPTOR(...)                                                                                                                                 \
    extern "C" __declspec(dllexport) const OXOS_EXTENSION oxos_extension = {                                                                                \
        .Magic = OXOS_EXTENSION_MAGIC, .ApiVersion = OXOS_API_VERSION, .Context = &oxos_context, __VA_ARGS__}
#define OXOS_SAME_TYPE(Function, Hook) static_cast<__typeof__(&Function)>(Hook) // only a hook of the very same type converts
#else
#define OXOS_DESCRIPTOR(...)                                                                                                                                 \
    __declspec(dllexport) const OXOS_EXTENSION oxos_extension = {                                                                                           \
        .Magic = OXOS_EXTENSION_MAGIC, .ApiVersion = OXOS_API_VERSION, .Context = &oxos_context, __VA_ARGS__}
#define OXOS_SAME_TYPE(Function, Hook) (1 ? (Hook) : (__typeof__(&Function))0)
#endif
