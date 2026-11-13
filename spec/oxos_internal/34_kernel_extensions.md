# 34. Kernel extensions (OXOS's own)

Status: **draft, 2026-11-13, not implemented.** Nothing here is in the kernel yet; sections marked *open* are
decisions still to make.

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
| `MODULE_LICENSE`, `MODULE_AUTHOR`, `MODULE_DESCRIPTION` | the descriptor's name, version, author, description, licence (any; open source preferred, §11) |
| vermagic (refuses a module built for another kernel) | `ApiVersion` in the descriptor (§4.2) |
| `EXPORT_SYMBOL` symbols | the `xboxkrnl.exe` exports (332 functions, 34 variables), called as `oxos::Name` (§7.1) |
| `kthread_run` | `Main`, started on a system thread (§4.3) |
| `printk` / `dmesg` | `Log`, written to the debug serial port (§5.1) |
| kprobes / ftrace on syscalls | hooks on kernel functions, as titles call them (§7.1) |
| `init=/bin/sh`, `modprobe.blacklist` | hold **B** at boot: safe mode, no extensions (§3.3) |
| kbuild | the SDK's `extension.mk` (§9) |

The main difference: Linux keeps a module loaded until it is removed. OXOS **reloads every extension from the disk
at each launch**, because a quick reboot restarts the kernel (§3.2). Only the state page carries over.

## 3. Loading

### 3.1 `oxos_extensions.ini`: which extensions, in which order
**Without `C:\OXOS\oxos_extensions.ini`, no extension is loaded**, whatever is in the folder; as with
`oxos_launch_redirect.ini` and the patch file, nothing happens without the file (decided 2026-11-13).

- **The file:** `C:\OXOS\oxos_extensions.ini` (`\Device\Harddisk0\Partition2\OXOS\oxos_extensions.ini`;
  `/C/OXOS/oxos_extensions.ini` on an exFAT disk), text, at most 64 KB, read with the ini reader the other OXOS files
  use (`kernel/src/titles/ini_file.cpp`, the same format rules: sections, `key = value`, `;` and `#` comments, LF or
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
- cold boot: before the disc, the dashboard or a netbooted title (`XepChooseTitle`, `kernel/src/titles/loader.cpp`);
- quick reboot: before `XepLaunchTitle` reads the launch data page, so an extension sees every title start (§7.2).

A quick reboot restores the kernel's `.data`, zeroes its `.bss` and stops every thread (`quick_reboot.S`). The
extensions' images, threads, hooks and events are all gone after it; the kernel reads `oxos_extensions.ini` and the
files again and calls `Load` again. Each launch therefore starts from what is on the disk: a change to the ini or a file
counts from the next launch, and a broken image in memory cannot outlive one title.

Not loaded:
- **The OXOS File Manager** (X at boot, spec/oxos_internal/32): it runs with no extensions, so a broken extension can
  always be switched off or deleted from it.
- **Safe mode** (§3.3).

### 3.3 Safe mode: B at boot
- Holding **B** during the boot splash, like X for the File Manager (`kernel/src/gamepad/pads.cpp` reads the pads
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
- **`Load` returns an error status:** the image is freed, its hooks and events are dropped, the others go on.
- **A crash in extension code** (in `Load`, `Main`, a hook or an event): the kernel's fatal error screen
  (spec/oxos_internal/29) names the extension (its section name) when the faulting address lies in an extension's
  image, and says "Hold B at boot to start without extensions". *Open:* also mark that extension off for the rest of
  the boot (a persisted "crashed" list), so the next launch skips it without B.

## 4. The extension's interface
Extensions are written as a C++ class with the SDK (§4.4); the descriptor and context below are what the SDK
generates and what the kernel sees. Both are plain C structures, so an extension can also be written without the
class.

### 4.1 The descriptor
Each `.xos` exports one data symbol by name, **`XosExtension`**, an `XOS_EXTENSION`. The SDK's `XOS_EXTENSION(...)`
macro writes it.

```cpp
struct XOS_EXTENSION {
    ULONG Magic;                 // 'XOSX'
    ULONG ApiVersion;            // XOS_API_VERSION of the SDK it was built with (§4.2)
    const char *Name;            // short name, the key of its state page (§6), e.g. "netlog"
    const char *Version;         // the extension's own version, e.g. "1.2.0"
    const char *Author;
    const char *Description;
    const char *Licence;         // SPDX name, e.g. "GPL-2.0-or-later", "MIT", "Proprietary" (§11)
    ULONG Flags;                 // XOS_FLAG_*, none defined yet: 0
    NTSTATUS (NTAPI *Load)(XOS_CONTEXT *Context);   // required (the SDK generates it, §4.4)
    void (NTAPI *Main)(XOS_CONTEXT *Context);       // optional: run on its own thread
    void (NTAPI *Unload)(XOS_CONTEXT *Context);     // optional
};
```

### 4.2 Versions
- `XOS_API_VERSION` is `major << 16 | minor`. The kernel loads an extension whose major equals its own and whose
  minor is at most its own.
- Within a major, the API only grows: new functions are added at the end of the context's tables, which carry their
  own size (§5), and existing ones keep their meaning.
- A new major is needed only to change or remove something; extensions are then rebuilt with the new SDK.

### 4.3 The order of calls in one launch
1. The loader reads `oxos_extensions.ini`, loads each enabled extension's image (§8) in order, and fills an
   `XOS_CONTEXT` for it.
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
   connections. *Open:* how long the kernel waits for `Main` to return (a few hundred milliseconds), and whether
   `Unload` runs at all on a full reset or power-off.

### 4.4 The extension class (SDK)
The author derives from `xos::Extension<T>` and writes **only what the extension needs**. Every member function
below is optional: one that is not written does not exist in the extension, and the kernel never calls it. A kernel
function that is not written is not hooked.

```cpp
#include <xos/extension.h>

class SaveWatch : public xos::Extension<SaveWatch> {
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
    void OnTitleLoaded(const xos::Title &title) {
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

XOS_EXTENSION(SaveWatch, .Name = "savewatch", .Version = "1.0.0", .Author = "OXOS",
              .Description = "Logs file opens, refuses deletes", .Licence = "GPL-2.0-or-later");
```

- **`XOS_EXTENSION(Class, ...)`** writes the descriptor and a generated `Load`. For each hookable kernel function in
  the SDK's table (§9), a C++20 check asks whether the class has a member function of that name:
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
  `Context()` for the raw `XOS_CONTEXT`.

## 5. The context
The kernel passes each extension its own `XOS_CONTEXT`, in pool memory that stays valid until the title ends.
Everything an extension knows about the kernel, beyond the `xboxkrnl.exe` exports, comes through it. The extension
does not use kernel addresses it found some other way: they change with every build.

```cpp
struct XOS_CONTEXT {
    ULONG Size;                  // sizeof(XOS_CONTEXT) of the kernel that filled it
    ULONG ApiVersion;            // the kernel's XOS_API_VERSION
    const char *KernelVersion;   // e.g. "0.4.7"
    ULONG Generation;            // launches since the cold boot: 1 = the cold boot's title (§1)
    ULONG BootFlags;             // XOS_BOOT_COLD, XOS_BOOT_QUICK; XOS_BOOT_128MB, ...

    // This extension
    const char *SectionName;     // its section in oxos_extensions.ini, e.g. "netlog"
    const XOS_SETTING *Settings; // the section's other keys (§3.1): {Key, Value} pairs
    ULONG SettingCount;
    PVOID ImageBase;             // where it is loaded this launch (a different address each launch is possible)
    ULONG ImageSize;
    PVOID State;                 // its state page (§6)
    ULONG StateSize;             // 4096
    BOOLEAN StateIsNew;          // the page was just created (zeroed): first launch after a cold boot, or a new Name
    PKEVENT StopEvent;           // signalled before Unload (§4.3)

    const XOS_API *Api;          // the loader's functions (§5.1): only in Load, null afterwards
    const XOS_KERNEL_INFO *Kernel; // read-only facts about the console and the title (§5.2)
};
```

### 5.1 Functions
**The loader's (`XOS_API`):** a table of `NTAPI` function pointers, starting with its own `Size`. They belong to the
loader (§8.1), which is gone once every `Load` has returned, so they are **only for `Load`**: the kernel clears
`Context->Api` before it frees the loader, and a later call faults on a null pointer instead of running freed
memory. The class (§4.4) calls them for the author.

| Function | What it does |
|---|---|
| `HookExport(Context, Ordinal, Replacement, &Next)` | hook a kernel function as titles call it (§7.1) |
| `RegisterEvent(Context, Event, Callback, CallbackContext)` | be called at a launch event (§7.2) |

**The SDK's**, usable at any time (`Load`, `Main`, hooks, events): built into each extension on top of the kernel's
exports, so the kernel keeps no code for them:

| SDK function | What it does |
|---|---|
| `Log(Format, ...)` | one line to the debug serial port through `DbgPrint`, prefixed with the extension's section name (a `SERIAL=1` kernel; the others keep only the in-memory debug log) |
| `ReadFile(Path, MaxSize, &Buffer, &Size)` | read a whole file into pool memory (drive letter or device path) |
| `WriteFile(Path, Buffer, Size)` | write a whole file (created or replaced) |
| `ResolvePath(DosPath, Buffer, BufferSize)` | `E:\Apps\x.xbe` to `\Device\Harddisk0\Partition1\Apps\x.xbe`, the same drive table as the kernel (spec/oxos_internal/33 §1) |

**The kernel's own functions:** every `xboxkrnl.exe` export, as **`oxos::Name`**: `oxos::ExAllocatePoolWithTag`,
`oxos::KeWaitForSingleObject`, `oxos::PsCreateSystemThreadEx`, `oxos::HalReadSMBusValue`, and so on (§7.1 for what
`oxos::` means for a hooked function).

### 5.2 Kernel facts (`XOS_KERNEL_INFO`)
Read-only, filled at each launch, also starting with its `Size`: the console revision and video encoder, memory
size, the disk layout (FATX or exFAT, partitions), whether the network card is up, and, from the
`XOS_EVENT_TITLE_LOADED` event on, the running title's ID, name, XBE path and certificate. *Open:* the exact list,
decided by the first extensions that need it. Kernel internals reach extensions only through fields added here and
functions added to `XOS_API`, never as raw addresses.

## 6. The state page
One page, 4096 bytes, per extension, that keeps its contents from one launch to the next until the next cold boot.

- **What survives:** the page is allocated as contiguous memory and persisted (`MmPersistContiguousMemory`, PTE
  bit 10), so a quick reboot takes it back at the same physical address (spec/stock/09; `kernel/src/memory/init.cpp`,
  the re-adoption loop). The kernel finds the pages again through an index page, also persisted, whose address is in
  the persisted section (`XK_PERSIST`), as `LaunchDataPage` is kept (`kernel/src/titles/loader.cpp`).
- **Index page:** a magic number, a checksum over the index, and one entry per extension: its descriptor `Name` and
  its state page's address. At a launch the kernel checks the magic and the checksum; if either is wrong (a cold
  boot, or memory written over), it starts a new, empty index.
- **Which page an extension gets:** the one under its descriptor `Name`. A new name gets a new zeroed page
  (`StateIsNew`). A page whose extension is not loaded at a launch (removed from the ini, `enabled = no`) is freed then.
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
  namespace `oxos` (`namespace oxos { extern "C" NTSTATUS NTAPI NtCreateFile(...); }`), so `oxos::NtCreateFile`
  calls through the extension's own import entry.
- `HookExport(Context, Ordinal, Replacement, &Next)` records the hook. During `Load` the imports point at the kernel.
- After every `Load` (§4.3 step 3), the loader links each chain: an extension's import entry for a hooked ordinal is
  pointed at the next hook below it (or left at the kernel's function), and the title's thunk for that ordinal
  (`XepResolveKernelThunks`) at the first hook. `Next` receives the same address, for extensions written without
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
  `HalReturnToFirmware` (use `XOS_EVENT_TITLE_EXITING`).
- **Kernel internals that are not exports** (`XepLaunchTitle`, the exFAT driver, the scheduler) are not hookable;
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
| `XOS_EVENT_TITLE_LOADING` | before the XBE is loaded: after `oxos_launch_redirect.ini` (spec/oxos_internal/33) | the XBE's path, the launch data page (type, title ID of the launching title, its data), the launch kind (cold boot, launch data page, dashboard return) | replace the XBE path (then the same rules as a redirect target: one replacement per launch, and the original is started if the replacement does not load) |
| `XOS_EVENT_TITLE_LOADED` | the XBE is loaded and its thunks resolved, before its entry point | the title ID, name, certificate, XBE header, base | read or patch the title's image (as `oxos_title_patches.xos` does, §10) |
| `XOS_EVENT_TITLE_EXITING` | the title calls `HalReturnToFirmware(2)` to start another XBE or the dashboard; the title is still in memory | the launch data page the title wrote | read it, or change the next launch |

*Open:* events for the disk (mounted, formatted), the network (link up), and the controllers.

## 8. The `.xos` file and the loader
- **PE32, i386, a DLL** (`IMAGE_FILE_DLL`), with a base relocation table: the kernel places the image where it finds
  memory and applies the relocations. Sections are mapped as the headers say; all of them are writable and
  executable (the CPU has no NX).
- **Imports:** only from `xboxkrnl.exe`, by ordinal, the same table titles use, resolved against the kernel's own
  export directory as `XepResolveKernelThunks` does for titles, then linked into the chains (§7.1). An import from
  any other DLL, or by name, refuses the file.
- **Exports:** `XosExtension` by name (§4.1). Other exports are ignored. *Open:* extensions calling each other.
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
can fail to start. The serial log reports what each extension holds after `Load`; *open:* showing it in the File
Manager too.

**The loader is not resident: it is an overlay in the flash** (decided 2026-11-13), read again at every launch, as
the File Manager's code is only there when it runs (spec/oxos_internal/31, 18 §2):
- The loader (`oxos_extensions.ini`, PE loading, relocation, import binding, chains, the state-page index, `HookExport`,
  `RegisterEvent`, safe mode) is linked as its own blob and stored **compressed** in the flash image, after the
  kernel stream, with the same Deflate (RFC 1951, `zlib` level 9) as the open image's kernel (`tools/mkbios.py`
  `build_open`). The 256 KB image of v0.4.6 has about 66 KB free there (0x29680-0x39C50); the loader should take a
  few KB.
- In front of the compressed loader sits a small **uncompressed stub**: the decoder the 2BL already uses,
  `include/xk/inflate.h` (header-only, about 1-2 KB of code). At each launch the kernel copies stub and compressed
  loader from flash into temporary pages and jumps to the stub; the stub inflates the loader into more temporary
  pages and runs it. All of them are freed before the title is loaded. Before a title loads all of its memory is
  free, so the pages cost titles nothing. The flash is mapped again for the copy, as `quick_reboot.S` does for the
  `.data` copy.
- So the resident kernel has no decompressor either: a copy and a jump (decided 2026-11-13, compressed from the
  start). The blob has a small header (magic, sizes, entry offset, the kernel build it belongs to); a mismatch, as
  with a flash from another build, loads no extensions and logs it.
- **Resident in the kernel** stays only what runs while the title runs: the title's thunks for hooked ordinals
  pointing at their chains (`XepResolveKernelThunks`), the three event call sites, signalling `StopEvent` and calling
  `Unload` from `HalReturnToFirmware(2)`, and the index page's persisted pointer. Tens of bytes to a few hundred:
  within the image's slack, or paid for as spec/oxos_internal/33 §1.1 did.
- **Netboot:** a netbooted kernel's flash holds the netboot image, not its own loader. The 2BL's downloaded image
  includes the blob; the kernel keeps a copy of it in held pages (spec/oxos_internal/09 §7), a cost only on
  development consoles. *Open:* the exact place.

## 9. The SDK: building an extension
Extension writers do not need nxdk. nxdk's runtime (its C library, `winapi`, heaps, threads, SDL) is made for a
title: it allocates title memory, sets up title-only state, and is torn down at the quick reboot. None of it is safe
in the kernel. An extension needs much less, and what it needs is the kernel's.

**Layout, `sdk/xos/` in this repository (MIT, §11):**

| Part | Contents |
|---|---|
| `include/xos/extension.h` | `xos::Extension<T>`, `XOS_EXTENSION(...)`, the events and their structures, `XOS_EXTENSION`, `XOS_CONTEXT`, `XOS_API`, `XOS_KERNEL_INFO`, `XOS_API_VERSION` |
| `include/xos/kernel.h` | the kernel's types and constants (NT types, `OBJECT_ATTRIBUTES`, `KEVENT`, status codes, ...) and every `xboxkrnl.exe` function in namespace `oxos` |
| `include/xos/exports.inc` | the table of kernel functions, one line each: `XOS_EXPORT(190, NtCreateFile, NTSTATUS, stdcall, (PHANDLE Handle, ...))`; generated from `kernel/exports.tsv` and the kernel's prototypes; drives `oxos::`, the hookable list and the class's compile-time checks |
| `lib/xboxkrnl.lib` | the import library: generated from `kernel/exports.tsv` with `tools/gen_def.py` and `lld-link /DEF`, the same `.def` the kernel links with |
| `lib/xosrt.lib` | a small freestanding runtime: `memcpy`/`memset`/`memmove`/`memcmp`, string functions, `snprintf`, C++ `new`/`delete` on `ExAllocatePoolWithTag`, the compiler's 64-bit division helpers, and the helpers of §5.1 |
| `extension.mk` | the build rules (below) |
| `docs/kernel_functions.md` | every hookable kernel function, ready to copy into a class (§9.1) |
| `examples/hello/` | the smallest extension: counts launches in its state page and logs each title |

### 9.1 The kernel function reference
`sdk/xos/docs/kernel_functions.md` lists **every kernel function an extension can hook**, each as the member
function declaration to paste into the class, with an empty body that passes the call on. A user finds the function,
copies the block, and edits the body.

- **Generated, never written by hand:** from the same table as the SDK (`include/xos/exports.inc`), by the tool that
  generates the table, so the reference, `oxos::` and the class's compile-time check cannot disagree. `make sdk`
  regenerates it, and a check fails the build when the committed file differs from the generated one.
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

**Build:** the same compiler and linker as the kernel and nxdk, `clang` and `lld-link`, nothing else:

```make
# examples/hello/Makefile
NAME := hello
SRCS := hello.cpp
include ../../extension.mk
# extension.mk: clang++ --target=i386-pc-win32 -march=pentium3 -std=c++20 -ffreestanding -fno-exceptions -fno-rtti
#               -fno-builtin -fno-threadsafe-statics -O2 ... ; lld-link /DLL /NOENTRY /NODEFAULTLIB /SUBSYSTEM:NATIVE
#               /OUT:$(NAME).xos ... xosrt.lib xboxkrnl.lib
```

```cpp
// examples/hello/hello.cpp
#include <xos/extension.h>

class Hello : public xos::Extension<Hello> {
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
    void OnTitleLoaded(const xos::Title &title) {
        Log("title %08lx %s", title.Id, title.Name);
    }
};

XOS_EXTENSION(Hello, .Name = "hello", .Version = "1.0.0", .Author = "OXOS", .Description = "Counts launches",
              .Licence = "GPL-2.0-or-later");
```

**Install:** copy `hello.xos` to `C:\OXOS\extensions\` (File Manager or FTP) and add it to
`C:\OXOS\oxos_extensions.ini`:

```ini
[hello]
extension = hello.xos
```

It runs from the next launch.

### 9.2 Test extensions
`tests/extensions/`, built with the SDK, run in xemu by `make test_extensions` on a test disk with an
`oxos_extensions.ini`, through a cold boot and two quick reboots (`test_relaunch`'s title). Checked from the serial log
and from the test title.

**`xostest.xos`**, every part of the API:

| It does | It proves |
|---|---|
| `Load`: on `StateIsNew()` writes a magic and `Launches = 0` to its state page; `Launches++`; logs `xostest: launch N gen G` | the state page survives quick reboots: `N` = `Generation()`, and both are 1 after a cold boot |
| hooks `NtQueryFullAttributesFile`: for `D:\__xostest__` it answers success with size 0x0C0FFEE, other paths go to `oxos::` | a hook replaces a title's call and passes the others on; the test title asks for the path and checks the answer (without the extension the file does not exist) |
| `OnTitleLoading`, `OnTitleLoaded`, `OnTitleExiting` log path, title ID, launch kind | each event once per launch, in order, with the data `test_relaunch` expects |
| `Main`: counts a heartbeat in its state page every 10 ms until `WaitForStop` | `Main` runs while the title runs and stops when asked |
| `Unload`: sets a "clean exit" flag in its state page; the next `Load` logs `xostest: previous unload ok, heartbeat H` (H > 0) | `Unload` runs at the quick reboot, and `Main` ran |
| reads `Setting("answer")` from its section, `ReadFile`s a file next to it, allocates and frees pool with `oxos::` | settings, the SDK runtime and the kernel imports |

**Companions**, one check each:
- `chain.xos`, listed after `xostest`: hooks the same `NtQueryFullAttributesFile` and adds 1 to the size its
  `oxos::` call returns. The title must see 0x0C0FFEF: the chain runs in ini order and both hooks run.
- `badver.xos`: a wrong `ApiVersion`; refused and logged, the others load.
- `failload.xos`: `Load` returns `STATUS_UNSUCCESSFUL`; unloaded, its hook not applied, the others go on.
- `badimport.xos`: imports a function by name from another DLL; refused.
- `disabled.xos` with `enabled = no`, and `unlisted.xos` in the folder but not in the ini: neither loads.

**Runs:**
1. `test_extensions`: the ini and the files above; cold boot and two quick reboots.
2. Safe mode: the same disk with B held at boot (xemu's controller input): no `xostest:` lines, no fake file, also
   after a quick reboot.
3. No `oxos_extensions.ini` (the files still in the folder): nothing loads, and the memory test title starts with the
   same free pages as without the feature (15 960 in xemu at 64 MB).
4. `crash.xos` (writes to address 0 in `Load`), a run of its own: the error screen names its section.

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
| **Title patches** (`title_patches.ini`, spec/oxos_internal/25 §3; `patchfile.cpp`, `XepLoadPatches`) | in the kernel, 1 704 bytes resident plus the records | `oxos_title_patches.xos`, shipped in the trainers zip with its patch file, now **`oxos_title_patches.ini`** (the same format as `title_patches.ini`), and an `oxos_extensions.ini` that lists it; reads `C:\OXOS\oxos_title_patches.ini` in `OnTitleLoaded` | `OnTitleLoaded` (the title's ID, version, section headers) patches the preloaded sections; a hook on `XeLoadSection` (327) patches a section after `oxos::XeLoadSection` has read it |
| **Netbooted title** (`nettitle.cpp`, spec/oxos_internal/09 §7) | in the kernel, 981 bytes resident | `oxos_nettitle.xos`; installs the package as `E:\NAME\` and replaces the launch's XBE path with it | the netboot package's address and size in the kernel facts (§5.2; the kernel still keeps those pages from the memory manager); `OnTitleLoading` replaces the path |
| **Call tracer** (`trace.cpp`, `trace.S`, spec/oxos_internal/01) | a separate kernel build, `TRACE_IMPORTS=1/2` | `oxos_tracer.xos`, listed first in `oxos_extensions.ini` so it sees the title's calls first; release kernels can be traced | `HookExport` from plain C with its own assembly stubs for every hookable ordinal (the stubs keep the registers and jump to `Next`); its ring outside the memory manager at a fixed physical address with 128 MB, as `TRACE_IMPORTS=2` does |

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
  - Needs a new event: **`XOS_EVENT_CONTROLLER`**, every controller state a title reads, from the hook the in-game
    reset already has on `XInputGetState` (spec/oxos_internal/22 §2).
  - The event only signals `Main`; `Main` reads the front buffer (its address, pitch and format from the GPU's
    display registers) and writes a BMP or PNG to `E:\Screenshots\` while the title goes on.
- Others: a network log of a title's file and network calls, cheats (memory search and writes, on top of the
  trainers), save-game redirection or backup, a background FTP server for titles that do not use the network.

## 11. Decisions
- **Licence (2026-11-13).** The kernel stays GPL-2.0. The SDK (headers, `xosrt.lib`, build files, examples) is open
  source under the **MIT** licence, so an extension built with it may use any licence: open source is preferred,
  closed source is allowed. (A GPL SDK would make every extension that links `xosrt.lib` GPL too, which is why the
  SDK is not GPL.) The SDK is written for it, not copied from GPL kernel sources. The descriptor's `Licence` field
  (an SPDX name, e.g. `GPL-2.0-or-later`, `MIT`, or `Proprietary`) is required and is logged at load.
- **Trust (2026-11-13):** every enabled extension in `oxos_extensions.ini` is loaded and run, with no signing and no
  confirmation, as XBEs are today. The ways out are the File Manager (X) and safe mode (B).
- **`oxos_extensions.ini` is required (2026-11-13):** without it nothing loads; it gives the order (and so the chains),
  switches and settings (§3.1).
- **Hooks are for titles' calls (2026-11-13):** an extension implements kernel functions by name, and `oxos::Name`
  passes a call on to OXOS (§4.4, §7.1).
- **The loader is a compressed overlay in the flash (2026-11-13)** (§8.1).

**Open:** a crashed extension's persisted "off" mark (§3.4), `Unload` timing (§4.3), the kernel facts list (§5.2), more
events (§7.2), extensions calling each other (§8), where the netbooted loader is kept (§8.1), the proposals of §10.1.
