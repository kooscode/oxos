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
// serial port, which only debug kernels have, and only when the extension's section of oxos_extensions.ini says "logging_enabled = yes" (the user's choice,
// which the extension need not read). Without it, or with no: no line anywhere, whatever oxos_set_log_target asks.
void oxos_log(const char *Format, ...);

#define OXOS_LOG_SERIAL 1
#define OXOS_LOG_FILE 2
void oxos_set_log_target(ULONG Target); // OXOS_LOG_FILE, OXOS_LOG_SERIAL, or both (the default); at any time

// Memory kept while a title runs (spec/oxos_internal/34 §8.2): take it with MmAllocateSystemMemory(Bytes, PAGE_READWRITE) (MmFreeSystemMemory frees it):
// its pages are ones a title's contiguous request may move away, as the stock kernel's own pool pages, so they are never in a title's way. The kernel keeps
// the extensions' images in such pages too. Contiguous memory stays where a title would claim it: at the top of memory, a title's Direct3D block moved off
// its 64 KB boundary drew garbage and stopped; in the middle, a title that claims a fixed range (from 0x61000) found it taken and crashed. Only memory the
// hardware reads by its physical address needs contiguous pages: take them below this address, MmAllocateContiguousMemoryEx(Bytes, 0,
// OXOS_KEPT_MEMORY_HIGHEST, 0, PAGE_READWRITE), where the stock kernel's own image lies (no title claims it); there are only a few dozen pages, shared with
// the kernel's.
#define OXOS_KEPT_MEMORY_HIGHEST 0x00060FFF
#if !defined(PAGE_READWRITE) && !defined(OXOS_SDK_IN_KERNEL)
#define PAGE_READWRITE 0x04 // MmAllocateContiguousMemoryEx's Protect: read and write, cached (the kernel has its own)
#endif

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
const char *oxos_title_path(void);     // the running title's XBE (\Device\...), from the title-loaded event on; null before

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
// The names of the file's sections, in its order, as oxos_file_list gives names (free them with oxos_list_free); none: Count 0. For an extension whose
// sections are the user's to name (each a shortcut, say).
NTSTATUS oxos_ini_sections(char **&Names, uint32_t &Count);
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

// --- 7. Pads, title launches and resets (spec/oxos_internal/34 §7.3)
//
// The pads' state comes from the USB host controllers (spec/oxos_internal/22 §9), for every title however it was built. Buttons: the OXOS_PAD_* bits, the
// analog buttons counting as held past half way.

#define OXOS_PAD_DPAD_UP 0x0001
#define OXOS_PAD_DPAD_DOWN 0x0002
#define OXOS_PAD_DPAD_LEFT 0x0004
#define OXOS_PAD_DPAD_RIGHT 0x0008
#define OXOS_PAD_START 0x0010
#define OXOS_PAD_BACK 0x0020
#define OXOS_PAD_LEFT_THUMB 0x0040 // the left stick pressed
#define OXOS_PAD_RIGHT_THUMB 0x0080
#define OXOS_PAD_A 0x0100
#define OXOS_PAD_B 0x0200
#define OXOS_PAD_X 0x0400
#define OXOS_PAD_Y 0x0800
#define OXOS_PAD_BLACK 0x1000
#define OXOS_PAD_WHITE 0x2000
#define OXOS_PAD_LEFT_TRIGGER 0x4000
#define OXOS_PAD_RIGHT_TRIGGER 0x8000

typedef struct OXOS_PAD_STATE {
    ULONG Port;      // the pad: 0-3, in the order the kernel first saw each pad's reports (not necessarily the port it is plugged into)
    ULONG Buttons;   // OXOS_PAD_* bits
    UCHAR Analog[8]; // the analog buttons A B X Y BLACK WHITE LT RT, 0-255
    SHORT ThumbLX, ThumbLY, ThumbRX, ThumbRY;
} OXOS_PAD_STATE;

#ifdef __cplusplus // C++: references (as oxos_file_list; C extensions do without)
// Callback runs on the run-loop thread, at passive level, once for each change of a pad's Buttons (not for every report). Register in ExtensionStart.
BOOLEAN oxos_on_pad_changed(void (*Callback)(const OXOS_PAD_STATE &State, void *UserData), void *UserData);
// The last report of pad Port; FALSE when no report of that pad has been seen.
BOOLEAN oxos_pad_state(ULONG Port, OXOS_PAD_STATE &State);
#endif

#define OXOS_RESET_COLD 1 // power the console off and on (through the SMC): the BIOS, netboot and the disc start again
#define OXOS_RESET_WARM 2 // the console's reset (through the SMC)

// Stops the title (its threads suspended) and starts Path (a \Device\ path, as section 4's); null: the dashboard. At passive level only
// (the run loop, an extension's own thread, a pad callback), never from a hook. Returns only on failure: STATUS_INVALID_DEVICE_STATE (not at passive
// level), STATUS_DEVICE_BUSY (another launch or reset is already under way), STATUS_OBJECT_NAME_NOT_FOUND (no such XBE), STATUS_INVALID_PARAMETER.
NTSTATUS oxos_title_launch(const char *Path);
// Resets the console (OXOS_RESET_COLD or OXOS_RESET_WARM); returns only on failure (STATUS_INVALID_PARAMETER, STATUS_DEVICE_BUSY).
NTSTATUS oxos_console_reset(ULONG Kind);

// --- 8. Toasts: a short message over the running title (spec/oxos_internal/36)
//
// A box with one or two lines of text, on the GPU's video overlay: the title's own picture is never touched. The kernel queues the toasts of every extension
// (8 at most) and shows them one after the other, each for its time, once the title has set its video mode. The user can switch toasts off ([toasts] in
// C:\OXOS\oxos_settings.ini); every call then returns STATUS_NOT_SUPPORTED.
//
//   OXOS_TOAST toast = {"Saving: savegame.dat", OXOS_TOAST_TOP_RIGHT, 3};
//   if (NT_SUCCESS(oxos_toast_enqueue(toast)))
//       oxos_log("toast %lu queued", toast.ToastId);

typedef enum OXOS_TOAST_PLACE {
    OXOS_TOAST_TOP_LEFT,
    OXOS_TOAST_TOP_CENTRE,
    OXOS_TOAST_TOP_RIGHT,
    OXOS_TOAST_MIDDLE_LEFT,
    OXOS_TOAST_MIDDLE_CENTRE,
    OXOS_TOAST_MIDDLE_RIGHT,
    OXOS_TOAST_BOTTOM_LEFT,
    OXOS_TOAST_BOTTOM_CENTRE, // the usual place
    OXOS_TOAST_BOTTOM_RIGHT,
} OXOS_TOAST_PLACE;

#define OXOS_TOAST_LINE_MAX 42 // characters in one line of a toast: the box then fits between the side margins

typedef struct OXOS_TOAST {
    const char *Text;       // one or two lines ('\n' between them) of printable ASCII, OXOS_TOAST_LINE_MAX characters each; the kernel copies it
    OXOS_TOAST_PLACE Place; // where it shows
    ULONG Seconds;          // how long it stays up, 1 to 30 seconds; 0 means the default (5 seconds)
    ULONG ToastId;          // written by the kernel when the toast is queued (0 when it was not); the id its events carry
} OXOS_TOAST;

typedef enum OXOS_TOAST_EVENT {
    OXOS_TOAST_SHOWN,   // it came up
    OXOS_TOAST_HIDDEN,  // its time is up
    OXOS_TOAST_DROPPED, // it never showed: no memory for its picture, or the title had the overlay on for the toast's whole time
} OXOS_TOAST_EVENT;

#ifdef __cplusplus // C++: references (as oxos_file_list; C extensions do without)
// Queues Toast; never waits: any IRQL up to DISPATCH_LEVEL, hooks included. STATUS_SUCCESS (ToastId set); STATUS_DEVICE_BUSY (the queue is full: try again
// later, or with oxos_toast_enqueue_wait); STATUS_CANCELLED (the title is ending); STATUS_NOT_SUPPORTED (toasts are off); STATUS_INVALID_PARAMETER (the
// text, place or time is wrong).
NTSTATUS oxos_toast_enqueue(OXOS_TOAST &Toast);
// The same, waiting up to TimeoutMs for room in the queue (STATUS_TIMEOUT when there was none). At passive level only (the run loop, an extension's own
// thread, an event callback), never from a hook.
NTSTATUS oxos_toast_enqueue_wait(OXOS_TOAST &Toast, ULONG TimeoutMs);
#endif
// One line, formatted as oxos_log formats it, at the bottom centre for 5 seconds; never waits.
NTSTATUS oxos_toast(const char *Format, ...);
// Callback gets the extension's own toasts' events, on the run-loop thread at passive level. Register in ExtensionStart, once (a second call returns FALSE).
BOOLEAN oxos_on_toast(void (*Callback)(ULONG ToastId, OXOS_TOAST_EVENT Event, void *UserData), void *UserData);

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
