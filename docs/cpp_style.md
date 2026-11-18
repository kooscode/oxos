# OXOS C++ style guide

How OXOS's C++ is written: the kernel, the 2BL, the host tools and the nxdk apps. The rules follow the [C++ Core
Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines), the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and
the [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html), adapted to a freestanding kernel whose exported names and structures must match the
original Xbox kernel's ABI. Where those guides disagree, or where the ABI decides, the choice is stated here.

The aim: a reader who opens any folder can tell what it holds from its name, who opens any file can tell what it does from its first lines, find each piece of
state and each constant where the rules say it is, and trust every comment to describe the code directly below it.

## 1. Language

- C++20 for all native code; there are no `.c` files. One exception: the kernel extension SDK (`sdk/`) supports extensions in plain C (spec/oxos_internal/34
  §9), so its public headers are valid C17 as well as C++20 (their constants are `#define`s, their structures `typedef struct`), and
  `tests/extensions/plain_c.c` checks that a C extension builds and runs: it is the repository's only `.c` file. The SDK's own code and its example
  (`sdk/examples/example_*/`) are C++.
- The kernel and the 2BL are freestanding: no exceptions, no RTTI, no standard library, no static constructors. The kernel is built `-Oz` on purpose (free
  memory for titles); a readability change must not cost code size.
- OXOS is its own kernel, not Windows NT. Strings, memory and formatting use the C library, under its standard names (`kernel/src/runtime/c_library.h`:
  `strlen`, `strcmp`, `strcasecmp`, `strlcpy`, `memcpy`, `memset`, `memchr`, `snprintf`, ...), not hand-written loops; files, devices and links are named
  with C strings (`io::OpenFile`, `io::CreateFile`, `io::DeleteFile`, `io::RenameFile`, `io::CreateDevice`, `io::CreateLink`, `objects::CreateDirectory`).
  The Xbox kernel's `Rtl*` string, memory and formatting exports are for titles only: `tools/check_c_library_use.py` fails the build when OXOS's own code
  calls one (user, 2026-11-14). A C function the kernel does not use itself goes in `runtime/c_library_extensions.cpp`, which only the SDK carries.
- Constants are `constexpr`, not `#define` (Core Guidelines ES.31). Macros are for build options (`XK_DEBUG_SERIAL`, `XK_BOOT_SELFTEST`) and for what only a
  macro can do (`XK_SERIAL`, `XK_SIZE`, the unit-test `TEST`).
- Layouts shared with titles, hardware or the reference kernel are checked at compile time: `XK_SIZE`, `XK_OFFSET`, `static_assert`.
- `nullptr`, never `NULL` or `0`, for pointers.
- `auto` only where the type is spelled on the same line (a cast) or is obvious from it.
- C-style casts are accepted where the code converts between integers, pointers and hardware addresses, as the ABI makes it do everywhere; elsewhere prefer
  `static_cast`/`reinterpret_cast`.
- No comma operator to chain statements (`a = 1, b = 2;`): one statement per line (Core Guidelines ES.10, ES.43).
- Code that is never used is deleted, not left behind or commented out: a variable written and never read, a flag that is always 0, an empty macro, the
  leftovers of a removed feature.

## 2. Names

- Exports, and the structures titles see, keep the Xbox kernel's names: `KeWaitForSingleObject`, `KTHREAD`, `IRP`, `OBJECT_ATTRIBUTES`, and their parameter
  names. They are global (`XBAPI`, C names), because titles and kernel extensions import them by those names; so is anything assembly uses.
- This is OXOS's own kernel, not NT: everything else lives in a namespace per source folder, `xk::<folder>` (`xk::storage`, `xk::memory`, `xk::titles`, ...),
  with names that say what they do, without subsystem prefixes. The hardware drivers' folders are one level down and so are their namespaces: `hardware/ide/` is
  `xk::hardware::ide`.
  Functions put the verb first: `storage::DescribeDisk`, `hardware::ide::InitDvdDrive`, `storage::ReadDiskSectors`; data says what it holds:
  `hardware::ide::DiskLengthInSectors`, `hardware::ide::DvdDeviceObject`.
  - Inside its subsystem a name is written plain (`DescribeDisk(...)`); another subsystem writes `storage::DescribeDisk`; the tests outside the kernel write
    `using namespace xk;` and the same.
  - A subsystem's `.cpp` files are wrapped in `namespace xk::<folder> { ... }`, its headers declare its names in the same namespace. An exported variable is
    defined outside the namespace, so it keeps its C name; an exported function may be defined inside (`XBAPI` keeps its C name).
  - The tools that read the linker map (`tests/boot_test.py`, `tools/check_kernel_layout.py`) take these names as `storage::DiskLengthInSectors`.
  - Converted 2026-11-13, every subsystem. What keeps an NT-style name: the exports; the names assembly uses (`KiPcr`, `KiQuickRebootFlag`, the trap and
    interrupt stubs, `HalpIgrCheck`, `XkUDivMod64`, ...); the call tracer (`titles/title_tracer.cpp`, until it becomes `oxos_tracer.xos`); the encoder tables
    `tools/gen_av_tables.py` generates; types and enumerators (`MMPFN_USAGE`'s `MmPoolUsage`, ...). `arch/` is assembly, plus `arch.h` (x86 primitives).
- Format and standard names inside an identifier are written in capitals: `filesystems::FATXLookup`, `filesystems::EXFATWalk`, `storage::DiskHasEXFATLayout`.
  Written `Exfat...` they read like the NT executive's `Ex` prefix (`ExAllocatePoolWithTag`), and `Fatx...` like an unrelated word. In comments the formats keep
  their proper names, FATX and exFAT.
- Something only one file uses goes in an anonymous namespace (or is `static`), named for what it does in that file (`DvdStartTransfer`, `Reg32` in the AC'97
  player, `Sine` in the synthesizer).
- Local variables and the parameters of internal functions: camelCase, short when the scope is short.
- Constants and enumerators: `UPPER_SNAKE_CASE` (`SMB_STATUS_DONE`, `IDE_MAX_PARTITIONS`). A register block taken from a datasheet may keep the datasheet's
  names.
- Types: the reference's `UPPER_SNAKE` structures for kernel and ABI types; PascalCase for small helper classes inside one file.

### File and folder names

- Plain English, lower case, words joined with underscores, never dashes: `core/`, `storage/disk_layout.cpp`, `spec/oxos_internal/00_boot_splash.md`. Python
  needs underscores for modules, so the whole repo uses them.
- The same rule covers every name the repo defines: specs, docs, scripts, generated files and build folders (`build/trace_full`, `oxos_ftp.iso`), xemu state
  folders and make targets (`make test_dash`).
- A name says what the file holds, not where it came from: `dvd_drive.cpp`, not `cdrom.cpp`; `object_manager.cpp`, not `object.cpp`. No two- or three-letter
  abbreviations.
- Names the repo does not own stay as they are: vendored files (Intel's microcode, the game trainers), upstream projects and their URLs (`xemu-dashboard`),
  command-line flags (`--title-name`), the user's own files.

### Names users and extension authors see

- **Descriptive, never short or jargon:** a console user or an extension author must understand a name without reading the code. `oxos_extensions.ini`, not
  `oxos_ext.ini`; `oxos_title_patches.xos`, not `oxos_patches.xos` (which reads like kernel patches); `oxos_extension_context.h`, not `abi.h`. When a name could
  be read two ways, take the longer one that cannot.
- **The `oxos_` prefix** marks everything OXOS provides that sits next to other people's things: files on the console (`C:\OXOS\oxos_settings.ini`,
  `oxos_extensions.ini`, `cache\oxos_kernel.cache`), OXOS's own extensions and their settings (`extensions\oxos_NAME.xos` with
  `extensions\oxos_NAME.ini`), and the SDK's headers (`oxos_sdk.h`, `oxos_kernel.h`). Names without it belong to users and third-party extensions.
- **OXOS is written `OXOS` or `oxos`, never `Oxos`**, in identifiers as in text: `OXOS_CONTEXT`, `OXOS_API_VERSION`, `oxos_log`, `oxos::NtCreateFile`.
- **Everything the user writes or reads:** ini keys (`extension =`, `enabled =`, never `ext =`), folders (`C:\OXOS\extensions\`, not `ext\`), section names, log
  and screen text.

## 3. Source tree

The folders and what each holds are listed in the kernel README (`kernel/README.md`, "Layout"). The style behind them:

- Code is grouped by what it does, and a folder or file is named for that in plain words: `storage/`, `boot_splash/`, `disk_layout.cpp`, never a two- or
  three-letter abbreviation (`ke/`, `idex/`, `vd/`). New code goes in the folder of what it does; a file that does two jobs is split.
- A header sits in the folder of the code it declares (`kernel/src/storage/storage.h`), and is included with that folder (`#include <storage/storage.h>`;
  `kernel/src` is the include path). Each folder has one header with its name for what other folders use, and declares only what its own folder defines (exports
  included); what only some files share has a header named for it (`core/scheduler.h`, `extensions/extension_state.h`, `hardware/chipset/serial.h`,
  `filesystems/hddfs/hddfs.h`, `boot_splash/boot_splash.h`). A header that seems to have no home goes in the folder whose code it supports (the ABI's types are
  `runtime/types.h`), or in a new folder with a descriptive name; there is no `common/` or `misc/` folder. The 2BL's own headers are in `kernel/boot/`.
- Test and debug code is kept out of the release kernel by the build, not by comments: the boot self-tests are in `tests/kernel_self_tests/` (compiled into the
  kernel only with `SELFTEST=1`), debug-only code under `#if XK_...`.

## 4. File layout

Every source file follows the same order, so its parts are always in the same place:

1. **The file comment**: what the file does, and the spec it implements (`spec/stock/...`, `spec/oxos_internal/...`).
2. **Includes**: the file's own header first (its folder's, or the one named for its files: `filesystems/hddfs/hddfs.h` in `filesystems/hddfs/`), then the other
   `xk/` headers, then anything else. Only what the file uses; never the same header twice.
3. **File-wide constants and types**: those that more than one section of the file uses.
4. **File-wide state**: variables that more than one section uses, each with a comment saying what it holds.
5. **Sections**, each under a `// --- Title` header, in dependency order: a section starts with its own constants, types and state, then its helpers, then the
   functions built on them.

State is never declared between two functions in the middle of a section. A variable or constant only one section uses lives at the top of that section (Core
Guidelines ES.5: keep scopes small); anything used by more than one section moves to the top of the file. Two exceptions:

- A table that names the file's functions (a `DRIVER_OBJECT`, an `OBJECT_TYPE`, a dispatch table) follows the functions it names, at the end of their section,
  rather than needing a forward declaration of each one.
- In test files, the state of one test (its events, counters, workers' flags) sits directly above that test's functions, under the test's comment: each test is
  its own small section.

**Declarations of other files' code come from headers.** A `.cpp` file does not declare another file's functions or variables (`void HalpFoo();`, `extern ULONG
XeBar;`) itself: the declaration goes in the header of the code's folder, where the compiler checks it against the definition and every caller (Google: header
files; Core Guidelines SF.2, SF.5). The exceptions:

- symbols defined in assembly or by the linker script, declared next to the code that uses them;
- a forward declaration of a `static` function further down the same file.

**One declaration, one definition.** Each function, variable and type is declared in exactly one header and defined in one file. A type several files need
(`TIME_FIELDS`, `SLIST_HEADER`, `RTL_CRITICAL_SECTION`) is defined once in a header, never copied into each file or test that uses it.

## 5. Comments

- **A comment describes what is directly below it**, and only that: one declaration, a group of declarations (the group then ends with a blank line), a
  function, or a section. Nothing else comes between the comment and what it describes: no unrelated declaration between a comment and its function, no comment
  that sits on a declaration it does not describe. A comment that introduces a list ends with what it introduces.
- A comment on a function says what it does and what a caller must know (preconditions, IRQL, ownership), not how each line works. Inside a function, comments
  explain why, where the code alone cannot.
- **Comments are true.** A change to the code updates its comments; a moved or removed function, file or feature takes every mention of it along. Code that does
  something different from what its comment says is fixed, or the comment is.
- References name things that exist: a spec by file and section (`spec/oxos_internal/16_virtual_discs.md §3`), a source file by its path under `kernel/src/`
  (`hardware/ide/hard_disk.cpp`), or by its name alone for a file in the same folder (or, in a header, in the folder its first line names). A hardware finding
  keeps its date (`(2026-10-28)`) when the finding is the reason for the code.
- Trailing comments describe one declaration or one field; those of a group are aligned.
- A section header (`// --- Title`) sits directly above the code it names; a description of the section may follow the header as a paragraph.
- No commented-out code; no history of how the code used to be (git has it).
- A generated file says so in its first line, with the tool that writes it (`Generated by tools/gen_av_tables.py ... Do not edit; regenerate.`); the build
  checks it is current where it can.

## 6. Text on screen

- Text a user reads (error and help screens, the File Manager, the FTP server's messages) is short and plain: the fewest words that say what happened and what
  to do. A title, then a list or one line, beats a sentence; less reading is better.

## 7. Functions and statements

- Function bodies span several lines: no `int F() { return x; }` one-liners. A function with nothing to do has a comment in its body saying why. A constructor
  whose work is all in its member initializers may end in `{}`.
- A function does one thing; its name says what. Prefer early returns to deep nesting (LLVM: "Use early exits").
- A body of one simple statement may omit braces when it fits on one line, on the line after the `if`/`for`/`while`. A body that spans several lines (including
  one statement wrapped over several lines, or a body that is itself an `if`, `for` or `while`) has braces. When one branch of an `if` / `else if` / `else`
  chain has braces, every branch has them (LLVM).
- The body of an `if`, `for` or `while` is on its own line, never on the line of the condition.
- A loop with nothing to do in its body has `{}` with a comment saying what the loop waits for, never a lone `;`.
- Hardware registers, ports and magic values get a named constant where they are used more than once, or where the name says more than the number; a value used
  once with a comment naming it is acceptable.

## 8. Headers

- `#pragma once`; each header includes what its own declarations need.
- Grouped by topic, each group with a comment; a declaration whose meaning is not obvious from its name and parameters gets its own comment.
- A header declares what other files use; anything one file uses stays in that file.

## 9. Formatting

- Four spaces, no tabs.
- Lines stay within 160 characters (characters, not bytes: `§` counts as one).
- Braces on the same line as the statement (K&R), `} else {` on one line.
- One blank line between functions and between groups; never two in a row.

## 10. Checking a change

A change that is meant only to make code easier to read (moving, renaming, re-wrapping, splitting a file) must not change what the code does:

- Build every kernel configuration (release, self-test, trace, netboot) and the 2BL, and compare them with the build before the change, function by function.
  Any difference has to be one the change intended.
- Run the host unit tests (`tools/unit_tests.sh`) and the dashboard boot (`make -C kernel test_dash`).
- Search for references the change left stale: names in comments of functions or files that no longer exist, spec sections that do not exist, old paths in
  specs, docs, tests and tools.
