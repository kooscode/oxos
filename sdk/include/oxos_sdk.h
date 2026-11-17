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
//
// An extension's own file is C:\OXOS\extensions\NAME.ini, next to NAME.xos (the same name). It holds two kinds of things:
//
//   ; SETTINGS: the keys at the top, before the first [section]. Read into memory when the extension loads;
//   ; oxos_setting() returns them at any time, also in hooks. At most 32, values up to 255 characters.
//   buttons = back black
//   format = png
//
//   ; DATA: the [sections] after the settings. Never loaded with the extension: oxos_ini_section() reads one
//   ; section from the disk each time it is called (section 4a). No limit on sections or keys; keys may have spaces.
//   [4D530064]
//   Infinite Ammo = yes
//
// Settings still in the extension's section of oxos_extensions.ini (the older place) are used for the keys NAME.ini does not set.

// The value of the setting Key (any case), or null. The kernel owns the text; the extension never frees it.
const char *oxos_setting(const char *Key);

// The value of the setting Key as a whole number ("interval = 10" gives 10), or Default when it is missing or does not start with a digit.
ULONG oxos_setting_number(const char *Key, ULONG Default);

ULONG oxos_launch_number(void);        // launches since the cold boot: 1 = the cold boot's title
const char *oxos_kernel_version(void); // e.g. "0.4.7"

// The state page: 4 KB that keep their contents from one launch to the next, until the next cold boot. All zero at the first launch, and again when the
// extension's Version changes (a new build may keep other data in it). Never null.
void *oxos_state(void);

// --- 4. Files and folders, by path: a \Device\... path (C:\ is \Device\Harddisk0\Partition2\), any case. Everything here is synchronous, and opens inside
// the kernel: no extension's hook sees these calls (hooks see what titles call).

#ifndef OXOS_SDK_IN_KERNEL // the kernel has its own (io::OpenFile, ...)

// A whole file at once. oxos_file_read reads it into Buffer, its length into *Size; a file larger than BufferSize is not read (STATUS_BUFFER_TOO_SMALL, *Size
// says how large it is).
NTSTATUS oxos_file_read(const char *Path, void *Buffer, ULONG BufferSize, ULONG *Size);
NTSTATUS oxos_file_write(const char *Path, const void *Data, ULONG Size); // creates the file, or replaces it

// Open handles: read and write them with NtReadFile and NtWriteFile, close them with NtClose.
NTSTATUS oxos_file_open(const char *Path, ACCESS_MASK Access, ULONG Share, ULONG Options, HANDLE *File); // Options: FILE_NON_DIRECTORY_FILE, ...
// Creates or opens, as Disposition says (FILE_CREATE, FILE_OPEN_IF, FILE_OVERWRITE_IF, ...); Size, when not 0, is the space a new file starts with.
NTSTATUS oxos_file_create(const char *Path, ACCESS_MASK Access, ULONG Share, ULONG Disposition, ULONG Options, HANDLE *File, ULONGLONG Size);
NTSTATUS oxos_file_delete(const char *Path);

// NewName: a name, which renames it in its own folder, or a whole path on the same drive, which moves it there. An existing NewName is not replaced.
NTSTATUS oxos_file_rename(const char *Path, const char *NewName);
BOOLEAN oxos_file_exists(const char *Path); // a file, not a folder

NTSTATUS oxos_directory_create(const char *Path); // fails when it exists (STATUS_OBJECT_NAME_COLLISION)
NTSTATUS oxos_directory_delete(const char *Path); // an empty folder
NTSTATUS oxos_directory_rename(const char *Path, const char *NewName);
BOOLEAN oxos_directory_exists(const char *Path);

// The names of the files in Folder (oxos_file_list) or of the folders in it (oxos_directory_list): Names gets an array of Count names, each ending in 0.
// Free the array with oxos_list_free when done (one call frees it all):
//
//   char **names;
//   uint32_t count;
//   if (NT_SUCCESS(oxos_file_list(Folder, names, count))) {
//       for (uint32_t i = 0; i < count; i++)
//           oxos_log("%s", names[i]);
//       oxos_list_free(names);
//   }
#ifdef __cplusplus // C++: references (the SDK's header is also plain C, which has none; C extensions do without these two)
NTSTATUS oxos_file_list(const char *Folder, char **&Names, uint32_t &Count);
NTSTATUS oxos_directory_list(const char *Folder, char **&Names, uint32_t &Count);
#endif
void oxos_list_free(char **Names);

#endif

// --- 4a. Data: one [section] of the extension's own NAME.ini, read from the disk when asked
//
// oxos_ini_section reads the file now, from the disk, and keeps only Section (any case): Keys gets an array of Count keys and values, in the file's order,
// in one block of memory that oxos_ini_free frees: the strings oxos_ini_value returns too, so use or copy them first. The rest of the file is never held
// in memory, however large it is. Each call reads the file again, so read a section once (in ExtensionStart or a title event) and keep what you need.
// Only where the disk may be read: ExtensionStart, ExtensionRunLoop, ExtensionTerminate and the title events, never in a hook (STATUS_INVALID_DEVICE_STATE
// above PASSIVE_LEVEL). A key is the text before '=' (spaces allowed), its value the rest of the line. STATUS_OBJECT_NAME_NOT_FOUND when the file or the
// section does not exist.
//
//   OXOS_INI_KEY *keys;
//   uint32_t count;
//   if (NT_SUCCESS(oxos_ini_section("4D530064", keys, count))) {
//       const char *ammo = oxos_ini_value(keys, count, "Infinite Ammo");   // "yes", or null
//       oxos_ini_free(keys);
//   }

#ifndef OXOS_SDK_IN_KERNEL // only extensions use these

// One key of a section, and its value.
typedef struct OXOS_INI_KEY {
    const char *Key;
    const char *Value;
} OXOS_INI_KEY;

#ifdef __cplusplus // C++: references (as oxos_file_list; C extensions do without)
NTSTATUS oxos_ini_section(const char *Section, OXOS_INI_KEY *&Keys, uint32_t &Count);
#endif
// The value of Key (any case) among the Count keys oxos_ini_section returned, or null.
const char *oxos_ini_value(const OXOS_INI_KEY *Keys, uint32_t Count, const char *Key);
void oxos_ini_free(OXOS_INI_KEY *Keys);

#endif

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
    char *Target;         // where oxos_title_redirect writes; null when this load cannot be redirected (kernels before 0.4.10 leave both out)
    ULONG TargetSize;
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

// In the title-loading event: starts the XBE at Path (\Device\...) in place of Title->Path, as a launch redirect does (spec/oxos_internal/33 §3): the target
// gets the launch data page unchanged (its LaunchPath still names what was asked for) and its own folder as D:, and goes through the title-loading event itself
// (it cannot be redirected again). If it does not load, Title->Path is loaded as asked. Only a launch through the launch data page can be redirected (a title
// or a dashboard started Title->Path), or the disc the console starts without one (power-on; not the dashboard, a netbooted title, or a launch redirect's own
// target). FALSE when this one cannot be, when an extension loaded earlier has already redirected it, or when Path is longer than 359 characters.
BOOLEAN oxos_title_redirect(const OXOS_TITLE_LOADING *Title, const char *Path);

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
