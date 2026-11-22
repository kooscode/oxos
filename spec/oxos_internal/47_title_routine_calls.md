# Spec 47 — Routines a title supplies, and contiguous memory freed through the GPU window

OXOS's own design, from two homebrew titles that stopped the kernel at their start (issue #9). **Status: implemented** (2026-11-22). What the stock kernel
does: spec/stock/27.

## 1. The failure
Two titles built with GCC against an XDK-style library set (their XBEs hold `.eh_frame` and `.gcc_except_table` sections and the libraries' usual sections:
`XPP`, `D3D`, `DSOUND`) stopped with bug check 0x1E right after their start-up called `main`. Observed in xemu with a serial build:
| What | Observed |
|---|---|
| The title's start-up | `KeInitializeDpc` and `KeInitializeTimerEx` for its USB host controller's DPC and timer (the routine in its `XPP` section), `KeConnectInterrupt` for IRQ 1 |
| The first timer expiry | the kernel queues the DPC with the system time as its two system arguments (spec/stock/06 §5) and the DPC loop calls the routine |
| The routine's return | a plain `ret`: it leaves its four stdcall arguments on the stack (the routine was built cdecl) |
| OXOS's DPC loop (clang `-Oz`) | its epilogue pops its saved registers and its return address relative to the stack pointer: from the four arguments. `esi` = the DPC, `edi` = the context, `ebp` = SystemArgument1, and the return goes to SystemArgument2, the high half of the system time (0x01DD7AB3 in xemu; 0x01DD7A73 on the reporter's console) |
| The result | an instruction fetch at an address with nothing mapped (0xC0000005), bug check 0x1E |
| A diff of the kernel image before the title's start and at the bug check | no byte of kernel code changed: the start-up's "kernel patches" step changed nothing in OXOS |

With the DPC fixed, the start-up went on to call `MmFreeContiguousMemory(0xF3FAE000)`: a contiguous allocation freed through its alias in the
0xF0000000–0xF7FFFFFF window (the GPU's write-combined view of RAM, spec/stock/23 §5), not through the 0x80000000 address it was allocated at. OXOS took the
page index as the address's low 31 bits, 0x73FAE, far past the page table, and faulted.

## 2. Routines a title supplies: called with the stack pointer restored
Every call the kernel makes to a routine a title may have supplied goes through one assembly helper, `KiCallTitleRoutine(Routine, Count, Arguments)`
(`kernel/src/arch/context_switch.S`; C++: `CallTitleRoutine(Routine, arguments...)` in `kernel/src/arch/arch.h`). It pushes the arguments, calls the routine,
and then restores the stack pointer from its own frame, so a routine that returns without removing its arguments leaves the caller's stack intact. The value in
`eax` is the result.
| Call | Where |
|---|---|
| a DPC's DeferredRoutine (Dpc, Context, SystemArgument1, SystemArgument2), timer DPCs included | `core/deferred_calls.cpp` (`RetireDpcList`) |
| an interrupt object's ServiceRoutine (Interrupt, ServiceContext) | `core/interrupts.cpp` |
| an APC's KernelRoutine (5 arguments) and NormalRoutine (3), kernel and user APCs | `core/async_calls.cpp` |
| `KeSynchronizeExecution`'s SynchronizeRoutine (Context) | `core/queue.cpp` |
| `PsSetCreateThreadNotifyRoutine` routines (Thread, ThreadId, Create) | `core/system_threads.cpp` |
| a driver's dispatch routine (DeviceObject, Irp) from `IofCallDriver` | `io/io_requests.cpp` |
| an I/O completion routine (DeviceObject, Irp, Context) from `IofCompleteRequest` | `io/io_requests.cpp` |
| `HalRegisterShutdownNotification` routines (Registration) | `hardware/chipset/smbus.cpp` |
A thread's system routine needs no helper: it is entered from the thread start-up code and never returns to it.

**The stock kernel** survives a cdecl return only at some callers (spec/stock/27 §5): its timer expiration calls a timer's DPC directly, from code that resets
its stack pointer from its frame pointer, so the titles' timer DPC runs there; its DPC queue, its interrupt dispatch, `IofCallDriver` and
`IofCompleteRequest` do not survive one. OXOS queues timer DPCs through its DPC list (spec/stock/06 §5), so it needs the same tolerance there, and it gives
it to every caller: a routine with the wrong calling convention cannot stop the kernel.

## 3. Contiguous memory freed through the GPU window
The stock `MmFreeContiguousMemory` takes the page as bits 12–27 of the address it is given, with no test of the address (spec/stock/27 §2, §3). The
allocation's kernel address (0x80000000 + physical) and its alias in the GPU's write-combined window (0xF0000000 + physical) therefore free the same pages.
OXOS takes the page the same way (`kernel/src/memory/memory.cpp`). Unlike the reference, it stops at a page past the last physical page (the reference
faults reading its page table there) and at a page that is not in use (the reference frees whatever page the address names). The other `Mm*` exports are
unchanged: the titles use none of them with a window address, and the reference handles most of them wrongly for one (spec/stock/27 §4).

## 4. Tests
- Host unit tests: every caller above runs through `KiCallTitleRoutine`'s host version (`tests/unit/host/hostcpu.cpp`), 229 tests.
- xemu, a serial build, both titles from a disc image: before, bug check 0x1E right after `main`; after, both reach their intro screens.
- Full regression (`make -C kernel regression`): see the commit.
