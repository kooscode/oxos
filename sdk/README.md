# OXOS kernel extension SDK

Builds kernel extensions (`.xos` files) for the OXOS kernel, in C++ (plain C works too). An extension is a small DLL that the kernel loads from
`C:\OXOS\extensions\` before every title, as `C:\OXOS\oxos_extensions.ini` lists it. MIT licence: an extension built with it may use any licence; open source is
preferred. The full rules: `spec/oxos_internal/34_kernel_extensions.md` in the OXOS repository.

## 1. What you need
LLVM's `clang` and `lld-link` (18 or newer) and GNU make, on Linux, macOS or Windows. Not nxdk.

| | |
|---|---|
| Linux | `apt install clang lld make` (or your distribution's packages) |
| macOS | `brew install llvm lld`, and the `make` of the Xcode command-line tools |
| Windows | LLVM from its installer, and `make` (`scoop install make` or `choco install make`) |

## 2. Get the SDK
- **From a release:** unpack `oxos_sdk_vX.Y.Z.zip` (next to `oxos_vX.Y.Z.zip`). You get the `oxos_sdk/` folder. Use the OXOS kernel of the same release on the
  console: an extension imports kernel functions an older kernel does not have.
- **From the OXOS repository** (also needs `python3`): `make -C sdk` builds the SDK as it ships into `build/sdk/oxos_sdk/` and every example with it (the `.xos`
  files land in `build/sdk/examples/NAME/`). `make -C sdk VERSION=X.Y.Z` also makes the zip, `build/sdk/oxos_sdk_vX.Y.Z.zip`.

## 3. Build an example
```sh
cd oxos_sdk/examples/example_run_loop
make                                   # example_run_loop.xos, here
make LLVM=C:/LLVM/bin                  # Windows, when clang is not on the PATH
```

| Example | Shows |
|---|---|
| [`example_run_loop`](examples/example_run_loop) | `ExtensionRunLoop`: work while a title runs, timed with the kernel's millisecond count; a number setting from its own `.ini` |
| [`example_kernel_hook`](examples/example_kernel_hook) | a hook on a kernel function (`NtOpenFile`) that uses a setting from its own `.ini` at every call; log lines in `ExtensionStart` and `ExtensionTerminate` |
| [`example_title_event`](examples/example_title_event) | a title event, the state page that keeps data from one launch to the next, a setting and a data section from its own `.ini`, kernel functions called by name: a greeting per title, and the LED turns orange for one title |

## 4. Write your own
Copy the example closest to what you want into a folder of your own, rename it, and set its `Makefile`:

```make
NAME := my_extension
SOURCES := my_extension.cpp
include path/to/oxos_sdk/oxos_extension.mk
```

The smallest extension:

```cpp
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "my_extension", .Version = "1.0.0", .Start = ExtensionStart);

static BOOLEAN ExtensionStart(void) {
    oxos_log("Hello from launch %lu.", oxos_launch_number());
    return TRUE; // stay loaded (FALSE: unload)
}
```

## 5. Install it
1. Copy the `.xos` to `C:\OXOS\extensions\` on the console (the OXOS File Manager, or FTP). The kernel creates the folder at every boot.
2. List it in `C:\OXOS\oxos_extensions.ini`, which turns extensions on and sets the order they load in. The SDK's root has a sample listing the three
   examples (copy it to `C:\OXOS\`):

   ```ini
   [example_run_loop]
   extension = example_run_loop.xos
   enabled = yes
   ```

   **An extension loads only with `enabled = yes`.** Without that line, or with `enabled = no`, it stays off.

3. Put its settings, and any data, in its own file next to it, with its name: `C:\OXOS\extensions\example_run_loop.ini` (each example has one; see
   "Settings and data" below).
4. Start any title: the extension loads before it, and at every launch after it. `enabled = no` switches it off again; holding **B** at boot starts without
   any extension (until the next cold boot).

### Settings and data: the extension's own `NAME.ini`
An extension's own file, `C:\OXOS\extensions\NAME.ini` next to `NAME.xos`, holds two different things, read in two different ways:

```ini
; SETTINGS: the keys at the top, before the first [section].
buttons = back black
format = png

; DATA: the [sections] after the settings.
[4D530064]
Infinite Ammo = yes
Never Die = no
```

| | **Settings** | **Data** |
|---|---|---|
| Where | the keys at the top of `NAME.ini`, before the first `[section]` | the `[sections]` after them |
| When read | **once, into memory, when the extension loads** (before each title) | **from the disk, one section each time it is asked for**; the rest of the file is never in memory |
| How | `oxos_setting("buttons")`, `oxos_setting_number("interval", 10)` | `oxos_ini_section("4D530064", keys, count)`, `oxos_ini_value(keys, count, "Infinite Ammo")`, `oxos_ini_free(keys)` |
| Where it can be called | anywhere, **hooks included** | only where the disk may be read: `ExtensionStart`, `ExtensionRunLoop`, `ExtensionTerminate`, title events; **never in a hook** |
| Limits | **at most 32 settings, each value up to 255 characters**, one-word keys | **no limit** on sections or keys (the file up to 1 MB); keys may have spaces |
| For | a few options the user sets: a button, a format, an interval | lists that grow: a section per game, per cheat, per anything |

Read data once, in `ExtensionStart` or a title event, and keep what you need: every `oxos_ini_section` call reads the file again. Settings still written in
the extension's section of `oxos_extensions.ini` (how it was done before) keep working for the keys `NAME.ini` does not set.

`oxos_log` lines go to `C:\OXOS\log\extensions.log` (read it over FTP or with the File Manager; it moves to `extensions_1.log` at 10 MB), and to the debug
serial port of a debug kernel, **only when the extension's section of `oxos_extensions.ini` says `logging_enabled = yes`**. Logging is off unless turned
on: without that line, or with `logging_enabled = no`, the extension logs nothing anywhere, whatever `oxos_set_log_target` asked. The SDK and the kernel
apply it, so the extension needs no code for it; the SDK's `oxos_extensions.ini` turns it on for the examples.

## 6. What an extension can use
`include/oxos_sdk.h` explains each part where it is declared.

| | |
|---|---|
| Its functions | `ExtensionStart` (before each title: read settings, hook, register for events; TRUE to stay), `ExtensionRunLoop` (again and again while the title runs), `ExtensionTerminate` (the title starts another) |
| Logging | `oxos_log(Format, ...)`, `oxos_set_log_target` |
| Settings (in memory) and launches | `oxos_setting("key")`, `oxos_setting_number("key", Default)`, `oxos_launch_number()`, `oxos_kernel_version()` |
| Data (from the disk, one section at a time) | `oxos_ini_section("section", keys, count)`, `oxos_ini_value(keys, count, "key")`, `oxos_ini_free(keys)` (C++) |
| State page | `oxos_state()`: 4 KB that keep their contents from one launch to the next, until the next cold boot (all zero at first, and for a new `Version`) |
| Files and folders | `oxos_file_read`, `oxos_file_write` (a whole file), `oxos_file_open`, `oxos_file_create`, `oxos_file_delete`, `oxos_file_rename`, `oxos_file_exists`, `oxos_file_list`; `oxos_directory_create`, `oxos_directory_delete`, `oxos_directory_rename`, `oxos_directory_exists`, `oxos_directory_list`; `oxos_list_free` |
| Hooks | `oxos_hook(NtOpenFile, My_NtOpenFile)` and `oxos_next(NtOpenFile)(...)` (in `ExtensionStart`) |
| Title events | `oxos_on_title_loading`, `oxos_on_title_loaded`, `oxos_on_title_exiting` (in `ExtensionStart`); `oxos_title_redirect` starts another XBE in place of a title being loaded |
| The C library | `strlen`, `strcmp`, `strcasecmp`, `strchr`, `strstr`, `strlcpy`, `snprintf`, `strtoul`, `memcpy`, `memset`, `toupper`, `isdigit`, ... under their usual names |
| The kernel | every function of the Xbox kernel, by name, as `include/oxos_kernel.h` declares it (`NtReadFile`, `KeTickCount`, `HalWriteSMBusValue`, ...) |

## 7. Things to know
- **Each launch starts afresh.** The kernel reloads every extension from the disk before each title, so global variables start from their initial values every
  time. Only the state page carries over.
- **`ExtensionRunLoop` must return quickly.** One kernel thread calls every extension's in turn, about every millisecond; a slow one holds up the others. Do a
  little work per call and keep time with `KeTickCount`; a call that takes over 500 ms is logged in `C:\OXOS\log\extensions.log`.
- **Hooks see the title's calls only**, not the kernel's own (`oxos_file_*` run inside the kernel, so no hook sees those either). A hook has exactly the kernel
  function's arguments, passes a call on with `oxos_next`, and runs on the title's threads, possibly several at once: count with `InterlockedIncrement`.
- **The Xbox kernel's `Rtl*` functions** are there for hooking what titles call; an extension's own code uses the C library.
- **No floating point** in kernel code: the kernel does not save those registers for extensions (the SDK's build rules turn it off).

## In the SDK
| File | Contents |
|---|---|
| `include/oxos_sdk.h` | what an extension includes: the descriptor, the SDK's functions, the title events |
| `include/oxos_kernel.h` | the Xbox kernel's functions and the C library, with their types (generated from the OXOS kernel) |
| `lib/oxos_sdk.lib` | the one library: the SDK's functions, the C functions the kernel does not have itself, and the kernel's import library |
| `oxos_extension.mk` | the build rules (or use your own: the flags are in it) |
| `oxos_extensions.ini` | which extensions OXOS loads, in which order: a sample listing the examples, for `C:\OXOS\` |
| `examples/` | example_run_loop, example_kernel_hook, example_title_event, each with its own settings file (`NAME.ini`) |

**The SDK's source** (`sdk/` in the OXOS repository) holds the two headers, the SDK's functions (`src/oxos_sdk.cpp`, with `src/oxos_extension_context.h`, the
context the kernel fills, shared with the kernel and not shipped), and what `tools/gen_oxos_sdk.py` generates from the kernel: `include/oxos_kernel.h`, the
kernel's export list (`src/oxoskrnl.def`), its C functions for the library (`src/oxos_c_library.cpp`) and a link check of every kernel prototype
(`src/kernel_table_check.c`). Examples are only built with the package, never from the source, so what ships is what is tested.
