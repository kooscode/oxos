# 34. Kernel extensions (OXOS's own)

Status: **being implemented, 2026-11-13.** In the kernel: the loader (linked on its own, run from a disk cache, §8.1), hooks, events, Start, RunLoop and
Terminate, state pages, safe mode, the runtime exports for extensions (§9.1), and the SDK's C base with its tests (§9, §9.3: `make -C kernel test_extensions`).
Not yet: `oxos_resolve_path` (§5.1), naming a crashed extension on the error screen (§3.4), the OXOS features as extensions (§10).
Sections marked *open* are decisions still to make.

A kernel extension is a DLL, file type `.xos`, that the kernel loads from the hard disk before each title and runs in kernel mode, with the kernel's own rights.
It can watch and change what titles ask of the kernel (hooks on kernel functions), act when titles start and end (events), run its own thread while a title
runs, and keep a page of state from one title to the next. The model is a Linux loadable kernel module: a file, an init and an exit function, a version check,
and the kernel's exported functions as its API.

Background used here: the call tracer's thunk wrapping (spec/oxos_internal/01 §2), the quick reboot and persisted memory (spec/stock/02 §1, spec/stock/09,
`kernel/src/arch/quick_reboot.S`), the title launch (spec/stock/05), X at boot (spec/oxos_internal/32), launch redirects and the shared ini reader
(spec/oxos_internal/33, 25 §3.1).

## 1. Terms
| Term | Meaning |
|---|---|
| extension | one `.xos` file, loaded into the kernel |
| launch | the start of one title: a cold boot's title or dashboard, or a title started through the launch data page after a quick reboot |
| generation | the number of launches since the cold boot (1 = the cold boot's title) |
| chain | the extensions hooking one kernel function, in `oxos_extensions.ini` order, ending at the kernel's own function (§7.1) |
| state page | one 4 KB page per extension that keeps its contents across launches (§6) |
| SDK | the header (with its inline helpers), import library and build files extensions are made with (§9) |

## 2. Linux, side by side
| Linux module | OXOS kernel extension |
|---|---|
| `foo.ko` in `/lib/modules/…` | `foo.xos` in `C:\OXOS\extensions\` |
| `/etc/modules-load.d`, `modprobe.d` options | `C:\OXOS\oxos_extensions.ini`: which extensions load, their order, their settings (§3.1) |
| `insmod` / `rmmod` | add / remove (or `enabled = no`) a section in `oxos_extensions.ini`; counts from the next launch |
| `module_init` / `module_exit` | `ExtensionStart` / `ExtensionTerminate` (§4) |
| module parameters | the section's other keys, read with `oxos_setting("key")` (§3.1) |
| `MODULE_LICENSE`, `MODULE_AUTHOR`, `MODULE_DESCRIPTION` | none: the descriptor has only a name and a version (§4.1, §11) |
| vermagic (refuses a module built for another kernel) | `ApiVersion` in the descriptor (§4.2) |
| `EXPORT_SYMBOL` symbols | the `oxoskrnl.exe` exports (332 functions, 34 variables), declared in `oxos_kernel.h`, called by name (§7.1) |
| `kthread_run`, a kernel timer | `ExtensionRunLoop`, called about every millisecond by the kernel's one loop thread (§4.3) |
| `printk` / `dmesg` | `oxos_log`, written to `C:\OXOS\log\extensions.log` and the debug serial port (§5.1) |
| kprobes / ftrace on syscalls | hooks on kernel functions, as titles call them (§7.1) |
| `init=/bin/sh`, `modprobe.blacklist` | hold **B** at boot: safe mode, no extensions (§3.3) |
| kbuild | the SDK's `oxos_extension.mk` (§9) |

The main difference: Linux keeps a module loaded until it is removed. OXOS **reloads every extension from the disk at each launch**, because a quick reboot
restarts the kernel (§3.2). Only the state page carries over.

## 3. Loading

### 3.1 `oxos_extensions.ini`: which extensions, in which order
**Without `C:\OXOS\oxos_extensions.ini`, no extension is loaded**, whatever is in the folder; as with `oxos_launch_redirect.ini` and the patch file, nothing
happens without the file (decided 2026-11-13).

- **The file:** `C:\OXOS\oxos_extensions.ini` (`\Device\Harddisk0\Partition2\OXOS\oxos_extensions.ini`; `/C/OXOS/oxos_extensions.ini` on an exFAT disk), text,
  at most 64 KB, read with the ini reader the other OXOS files use (`kernel/src/ini_file/ini_file.cpp`, the same format rules: sections, `key = value`, `;` and
  `#` comments, LF or CR LF).
- **The extensions:** `C:\OXOS\extensions\` (`/C/OXOS/extensions/` on exFAT). Only files named in the ini are read; other files in the folder are ignored, so an
  extension can keep its data next to it.

```ini
; Extensions load top to bottom. The first one sees a title's calls first (the outermost hook); its
; oxos_next calls go to the next one down, and the last one's to the OXOS kernel (§7.1).

; C:\OXOS\extensions\savewatch.xos
[savewatch]
extension = savewatch.xos

; kept in the list, not loaded
[cheats]
extension = cheats.xos
enabled = no

; any other key is a setting for this extension
[netlog]
extension = netlog.xos
host = 192.168.1.10
port = 9000
```

Comments go on lines of their own: as in the other OXOS ini files, a value is the rest of its line, so a `;` after a value would be part of it
(spec/oxos_internal/33 §1).

- **Sections:** one per extension, loaded **in the file's order**. The name in the brackets is the user's: it is shown in the serial log and on the error
  screen. The extension's own descriptor `Name` (§4.1) decides which state page it gets (§6), so renaming a section keeps the state.
- **`extension =`** (required): the `.xos` file's name in the extensions folder (no path, no subfolders).
- **`enabled = no`** (optional; `yes` is the default): the section is skipped, so an extension can be switched off without deleting it or its settings.
- **Settings:** every other key in the section is the extension's own. The loader copies them, as text, into pool memory for the extension, which reads them
  with `Setting("host")` (§5). At most 32 keys per section, values up to 255 characters.
- **Errors**, as in the other ini files: a line that cannot be read is skipped and counted; a section without `extension =`, a file that is missing or is not a
  valid `.xos`, a second section naming the same file: skipped and logged. The other sections still load.
- **Limits:** at most 8 extensions loaded; each file at most 256 KB; all loaded images together at most 512 KB. A section past a limit is skipped and logged.

### 3.2 When
At **every launch**, after the disk is mounted and before the title is chosen and loaded:
- cold boot: before the disc, the dashboard or a netbooted title (`XepChooseTitle`, `kernel/src/titles/title_loader.cpp`);
- quick reboot: before `titles::LaunchTitle` reads the launch data page, so an extension sees every title start (§7.2).

A quick reboot restores the kernel's `.data`, zeroes its `.bss` and stops every thread (`quick_reboot.S`). The extensions' images, threads, hooks and events are
all gone after it; the kernel reads `oxos_extensions.ini` and the files again and calls each `Start` again. Each launch therefore starts from what is on the
disk: a change to the ini or a file counts from the next launch, and a broken image in memory cannot outlive one title.

Not loaded:
- **The OXOS File Manager** (X at boot, spec/oxos_internal/32): it runs with no extensions, so a broken extension can always be switched off or deleted from it.
- **Safe mode** (§3.3).

### 3.3 Safe mode: B at boot
- Holding **B** during the boot splash, like X for the File Manager (`kernel/src/gamepad/gamepad.cpp` reads the pads between the splash's frames; B is the
  report's analog byte 5, X byte 6), starts the console in safe mode: no extension is loaded at this launch or any launch until the next cold boot.
- The flag is kept in the persisted section (`XK_PERSIST`), so it survives quick reboots and is cleared by a cold boot.
- The serial log says `extensions: safe mode (B at boot): none loaded`.
- X and B together: the File Manager (which loads none anyway).
- Limitation: without an AV pack there is no splash and no button is read (as for X today); the extensions then load. The way out is the same as for any file on
  the disk: FTP from a working title, or the disk on a PC.

### 3.4 A failing extension
- **Refused by the loader** (bad format, too large, wrong `ApiVersion`, an import other than `oxoskrnl.exe`, a missing kernel ordinal): skipped and logged; the
  others load.
- **`Start` returns FALSE:** the image and its state page are freed, its hooks and events are dropped, the others go on.
- **A crash in extension code** (in `Start`, `RunLoop`, `Terminate`, a hook or an event): the kernel's fatal error screen (spec/oxos_internal/29) names the
  extension (its section name) when the faulting address lies in an extension's image, and says "Hold B at boot to start without extensions". *Open:* also mark
  that extension off for the rest of the boot (a persisted "crashed" list), so the next launch skips it without B.

## 4. The extension's interface
An extension has up to three functions of its own and a descriptor that names them; everything else it does through the SDK's functions (§5.1) and the kernel's
(§7.1). The same code is C17 and C++20: `sdk/include/oxos_sdk.h` is written so (decided 2026-11-14: plain functions and macros, no class).

### 4.1 The descriptor
Each `.xos` exports one data symbol by name, **`oxos_extension`**, an `OXOS_EXTENSION` (at the end of `sdk/include/oxos_sdk.h`, which the kernel includes too,
without `oxos_kernel.h`). `OXOS_DEFINE_EXTENSION(...)` writes it, after the extension's functions are declared:

```c
static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
OXOS_DEFINE_EXTENSION(.Name = "example_run_loop", .Version = "1.0.0", .Start = ExtensionStart, .RunLoop = ExtensionRunLoop);
```

```c
typedef struct OXOS_EXTENSION {
    ULONG Magic;             // 'XOSX'
    ULONG ApiVersion;        // OXOS_API_VERSION of the SDK it was built with (§4.2)
    OXOS_CONTEXT **Context;  // where the kernel puts the launch's context before Start: the SDK's oxos_context (§5)
    const char *Name;        // short name, the key of its state page (§6), e.g. "netlog"
    const char *Version;     // the extension's own version, e.g. "1.2.0"
    ULONG Flags;             // none defined yet: 0
    BOOLEAN (*Start)(void);  // required: before each title; FALSE unloads the extension
    void (*RunLoop)(void);   // optional: again and again while the title runs (§4.3)
    void (*Terminate)(void); // optional: when the title starts another title or the dashboard
} OXOS_EXTENSION;
```

The macro fills `Magic`, `ApiVersion` and `Context`; the extension gives the rest, in this order (C++ wants designated fields in declaration order). The kernel
refuses a descriptor whose `Context` does not point inside the extension's own image.

### 4.2 Versions
- `OXOS_API_VERSION` is `major << 16 | minor`. The kernel loads an extension whose major equals its own and whose minor is at most its own.
- Within a major, the API only grows: new functions are added at the end of the context's tables, which carry their own size (§5), and existing ones keep their
  meaning.
- A new major is needed only to change or remove something; extensions are then rebuilt with the new SDK.

### 4.3 The order of calls in one launch
1. The loader reads `oxos_extensions.ini`, loads each enabled extension's image (§8) in order, and fills an `OXOS_CONTEXT` for it (§5).
2. **`Start()`** of each, in order, on the loading thread, before the title is loaded; the kernel has put the context in the extension's `oxos_context` first.
   Here the extension hooks kernel functions (§7.1), registers for events (§7.2) and reads its state page (§6) and settings. It returns `TRUE`, or `FALSE` to be
   unloaded (§3.4). It must return promptly: the title waits for it.
3. After every `Start`, the loader links the chains (§7.1) and is freed (§8.1).
4. **The run loop** starts, when an extension has a `RunLoop`: **one** kernel thread for all extensions, which waits 1 ms, calls the `RunLoop()` of each
   extension that has one in `oxos_extensions.ini` order, and starts again. An extension does not choose when it is called and never waits for the kernel: the
   kernel owns the timing and the stop. `RunLoop` is the place for work that would hold up the title in `Start` (a server, a timer, file I/O), a little at a
   time; it keeps its own time with `KeTickCount`. It must return quickly, since every other `RunLoop` waits for it: a call that takes over **500 ms** is logged
   as `extension NAME delayed extension loop by N ms` in `C:\OXOS\log\extensions.log` (the kernel's log files, spec/oxos_internal/35) and on the debug serial
   port. An extension with only hooks and events has no `RunLoop`; with none at all, no loop thread is started.
5. The title runs. Hooks and events are called as the title calls the kernel and as launches happen.
6. **`Terminate()`**, if set, when the title quick-reboots to start another one (`HalReturnToFirmware(2)`), before the kernel's shutdown notifications, on the
   thread that asked for the reboot. The run loop is stopped first: no `RunLoop` is called after the one in progress, which gets up to 500 ms to return when the
   title asked for the reboot at `PASSIVE_LEVEL` (a title rebooting at a raised IRQL cannot wait); one still running then is logged the same way, with the time
   it has taken so far. The state page is already in memory and needs no saving; `Terminate` is for closing files and network connections. *Open:* whether
   `Terminate` runs on a full reset or power-off (it does not today).

### 4.4 An extension
The SDK's three examples show the parts one at a time (`sdk/examples`), each named for what it shows, and each logs its start and its end: `example_run_loop` (a
`RunLoop` that keeps time, a number setting), `example_kernel_hook` (a hook on `NtOpenFile`) and `example_title_event` (a title event with user data, the state
page, and kernel functions called by name: for the title its `led_title` setting names, the event notes it and the `RunLoop` turns the front LED orange a second
later with `HalWriteSMBusValue`, after the kernel's green). A whole extension:

```c
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);
static void ExtensionTerminate(void);

OXOS_DEFINE_EXTENSION(.Name = "nodelete", .Version = "1.0.0", .Start = ExtensionStart, .Terminate = ExtensionTerminate);

static LONG Refused; // this title's refused deletes; the title may delete from several threads at once

// A hook: the title's NtDeleteFile calls come here. This one answers them itself, without passing them on.
static NTSTATUS NTAPI RefuseNtDeleteFile(POBJECT_ATTRIBUTES Name) {
    InterlockedIncrement(&Refused);
    return STATUS_ACCESS_DENIED;
}

static BOOLEAN ExtensionStart(void) {
    oxos_log("loaded, launch %lu", oxos_launch_number());
    return oxos_hook(NtDeleteFile, RefuseNtDeleteFile);
}

static void ExtensionTerminate(void) {
    oxos_log("refused %ld deletes", Refused);
}
```

## 5. The context
The kernel fills an `OXOS_CONTEXT` for each extension at each launch, in pool memory that stays valid until the title ends, and puts its address in the
extension's `oxos_context` (through the descriptor, §4.1) before `Start`. Extension code never touches it, and `oxos_sdk.h` declares it only as an opaque type:
the SDK's functions (§5.1) read it. Its layout is in `sdk/src/oxos_extension_context.h`, shared by the kernel and the SDK's library and not shipped. Everything
an extension knows about the kernel, beyond the `oxoskrnl.exe` exports, comes through it; kernel addresses found some other way change with every build.

```c
struct OXOS_CONTEXT {
    ULONG Size;                  // sizeof(OXOS_CONTEXT) of the kernel that filled it
    ULONG ApiVersion;            // the kernel's OXOS_API_VERSION
    const char *KernelVersion;   // e.g. "0.4.7"                                         oxos_kernel_version()
    ULONG Generation;            // launches since the cold boot: 1 = the cold boot's title  oxos_launch_number()
    ULONG BootFlags;             // OXOS_BOOT_COLD or OXOS_BOOT_QUICK (the memory size: Kernel->MemoryPages)

    // This extension
    const char *SectionName;      // its section in oxos_extensions.ini: oxos_log's "[name]"
    const OXOS_SETTING *Settings; // the section's other keys (§3.1)                     oxos_setting()
    ULONG SettingCount;
    PVOID ImageBase;              // where it is loaded this launch (a different address each launch is possible)
    ULONG ImageSize;
    PVOID State;                  // its state page (§6): 4096 bytes, never null         oxos_state()

    const OXOS_API *Api;            // the loader's functions (§5.1): only during Start, null afterwards
    const OXOS_KERNEL_INFO *Kernel; // read-only facts about the console and the title (§5.2)
    ULONG LogTarget;                // where oxos_log writes                              oxos_set_log_target()
};
```

### 5.1 Functions
**The SDK's** (`sdk/include/oxos_sdk.h`, in `lib/oxos_sdk.lib`, built on the kernel's exports), with C names, so C and C++ extensions call the same ones:

| Function | What it does |
|---|---|
| `oxos_log(Format, ...)` | one line, as printf formats it, prefixed with the extension's section name, to where the log target says: the kernel's debug log (the serial port of a `SERIAL=1` kernel; a release kernel keeps nothing of it), `C:\OXOS\log\extensions.log` (spec/oxos_internal/35; at `PASSIVE_LEVEL` only), or both. The kernel adds the name and sends it on (export 416, §9.1) |
| `oxos_set_log_target(Target)` | where `oxos_log` writes from then on: `OXOS_LOG_SERIAL`, `OXOS_LOG_FILE` or both; at any time. The kernel starts each extension at both (user, 2026-11-14: a release kernel has no serial output, so the serial half costs nothing there, and a debug or trace kernel prints the lines too) |
| `oxos_setting(Key)` | the value of `Key` in the extension's section of `oxos_extensions.ini` (any case), or null. The kernel owns the text, valid until the title ends |
| `oxos_setting_number(Key, Default)` | the value of `Key` as a whole decimal number, or `Default` when it is missing or does not start with a digit (user, 2026-11-14: extensions should not each parse digits) |
| `oxos_launch_number()`, `oxos_kernel_version()` | launches since the cold boot (1 = the cold boot's title); the kernel's version |
| `oxos_state()` | the state page (§6): never null, all zero at the first launch and when the extension's `Version` changes |
| `oxos_file_read(Path, Buffer, BufferSize, &Size)`, `oxos_file_write(Path, Data, Size)` | a whole file into the extension's buffer (too small: `STATUS_BUFFER_TOO_SMALL`, `Size` says how large it is), or written (created or replaced) |
| `oxos_file_open`, `oxos_file_create`, `oxos_file_delete` | the kernel's own `io::OpenFile`, `io::CreateFile`, `io::DeleteFile`, exported as they are at 429-431 (`kernel/exports.tsv`'s `kernel_symbol` column names them; 432 is `io::RenameFile`, for the SDK's renames). A handle is read and written with `NtReadFile`/`NtWriteFile` and closed with `NtClose` |
| `oxos_file_rename`, `oxos_file_exists`, `oxos_directory_create`, `oxos_directory_delete`, `oxos_directory_rename`, `oxos_directory_exists` | the SDK's, on those: by path (user, 2026-11-14: `oxos_file_*` and `oxos_directory_*` names). Every one of these opens inside the kernel: no extension's hook sees it (§7.1) |
| `oxos_file_list(Folder, Names, Count)`, `oxos_directory_list(...)`, `oxos_list_free(Names)` | the names of a folder's files, or of its folders: an array of `Count` names (`char **`, a null after the last) in one block of pool memory, which `oxos_list_free` frees (user, 2026-11-14: the usual array-and-count pattern, not callbacks; C++ references, `char **&` and `uint32_t &`, so C extensions do without them). On `NtQueryDirectoryFile`, read twice: counting, then copying |
| `oxos_hook(Function, Hook)`, `oxos_next(Function)` | hooks a kernel function, named as `oxos_kernel.h` declares it, and passes a call on (§7.1); in `ExtensionStart` only |
| `oxos_on_title_loading`, `oxos_on_title_loaded`, `oxos_on_title_exiting` `(Callback, UserData)` | registers for an event (§7.2), each with its own typed callback; in `ExtensionStart` only |
| `oxos_resolve_path(DosPath, Buffer, BufferSize)` | *planned:* `E:\Apps\x.xbe` to `\Device\Harddisk0\Partition1\Apps\x.xbe`, the kernel's drive table (spec/oxos_internal/33 §1) |

**The loader's (`OXOS_API`)**, which the SDK's hook and event functions call: a table of `NTAPI` function pointers, starting with its own `Size`. They belong to
the loader (§8.1), which is gone once every `Start` has returned, so they work **only during `Start`**: the kernel clears `Context->Api` before it frees the
loader, and the SDK's functions then return FALSE.

| Function | What it does |
|---|---|
| `HookFunction(Context, Function, Hook)` | hook the kernel function at `Function` as titles call it (§7.1) |
| `RegisterEvent(Context, Event, Callback, UserData)` | be called at a launch event (§7.2) |

**The kernel's own functions:** every `oxoskrnl.exe` export, declared in `sdk/include/oxos_kernel.h` under its own name (`NtCreateFile(...)`), with the
prototype the kernel declares (§9.2). The memory functions and the compiler's helpers are the kernel's too (§9.1).

## 6. The state page
One page, 4096 bytes, per extension, that keeps its contents from one launch to the next until the next cold boot.

- **What survives:** the page is allocated as contiguous memory and persisted (`MmPersistContiguousMemory`, PTE bit 10), so a quick reboot takes it back at the
  same physical address (spec/stock/09; `kernel/src/memory/init.cpp`, the re-adoption loop). The kernel finds the pages again through an index page, also
  persisted, whose address is in the persisted section (`XK_PERSIST`), as `LaunchDataPage` is kept (`kernel/src/titles/title_loader.cpp`).
- **Index page:** a magic number, a checksum over the index, and one entry per extension: its descriptor `Name` and `Version` and its state page's address.
  At a launch the kernel checks the magic and the checksum; if either is wrong (a cold boot, or memory written over), it starts a new, empty index.
- **Which page an extension gets:** the one under its descriptor `Name`. A new name gets a new zeroed page; a page written by another `Version` of the
  extension is zeroed again, since the new build may lay it out differently (the index keeps the version with the name). Every loaded extension has its page:
  when there is no memory for it, the extension is skipped and the serial log says so, so `oxos_state` never returns null (user, 2026-11-14: magic numbers,
  "is new" flags and null checks are the kernel's business, not the extension author's). A page whose extension is not loaded
  at a launch (removed from the ini, `enabled = no`, its start failed, or `oxos_extensions.ini` deleted) is freed then: the loader runs while state pages are
  kept, also without the ini. Only without a loader cache for this kernel (§8.1) do they stay until the next cold boot.
- **Contents:** entirely the extension's; the kernel never reads them. The kernel zeroes the page for a new `Version`, so an extension needs no magic number
  or layout version of its own; it bumps `Version` when it changes what it keeps there.
- **Pointers:** the page keeps bytes, not objects. A pointer into the extension's image, a pool block, a handle, `KEVENT` or thread from a previous launch is
  not valid in the next one (the image may load at another address, and every kernel object is gone).
- **Cold boot:** every state page starts zeroed.
- *Later (not specified yet):* the state page as a cache of a file that the extension flushes to the disk, so state also survives a power-off.

## 7. Hooks and events

### 7.1 Hooks on kernel functions
**Purpose:** to watch and change what titles do. A hook replaces a kernel function **for the title**: the title's call goes to the extension's function, which
decides what happens. `oxos_hook(NtCreateFile, MyNtCreateFile)` in `ExtensionStart` makes one, by the function itself (no numbers); the hook has exactly the
kernel function's arguments, result and calling convention as `oxos_kernel.h` declares them, or it does not compile.

**`oxos_next(Name)` is where the hook passes a call on.** A hook that wants the kernel to do the work calls `oxos_next(Name)(...)` with the (possibly changed)
arguments and returns its (possibly changed) result. A hook that does not call it has answered the call itself.

| A hook can | How |
|---|---|
| watch | log the arguments, `return oxos_next(Name)(...)`, log the result |
| change the input | change the arguments, then call `oxos_next(Name)` |
| change the output | call `oxos_next(Name)`, then change the status, the buffer or the I/O status block |
| replace | not call `oxos_next(Name)`; return its own answer (and do the kernel's job: the I/O status block, the event, the APC) |
| refuse | return an error status without calling `oxos_next(Name)` |

**Chains.** Several extensions may hook one function. The title's call goes to the first in `oxos_extensions.ini` order; its `oxos_next(Name)` goes to the next
one that hooks it; the last one's goes to the kernel:

```
title -> [savewatch] NtCreateFile -> oxos_next -> [netlog] NtCreateFile -> oxos_next -> OXOS kernel NtCreateFile
```

- A hook's call on never comes back to itself or to an extension above it, so a hook cannot loop.
- With one extension, or a function only it hooks, `oxos_next(Name)` is exactly the kernel's function.
- An extension's own calls of a kernel function by its name (from `RunLoop`, events, the SDK's functions, or a hook calling another function) follow the same
  rule: they pass the hooks of the extensions below it, then reach the kernel.

**How the loader does it** (no dispatcher stays in the kernel):
- Each extension imports the kernel's functions from `oxoskrnl.exe` by ordinal (§8). `oxos_kernel.h` declares them as C functions, so a call goes through the
  extension's own import entry.
- `oxos_hook(Name, Hook)` passes `&Name`: the extension's import of the function, which during `Start` still holds the kernel's own address. The loader's `HookFunction(Context, Function, Hook)` finds the export at that address and records the hook. The extension never sees an ordinal.
- After every `Start` (§4.3 step 3), the loader links each chain: an extension's import entry for a hooked function is pointed at the next hook below it (or left at the kernel's function), and the title's thunk for it (`titles::ResolveKernelThunks`) at the first hook. So the extension's own call of the function, which is what `oxos_next(Name)` is, goes on down the chain.
- A hooked call costs one jump (the stub) and one call more than an unhooked one.

**Rules:**
- **Titles only:** only the title's kernel imports are redirected. The kernel's own calls inside itself (reading `oxos_launch_redirect.ini`, the I/O manager
  calling a driver) are never hooked.
- **When:** hooks are made in `Start`, before the title's thunks are resolved, and hold for that whole title. A hook made later is refused. Nothing needs to be
  unhooked: the next launch starts without hooks.
- **The kernel function's prototype and calling convention:** `stdcall` (`NTAPI`), or `fastcall` (`FASTCALL`) where `kernel/exports.tsv` lists it, as
  `oxos_kernel.h` declares it; `oxos_hook` refuses any other type at compile time.
- **The kernel function's rules:** a hook runs on the title's thread, at the IRQL the title called at, and may run on several threads at once (shared data needs
  a lock or atomic operations, such as `InterlockedIncrement`). A hook on a function titles call at `DISPATCH_LEVEL` (`KeSetEvent`, `KeInsertQueueDpc`, ...)
  must not wait or call anything that does.
- **Not hookable** (`oxos_hook` returns FALSE; `HookFunction` returns `STATUS_NOT_SUPPORTED`): the 34 data exports (`KeTickCount`, `LaunchDataPage`, ...: an
  extension reads or writes them directly); `KeGetCurrentIrql`, `KfRaiseIrql`, `KfLowerIrql`, `KeRaiseIrqlToDpcLevel` (called at every IRQL, in titles'
  interrupt paths); `HalReturnToFirmware` (use the title-exiting event); OXOS's runtime exports for extensions, 402-428 (§9.1: titles never import them).
- **Kernel internals that are not exports** (`titles::LaunchTitle`, the exFAT driver, the scheduler) are not hookable; extensions reach them through events
  (§7.2) and the kernel facts (§5.2). *Later, if a use needs it:* hooks that also catch the kernel's own calls, as a separate API (patching the function's first
  instructions, with a generated trampoline).
- **The call tracer** (`TRACE_IMPORTS`, a debug build) wraps the title's thunk outside the first hook, so it records the title's calls as the title made them.

### 7.2 Events
Registered in `ExtensionStart` with `oxos_on_title_loading`, `oxos_on_title_loaded` or `oxos_on_title_exiting` (`Callback`, `UserData`); each callback gets the
event's data, typed, and the `UserData` it was registered with. Called in `oxos_extensions.ini` order.

| Event | When | The callback gets | It may |
|---|---|---|---|
| title loading (`OXOS_TITLE_LOADING`) | before the XBE is loaded: after `oxos_launch_redirect.ini` (spec/oxos_internal/33) | the XBE's path, the launch data page (type, title ID of the launching title, its data), the launch kind (cold boot, launch data page, dashboard return) | read them (version 1: replacing the XBE path is left to launch redirects, spec/oxos_internal/33) |
| title loaded (`OXOS_TITLE_LOADED`) | the XBE is loaded and its thunks resolved, before its entry point | the title ID, name (plain text: the kernel turns the certificate's UTF-16 into 41 bytes of ASCII, others as `?`; user, 2026-11-14), certificate, XBE header, base | read or patch the title's image (as `oxos_title_patches.xos` does, §10) |
| title exiting (`OXOS_TITLE_EXITING`) | the title calls `HalReturnToFirmware(2)` to start another XBE or the dashboard; the title is still in memory | the launch data page the title wrote | read it, or change the next launch |

*Open:* events for the disk (mounted, formatted), the network (link up), and the controllers.

## 8. The `.xos` file and the loader
- **PE32, i386, a DLL** (`IMAGE_FILE_DLL`), with a base relocation table: the kernel places the image where it finds memory and applies the relocations.
  Sections are mapped as the headers say; all of them are writable and executable (the CPU has no NX).
- **Imports:** only from `oxoskrnl.exe`, by ordinal, the same table titles use, resolved against the kernel's own export directory as
  `titles::ResolveKernelThunks` does for titles, then linked into the chains (§7.1). An import from any other DLL, or by name, refuses the file.
- **Exports:** `oxos_extension` by name (§4.1). Other exports are ignored. *Open:* extensions calling each other.
- **No entry point is called:** the kernel uses the descriptor, not `DllMain` (built with `/NOENTRY`).
- **No CRT, no TLS, no C++ exceptions or RTTI, no static constructors** in the first version. The SDK builds with these off (§9); the loader refuses an image
  with a TLS directory.
- **Memory:** the image is loaded into contiguous memory, not persisted (a quick reboot frees it). Its globals start from the file's values at each launch.

### 8.1 What extensions cost titles
Every loaded image, pool block, thread stack and state page is memory a title does not get. Without `oxos_extensions.ini`, or with no enabled section, titles
keep exactly the memory they had (the parity rule, spec/oxos_internal/18): the loader runs, finds nothing to load, and allocates nothing (the index page is only
created for the first extension). With extensions, titles lose their size, and a title that needs nearly all of its 64 MB can fail to start. The serial log
reports each extension's image size after its start; *open:* its pool, threads and state page too, and showing it in the File Manager.

**The loader is not resident: it is linked on its own and run from a cache file on the disk** (decided 2026-11-13):
- **Built on its own.** The loader's sources (`kernel/src/extensions/`: `extension_loader.cpp`, `extension_parser.cpp`;
  `kernel/src/ini_file/extensions_ini.cpp`; `oxos_extensions.ini`, PE loading, relocation, import binding, chains, the state-page index, `HookFunction`,
  `RegisterEvent`) are compiled like the kernel but are not part of its link. `tools/link_extension_loader.py` links them at the fixed address 0xB0000000
  (`XK_EXTENSION_LOADER_BASE`, a page directory entry nothing else uses) and gives each kernel symbol they use as an absolute symbol at this kernel's address,
  from the kernel's map: the loader calls the kernel directly. A symbol in the kernel's boot-only sections stops the build.
- **In the kernel image.** `tools/mkbios.py --extension-loader` appends it to the kernel image after the boot-only sections, behind a 20-byte header
  (`XK_EXTENSION_LOADER_HEADER`: magic, the kernel's PE stamp, base, size, entry), and names the header in the kernel's DOS header (offset 0x38). Its pages are
  boot-only: freed before the first title.
- **Cached on the disk.** At a cold boot the kernel writes it to `C:\OXOS\extensions\oxos_extension_loader.cache` (`extensions::WriteLoaderCache`, boot-only),
  always: the file is a cache, overwritten with the known-good copy at every cold boot. The kernel creates the folder at every cold boot, before it writes the
  cache (`diagnostics::CreateOxosFolders`, with `C:\OXOS` and `C:\OXOS\log`; user, 2026-11-14: ready for the first extension), so extensions copied there later
  load at the next launch without a cold boot first.
- **Run from the cache at every launch** (the cold boot's too): `extensions::LoadExtensions` (`src/extensions/ extension_runtime_loader.cpp`) reads the file
  only when `oxos_extensions.ini` exists, checks the header (magic, this kernel's stamp, base, sizes), copies the image into temporary contiguous pages, maps
  them at 0xB0000000 through a temporary page table, calls the entry, then unmaps and frees everything before the title loads. No cache, or one from another
  kernel: no extensions at that launch (logged).
- **Not from the flash.** Reading the flash while the kernel runs coincided with RAM bits changing on the Modxo console (2026-10-28, `kernel/boot/bl.cpp`), and
  some modchips hide themselves after the boot; a copy kept in RAM instead would cost about as much as the resident loader. So the flash is not read after the
  boot, and the loader is not compressed (nothing extra goes into the flash).
- **Netboot:** the image the 2BL downloads holds the loader like a flashed one; the cold boot writes the cache.
- **Resident in the kernel** stays what runs while the title runs (`extension_runtime.cpp`: the hooked imports, the three events, the end of a title, the run
  loop thread) and the loader's runner (`extension_runtime_loader.cpp`): about 1.1 KB. To keep every page titles had, the cold boot's launch choice and the
  netboot title's installation moved to the boot-only pages in the same change (spec/oxos_internal/18 §2). Checked: the memory test title starts with 15 960
  free pages (xemu, 64 MB), as before kernel extensions.

## 9. The SDK: building an extension
Extension writers do not need nxdk. nxdk's runtime (its C library, `winapi`, heaps, threads, SDL) is made for a title: it allocates title memory, sets up
title-only state, and is torn down at the quick reboot. None of it is safe in the kernel. An extension needs much less, and what it needs is the kernel's.

- **One API for C and C++** (decided 2026-11-14): plain functions and macros, every header valid C17 and C++20. Nothing an extension writes depends on the
  context or the loader's tables; the SDK keeps those (§5).
- **Windows, macOS and Linux alike:** an extension needs only LLVM's `clang` and `lld-link` (18 or newer) and GNU make. The library comes prebuilt, and the
  build rules run no other command, so they work from `cmd.exe`, PowerShell, bash or zsh. No CMake (decided 2026-11-13).
- **The same bytes everywhere:** objects without time stamps and `/Brepro` links, so a source builds the same `.xos` on any system.
- **Its own repository later:** `sdk/` is self-contained (it never reads the kernel's sources; what it takes from them is generated into it and committed), so
  it can move to its own repository as it is; the kernel then takes `oxos_sdk.h` and `src/oxos_extension_context.h` from it (decided 2026-11-13: later).

**What a release ships: `oxos_sdk_vX.Y.Z.zip`** (MIT, §11), next to `oxos_vX.Y.Z.zip`; `make -C kernel release VERSION=X.Y.Z` makes it with `sdk/Makefile`:

```
oxos_sdk/
  include/oxos_sdk.h      what an extension includes, C and C++: its descriptor, the SDK's functions (§5.1), the title events' data
  include/oxos_kernel.h   the Xbox kernel: every function an extension can call or hook, with its types (§9.2)
  lib/oxos_sdk.lib        the one library: the SDK's functions and the kernel's import library
  oxos_extension.mk       the build rules (optional for a build of one's own)
  examples/example_run_loop, example_kernel_hook, example_title_event
  README.md, LICENSE
```

**The SDK's source, `sdk/` in this repository** (the kernel's SDK; its files keep the `oxos_` names):

| Part | Contents |
|---|---|
| `include/oxos_sdk.h` | what an extension includes, written for its author: what an extension is, the descriptor macro, the SDK's functions in groups (logging; settings, the launch and the state page; files; hooks; title events and their data), and at the end the descriptor structure the macro needs. The kernel includes it too (without `oxos_kernel.h`: `OXOS_SDK_IN_KERNEL`), so the descriptor and the events' data have one definition |
| `include/oxos_kernel.h` | **generated** (§9.2): the base NT types and every kernel function, with the types its prototype needs |
| `src/oxos_extension_context.h` | the context the kernel fills (`OXOS_CONTEXT`), the loader's functions (`OXOS_API`), the settings and kernel facts, the event numbers: shared by the kernel and the SDK's library, not shipped |
| `src/oxos_sdk.cpp` | the SDK's functions of §5.1 (C names), built into `lib/oxos_sdk.lib` |
| `src/oxoskrnl.def` | **generated**: every `oxoskrnl.exe` export by ordinal; the import library is made from it (`lld-link /lib /def:`) |
| `src/kernel_table_check.c` | **generated**: the address of every function of `oxos_kernel.h`; the SDK's build links it against the import library, so a prototype that does not match its export fails there |
| `src/oxos_c_library.cpp` | **generated**: the kernel's `runtime/c_library_extensions.cpp`, the C functions the kernel does not have itself (§9.1), built into `lib/oxos_sdk.lib` one section per function, so an extension links only those it calls |
| `oxos_extension.mk` | the build rules extensions include (below); they take the library from the package's `lib/`, so extensions are built only with the SDK as it ships, never from its source |
| `Makefile`, `make_sdk_package.py` | build the SDK as it ships into `build/sdk/oxos_sdk/` (the library, the headers, the rest copied), check the kernel table, then build each example from that package, as a user builds it, into `build/sdk/examples/NAME/`, and with `VERSION` the zip; for the SDK's maintainers (a POSIX shell and Python) |
| `examples/example_run_loop`, `example_kernel_hook`, `example_title_event` | the examples to start from, each named for what it shows, in C++ (like all of OXOS; C extensions are supported, not shown), each with its Makefile, a sample `oxos_extensions.ini` and a README |

Nothing built is committed: the library exists only in `build/sdk/` and the release zip.

### 9.1 What the kernel provides
Extensions call the kernel for everything a freestanding program needs, as Linux modules and Windows drivers do; the SDK carries only the C functions the
kernel does not use itself (417-428 below). The Xbox kernel never exported these (titles link their own C library), so OXOS exports them at ordinals of its
own, after its other exports (400-401, spec/oxos_internal/30 §1: the disk format and partition delete, for the dashboard's tools, which `oxos_kernel.h` does not
declare for extensions). Titles never import them; 402-428 cannot be hooked (§7.1).

| Ordinals | Exports | Why |
|---|---|---|
| 402-405 | `memcpy`, `memset`, `memmove`, `memcmp` (`cdecl`) | the compiler calls them for copies and zeroing, even with `-fno-builtin` |
| 406-415 | `_allmul`, `_alldiv`, `_aulldiv`, `_allrem`, `_aullrem`, `_alldvrm`, `_aulldvrm`, `_allshl`, `_allshr`, `_aullshr` | the compiler's 64-bit arithmetic helpers for the `i386-pc-win32` target, each with its own register convention (`kernel/src/arch/int64.S`) |
| 417-428 | `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strlcpy`, `toupper`, `isdigit`, `memchr`, `snprintf`, `vsnprintf` (`cdecl`) | the C library OXOS's own code uses (`kernel/src/runtime/c_library.h`, user, 2026-11-14: "a small C runtime in the kernel, used by the kernel and by extensions alike", not the Xbox kernel's `Rtl*` functions, which are for titles). The C functions the kernel does not use itself (`strchr`, `strrchr`, `strstr`, `strlcat`, `strtol`, `strtoul`, `atoi`, `tolower`, `isspace`, `isalpha`, `isalnum`) are in `runtime/c_library_extensions.cpp`, which the SDK's library carries (`src/oxos_c_library.cpp`, generated) instead of the kernel, so the resident kernel keeps stock's size; `oxos_kernel.h` declares both groups alike |
| 416 | `OXOSWriteExtensionLog(Context, Text)` (`stdcall`) | one line of `oxos_log`, `[SECTION] Text`, to where `Context->LogTarget` says: the debug log (`diagnostics::LogLine`) and/or `C:\OXOS\log\extensions.log` (spec/oxos_internal/35); extensions name no file, so they cannot write outside it. The SDK's own: `oxos_kernel.h` leaves it out |
| 429-432 | `oxos_file_open`, `oxos_file_create`, `oxos_file_delete` and the SDK's renames' `oxos_file_rename_handle` (`cdecl`) | the kernel's own `io::OpenFile`, `io::CreateFile`, `io::DeleteFile` and `io::RenameFile`, exported as they are: `kernel/exports.tsv`'s `kernel_symbol` column names each one's C++ symbol, so the kernel's code is unchanged (user, 2026-11-14). The SDK's other file and folder functions (§5.1) are built on them |

`kernel/exports.tsv` lists them with the convention `cdecl` or `compiler`. They cost the resident kernel 4 bytes each in its export table, and `memcmp`.

### 9.2 The kernel function table (`oxos_kernel.h`)
`tools/gen_oxos_sdk.py` generates `sdk/include/oxos_kernel.h`, `sdk/src/oxoskrnl.def` and `sdk/src/kernel_table_check.c` from the kernel's own sources, so the
SDK and the kernel cannot disagree; `make -C kernel test_extensions` regenerates them, and they are committed with the change that changes them.

- **Prototypes:** each exported function as the kernel declares it (its `XBAPI` declarations and definitions in `kernel/src`; for the few written in assembly,
  the prototype in the comment above them), with the calling convention of `kernel/exports.tsv`. All 330 function exports below 400 and OXOS's own 400-401.
- **Types:** the base NT types and the structures a file call takes (`OBJECT_ATTRIBUTES`, `IO_STATUS_BLOCK`, `UNICODE_STRING`, ...) are written out; the
  kernel's callback types (`PKDEFERRED_ROUTINE`, ...) are copied from its headers; kinds and counts passed by value (`EVENT_TYPE`, `KPRIORITY`, ...) are 32-bit
  integers; every other kernel object (`KTHREAD`, `IRP`, `DEVICE_OBJECT`, ...) is an opaque structure the functions take pointers to.
- **No ordinals:** the header names functions only; `oxos_hook` finds a function by its address (§7.1).
- **Checked:** `kernel_table_check.c` takes every function's address; the SDK's build links it against the import library, so a prototype whose arguments do not
  add up to its export's size (`_Name@N`) fails the build.

**Build:** GNU make with the SDK's rules; `clang` and `lld-link` from the `PATH`, or from the folder `LLVM=` names:

```make
# examples/example_run_loop/Makefile
NAME := example_run_loop
SOURCES := example_run_loop.cpp
include ../../oxos_extension.mk
# oxos_extension.mk: clang --target=i386-pc-win32 -march=pentium -mno-sse -mno-mmx -mno-80387 -ffreestanding
#                    -nostdinc -fno-builtin -O2 -std=c17 (.c) or -std=c++20 -fno-exceptions -fno-rtti (.cpp) ...
#                    (no floating point or SIMD: the kernel does not save those registers for extensions);
#                    lld-link /DLL /DRIVER /NOENTRY /NODEFAULTLIB /OUT:example_run_loop.xos ... oxos_sdk.lib
```

The output folder (`OXOS_OUT`, default the current one) must exist; the rules create no folders, so they need no shell commands. Sources are in the Makefile's
folder; their objects go to the output folder, named after the extension.

**Install:** copy `example_run_loop.xos` to `C:\OXOS\extensions\` (File Manager or FTP) and add it to `C:\OXOS\oxos_extensions.ini`:

```ini
[example_run_loop]
extension = example_run_loop.xos
interval = 10
```

It runs from the next launch.

### 9.3 Test extensions
`tests/extensions/`, built with the SDK as it ships; `make -C kernel test_extensions` (`tests/boot_test.py --extensions`) puts them, the SDK's three examples
and an `oxos_extensions.ini` on the test disk and runs `test_relaunch`'s title through a cold boot and two quick reboots. The extensions record what the kernel
did in their state pages; the test reads those back through `extensions::LoadedExtensions` (the offsets are checked at compile time in
`src/extensions/extension_state.h`), and the log lines in `C:\OXOS\log\extensions.log` from the disk.

**`extension_test.xos`:**

| It does | It proves |
|---|---|
| `ExtensionStart`: at the first launch (an all-zero state page) writes its magic, which the test finds it by; counts its starts and keeps `oxos_launch_number()` | the state page survives quick reboots: 3 starts, launch 3, never a mismatch |
| hooks `NtOpenFile` and counts the title's calls, passing each on with `oxos_next` | the title's calls go through the hook, and the title still works |
| registers the three events, each with its own counter as `UserData`, and counts them | a loading and a loaded title per launch, an exit per quick reboot |
| `ExtensionRunLoop`: one `NtOpenFile` of its own at its first call, then counts its calls, and any after `ExtensionTerminate` | `RunLoop` is called while the title runs and never after `Terminate`; its own call passes only the extensions below it |
| `ExtensionTerminate` counts | `Terminate` at each quick reboot |
| `oxos_setting("Answer")` is `42`; `oxos_file_read` finds its own `.xos` too large for 2 bytes; at the first launch a folder in `C:\OXOS` with a file and a folder in it, created, written, read, listed, renamed and deleted | settings, the SDK's file and folder functions, the kernel imports |

**Companions:**
- `chain.xos`, listed after `extension_test`: hooks the same `NtOpenFile` and counts: the title's calls (through extension_test's hook, then this one) and
  extension_test's own from `RunLoop` (this one only).
- `plain_c.xos`, written in C: counts its starts and loaded titles and reads its setting, so the SDK's header, macros and functions work without C++; checks a
  struct copy, `memset`, `memcmp` and signed and unsigned 64-bit division and remainder at each start, which the compiler turns into calls to the kernel's
  exports 402-410 (§9.1).
- The SDK's three examples, built from the package, so they keep building and working: `example_run_loop` (with `interval = 1`: "1 s into this title.",
  and how long each title ran at its end), `example_kernel_hook` (its start line at every launch, in `extensions.log` and on the serial port: `oxos_log`'s
  default; its `NtOpenFile` count and end line at each title's end), `example_title_event` (its state page counts the launches; each title's name, and the one
  before it; with `led_title` naming the test title in another case, its "the LED is orange" line).
- `slow_loop.xos`, listed last: its first `RunLoop` call after the cold boot takes 600 ms. `C:\OXOS\log\extensions.log` must then hold exactly one line,
  `extension slow_loop delayed extension loop by N ms` with 500 < N < 2000, and the loop must go on calling `RunLoop` in the later launches. The cold boot's
  test title may end before the 600 ms are up, so the line comes either from the call's return or from the end of the title (both paths of §4.3).
- `failload.xos`: hooks `NtOpenFile`, then returns FALSE from its start: unloaded, its hook dropped (a title call through it would run freed memory), its state
  page freed.
- `badversion.xos`: built for the next major API version: refused.
- `disabled.xos` (`enabled = no`) and `unlisted.xos` (in the folder, not in the list): neither loads.

*Not yet automated:* safe mode (B at the splash), an extension that crashes (the error screen naming it, §3.4), and state pages freed when `oxos_extensions.ini`
is deleted between launches (§6). Without `oxos_extensions.ini` nothing loads: `make test_memory` checks that titles keep 15 960 free pages.

## 10. OXOS features as extensions
Decided 2026-11-13. What runs only for some users, or only on development consoles, leaves the kernel and becomes an extension; what must always work stays in
the kernel.

**Stays in the kernel:** launch redirects (spec/oxos_internal/33: guaranteed to work with only `oxos_launch_redirect.ini`, also in safe mode) and therefore the
ini reader (`ini_file.cpp`, which `oxos_extensions.ini` and the loader share); the trainer hook instruction (spec/oxos_internal/23: dashboards search the kernel
image for it); the in-game reset; the title compatibility answers (spec/oxos_internal/25 §1-2: games need them to run at all); virtual discs; the error screens;
the File Manager (the way out of a broken extension).

**Names (decided 2026-11-13):** every extension OXOS provides is `C:\OXOS\extensions\oxos_NAME.xos`, its section in `oxos_extensions.ini` is `[oxos_NAME]`, and
a file of its own data is `C:\OXOS\oxos_NAME.ini`. Extensions from others use any other name; the `oxos_` prefix is OXOS's. Kernel features keep their files
(`oxos_launch_redirect.ini`).

```ini
; oxos_extensions.ini for the title patches (in the trainers zip)
[oxos_title_patches]
extension = oxos_title_patches.xos
```

| Feature | Today | As an extension | Needs from §5/§7 |
|---|---|---|---|
| **Title patches** (`oxos_title_patches.ini`, spec/oxos_internal/25 §3; `title_patcher_ini.cpp`, `titles::LoadPatches`) | in the kernel, 1 704 bytes resident plus the records | `oxos_title_patches.xos`, shipped in the trainers zip with its patch file, **`oxos_title_patches.ini`** (the kernel's own file today, renamed in v0.4.7; user, 2026-11-14), and an `oxos_extensions.ini` that lists it; reads `C:\OXOS\oxos_title_patches.ini` in the title-loaded event | the title-loaded event (the title's ID, version, section headers) patches the preloaded sections; a hook on `XeLoadSection` (327) patches a section after `oxos_next(XeLoadSection)` has read it |
| **Netbooted title** (`title_netboot.cpp`, spec/oxos_internal/09 §7) | in the kernel, 981 bytes resident | `oxos_nettitle.xos`; installs the package as `E:\NAME\` and replaces the launch's XBE path with it | the netboot package's address and size in the kernel facts (§5.2; the kernel still keeps those pages from the memory manager); the title-loading event replaces the path |
| **Call tracer** (`title_tracer.cpp`, `trace.S`, spec/oxos_internal/01) | a separate kernel build, `TRACE_IMPORTS=1/2` | `oxos_tracer.xos`, listed first in `oxos_extensions.ini` so it sees the title's calls first; release kernels can be traced | `oxos_hook` with its own assembly stubs for every hookable ordinal (the stubs keep the registers and jump to `Next`); its ring outside the memory manager at a fixed physical address with 128 MB, as `TRACE_IMPORTS=2` does |

- **Netboot packages carry extensions:** a development console should not need a prepared disk. `netboot.py` adds `.xos` files to the title package
  (`--extension FILE`, and `oxos_nettitle.xos` by itself with `--title`); the loader loads the package's extensions first, in the order given, in addition to
  `oxos_extensions.ini` and also without it. Only a netbooted kernel does this.
- **Consequences, accepted:** title patches and trainers' patching need `oxos_title_patches.xos` installed and listed; safe mode (B) turns them off with the
  other extensions. The kernel's tracer builds (`TRACE_IMPORTS`) are retired once `oxos_tracer.xos` records the same.
- **Memory:** about 2.7 KB less resident kernel in release builds (patches and the netboot title), which also pays for the extensions' resident code (§8.1).

### 10.1 Proposed extensions (not decided)
- **GDB remote debugging (`gdbstub.xos`)**: debug a title (an nxdk game netbooted with its `.xos`, §10) from `gdb` on a PC with the GDB remote protocol:
  breakpoints, single steps, hardware watchpoints (DR0-DR7), reading and writing memory and registers, the title's threads.
  - Needs a new kernel interface: **exception callbacks**: first chance at breakpoint (`int 3`, `STATUS_BREAKPOINT`), single-step and debug-register exceptions
    and faults, with the trap frame, able to handle one and resume (`kernel/src/core/exceptions.cpp` today ends them in the debug service or the fatal screen).
    While stopped, the stub keeps the processor in its handler and polls its connection, as a kernel debugger does, so every thread of the title stands still.
  - **Connection:** the serial port first (debug kits, the Modxo's COM1 and other LPC serial ports; polled, no sharing). Over the network only for titles that
    do not use the network themselves: an nxdk title drives the network card with its own lwIP driver, and two drivers on one card do not work. *Open:* a way to
    share it.
  - Symbols: the title built with DWARF (`lld-link /DEBUG:DWARF`); its addresses are the XBE's, so `gdb` loads the `.exe` as it is.
- **Screenshots (`screenshot.xos`)**: a controller combination saves the screen the console shows as a file on the disk.
  - Needs a new event: **`OXOS_EVENT_CONTROLLER`**, every controller state a title reads, from the hook the in-game reset already has on `XInputGetState`
    (spec/oxos_internal/22 §2).
  - The event only sets a flag; the next `RunLoop` reads the front buffer (its address, pitch and format from the GPU's display registers) and writes a BMP or
    PNG to `E:\Screenshots\` a part per call, so the loop is never held up, while the title goes on.
- Others: a network log of a title's file and network calls, cheats (memory search and writes, on top of the trainers), save-game redirection or backup, a
  background FTP server for titles that do not use the network.

## 11. Decisions
- **Licence (2026-11-13).** The kernel stays GPL-2.0. The SDK (header, `oxos_sdk.lib`, build files, examples) is open source under the **MIT** licence
  (`sdk/LICENSE`), so an extension built with it may use any licence: open source is preferred, closed source is allowed. (A GPL SDK would make every extension
  that links `oxos_sdk.lib` GPL too, which is why the SDK is not GPL.) The SDK is written for it, not copied from GPL kernel sources.
- **The descriptor has no author, description or licence (2026-11-13):** nothing uses them. It names the extension (its state page) and its version (the log),
  and points at its functions (§4.1).
- **Trust (2026-11-13):** every enabled extension in `oxos_extensions.ini` is loaded and run, with no signing and no confirmation, as XBEs are today. The ways
  out are the File Manager (X) and safe mode (B).
- **`oxos_extensions.ini` is required (2026-11-13):** without it nothing loads; it gives the order (and so the chains), switches and settings (§3.1).
- **Hooks are for titles' calls (2026-11-13):** an extension hooks kernel functions by name, and passes a call on to OXOS (§7.1).
- **The loader is linked on its own and run from a cache file on the disk (2026-11-13)**, not from the flash (§8.1).
- **The SDK (2026-11-13):** `sdk/` (named `oxos_sdk/` until 2026-11-14); plain C as the base with C++ on top, so extensions may be written in C; the SDK's own
  code and its example are C++, and `tests/extensions/plain_c.c` is the only C source (2026-11-14). It ships as one header and one prebuilt library
  (`oxos_sdk.h`, `oxos_sdk.lib`) in `oxos_sdk_vX.Y.Z.zip` with each release; GNU make only, on Windows, macOS and Linux; no nxdk; its own repository later (§9).
  The kernel exports `memcpy` and its kin and the compiler's 64-bit helpers, as Windows and Linux kernels do (§9.1). Names: `OXOS_` for types and macros,
  `oxos_` for functions and files, never `Oxos` (docs/cpp_style.md §2).
- **A failed start frees its state page (2026-11-13)** (§3.4).
- **One run loop instead of a thread per extension (2026-11-14):** `Main` on its own thread trusted every extension to watch a stop event; one that did not
  could still be running when its image was freed. Now the kernel's one loop thread calls each `RunLoop` in `oxos_extensions.ini` order, 1 ms apart, and owns
  the timing and the stop; extensions do not choose an interval. A call over 500 ms is logged in `C:\OXOS\log\extensions.log` (§4.3). `StopEvent` and
  `oxos_wait_for_stop` are gone (extensions were not yet in a release, so the API version stayed 1.0).
- **A simpler SDK (2026-11-14, user):** one API for C and C++, without the planned C++ class: an extension writes `ExtensionStart` (TRUE or FALSE),
  `ExtensionRunLoop` and `ExtensionTerminate`, with no arguments, names them in `OXOS_DEFINE_EXTENSION` near the top of its file, and calls the SDK's functions
  without a context (the kernel puts it in the SDK's `oxos_context` through the descriptor). Hooks with `oxos_hook(Name, Hook)` and `oxos_next(Name)`,
  type-checked against the kernel's prototype and found by the function itself, with no ordinals in the SDK; events with typed callbacks and user data. The
  SDK's functions live in `lib/oxos_sdk.lib` (they were inline in the header for a day), and every kernel function is in the generated `oxos_kernel.h`, checked
  at link time (§9.2). Two shipped headers only, `oxos_sdk.h` and `oxos_kernel.h`: the context's layout stays in the SDK's source. Small examples named for
  what they show (example_run_loop, example_kernel_hook, example_title_event) instead of one that did everything; example_start was dropped (user,
  2026-11-14): every example has an `ExtensionStart`. The title-loaded event gives the title's name as plain text, and `oxos_setting_number` reads a number, so
  extensions do neither conversion themselves.

**Open:** a crashed extension's persisted "off" mark (§3.4), `Terminate` on a reset (§4.3), the kernel facts list (§5.2), more events (§7.2), extensions calling
each other (§8), the proposals of §10.1.
