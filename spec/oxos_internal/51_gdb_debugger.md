# 51. The GDB debugger: the debug port, debug transports, and `oxos_gdb.xos`

Status: **implemented 2026-11-27**. OXOS's own design. It implements the debugger of spec 41 §2 over the serial port (spec 41 §3, choice 2) and the
debugger extension of spec 34 §10.1. User, 2026-11-27: remote debugging means debugging a console that is not this PC; the serial port through the Modxo
for now, built so that a network-transport extension added later just works. The same day (phase 2): the library list, gdb following the console from one
title into the next, starting titles from gdb, watchpoints of any size.

Code:
- the stub: `shared/gdb_stub/gdb_stub.cpp` (the GDB remote protocol, breakpoints, registers, threads, stops), `shared/gdb_stub/serial_transport.cpp`;
- the kernel's debug port: `kernel/src/debugger/debug_port.{h,cpp}`, its hooks in `core/exceptions.cpp`, `core/interrupts.cpp`, `core/bugcheck.cpp`,
  `titles/title_loader.cpp`, `diagnostics/serial_log.cpp`, `extensions/extension_runtime.cpp`; the SDK's side in `sdk/include/oxos_sdk.h` §9;
- the extension: `kernel/extensions/oxos_gdb/` (`oxos_gdb.cpp`, `oxos_gdb.ini`);
- a GDB=1 kernel's own stub: `kernel/src/debugger/gdb_stub.cpp`.

## 1. What it is
gdb on a PC debugs the running title, and the kernel under it, on a console: breakpoints (int3, up to 64), the four hardware breakpoints and watchpoints
(DR0-DR3), single steps, Ctrl-C, registers (the integer ones, x87 and SSE), memory, the threads, a function called from gdb (`call`), the title's unhandled
exceptions and the bug check, which stop in gdb at their cause; gdb loads the symbols of the kernel and the extensions itself (the library list, §6.1),
follows the console into each new title (§6.2) and starts titles (`run`, `monitor launch`, §6.3). One stub, two homes:

| | `oxos_gdb.xos` (§3) | A `GDB=1` kernel's own stub (§5) |
|---|---|---|
| Debugs | the title, and the kernel while the title runs | the kernel from its first steps, and titles |
| On | any release kernel with the debug port: copy the `.xos`, `enabled = yes` | a debug build, netbooted or flashed |
| Session kept | in the extension's state page (spec 34 §6) | in the kernel's persistent data |

Both are the same code (`shared/gdb_stub`): everything the stub needs from the kernel goes through the debug port (§2), and every byte through a debug
transport (§4).

## 2. The kernel's debug port
Every kernel has it (resident, also in release kernels). OXOS's export **438, `oxos_debug_port`**, returns its table, `OXOS_DEBUG_PORT` (`oxos_sdk.h` §9),
which carries its own `Size` (it only grows).

### 2.1 Attaching
- **One debugger at a time** (`OXOS_DEBUGGER`: a table of the debugger's functions). `AttachDebugger` answers `STATUS_DEVICE_BUSY` (0x80000011) when one is
  attached; a `GDB=1` kernel's own stub is attached from the boot on and stays, so `oxos_gdb.xos` refuses to start there (it logs why).
- **One registered transport at a time** (`RegisterTransport`, `STATUS_DEVICE_BUSY` likewise); `Transport()` returns it, or null.
- **Lifetime:** extensions are loaded for one title (spec 34 §4.3). After every extension's `Terminate` at the title's quick reboot, the kernel forgets an
  extension's debugger and the registered transport (`ForgetExtensionDebugger`), as their code goes away; each extension attaches or registers again in its
  next `ExtensionStart`. A debugger's own `Terminate` takes its int3s and debug registers out of memory first (§3.3).

### 2.2 Where the kernel calls the debugger
Each place calls the attached debugger's function, and returns at once without one:

| Function | Called from | The debugger's answer |
|---|---|---|
| `HandleTrap(Frame)` | `KiDispatchTrap`, the first thing for every processor trap (before the S-list page-fault restart, the debug service, the exception dispatch) | TRUE: the trap was its own (breakpoint, single step, break-in, a fault in its own memory access); the frame resumes |
| `StopOnUnhandledException(Frame)` | the exception dispatch, when no handler took the exception, before the unhandled-exception lines and the bug check | TRUE: resume the frame (gdb changed it); FALSE: the bug check follows |
| `StopOnBugCheck(Code)` | `KeBugCheckEx`, after the last-words record, before the screen | returns when gdb resumes |
| `WaitAfterBugCheck()` | after the bug-check screen, again and again (without a debugger: `hlt`) | serves gdb: a debugger that connects after the crash looks at the console as it was |
| `PollBreakIn(InterruptedFrame)` | the clock interrupt (IRQ 0), every millisecond: the interrupted eip, cs, eflags | a byte from gdb (Ctrl-C 0x03, or the `$` of an attaching gdb's first packet) sets the interrupted code's trap flag; its single-step trap is the stop |
| `TitleLoaded(Path)` | the title launcher, after the extensions' title-loaded event, before the title's entry point | breakpoints set in the title before it was there go in |
| `PrintDebugText(Text, Length)` | the debug log: a title's `DbgPrint` and `OutputDebugString`, an extension's `oxos_log`; in a `SERIAL=1` kernel also the kernel's own lines | TRUE when it sent the text to gdb (O packets) |

The trap frame is the kernel's (`OXOS_TRAP_FRAME`: edi esi ebp esp-slot ebx edx ecx eax, vector, error code, eip cs eflags; the stopped stack pointer is the
address after eflags, no privilege change). A release kernel keeps no debug log (spec 03): it formats a debug line only while a debugger is attached.

### 2.3 What the port tells a debugger
Threads are KTHREAD addresses; the stub's thread ids are those / 4 (1 before the scheduler runs).

| Function | Gives |
|---|---|
| `IsAddressValid(Address)` | `MmIsAddressValid`: the stub reads and writes only addresses whose page is present, below 0xFD000000 (the GPU, MCPX and flash registers: reads can have side effects) |
| `CurrentThread()` | the running thread (KPRCB's), null before the scheduler |
| `ListThreads(Threads, Max)` | the idle thread, then the system process's thread list (titles' threads included), each one whose KTHREAD is readable |
| `ReadThreadRegisters(Thread, Registers)` | a switched-out thread's registers in gdb's order, from its switch frame at KernelStack: ebx, ebp, esi, edi, eip (the return address), eflags, esp (after the frame); eax, ecx, edx 0; cs 8, ss ds es fs gs 0x10 |
| `DescribeThread(Thread, State, Priority)` | its state (0 new ... 5 waiting) and priority |
| `FloatOwnerThread()`, `FloatArea(Thread)` | the thread whose x87/SSE state is in the processor (lazy switching), and where another thread's FXSAVE image is (StackBase − 0x210) |
| `ListImages(Images, Max)` | the images besides the kernel and the title (`OXOS_DEBUG_IMAGE`: name, base, kind): the kernel modules while mapped (`kernel_modules`, 0xB0000000, only during a launch), then each loaded extension (its `oxos_extensions.ini` section), in load order. Their PE headers are at their bases |

## 3. `oxos_gdb.xos`
`C:\OXOS\extensions\oxos_gdb.xos`, shipped off (`[oxos_gdb] enabled = no` in `oxos_extensions.ini`). Its own `oxos_gdb.ini`:

| Setting | Meaning |
|---|---|
| `wait_for_gdb = yes` | the title stops before its entry point until gdb attaches (`TitleLoaded` waits as at a breakpoint, after writing `gdb: waiting for gdb at the title's start` as plain text on the transport). Default no |
| `title = NAME` | the title gdb debugs: the one whose path contains NAME (any case), e.g. `CdRom0`; gdb's `monitor title NAME` changes it for the session. Its title breakpoints go only into it, and only it waits. Default: every title. Set only when the session starts |
| `title_symbols = FILE` | the title's symbol file (e.g. `main.exe`) as the library list names it (§6.1), for a gdb that does not have the title as its own program; `monitor symbols FILE` changes it. Default: none (the title is not listed) |

### 3.1 At each title's start (`ExtensionStart`)
1. The port (`oxos_debug_port`); a kernel whose table is smaller than the extension's is refused (the extension does not load).
2. The serial transport is set up (§4.1).
3. The session (§3.2) is taken from the state page; breakpoints in titles (below 0x80000000) are marked pending, the kernel's are put in again, the debug
   registers are loaded.
4. `wait_for_gdb`, `title` and `title_symbols` from its ini (the last two only when the session has none yet).
5. `AttachDebugger`; refused (another debugger, a `GDB=1` kernel's): the breakpoints come out again and the extension unloads.
6. `gdb: stub ready` as plain text on the transport while gdb is not attached: the Modxo logger's bridge (`tools/gdb_bridge.py`) passes gdb's bytes on
   only after that line.

Its run loop (spec 34 §4.3) starts the XBE gdb asked for (§6.3) with `oxos_title_launch`, at passive level, once the target runs again.

### 3.2 The session
`oxos::gdb_stub::SESSION` (well under the 4096-byte state page): a magic (a zeroed page is a new session), gdb attached, waiting for a stop reply, stepping,
the wait at the title's start, a launch asked for, the 64 breakpoints (address, saved byte, inserted), the four debug registers (address, kind, length,
and the watchpoint each is part of), the title filter, the title's symbol file, the debugged title's path (from `TitleLoaded`) and the path of a launch.
gdb's session goes on across launches: a breakpoint in the dashboard's code is hit in the next launch of the dashboard; gdb waiting for a stop reply (after
`c`) keeps waiting through the launch and gets a library stop when the title it debugs is there again (§6.2).

### 3.3 At the title's end (`ExtensionTerminate`)
Detaches, then takes every int3 out of memory (the session keeps them, pending) and clears DR7: between two launches no code of the debugger is there, so
nothing may trap into it.

### 3.4 Costs
The extension's image and buffers (its code, a 4 KB packet buffer and a 4 KB reply buffer, an 8 KB stack of its own for stops, a 512-byte FXSAVE buffer)
while enabled. Nothing in the extension runs while no gdb talks to it except one transport poll per millisecond (one port read). The kernel's debug port
costs about 1 KB of code and 16 bytes of data; `test_memory` keeps 15 968 free pages (2026-11-27, as before it).

## 4. Debug transports
`OXOS_DEBUG_TRANSPORT`: `ReadByte` (a byte if one has come, never waits), `WriteByte` (waits, bounded, for room; FALSE when it could not), `IsHostPresent`,
`Flush` (waits, bounded, until everything has left), and a `Name` for the log. **Every function must work with interrupts off, at any IRQL, from a trap:**
no waiting on interrupts, DPCs, locks or other threads, as a stopped console runs nothing else. The stub uses the transport registered with the port when
there is one, else the serial transport, looked up for every byte: a transport extension loaded before or after `oxos_gdb.xos` is used from its
registration on, with no change to the debugger.

### 4.1 The serial transport
COM1 at 0x3F8, a 16550 subset that the XboxDev serial adapter, a debug kit's port, xemu's `lpc47m157` and the Modxo's firmware all answer: LSR bit 0 (a byte
is there), bit 5 (room), bit 6 (all sent), RBR, THR; nothing else is read.
- **Set-up** (`serial_transport::Start`): when LSR answers (not 0xFF), nothing: the port is set up already (a `SERIAL=1` kernel), or it is the Modxo with a PC
  attached, which ignores set-up writes and empties its USB buffers when the SuperIO leaves its configuration mode. Otherwise the SuperIO at 0x2E: 0x55
  (configuration mode), register 0x26 must read 0x2E (else no SuperIO: 0xAA, done); logical device 4 (serial port 1) at 0x3F8, IRQ 4, activated; 0xAA; then
  115 200 baud (divisor 1), 8N1, FIFOs on (FCR 7), MCR 3, no interrupts.
- **The Modxo** (the debug rig, `tools/modxo/README.md`): RBR is read only after LSR bit 0 (otherwise it returns an undefined byte); LSR 0xFF means no
  host (no PC holds DTR). Its firmware (kooscode/modxo, branch `oxos`) takes the PC's power-button command, 0xFD 'P', out of the stream wherever it is,
  so a stopped stub never sees it and cannot hold it back.
- A byte that finds no room within about 200 000 LSR polls (about 0.2 s) is dropped (`WriteByte` FALSE).

### 4.2 A network transport (later)
A future extension (for titles that do not use the network) drives the network card polled, carries the protocol's bytes in UDP or TCP, and registers its
`OXOS_DEBUG_TRANSPORT` in its `ExtensionStart`; `oxos_gdb.xos` then talks over it from that moment, in that launch and every later one the extension
registers in. Its rules are §4's.

## 5. A `GDB=1` kernel's own stub
`make -C kernel GDB=1 [GDB_WAIT=1]` builds `shared/gdb_stub` into the kernel (only then: release kernels carry the port, not the stub). After the processor
set-up (`InitGdbStub`): a cold boot starts a new session (a quick reboot keeps it, in persistent data), the serial transport (set up by the kernel's own serial
log already), `AttachKernelDebugger`, the log line `gdb: stub ready`; with `GDB_WAIT=1` a cold boot then logs `gdb: waiting for gdb` and stops until gdb
resumes it. Its stop buffers lie at the end of `.bss` (`.bss$zz_gdb_stub`), so the kernel's own data stays where the other builds have it. The kernel's log
lines go to gdb as O packets while gdb waits for a stop. A thread of its own (priority 8, every 20 ms) starts the XBE gdb asked for (§6.3) with the title
launcher's `SwitchTitle`, as an extension's `oxos_title_launch` does.

## 6. The protocol
The GDB remote serial protocol, from the public GDB documentation ("Remote Protocol"). All-stop on the one processor: a stop runs inside the trap with
interrupts off, on the stub's own 8 KB stack (so gdb may write below the stopped stack pointer, as `call` does), until gdb resumes or detaches.
- **Packets served:** `?`, `qSupported` (`PacketSize=1000;swbreak+;hwbreak+;qXfer:libraries:read+;qXfer:features:read+`), `qAttached`, `qC`,
  `qfThreadInfo`/`qsThreadInfo`, `qThreadExtraInfo`, `qSymbol`, `qXfer:libraries:read` (§6.1), `qXfer:features:read:target.xml` (§6.1), `qSearch:memory` (§6.2), `qRcmd` (gdb's
  `monitor`: `title [NAME]`, `symbols [FILE]`, `launch [PATH]`), `H`g, `T`, `g`/`G` (16 registers, gdb's i386 order), `p`/`P` (also 16-40: st0-st7, the x87
  control registers, xmm0-xmm7, mxcsr; 41 orig_eax, -1), `m`/`M` (hex), `Z0`-`Z4`/`z0`-`z4`, `c`, `s`, `C`, `S`, `D`, `k`, `!` (extended-remote mode),
  `vRun` (§6.3), `vKill`. Anything else gets the empty reply. Binary `X` is not offered: the Modxo's stream carries its own marker bytes (0xFC, 0xFE,
  0xFD), so everything is plain ASCII. Acks stay on: a packet emptied by a boot (the SuperIO's set-up empties the Modxo's buffers) is sent again (eight
  tries, 330 ms apart).
- **Stops:** `T05` with `swbreak:`, `hwbreak:`, `watch:ADDR`, `rwatch:`, `awatch:`, `library:`, the thread, and esp, ebp, eip expedited; `T02` for a
  break-in; `T0b`, `T04`, `T07`, `T08` for the unhandled exceptions (by vector); `T06` after a bug check.
- **Watchpoints of any size:** a debug register watches an aligned 1, 2 or 4 bytes, so a watchpoint is split into the fewest such pieces (an unaligned
  4-byte one at A+1: 1 at A+1, 2 at A+2, 1 at A+4) and takes one register each; refused (`E28`) when the free registers are too few. The stop names the
  piece that triggered.
- **Title breakpoints** (below 0x80000000) go into memory only once `TitleLoaded` says the title gdb debugs is there: before, the low addresses map other
  pages (at a cold boot, the kernel's own), and an int3 there once hung a console's boot. A breakpoint on a page that is not present stays pending until a
  resume finds it present.
- **Writes** go through read-only pages too (CR0.WP off for the byte); every byte is copied with a recovery point, so a fault in the stub's own access
  answers an error instead of a crash.
- **gdb `call`** returns to a breakpoint gdb puts on the stack above the code segment's limit (0xBFFFFFFF): the function's `ret` raises #GP on a console. When
  the ret's target is an inserted breakpoint, the stub finishes the ret itself and stops there.
- **A resume that moved the stack pointer, or after writes into the frames below it,** goes out through the stub's own resume (an iret frame built at the
  new stack pointer), not back through the trap's frames.

### 6.1 The library list
`qXfer:libraries:read` answers gdb's library list (XML, `<library name="FILE"><segment address="0x..."/></library>`), built afresh each time gdb reads it
from its start. gdb finds each FILE in its `solib-search-path` and loads its symbols relocated to the address given: a PE file by its lowest section, an ELF
file by its first segment.

| Library | FILE | Address |
|---|---|---|
| the kernel | `oxoskrnl.elf`, next to `oxoskrnl.exe` in every build (release kernels included, §7.1); the release zip ships it in `symbols/` (`symbols/1.6/` for the 1.6 image) | 0x80010000, its image base: the ELF's first segment starts there |
| the kernel modules, while mapped (during a launch) | `kernel_modules.exe` | their image's lowest section (0xB0000000 and up) |
| each loaded extension | its `oxos_extensions.ini` section + `.xos` | its image's lowest section, wherever the loader put it this launch |
| the title, when `title_symbols` / `monitor symbols FILE` names its file | that FILE (e.g. `main.exe`) | the XBE's lowest section (main.exe's .text, 0x11000 for nxdk) |

Names keep letters, digits and `._-`; `<>&"$#}*` become `_`. The title is left out by default because gdb usually has it as its own program (`gdb
main.exe`, VS Code's `program`): listing it too would load its symbols twice.

gdb picks the library list only with an OS ABI that has no system libraries of its own: the stub answers `qXfer:features:read` for `target.xml` with an
i386 description whose OS ABI is `none` (the registers stay gdb's own i386 ones). A GNU/Linux OS ABI, gdb-multiarch's default on Linux PCs, would look for
libraries the Linux way and find none.

### 6.2 Following the console from one title into the next
When the title gdb debugs is loaded (`TitleLoaded`: the title filter matches, or there is none) while gdb is attached and waits for a stop, the stub stops
with `T05 library:` (an int3 in `TitleLoaded`). gdb reads the library list again (the new title's and extensions' symbols in, the old ones out), puts its
breakpoints in again, and resumes by itself (unless `set stop-on-solib-events 1`). The title's breakpoints went in already: they wait in the session until
their title is there (§6, title breakpoints). A `GDB=1` kernel keeps the session in persistent data across the quick reboot; `oxos_gdb.xos` in its state page, attaching
again in the next launch's `ExtensionStart`.

### 6.3 Starting titles from gdb
- **`vRun;PATH`** (gdb's `run` in `target extended-remote` mode; PATH is `set remote exec-file`, empty by default): PATH, or with none the title gdb debugs
  again (the path `TitleLoaded` gave). The reply is a stop where the target is now; the launch follows when gdb resumes it (gdb's `run` does at once), and
  §6.2's library stop comes with the new title.
- **`monitor launch [PATH]`**: the same, started when gdb continues.
- PATH is a `\Device\...` path, or a drive letter as the title loader's users write it (C: Partition2, D: the disc, E: Partition1, F: Partition6, G:
  Partition7, X:/Y:/Z: Partition3-5); `/` stands for `\`. A launch is started at passive level by `oxos_gdb.xos`'s run loop or a `GDB=1` kernel's launcher
  thread (§5), through the title launcher (`SwitchTitle`: the title's threads suspended, the launch data page, the quick reboot).
- From the request on, the old title's breakpoints are out of memory (pending): the old title runs on until the launch stops it and must not stop in them.

### 6.4 Using it
```
gdb-multiarch main.exe
(gdb) set sysroot
(gdb) set solib-search-path BUILD/kernel/release:BUILD/kernel/extensions/oxos_gdb
(gdb) target extended-remote localhost:2159          (the Modxo logger's bridge: tools/modxo/console.py log SERIAL --gdb 2159)
(gdb) break main
(gdb) run                                            (starts the title again; or continue)
```
`set sysroot` (empty) tells gdb to look for the libraries on the PC (the stub offers no file transfer). `tools/debug_console.py` and the VS Code
configurations in `tools/vscode/` do all of this (`tools/README.md`).

### 6.2 Searching memory on the console (2026-11-27)
`qSearch:memory:ADDRESS;LENGTH;PATTERN` (gdb's `find`; PATTERN binary, `}` escapes as in every gdb packet, up to 64 bytes) answers `1,ADDRESS` with the
first match or `0`. The stub searches with the target stopped: each 4 KB page's mapping is checked once (unmapped pages and I/O space are skipped whole), and
a mapped page is read directly, so only the answer crosses the serial line; the 64 MB of a title's memory (0x10000-0x4000000) took 0.6 s on the 1.6. A tool
that wants every match asks again from the match plus one. Titles also keep data in contiguous memory, which they see through the kernel's window at
0x80000000 and up (a game's weapon state was found there, not below 0x4000000): search both ranges.

## 7. Limits
- The kernel's steps before the first title are debugged only by a `GDB=1` kernel; `oxos_gdb.xos` loads with the first title.
- A hang with interrupts off cannot be broken into (no NMI on the Xbox).
- Between a title's end and the next title's extensions nothing is debugged; breakpoints set meanwhile wait in the session.
- The library list names the title only by one file (`title_symbols`): following into another XBE with other symbols needs `monitor symbols FILE` first.
- A release kernel's `oxoskrnl.elf` has names only (functions and variables), no source lines or types: it is built without debug info (§7.1).
- Titles that drive the network card themselves cannot share it with a network transport (spec 41 §3).


### 7.1 The kernel's symbols (2026-11-27)
User, 2026-11-27: ship symbols for every kernel, so the kernel can be debugged through `oxos_gdb.xos` without an SDK. Every build writes `oxoskrnl.elf` next
to `oxoskrnl.exe`:
- `DEBUG=1`: the link with DWARF debug info (source lines, types), as before.
- Every other build (release included): `tools/make_kernel_symbols.py` converts the kernel file itself to ELF and adds a symbol for each name in the linker's
  map (`oxoskrnl.map`) at its address; C names also in their plain form (`_NtCreateFile@36` as `NtCreateFile`). The kernel is built and linked exactly as
  without it: compiling with `-g` or linking with `/DEBUG:SYMTAB` both changed the release kernel's code (layout, identical-code folding), so neither is used.
- `make release VERSION=X.Y.Z` puts both release kernels' files in the zip's `symbols/` folder (beside `OXOS/`, not copied to the console).
- `tools/debug_console.py` points gdb at the served kernel's `oxoskrnl.elf` (`.oxos/console.gdb`: `set solib-search-path`, `sharedlibrary`).
## 8. Tests
- `make -C kernel test_gdb` (`tests/gdb_test.py`, the gdb test title `tests/gdb_test_title`): a `GDB_WAIT=1` kernel in xemu, its serial port a TCP socket,
  driven as gdb does: the boot stop, a title breakpoint set before the title (the kernel page under its address left alone), Ctrl-C, the breakpoint hit,
  float registers from the save area and the processor, the thread list, a `call`, a step, the library list (with `monitor symbols`), watchpoints in
  pieces, `vRun` starting the title again with its library stop and the breakpoint hit in the new launch, a write watchpoint, the title's access
  violation, its DbgPrint as O packets, attaching after the bug check, the kernel's code unchanged.
- `make -C kernel test_gdb_extension` (`gdb_test.py --extension --bridge`): the release kernel with `oxos_gdb.xos` enabled on the disk (`wait_for_gdb = yes`,
  `title = CdRom0`): the same session from the title's start, through `tools/gdb_bridge.py` as on the Modxo rig (the bridge waits for `gdb: stub ready`).
- gdb-multiarch itself (2026-11-27, xemu, by hand): with `target extended-remote` it loaded `oxoskrnl.elf` and `oxos_gdb.xos` from the library list at their
  addresses, hit a title breakpoint, `run` started the title again and stopped in it, `monitor launch` likewise.
- Both are phases of `make regression`; `test_memory` checks the port's cost (§3.4). First run 2026-11-27: both PASS.
