# 34. Kernel extensions (OXOS's own)

Status: **being implemented, 2026-11-13.** In the kernel: the loader (linked on its own, run from a disk cache, §8.1),
hooks, events, Main and Unload, state pages, safe mode, the runtime exports for extensions (§9.1), and the SDK's C base
with its tests (§9, §9.3: `make -C kernel test_extensions`). Not yet: the SDK's C++ class and complete kernel function
table (§4.4, §9.2), `oxos_write_file` and `oxos_resolve_path` (§5.1), naming a crashed extension on the error screen
(§3.4), the OXOS features as extensions (§10). Sections marked *open* are decisions still to make.

A kernel extension is a DLL, file type `.xos`, that the kernel loads from the hard disk before each title and runs
in kernel mode, with the kernel's own rights. It can watch and change what titles ask of the kernel (hooks on kernel
functions), act when titles start and end (events), run its own thread while a title runs, and keep a page of state
from one title to the next. The model is a Linux loadable kernel module: a file, an init and an exit function, a
version check, and the kernel's exported functions as its API.

Background used here: the call tracer's thunk wrapping (spec/oxos_internal/01 §2), the quick reboot and persisted
memory (spec/stock/02 §1, spec/stock/09, `kernel/src/arch/quick_reboot.S`), the title launch (spec/stock/05),
X at boot (spec/oxos_internal/32), launch redirects and the shared ini reader (spec/oxos_internal/33, 25 §3.1).

## 1. Terms
| Term | Meaning |
|---|---|
| extension | one `.xos` file, loaded into the kernel |
| launch | the start of one title: a cold boot's title or dashboard, or a title started through the launch data page after a quick reboot |
| generation | the number of launches since the cold boot (1 = the cold boot's title) |
| chain | the extensions hooking one kernel function, in `oxos_extensions.ini` order, ending at the kernel's own function (§7.1) |
| state page | one 4 KB page per extension that keeps its contents across launches (§6) |
| SDK | the headers, import library, small runtime and build files extensions are made with (§9) |

## 2. Linux, side by side
| Linux module | OXOS kernel extension |
|---|---|
| `foo.ko` in `/lib/modules/…` | `foo.xos` in `C:\OXOS\extensions\` |
| `/etc/modules-load.d`, `modprobe.d` options | `C:\OXOS\oxos_extensions.ini`: which extensions load, their order, their settings (§3.1) |
| `insmod` / `rmmod` | add / remove (or `enabled = no`) a section in `oxos_extensions.ini`; counts from the next launch |
| `module_init` / `module_exit` | `Load` / `Unload` (§4) |
| module parameters | the section's other keys, read with `Setting("key")` (§3.1) |
| `MODULE_LICENSE`, `MODULE_AUTHOR`, `MODULE_DESCRIPTION` | none: the descriptor has only a name and a version (§4.1, §11) |
| vermagic (refuses a module built for another kernel) | `ApiVersion` in the descriptor (§4.2) |
| `EXPORT_SYMBOL` symbols | the `xboxkrnl.exe` exports (332 functions, 34 variables), called as `oxos::Name` (§7.1) |
| `kthread_run` | `Main`, started on a system thread (§4.3) |
| `printk` / `dmesg` | `Log`, written to the debug serial port (§5.1) |
| kprobes / ftrace on syscalls | hooks on kernel functions, as titles call them (§7.1) |
| `init=/bin/sh`, `modprobe.blacklist` | hold **B** at boot: safe mode, no extensions (§3.3) |
| kbuild | the SDK's `oxos_extension.mk` (§9) |

The main difference: Linux keeps a module loaded until it is removed. OXOS **reloads every extension from the disk
at each launch**, because a quick reboot restarts the kernel (§3.2). Only the state page carries over.

## 3. Loading

### 3.1 `oxos_extensions.ini`: which extensions, in which order
**Without `C:\OXOS\oxos_extensions.ini`, no extension is loaded**, whatever is in the folder; as with
`oxos_launch_redirect.ini` and the patch file, nothing happens without the file (decided 2026-11-13).

- **The file:** `C:\OXOS\oxos_extensions.ini` (`\Device\Harddisk0\Partition2\OXOS\oxos_extensions.ini`;
  `/C/OXOS/oxos_extensions.ini` on an exFAT disk), text, at most 64 KB, read with the ini reader the other OXOS files
  use (`kernel/src/ini_file/ini_file.cpp`, the same format rules: sections, `key = value`, `;` and `#` comments, LF or
  CR LF).
- **The extensions:** `C:\OXOS\extensions\` (`/C/OXOS/extensions/` on exFAT). Only files named in the ini are read;
  other files in the folder are ignored, so an extension can keep its data next to it.

```ini
; Extensions load top to bottom. The first one sees a title's calls first (the outermost hook); its
; oxos:: calls go to the next one down, and the last one's to the OXOS kernel (§7.1).

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

Comments go on lines of their own: as in the other OXOS ini files, a value is the rest of its line, so a `;` after a
value would be part of it (spec/oxos_internal/33 §1).

- **Sections:** one per extension, loaded **in the file's order**. The name in the brackets is the user's: it is
  shown in the serial log and on the error screen. The extension's own descriptor `Name` (§4.1) decides which state
  page it gets (§6), so renaming a section keeps the state.
- **`extension =`** (required): the `.xos` file's name in the extensions folder (no path, no subfolders).
- **`enabled = no`** (optional; `yes` is the default): the section is skipped, so an extension can be switched off
  without deleting it or its settings.
- **Settings:** every other key in the section is the extension's own. The loader copies them, as text, into pool
  memory for the extension, which reads them with `Setting("host")` (§5). At most 32 keys per section, values up to
  255 characters.
- **Errors**, as in the other ini files: a line that cannot be read is skipped and counted; a section without
  `extension =`, a file that is missing or is not a valid `.xos`, a second section naming the same file: skipped and
  logged. The other sections still load.
- **Limits:** at most 8 extensions loaded; each file at most 256 KB; all loaded images together at most 512 KB. A
  section past a limit is skipped and logged.

### 3.2 When
At **every launch**, after the disk is mounted and before the title is chosen and loaded:
- cold boot: before the disc, the dashboard or a netbooted title (`XepChooseTitle`,
  `kernel/src/titles/title_loader.cpp`);
- quick reboot: before `titles::LaunchTitle` reads the launch data page, so an extension sees every title start (§7.2).

A quick reboot restores the kernel's `.data`, zeroes its `.bss` and stops every thread (`quick_reboot.S`). The
extensions' images, threads, hooks and events are all gone after it; the kernel reads `oxos_extensions.ini` and the
files again and calls `Load` again. Each launch therefore starts from what is on the disk: a change to the ini or a file
counts from the next launch, and a broken image in memory cannot outlive one title.

Not loaded:
- **The OXOS File Manager** (X at boot, spec/oxos_internal/32): it runs with no extensions, so a broken extension can
  always be switched off or deleted from it.
- **Safe mode** (§3.3).

### 3.3 Safe mode: B at boot
- Holding **B** during the boot splash, like X for the File Manager (`kernel/src/gamepad/gamepad.cpp` reads the pads
  between the splash's frames; B is the report's analog byte 5, X byte 6), starts the console in safe mode: no
  extension is loaded at this launch or any launch until the next cold boot.
- The flag is kept in the persisted section (`XK_PERSIST`), so it survives quick reboots and is cleared by a cold boot.
- The serial log says `extensions: safe mode (B at boot): none loaded`.
- X and B together: the File Manager (which loads none anyway).
- Limitation: without an AV pack there is no splash and no button is read (as for X today); the extensions then
  load. The way out is the same as for any file on the disk: FTP from a working title, or the disk on a PC.

### 3.4 A failing extension
- **Refused by the loader** (bad format, too large, wrong `ApiVersion`, an import other than `xboxkrnl.exe`, a
  missing kernel ordinal): skipped and logged; the others load.
- **`Load` returns an error status:** the image and its state page are freed, its hooks and events are dropped, the
  others go on.
- **A crash in extension code** (in `Load`, `Main`, a hook or an event): the kernel's fatal error screen
  (spec/oxos_internal/29) names the extension (its section name) when the faulting address lies in an extension's
  image, and says "Hold B at boot to start without extensions". *Open:* also mark that extension off for the rest of
  the boot (a persisted "crashed" list), so the next launch skips it without B.

## 4. The extension's interface
Extensions are written in **plain C** or as a **C++ class** with the SDK. C is the base: the descriptor, the context
and the kernel's functions below are plain C, and an extension in C fills them itself (§4.5). The C++ class (§4.4) is
built on top and generates the same descriptor and calls for the author.

### 4.1 The descriptor
Each `.xos` exports one data symbol by name, **`oxos_extension`**, an `OXOS_EXTENSION`. The SDK writes it:
`OXOS_DEFINE_EXTENSION(...)` in C (§4.5), `OXOS_DEFINE_EXTENSION_CLASS(Class, ...)` for the C++ class (§4.4).

```cpp
struct OXOS_EXTENSION {
    ULONG Magic;                 // 'XOSX'
    ULONG ApiVersion;            // OXOS_API_VERSION of the SDK it was built with (§4.2)
    const char *Name;            // short name, the key of its state page (§6), e.g. "netlog"
    const char *Version;         // the extension's own version, e.g. "1.2.0"
    ULONG Flags;                 // OXOS_FLAG_*, none defined yet: 0
    NTSTATUS (NTAPI *Load)(OXOS_CONTEXT *Context);   // required (the SDK generates it, §4.4)
    void (NTAPI *Main)(OXOS_CONTEXT *Context);       // optional: run on its own thread
    void (NTAPI *Unload)(OXOS_CONTEXT *Context);     // optional
};
```

### 4.2 Versions
- `OXOS_API_VERSION` is `major << 16 | minor`. The kernel loads an extension whose major equals its own and whose
  minor is at most its own.
- Within a major, the API only grows: new functions are added at the end of the context's tables, which carry their
  own size (§5), and existing ones keep their meaning.
- A new major is needed only to change or remove something; extensions are then rebuilt with the new SDK.

### 4.3 The order of calls in one launch
1. The loader reads `oxos_extensions.ini`, loads each enabled extension's image (§8) in order, and fills an
   `OXOS_CONTEXT` for it.
2. **`Load(Context)`** of each, in order, on the loading thread, before the title is loaded. Here the extension
   hooks kernel functions (§7.1), registers for events (§7.2) and reads its state page (§6) and settings. Returns
   `STATUS_SUCCESS`, or an error to be unloaded (§3.4). It must return promptly: the title waits for it.
3. After every `Load`, the loader links the chains (§7.1) and is freed (§8.1).
4. **`Main(Context)`** of each that has one, on a new system thread (`PsCreateSystemThreadEx`). It runs while the
   title runs, and is the place for a server, a timer loop or file I/O that would hold up the title in `Load`. It
   returns when `Context->StopEvent` (§5) is signalled.
5. The title runs. Hooks and events are called as the title calls the kernel and as launches happen.
6. **`Unload(Context)`**, if set, when the title quick-reboots to start another one (`HalReturnToFirmware(2)`),
   before the kernel's shutdown notifications, on the thread that asked for the reboot. `StopEvent` is signalled
   first. The state page is already in memory and needs no saving; `Unload` is for closing files and network
   connections. The kernel waits up to 500 ms for each `Main` to return, when the title asked for the reboot at
   `PASSIVE_LEVEL` (a title rebooting at a raised IRQL cannot wait); a `Main` that has not returned is logged.
   *Open:* whether `Unload` runs on a full reset or power-off (it does not today).

### 4.4 The extension class (SDK)
The author derives from `oxos::Extension<T>` and writes **only what the extension needs**. Every member function
below is optional: one that is not written does not exist in the extension, and the kernel never calls it. A kernel
function that is not written is not hooked.

```cpp
#include <oxos_sdk.h>

class SaveWatch : public oxos::Extension<SaveWatch> {
public:
    // Lifecycle (§4.3)
    NTSTATUS Load() {
        Log("loaded, launch %lu, log to %s", Generation(), Setting("host"));
        return STATUS_SUCCESS;
    }
    void Main() {
        while (!WaitForStop(1000)) // every second until the reboot
            Log("still here");
    }
    void Unload() {}

    // Events (§7.2)
    void OnTitleLoaded(const oxos::Title &title) {
        Log("title %08lx %s", title.Id, title.Name);
    }

    // Kernel functions, by their own name and arguments: hooked for the title (§7.1)
    NTSTATUS NtCreateFile(PHANDLE Handle, ACCESS_MASK Access, POBJECT_ATTRIBUTES Attributes,
                          PIO_STATUS_BLOCK Iosb, PLARGE_INTEGER Size, ULONG FileAttributes,
                          ULONG Share, ULONG Disposition, ULONG Options) {
        Log("open %Z", Attributes->ObjectName);
        return oxos::NtCreateFile(Handle, Access, Attributes, Iosb, Size, FileAttributes,
                                  Share, Disposition, Options);  // pass it on to OXOS
    }
    NTSTATUS NtDeleteFile(POBJECT_ATTRIBUTES Attributes) {
        Log("delete refused %Z", Attributes->ObjectName);
        return STATUS_ACCESS_DENIED;                             // answered here: OXOS never sees it
    }
};

OXOS_DEFINE_EXTENSION_CLASS(SaveWatch, .Name = "savewatch", .Version = "1.0.0");
```

- **`OXOS_DEFINE_EXTENSION_CLASS(Class, ...)`** writes the descriptor and a generated `Load`. For each hookable
  kernel function in the SDK's table (§9), a C++20 check asks whether the class has a member function of that name:
  - **no:** nothing is generated, the function is not hooked;
  - **yes, with the kernel's arguments and result:** the generated `Load` hooks it (`HookExport`, §7.1) with a
    small stub that has the kernel function's calling convention and calls the member;
  - **yes, with other arguments:** a compile error that names the function and the arguments it should have.
  Then it calls the class's own `Load()`, if there is one.
- **The class is one global object, never constructed** (§8: no static constructors): plain members only, set up in
  `Load()`. The SDK refuses a class that is not trivially constructible.
- **Helpers in the base class:** `Log(Format, ...)`, `Generation()`, `IsColdBoot()`, `Setting(Key)` (the section's
  value, or `nullptr`), `State<S>()` (the state page as the extension's own struct, §6), `StateIsNew()`,
  `WaitForStop(Milliseconds)` (true once `StopEvent` is signalled), `ReadFile`, `WriteFile`, `ResolvePath` (§5.1),
  `Context()` for the raw `OXOS_CONTEXT`. They call the SDK's C functions (§5.1).
- **The class lives in namespace `oxos`** with the kernel's functions (`oxos::Extension`, `oxos::Title`,
  `oxos::NtCreateFile`): one namespace for everything the SDK gives C++.

### 4.5 An extension in plain C
The same extension without the class: the author writes `Load` (and `Main`, `Unload`, if wanted), registers hooks and
events through `Context->Api`, and names the functions in the descriptor. A hook has a name of its own, with the
kernel function's prototype and calling convention; it passes a call on through the kernel function's own name, which
goes through the extension's import of it, as `oxos::` does in C++ (§7.1). The C helpers are `oxos_log`,
`oxos_get_setting`, `oxos_wait_for_stop`, `oxos_read_file` (§5.1).

```c
#include <oxos_sdk.h>

static NTSTATUS NTAPI WatchNtDeleteFile(POBJECT_ATTRIBUTES Attributes) {
    return STATUS_ACCESS_DENIED; // answered here: OXOS never sees it
}

static NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    oxos_log(Context, "loaded, launch %lu, log to %s", Context->Generation, oxos_get_setting(Context, "host"));
    return Context->Api->HookExport(Context, OXOS_ORDINAL_NtDeleteFile, (PVOID)WatchNtDeleteFile, NULL);
}

OXOS_DEFINE_EXTENSION(.Name = "nodelete", .Version = "1.0.0", .Load = Load);
```

## 5. The context
The kernel passes each extension its own `OXOS_CONTEXT`, in pool memory that stays valid until the title ends.
Everything an extension knows about the kernel, beyond the `xboxkrnl.exe` exports, comes through it. The extension
does not use kernel addresses it found some other way: they change with every build.

```cpp
struct OXOS_CONTEXT {
    ULONG Size;                  // sizeof(OXOS_CONTEXT) of the kernel that filled it
    ULONG ApiVersion;            // the kernel's OXOS_API_VERSION
    const char *KernelVersion;   // e.g. "0.4.7"
    ULONG Generation;            // launches since the cold boot: 1 = the cold boot's title (§1)
    ULONG BootFlags;             // OXOS_BOOT_COLD or OXOS_BOOT_QUICK (the memory size: Kernel->MemoryPages)

    // This extension
    const char *SectionName;     // its section in oxos_extensions.ini, e.g. "netlog"
    const OXOS_SETTING *Settings; // the section's other keys (§3.1): {Key, Value} pairs
    ULONG SettingCount;
    PVOID ImageBase;             // where it is loaded this launch (a different address each launch is possible)
    ULONG ImageSize;
    PVOID State;                 // its state page (§6)
    ULONG StateSize;             // 4096
    BOOLEAN StateIsNew;          // the page was just created (zeroed): first launch after a cold boot, or a new Name
    PVOID StopEvent;             // a KEVENT, signalled before Unload (§4.3)

    const OXOS_API *Api;          // the loader's functions (§5.1): only in Load, null afterwards
    const OXOS_KERNEL_INFO *Kernel; // read-only facts about the console and the title (§5.2)
};
```

### 5.1 Functions
**The loader's (`OXOS_API`):** a table of `NTAPI` function pointers, starting with its own `Size`. They belong to the
loader (§8.1), which is gone once every `Load` has returned, so they are **only for `Load`**: the kernel clears
`Context->Api` before it frees the loader, and a later call faults on a null pointer instead of running freed
memory. The class (§4.4) calls them for the author.

| Function | What it does |
|---|---|
| `HookExport(Context, Ordinal, Replacement, &Next)` | hook a kernel function as titles call it (§7.1) |
| `RegisterEvent(Context, Event, Callback, CallbackContext)` | be called at a launch event (§7.2) |

**The SDK's**, usable at any time (`Load`, `Main`, hooks, events): C functions in the SDK's static library
`oxos_sdk.lib`, linked into each extension and built on the kernel's exports, so the kernel keeps no code for them.
The C++ class calls them for its author.

| C function | Class method | What it does |
|---|---|---|
| `oxos_log(Context, Format, ...)` | `Log(Format, ...)` | one line to the debug serial port through `DbgPrint`, prefixed with the extension's section name (a `SERIAL=1` kernel; the others keep only the in-memory debug log) |
| `oxos_get_setting(Context, Key)` | `Setting(Key)` | the value of `Key` in the extension's section of `oxos_extensions.ini` (any case), or null |
| `oxos_wait_for_stop(Context, Milliseconds)` | `WaitForStop(Milliseconds)` | waits for the stop request (§4.3); for `Main`'s loop |
| `oxos_read_file(Path, MaxSize, &Size)` | `ReadFile(...)` | reads a whole file into pool memory (a `\Device\...` path) |
| `oxos_write_file(Path, Buffer, Size)` | `WriteFile(...)` | *planned:* writes a whole file (created or replaced) |
| `oxos_resolve_path(DosPath, Buffer, BufferSize)` | `ResolvePath(...)` | *planned:* `E:\Apps\x.xbe` to `\Device\Harddisk0\Partition1\Apps\x.xbe`, the kernel's drive table (spec/oxos_internal/33 §1) |

**The kernel's own functions:** every `xboxkrnl.exe` export, under its own name in C (`NtCreateFile(...)`) and as
**`oxos::Name`** in C++: `oxos::ExAllocatePoolWithTag`, `oxos::KeWaitForSingleObject`, `oxos::PsCreateSystemThreadEx`,
`oxos::HalReadSMBusValue`, and so on (§7.1 for what `oxos::` means for a hooked function). The memory functions and
the compiler's helpers are the kernel's too (§9.1).

### 5.2 Kernel facts (`OXOS_KERNEL_INFO`)
Read-only, filled at each launch, also starting with its `Size`: the console revision and video encoder, memory
size, the disk layout (FATX or exFAT, partitions), whether the network card is up, and, from the
`OXOS_EVENT_TITLE_LOADED` event on, the running title's ID, name, XBE path and certificate. *Open:* the exact list,
decided by the first extensions that need it. Kernel internals reach extensions only through fields added here and
functions added to `OXOS_API`, never as raw addresses.

## 6. The state page
One page, 4096 bytes, per extension, that keeps its contents from one launch to the next until the next cold boot.

- **What survives:** the page is allocated as contiguous memory and persisted (`MmPersistContiguousMemory`, PTE
  bit 10), so a quick reboot takes it back at the same physical address (spec/stock/09; `kernel/src/memory/init.cpp`,
  the re-adoption loop). The kernel finds the pages again through an index page, also persisted, whose address is in
  the persisted section (`XK_PERSIST`), as `LaunchDataPage` is kept (`kernel/src/titles/title_loader.cpp`).
- **Index page:** a magic number, a checksum over the index, and one entry per extension: its descriptor `Name` and
  its state page's address. At a launch the kernel checks the magic and the checksum; if either is wrong (a cold
  boot, or memory written over), it starts a new, empty index.
- **Which page an extension gets:** the one under its descriptor `Name`. A new name gets a new zeroed page
  (`StateIsNew`). A page whose extension is not loaded at a launch (removed from the ini, `enabled = no`, its `Load`
  failed, or `oxos_extensions.ini` deleted) is freed then: the loader runs while state pages are kept, also without
  the ini. Only without a loader cache for this kernel (§8.1) do they stay until the next cold boot.
- **Contents:** entirely the extension's; the kernel never reads them. An extension should put its own magic and
  layout version at the start, since an older build of it may have written the page.
- **Pointers:** the page keeps bytes, not objects. A pointer into the extension's image, a pool block, a handle,
  `KEVENT` or thread from a previous launch is not valid in the next one (the image may load at another address, and
  every kernel object is gone).
- **Cold boot:** every state page starts zeroed.
- *Later (not specified yet):* the state page as a cache of a file that the extension flushes to the disk, so state
  also survives a power-off.

## 7. Hooks and events

### 7.1 Hooks on kernel functions
**Purpose:** to watch and change what titles do. A hook replaces a kernel function **for the title**: the title's
call goes to the extension's function, which decides what happens.

**`oxos::Name` is OXOS's function, as seen from this extension.** A hook that wants the kernel to do the work calls
`oxos::Name(...)` with the (possibly changed) arguments and returns its (possibly changed) result. A hook that does
not call it has answered the call itself.

| A hook can | How |
|---|---|
| watch | log the arguments, `return oxos::Name(...)`, log the result |
| change the input | change the arguments, then call `oxos::Name` |
| change the output | call `oxos::Name`, then change the status, the buffer or the I/O status block |
| replace | not call `oxos::Name`; return its own answer (and do the kernel's job: the I/O status block, the event, the APC) |
| refuse | return an error status without calling `oxos::Name` |

**Chains.** Several extensions may hook one function. The title's call goes to the first in `oxos_extensions.ini` order;
its `oxos::Name` goes to the next one that hooks it; the last one's goes to the kernel:

```
title -> [savewatch] NtCreateFile -> oxos:: -> [netlog] NtCreateFile -> oxos:: -> OXOS kernel NtCreateFile
```

- An extension's `oxos::` call never comes back to itself or to an extension above it, so a hook cannot loop.
- With one extension, or a function only it hooks, `oxos::Name` is exactly the kernel's function.
- An extension's other calls (from `Main`, events, or a hook calling another function) follow the same rule: they
  pass the hooks of the extensions below it, then reach the kernel.

**How the loader does it** (no dispatcher stays in the kernel):
- Each extension imports the kernel's functions from `xboxkrnl.exe` by ordinal (§8). The SDK declares them in
  as C functions (`extern "C"`), in C++ in namespace `oxos` (`namespace oxos { extern "C" NTSTATUS NTAPI
  NtCreateFile(...); }`), so `NtCreateFile` in C and `oxos::NtCreateFile` in C++ call through the extension's own
  import entry.
- `HookExport(Context, Ordinal, Replacement, &Next)` records the hook. During `Load` the imports point at the kernel.
- After every `Load` (§4.3 step 3), the loader links each chain: an extension's import entry for a hooked ordinal is
  pointed at the next hook below it (or left at the kernel's function), and the title's thunk for that ordinal
  (`titles::ResolveKernelThunks`) at the first hook. `Next` receives the same address, for extensions written without
  the SDK's class.
- A hooked call costs one jump (the stub) and one call more than an unhooked one.

**Rules:**
- **Titles only:** only the title's kernel imports are redirected. The kernel's own calls inside itself (reading
  `oxos_launch_redirect.ini`, the I/O manager calling a driver) are never hooked.
- **When:** hooks are made in `Load`, before the title's thunks are resolved, and hold for that whole title. A hook
  made later is refused. Nothing needs to be unhooked: the next launch starts without hooks.
- **The kernel function's prototype and calling convention:** `stdcall`, or `fastcall` where `kernel/exports.tsv`
  lists it. The SDK's class checks the arguments at compile time (§4.4) and its stub supplies the convention.
- **The kernel function's rules:** a hook runs on the title's thread, at the IRQL the title called at, and may run on
  several threads at once (shared data needs a lock or atomic operations). A hook on a function titles call at
  `DISPATCH_LEVEL` (`KeSetEvent`, `KeInsertQueueDpc`, ...) must not wait or call anything that does.
- **Not hookable** (not in the SDK's table; `HookExport` returns `STATUS_NOT_SUPPORTED`): the 34 data exports
  (`KeTickCount`, `LaunchDataPage`, ...: an extension reads or writes them directly); `KeGetCurrentIrql`,
  `KfRaiseIrql`, `KfLowerIrql`, `KeRaiseIrqlToDpcLevel` (called at every IRQL, in titles' interrupt paths);
  `HalReturnToFirmware` (use `OXOS_EVENT_TITLE_EXITING`); OXOS's runtime exports for extensions, 402-415 (§9.1:
  titles never import them).
- **Kernel internals that are not exports** (`titles::LaunchTitle`, the exFAT driver, the scheduler) are not hookable;
  extensions reach them through events (§7.2) and the kernel facts (§5.2). *Later, if a use needs it:* hooks that
  also catch the kernel's own calls, as a separate API (patching the function's first instructions, with a generated
  trampoline for `oxos::`).
- **The call tracer** (`TRACE_IMPORTS`, a debug build) wraps the title's thunk outside the first hook, so it records
  the title's calls as the title made them.

### 7.2 Events
Written as member functions of the class (`OnTitleLoading`, `OnTitleLoaded`, `OnTitleExiting`), registered by the
generated `Load` (`RegisterEvent`). Called in `oxos_extensions.ini` order.

| Event | When | The callback gets | It may |
|---|---|---|---|
| `OXOS_EVENT_TITLE_LOADING` | before the XBE is loaded: after `oxos_launch_redirect.ini` (spec/oxos_internal/33) | the XBE's path, the launch data page (type, title ID of the launching title, its data), the launch kind (cold boot, launch data page, dashboard return) | read them (version 1: replacing the XBE path is left to launch redirects, spec/oxos_internal/33) |
| `OXOS_EVENT_TITLE_LOADED` | the XBE is loaded and its thunks resolved, before its entry point | the title ID, name, certificate, XBE header, base | read or patch the title's image (as `oxos_title_patches.xos` does, §10) |
| `OXOS_EVENT_TITLE_EXITING` | the title calls `HalReturnToFirmware(2)` to start another XBE or the dashboard; the title is still in memory | the launch data page the title wrote | read it, or change the next launch |

*Open:* events for the disk (mounted, formatted), the network (link up), and the controllers.

## 8. The `.xos` file and the loader
- **PE32, i386, a DLL** (`IMAGE_FILE_DLL`), with a base relocation table: the kernel places the image where it finds
  memory and applies the relocations. Sections are mapped as the headers say; all of them are writable and
  executable (the CPU has no NX).
- **Imports:** only from `xboxkrnl.exe`, by ordinal, the same table titles use, resolved against the kernel's own
  export directory as `titles::ResolveKernelThunks` does for titles, then linked into the chains (§7.1). An import from
  any other DLL, or by name, refuses the file.
- **Exports:** `oxos_extension` by name (§4.1). Other exports are ignored. *Open:* extensions calling each other.
- **No entry point is called:** the kernel uses the descriptor, not `DllMain` (built with `/NOENTRY`).
- **No CRT, no TLS, no C++ exceptions or RTTI, no static constructors** in the first version. The SDK builds with
  these off (§9); the loader refuses an image with a TLS directory.
- **Memory:** the image is loaded into contiguous memory, not persisted (a quick reboot frees it). Its globals start
  from the file's values at each launch.

### 8.1 What extensions cost titles
Every loaded image, pool block, thread stack and state page is memory a title does not get. Without
`oxos_extensions.ini`, or with no enabled section, titles keep exactly the memory they had (the parity rule,
spec/oxos_internal/18): the loader runs, finds nothing to load, and allocates nothing (the index page is only created
for the first extension). With extensions, titles lose their size, and a title that needs nearly all of its 64 MB
can fail to start. The serial log reports each extension's image size after `Load`; *open:* its pool, threads and
state page too, and showing it in the File Manager.

**The loader is not resident: it is linked on its own and run from a cache file on the disk** (decided 2026-11-13):
- **Built on its own.** The loader's sources (`kernel/src/extensions/`: `extension_loader.cpp`, `extension_parser.cpp`;
  `kernel/src/ini_file/extensions_ini.cpp`; `oxos_extensions.ini`, PE loading, relocation, import binding, chains, the
  state-page index, `HookExport`, `RegisterEvent`) are compiled like the kernel but are not part of its link.
  `tools/link_extension_loader.py` links them at the fixed address 0xB0000000 (`XK_EXTENSION_LOADER_BASE`, a page
  directory entry nothing else uses) and gives each kernel symbol they use as an absolute symbol at this kernel's
  address, from the kernel's map: the loader calls the kernel directly. A symbol in the kernel's boot-only sections
  stops the build.
- **In the kernel image.** `tools/mkbios.py --extension-loader` appends it to the kernel image after the boot-only
  sections, behind a 20-byte header (`XK_EXTENSION_LOADER_HEADER`: magic, the kernel's PE stamp, base, size, entry), and
  names the header in the kernel's DOS header (offset 0x38). Its pages are boot-only: freed before the first title.
- **Cached on the disk.** At a cold boot the kernel writes it to `C:\OXOS\extensions\oxos_extension_loader.cache`
  (`extensions::WriteLoaderCache`, boot-only), always: the file is a cache, overwritten with the known-good copy at
  every cold boot. Without the extensions folder nothing is written.
- **Run from the cache at every launch** (the cold boot's too): `extensions::LoadExtensions` (`src/extensions/
  extension_runtime_loader.cpp`) reads the file only when `oxos_extensions.ini` exists, checks the header (magic, this
  kernel's stamp, base, sizes), copies the image into temporary contiguous pages, maps them at 0xB0000000 through a
  temporary page table, calls the entry, then unmaps and frees everything before the title loads. No cache, or one
  from another kernel: no extensions at that launch (logged).
- **Not from the flash.** Reading the flash while the kernel runs coincided with RAM bits changing on the Modxo
  console (2026-10-28, `kernel/boot/bl.cpp`), and some modchips hide themselves after the boot; a copy kept in RAM
  instead would cost about as much as the resident loader. So the flash is not read after the boot, and the loader
  is not compressed (nothing extra goes into the flash).
- **Netboot:** the image the 2BL downloads holds the loader like a flashed one; the cold boot writes the cache.
- **Resident in the kernel** stays what runs while the title runs (`extension_runtime.cpp`: the hooked imports, the
  three events, the end of a title, the Main thread routine) and the loader's runner (`extension_runtime_loader.cpp`):
  about 1.1 KB. To keep every page titles had, the cold boot's launch choice and the netboot title's installation moved
  to the boot-only pages in the same change (spec/oxos_internal/18 §2). Checked: the memory test title starts with 15
  960 free pages (xemu, 64 MB), as before kernel extensions.

## 9. The SDK: building an extension
Extension writers do not need nxdk. nxdk's runtime (its C library, `winapi`, heaps, threads, SDL) is made for a
title: it allocates title memory, sets up title-only state, and is torn down at the quick reboot. None of it is safe
in the kernel. An extension needs much less, and what it needs is the kernel's.

- **Plain C is the base, C++ on top** (decided 2026-11-13): every header is valid C17 and C++20; the C++ class
  (§4.4) and namespace `oxos` are added only when the compiler is C++.
- **Windows, macOS and Linux alike:** an extension needs only LLVM's `clang` and `lld-link` (18 or newer) and GNU
  make. The library comes prebuilt, and the build rules run no other command, so they work from `cmd.exe`,
  PowerShell, bash or zsh. No CMake (decided 2026-11-13).
- **The same bytes everywhere:** objects without time stamps and `/Brepro` links, so a source builds the same `.xos`
  on any system.
- **Its own repository later:** `sdk/` is self-contained (it never reads the kernel's sources), so it can move to
  its own repository as it is; the kernel then takes `oxos_interfaces.h` from it (decided 2026-11-13: later).

**What a release ships: `oxos_sdk_vX.Y.Z.zip`** (MIT, §11), next to `oxos_vX.Y.Z.zip`; `make -C kernel release
VERSION=X.Y.Z` makes it with `sdk/Makefile`:

```
oxos_sdk/
  include/oxos_sdk.h      the one header an extension includes, C and C++
  lib/oxos_sdk.lib        the one library: the SDK's helpers (§5.1) and the kernel's import library
  oxos_extension.mk       the build rules (optional for a build of one's own)
  examples/hello/         the example to start from
  README.md, LICENSE
```

**The SDK's source, `sdk/` in this repository** (the kernel's SDK; its files keep the `oxos_` names):

| Part | Contents |
|---|---|
| `include/oxos_sdk.h` | what an extension includes: the parts below, `OXOS_DEFINE_EXTENSION(...)`, the C helpers (§5.1); in C++ also `oxos::Extension<T>` and `OXOS_DEFINE_EXTENSION_CLASS(Class, ...)` (§4.4, not yet). The release merges the parts into it, so it ships as one file |
| `include/oxos/oxos_interfaces.h` | the structures the kernel and an extension exchange: `OXOS_EXTENSION`, `OXOS_CONTEXT`, `OXOS_API`, `OXOS_KERNEL_INFO`, the events and their data, `OXOS_API_VERSION`; the kernel includes it too, so the layout has one definition |
| `include/oxos/oxos_kernel.h` | the kernel's types and constants (`OBJECT_ATTRIBUTES`, `KEVENT`, ...), every `xboxkrnl.exe` function (in C++ in namespace `oxos`), `memcpy` and its kin, the hookable functions' ordinals (`OXOS_ORDINAL_NtOpenFile`) |
| `include/oxos/oxos_types.h` | the NT base types (`ULONG`, `NTSTATUS`, `LARGE_INTEGER`, `STRING`, ...) and status codes |
| `include/oxos/oxos_exports.inc` | *not yet:* the table of kernel functions, one line each: `OXOS_EXPORT(190, NtCreateFile, NTSTATUS, stdcall, (PHANDLE Handle, ...))`; generated from `kernel/exports.tsv` and the kernel's prototypes; drives the declarations, the ordinals, the hookable list and the class's compile-time checks |
| `src/oxos_runtime.cpp` | the helpers of §5.1, with C names, so C and C++ extensions link the same library |
| `src/xboxkrnl.def` | every `xboxkrnl.exe` export by ordinal, generated from `kernel/exports.tsv` by `tools/gen_oxos_sdk.py`; the import library is made from it (`lld-link /lib /def:`) |
| `oxos_extension.mk` | the build rules extensions include (below); they take the library from the package's `lib/`, so extensions are built only with the SDK as it ships, never from its source |
| `Makefile`, `make_sdk_package.py` | build the SDK as it ships into `build/sdk/oxos_sdk/` (the library, the merged header, the rest copied), then each example from that package, as a user builds it, into `build/sdk/examples/NAME/`, and with `VERSION` the zip; for the SDK's maintainers (a POSIX shell and Python) |
| `docs/kernel_functions.md` | *not yet:* every hookable kernel function, ready to copy into a class (§9.2) |
| `examples/hello/` | the example to start from, in C++ (like all of OXOS; C extensions are supported, not shown): `Load`, `Main`, `Unload`, a hook, an event, settings and the state page, each commented; its Makefile, a sample `oxos_extensions.ini` and a README (build, install, what it prints) |

Nothing built is committed: the library and the merged header exist only in `build/sdk/` and the release zip.

### 9.1 What the kernel provides
Extensions call the kernel for everything a freestanding program needs, as Linux modules and Windows drivers do; the
SDK carries no copies. The Xbox kernel never exported these (titles link their own C library), so OXOS exports them at
ordinals of its own, after its other exports (400-401, spec/oxos_internal/30 §1). Titles never import them and they
cannot be hooked (§7.1).

| Ordinals | Exports | Why |
|---|---|---|
| 402-405 | `memcpy`, `memset`, `memmove`, `memcmp` (`cdecl`) | the compiler calls them for copies and zeroing, even with `-fno-builtin` |
| 406-415 | `_allmul`, `_alldiv`, `_aulldiv`, `_allrem`, `_aullrem`, `_alldvrm`, `_aulldvrm`, `_allshl`, `_allshr`, `_aullshr` | the compiler's 64-bit arithmetic helpers for the `i386-pc-win32` target, each with its own register convention (`kernel/src/arch/int64.S`) |

`kernel/exports.tsv` lists them with the convention `cdecl` or `compiler`. They cost the resident kernel 4 bytes each
in its export table, and `memcmp`.

### 9.2 The kernel function reference
`sdk/docs/kernel_functions.md` lists **every kernel function an extension can hook**, each as the member
function declaration to paste into the class, with an empty body that passes the call on. A user finds the function,
copies the block, and edits the body.

- **Generated, never written by hand:** from the same table as the SDK (`include/oxos/oxos_exports.inc`), by the tool
  that generates the table, so the reference, `oxos::` and the class's compile-time check cannot disagree.
  `tools/gen_oxos_sdk.py` regenerates it, and a check fails the build when the committed file differs from the
  generated one.
- **Grouped by area**, in ordinal order within each, with a table of contents: files and I/O (`Nt*File`, `Io*`),
  memory (`Mm*`, `Ex*Pool`), threads and synchronisation (`Ps*`, `Ke*`, `Nt*Event`, ...), objects (`Ob*`), runtime
  (`Rtl*`), hardware (`Hal*`, `Av*`, `Phy*`), settings (`ExQueryNonVolatileSetting`, ...), crypto (`Xc*`), titles
  and media (`Xe*`, `Xbox*`), debug (`Dbg*`).
- **Each entry:** the name and ordinal, a line saying what the kernel function does (with its spec section, e.g.
  spec/stock/11 for the I/O manager), the IRQL titles may call it at when that matters for a hook, and the block:

  ````markdown
  ### NtCreateFile (190)
  Opens or creates a file, directory or device (spec/stock/11 §4). Titles call it at PASSIVE_LEVEL.

  ```cpp
  NTSTATUS NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                        PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                        ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions) {
      return oxos::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                                FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
  }
  ```
  ````

- **No calling convention in the block:** the class's members are plain C++; the SDK's stub supplies `stdcall` or
  `fastcall` (§4.4).
- **At the end:** the functions that cannot be hooked and why (§7.1), and the 34 data exports with their types, for
  reading or writing directly (`oxos::KeTickCount`, ...).
- The events (`OnTitleLoading`, `OnTitleLoaded`, `OnTitleExiting`) and the lifecycle members (`Load`, `Main`,
  `Unload`) get their own copy-ready blocks at the top, so the file is the one place to start a new extension from.

**Build:** GNU make with the SDK's rules; `clang` and `lld-link` from the `PATH`, or from the folder `LLVM=` names:

```make
# examples/hello/Makefile
NAME := hello
SOURCES := hello.cpp
include ../../oxos_extension.mk
# oxos_extension.mk: clang --target=i386-pc-win32 -march=pentium -mno-sse -mno-mmx -mno-80387 -ffreestanding
#                    -nostdinc -fno-builtin -O2 -std=c17 (.c) or -std=c++20 -fno-exceptions -fno-rtti (.cpp) ...
#                    (no floating point or SIMD: the kernel does not save those registers for extensions);
#                    lld-link /DLL /DRIVER /NOENTRY /NODEFAULTLIB /OUT:hello.xos ... oxos_sdk.lib
```

The output folder (`OXOS_OUT`, default the current one) must exist; the rules create no folders, so they need no
shell commands. Sources are in the Makefile's folder; their objects go to the output folder, named after the
extension.

Once the class exists (§4.4), the example (`hello.cpp`, today plain functions) uses it:

```cpp
// examples/hello/hello.cpp
#include <oxos_sdk.h>

class Hello : public oxos::Extension<Hello> {
    struct Saved { ULONG Magic, Launches; };

public:
    NTSTATUS Load() {
        auto &saved = State<Saved>();
        if (StateIsNew() || saved.Magic != 'oleH')
            saved = {'oleH', 0};
        saved.Launches++;
        Log("launch %lu since the cold boot", saved.Launches);
        return STATUS_SUCCESS;
    }
    void OnTitleLoaded(const oxos::Title &title) {
        Log("title %08lx %s", title.Id, title.Name);
    }
};

OXOS_DEFINE_EXTENSION_CLASS(Hello, .Name = "hello", .Version = "1.0.0");
```

**Install:** copy `hello.xos` to `C:\OXOS\extensions\` (File Manager or FTP) and add it to
`C:\OXOS\oxos_extensions.ini`:

```ini
[hello]
extension = hello.xos
```

It runs from the next launch.

### 9.3 Test extensions
`tests/extensions/`, built with the SDK; `make -C kernel test_extensions` (`tests/boot_test.py --extensions`) puts them
and an `oxos_extensions.ini` on the test disk and runs `test_relaunch`'s title through a cold boot and two quick
reboots. The extensions record what the kernel did in their state pages; the test reads those back through
`extensions::LoadedExtensions` (the offsets are checked at compile time in `src/extensions/extension_state.h`).

**`extension_test.xos`:**

| It does | It proves |
|---|---|
| `Load`: on `StateIsNew` zeroes its state page and writes its magic; counts its Loads and keeps `Generation` | the state page survives quick reboots: 3 Loads, generation 3, never a mismatch |
| hooks `NtOpenFile` and counts the title's calls, passing each on with `oxos::` | the title's calls go through the hook, and the title still works |
| registers the three events and counts them | a loading and a loaded title per launch, an exit per quick reboot |
| `Main`: one `NtOpenFile` of its own, then a heartbeat every 10 ms until the stop request | `Main` runs while the title runs and returns when asked; its own call passes only the extensions below it |
| `Unload` counts | `Unload` at each quick reboot |
| `oxos_get_setting(Context, "Answer")` is `42`; `oxos_read_file` reads its own `.xos` | settings, the SDK runtime and the kernel imports |

**Companions:**
- `chain.xos`, listed after `extension_test`: hooks the same `NtOpenFile` and counts: the title's calls (through
  extension_test's hook, then this one) and extension_test's own from `Main` (this one only).
- `plain_c.xos`, written in C: counts its Loads and loaded titles and reads its setting, so the SDK's headers, macro
  and runtime work without C++; checks a struct copy, `memset`, `memcmp` and signed and unsigned 64-bit division and
  remainder at each Load, which the compiler turns into calls to the kernel's exports 402-410 (§9.1).
- `hello.xos`, the SDK's example (`sdk/examples/hello`), listed after `plain_c` with `heartbeat = 1`: built and
  loaded at every launch so the example keeps working; its state page counts the launches.
- `failload.xos`: hooks `NtOpenFile`, then fails its `Load`: unloaded, its hook dropped (a title call through it
  would run freed memory), its state page freed.
- `badversion.xos`: built for the next major API version: refused.
- `disabled.xos` (`enabled = no`) and `unlisted.xos` (in the folder, not in the list): neither loads.

*Not yet automated:* safe mode (B at the splash), an extension that crashes (the error screen naming it, §3.4), and
state pages freed when `oxos_extensions.ini` is deleted between launches (§6).
Without `oxos_extensions.ini` nothing loads: `make test_memory` checks that titles keep 15 960 free pages.

## 10. OXOS features as extensions
Decided 2026-11-13. What runs only for some users, or only on development consoles, leaves the kernel and becomes an
extension; what must always work stays in the kernel.

**Stays in the kernel:** launch redirects (spec/oxos_internal/33: guaranteed to work with only
`oxos_launch_redirect.ini`, also in safe mode) and therefore the ini reader (`ini_file.cpp`, which `oxos_extensions.ini`
and the loader share); the trainer hook instruction (spec/oxos_internal/23: dashboards search the kernel image for it);
the in-game reset; the title compatibility answers (spec/oxos_internal/25 §1-2: games need them to run at all); virtual
discs; the error screens; the File Manager (the way out of a broken extension).

**Names (decided 2026-11-13):** every extension OXOS provides is `C:\OXOS\extensions\oxos_NAME.xos`, its section in
`oxos_extensions.ini` is `[oxos_NAME]`, and a file of its own data is `C:\OXOS\oxos_NAME.ini`. Extensions from others
use any other name; the `oxos_` prefix is OXOS's. Kernel features keep their files (`oxos_launch_redirect.ini`).

```ini
; oxos_extensions.ini for the title patches (in the trainers zip)
[oxos_title_patches]
extension = oxos_title_patches.xos
```

| Feature | Today | As an extension | Needs from §5/§7 |
|---|---|---|---|
| **Title patches** (`title_patches.ini`, spec/oxos_internal/25 §3; `title_patcher_ini.cpp`, `titles::LoadPatches`) | in the kernel, 1 704 bytes resident plus the records | `oxos_title_patches.xos`, shipped in the trainers zip with its patch file, now **`oxos_title_patches.ini`** (the same format as `title_patches.ini`), and an `oxos_extensions.ini` that lists it; reads `C:\OXOS\oxos_title_patches.ini` in `OnTitleLoaded` | `OnTitleLoaded` (the title's ID, version, section headers) patches the preloaded sections; a hook on `XeLoadSection` (327) patches a section after `oxos::XeLoadSection` has read it |
| **Netbooted title** (`title_netboot.cpp`, spec/oxos_internal/09 §7) | in the kernel, 981 bytes resident | `oxos_nettitle.xos`; installs the package as `E:\NAME\` and replaces the launch's XBE path with it | the netboot package's address and size in the kernel facts (§5.2; the kernel still keeps those pages from the memory manager); `OnTitleLoading` replaces the path |
| **Call tracer** (`title_tracer.cpp`, `trace.S`, spec/oxos_internal/01) | a separate kernel build, `TRACE_IMPORTS=1/2` | `oxos_tracer.xos`, listed first in `oxos_extensions.ini` so it sees the title's calls first; release kernels can be traced | `HookExport` from plain C with its own assembly stubs for every hookable ordinal (the stubs keep the registers and jump to `Next`); its ring outside the memory manager at a fixed physical address with 128 MB, as `TRACE_IMPORTS=2` does |

- **Netboot packages carry extensions:** a development console should not need a prepared disk. `netboot.py` adds
  `.xos` files to the title package (`--extension FILE`, and `oxos_nettitle.xos` by itself with `--title`); the loader
  loads the package's extensions first, in the order given, in addition to `oxos_extensions.ini` and also without it.
  Only a netbooted kernel does this.
- **Consequences, accepted:** title patches and trainers' patching need `oxos_title_patches.xos` installed and listed;
  safe mode (B) turns them off with the other extensions. The kernel's tracer builds (`TRACE_IMPORTS`) are retired once
  `oxos_tracer.xos` records the same.
- **Memory:** about 2.7 KB less resident kernel in release builds (patches and the netboot title), which also pays
  for the extensions' resident code (§8.1).

### 10.1 Proposed extensions (not decided)
- **GDB remote debugging (`gdbstub.xos`)**: debug a title (an nxdk game netbooted with its `.xos`, §10) from `gdb`
  on a PC with the GDB remote protocol: breakpoints, single steps, hardware watchpoints (DR0-DR7), reading and writing
  memory and registers, the title's threads.
  - Needs a new kernel interface: **exception callbacks**: first chance at breakpoint (`int 3`, `STATUS_BREAKPOINT`),
    single-step and debug-register exceptions and faults, with the trap frame, able to handle one and resume
    (`kernel/src/core/exceptions.cpp` today ends them in the debug service or the fatal screen). While stopped, the
    stub keeps the processor in its handler and polls its connection, as a kernel debugger does, so every thread of
    the title stands still.
  - **Connection:** the serial port first (debug kits, the Modxo's COM1 and other LPC serial ports; polled, no
    sharing). Over the network only for titles that do not use the network themselves: an nxdk title drives the
    network card with its own lwIP driver, and two drivers on one card do not work. *Open:* a way to share it.
  - Symbols: the title built with DWARF (`lld-link /DEBUG:DWARF`); its addresses are the XBE's, so `gdb` loads the
    `.exe` as it is.
- **Screenshots (`screenshot.xos`)**: a controller combination saves the screen the console shows as a file on the
  disk.
  - Needs a new event: **`OXOS_EVENT_CONTROLLER`**, every controller state a title reads, from the hook the in-game
    reset already has on `XInputGetState` (spec/oxos_internal/22 §2).
  - The event only signals `Main`; `Main` reads the front buffer (its address, pitch and format from the GPU's
    display registers) and writes a BMP or PNG to `E:\Screenshots\` while the title goes on.
- Others: a network log of a title's file and network calls, cheats (memory search and writes, on top of the
  trainers), save-game redirection or backup, a background FTP server for titles that do not use the network.

## 11. Decisions
- **Licence (2026-11-13).** The kernel stays GPL-2.0. The SDK (header, `oxos_sdk.lib`, build files, examples)
  is open source under the **MIT** licence (`sdk/LICENSE`), so an extension built with it may use any licence:
  open source is preferred, closed source is allowed. (A GPL SDK would make every extension that links
  `oxos_sdk.lib` GPL too, which is why the SDK is not GPL.) The SDK is written for it, not copied from GPL
  kernel sources.
- **The descriptor has no author, description or licence (2026-11-13):** nothing uses them. It names the extension
  (its state page) and its version (the log), and points at its functions (§4.1).
- **Trust (2026-11-13):** every enabled extension in `oxos_extensions.ini` is loaded and run, with no signing and no
  confirmation, as XBEs are today. The ways out are the File Manager (X) and safe mode (B).
- **`oxos_extensions.ini` is required (2026-11-13):** without it nothing loads; it gives the order (and so the chains),
  switches and settings (§3.1).
- **Hooks are for titles' calls (2026-11-13):** an extension implements kernel functions by name, and `oxos::Name`
  passes a call on to OXOS (§4.4, §7.1).
- **The loader is linked on its own and run from a cache file on the disk (2026-11-13)**, not from the flash (§8.1).
- **The SDK (2026-11-13):** `sdk/` (named `oxos_sdk/` until 2026-11-14); plain C as the base with C++ on top, so
  extensions may be written in C; the SDK's own code and its example are C++, and `tests/extensions/plain_c.c` is the
  only C source (2026-11-14). It ships as one header and one prebuilt library (`oxos_sdk.h`, `oxos_sdk.lib`) in
  `oxos_sdk_vX.Y.Z.zip` with each release; GNU make only, on Windows, macOS and Linux; no nxdk; its own repository
  later (§9). The kernel exports `memcpy` and its kin and the compiler's 64-bit helpers, as Windows and Linux kernels
  do (§9.1). Names: `OXOS_` for types and macros, `oxos_` for functions and files, never `Oxos` (docs/cpp_style.md
  §2).
- **A failed `Load` frees its state page (2026-11-13)** (§3.4).

**Open:** a crashed extension's persisted "off" mark (§3.4), `Unload` on a reset (§4.3), the kernel facts list (§5.2),
more events (§7.2), extensions calling each other (§8), the proposals of §10.1.
