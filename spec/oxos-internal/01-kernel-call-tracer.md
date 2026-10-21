# Kernel call tracer (OXOS addition)

Not part of the reference: the retail kernel has no way to record what a title asks of it. This spec describes
OXOS's own opt-in tracer, which records a title's most recent kernel calls so a hang or crash can be traced back to
the call that started it. It is how Halo reached its main menu (milestone 10) and Halo 2 its title screen (11).
First implemented 2026-10-15 (milestone 10, "import tracer"); its current form, with the host tools, dates from
2026-10-17. Code: `kernel/src/xe/trace.cpp`, `kernel/src/arch/trace.S`, the hook in `kernel/src/xe/loader.cpp`;
host side `tools/trace.py`, `tools/run.py --trace-map`.

## 1. Builds
| `TRACE_IMPORTS` | Where the record lives | Use |
|---|---|---|
| 0 (default) | no tracer: `trace.cpp` and `trace.S` are not even compiled | release and self-test builds |
| 1 | ring from contiguous memory, stubs from pool: inside the console's 64 MB (about 2 MB less for the title) | `make run-trace-full TRACE=1` (`build/trace-full-64mb`) |
| 2 | ring and stubs mapped at physical 0x4000000, **above** the 64 MB the kernel manages; needs xemu with 128 MB | `make run-trace-full` (default, `build/trace-full`): the title sees exactly a release build's memory |

Trace builds also turn on the debug serial port (03-debug-serial.md); the host tools use its crash lines.

## 2. Hooking the title's imports
- When the XBE loader resolves the title's kernel thunks (`XepResolveKernelThunks`), each resolved address goes
  through `XkTraceWrap(ordinal, target)`.
- Only **code** exports are wrapped: a target outside the kernel image's first (code) section is a data export
  (KeTickCount, LaunchDataPage, ...) and is returned unchanged.
- A code export gets a 10-byte stub, `mov eax, index; jmp XkTraceEntry`, and the thunk points at the stub. At most
  512 imports are wrapped; later ones are left alone. `XkTraceTargets[index]` / `XkTraceOrdinals[index]` remember
  the real export and its ordinal.
- Calls the kernel makes internally are not traced: only the title's thunks are redirected.

## 3. Recording a call
`XkTraceEntry` (assembly) saves the flags and all registers, calls `XkTraceRecord(index, frame)` with `frame`
pointing at the caller's return address, restores everything and jumps to the real export. The export runs with
the caller's registers and stack unchanged.

`XkTraceRecord`, with interrupts disabled (a title's interrupt routine may be recording too):
- **Skipped ordinals**, too frequent to be useful: 103, 104, 119, 125, 126, 128, 129, 160, 161, 221, 234, 238, 277,
  278, 294, 295 (IRQL changes, spin locks, critical sections, NtYieldExecution; Halo 2 spins on 238 and takes a
  mutant, 221 / 234, around every step).
- **Ring entry** (15 dwords; the ring has 32768 entries, indexed by `XkTraceCount` modulo the size):

| Word | Content |
|---|---|
| 0 | ordinal |
| 1 | caller's return address |
| 2 | current thread |
| 3–5 | first three stack arguments |
| 6 | return value, `0xDEADDEAD` until the call returns |
| 7–14 | per-call extra (below), else word 7 = 0 |

- **Extras:**
  - NtCreateFile (190), NtOpenFile (202): the last 23 characters of the object name, then share access and
    create / open options.
  - NtQueryFullAttributesFile (210), NtDeleteFile (195): the last 31 characters of the name.
  - IoCreateSymbolicLink (67), IoDeleteSymbolicLink (69): the last 31 characters of the link name.
  - NtReadFile (219), NtWriteFile (236): tag `0x10000000 | ordinal`, APC context, I/O status block, buffer, length,
    the 64-bit byte offset (all ones when none), and the buffer's first dword.
  - NtSetInformationFile (226): tag, information class, length, the first four dwords of the information.
- `XkTraceCount` is in the persisted section, so with TRACE_IMPORTS=2 the numbering, and the ring above 64 MB,
  continue across a quick reboot (title to title). With TRACE_IMPORTS=1 the ring is newly allocated and cleared
  when the first title of each boot is loaded, so entries from before the reboot read as empty.

## 4. Catching the return value
- Per thread, a slot (128 slots) keeps up to 8 calls in flight: titles' APCs can nest kernel calls.
- On entry the caller's return address is saved in the slot with the ring index, and replaced on the stack by
  `XkTraceReturn`.
- `XkTraceReturn` (assembly) pushes a placeholder, saves flags and registers and calls `XkTraceLeave(eax)`. That
  stores eax as word 6 of the call's entry and returns the original return address, which goes into the
  placeholder; registers are restored and `ret` continues in the title.
- A call whose thread blocks forever (titles keep dozens of threads waiting) holds its slot until the thread ends.
  Thread termination (`ke/thread.cpp`) calls `XkTraceThreadExit`, which frees the thread's slot: it may have ended
  inside a traced call that never returns.
- More than 8 nested calls, or no free slot: the call is recorded but its return value stays `0xDEADDEAD`.

## 5. Reading the record (host)
- `tools/trace.py` finds `XkTraceCount` and `XkTraceRing` in the build's linker map, stops xemu over QMP, copies the
  ring with the monitor's `memsave`, and resumes it (a fraction of a second).
- Each line: sequence number, function name (from `kernel/exports.tsv`), return value or "(no return)", caller,
  thread, the three arguments and the extras decoded.
- Filters: `--last N`, `--only NAMES`, `--hide NAMES`, `--thread ID`. By default the tool also hides a list of very
  common calls (waits, events, timers, pool); `--all` shows them. `--save` / `--load` keep a raw copy.
- `tools/run.py --trace-map MAP` (used by `make run-trace-full`, `run-trace-xbe`):
  - dumps automatically when the kernel reports a crash on the serial port (a line starting with `***`, at most one
    dump per 2 s);
  - copies the record every second without pausing, and writes the last copy as the exit dump when xemu closes;
  - shows the last 30 calls in the terminal, writes all of them to `DIR/calls-<time>-<crash|exit>.txt` and the
    raw record next to it (`.bin`).
- `make print-trace-dump [LAST=N] [ONLY=...] [ALL=1]` prints the record from another terminal at any time; pass the
  same `TRACE` as the run.

## 6. Limits
- Data exports are not traced (reads of KeTickCount and the like are plain memory reads).
- Only the first three stack arguments are kept, except for the calls with extras.
- The ring keeps the last 32768 recorded calls; skipped ordinals never appear.
- With TRACE_IMPORTS=1 the title has about 2 MB less memory than on a release build, which can change its
  behaviour (Halo 2 needs nearly all 64 MB): use the default TRACE_IMPORTS=2 for memory-sensitive titles.
