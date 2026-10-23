# 11. Host unit tests of the core runtime

OXOS addition, a development aid. The kernel's core sources run as a 32-bit Linux program with GoogleTest, so the
memory manager, dispatcher, object manager and run-time library are tested directly on a PC, in seconds, with
coverage. Implemented 2026-10-23. User guide: `tests/unit/README.md`.

## 1. What is built
- The kernel's own sources, unchanged apart from `XK_HOST_TEST` hooks (§4): `ke/` (APCs, bug checks, clock, DPCs,
  events, IRQL and interrupts, FPU state, queues, scheduler, threads, timers, waits, processor tables), `mm/` (all),
  `ob/object.cpp`, `ex/objects.cpp`, `ps/thread.cpp`, `rtl/` (all), `hal/pic_timer.cpp`.
- 32-bit (`-m32`): the kernel's types assume 32-bit `long` and pointers. Kernel sources keep `-ffreestanding`,
  `-fno-builtin`, no SSE/MMX, as the real build.
- Linked statically and not position independent: no shared libraries or binary in the kernel's address windows.
- Each run boots the kernel once; the tests run in order on one kernel thread (§3) and leave the kernel as they found
  it (pages, pool, handles, queues).

## 2. The fake CPU (`tests/unit/host/hostcpu.cpp`)
- **Privileged instructions** fault in user mode (SIGSEGV). The handler decodes and emulates: `cli`, `sti` (with
  its one-instruction shadow before `hlt`), `hlt`, `in`/`out` (8/16/32 bits, immediate or DX port), `invlpg`,
  `wbinvd`/`invd`, `mov` to/from CR0/CR2/CR3/CR4 and DR0-DR7, `rdmsr`/`wrmsr`, `lgdt`/`lidt`/`ltr`.
- **The interrupt flag** is the harness's: user mode cannot change EFLAGS.IF, and `pushfl`/`popfl` do not fault, so
  the kernel saves and restores it only through `arch.h`'s `save_flags_cli`/`restore_flags` (host versions call the
  harness); the host context switch (`host_asm.S`) saves it where the kernel saves EFLAGS.
- **Interrupts** are delivered synchronously: when the flag turns on with an unmasked IRQ pending (`sti`,
  `restore_flags`), at `hlt`, and when a test advances the fake time. The handler pushes the interrupted EIP on the
  interrupted stack and continues in a trampoline that calls `KiDispatchHardwareInterrupt` there, outside the signal
  handler. Masking follows the fake 8259's IMR, as programmed by the kernel.
- **Time**: the PIT interrupt (IRQ 0) once per fake millisecond; `hlt` with nothing pending advances one tick; the
  ACPI PM timer (port 0x8008) follows the fake time. `SkipTime` moves the PM timer without clock interrupts.
- **Devices**: master/slave 8259 (ICW sequence, IMR, ISR reads), ELCR, CMOS/RTC (index 0x70/0x72, data 0x71),
  PCI configuration (host bridge register 0x84), MSRs (a table), everything else reads as all ones.
- **Bug checks**: `KeBugCheckEx` calls the harness first (host builds); a test may catch it (`EXPECT_BUGCHECK`),
  otherwise the run ends with the code and the kernel log.
- **Deadlocks**: 600 s of fake idle time within one test end the run with the test's name.

## 3. The fake MMU
- Physical RAM is a 64 MB `memfd`; addresses from 64 to 128 MB wrap to it (a 64 MB console); the GPU window
  0xFD000000-0xFDFFFFFF is a second, zeroed `memfd`; anything else reads as zeros.
- The kernel's windows are reserved `PROT_NONE` at start-up: 0x00010000-0x07FFFFFF and 0x70000000-0x7FFFFFFF (title
  space), 0x80000000-0x8FFFFFFF (physical map), 0xC0000000-0xC03FFFFF (page-table self-map), 0xD0000000-0xD7FFFFFF
  (system PTEs, committed from the bottom), and the GPU window.
- A fault in a window walks the kernel's page tables from the fake CR3 (4 KB and 4 MB pages) and maps the page with
  the PTE's write permission: a TLB fill. A not-present or write-protected translation is a kernel page fault: the run
  ends with the address and stack.
- `invlpg` unmaps the page, a CR0/CR3/CR4 write unmaps all: TLB flushes. A missing invalidation therefore behaves as
  on hardware (the stale translation stays).
- Boot (`host/boot.cpp`) recreates what the 2BL leaves (spec/stock/01, `kernel/boot/bl.cpp`): its page directory at
  0xF000, a PE header at 0x10000 for the image extent, CR3 = 0xF000; then the kernel's own start-up:
  `KiInitializeProcessor`, PIC and PIT, `MmInitSystem`, DPCs, timers, scheduler, `ObInitSystem`, the clock IRQ.
  GoogleTest runs on a kernel thread (priority 8, 1 MB stack); the boot thread becomes the idle thread.

## 4. Kernel changes for it (behaviour unchanged on the console)
- `types.h`: GNU calling-convention attributes and 8-byte-aligned 64-bit types under `XK_HOST_TEST` (the Windows
  layout the structure asserts expect; i386 Linux aligns them to 4).
- `arch.h`: host `save_flags_cli`/`restore_flags`; every flag save/restore in the kernel goes through them.
- `rtl/memory.cpp`: the kernel's `memset`/`memcpy`/`memmove` are `Xk_*` in host builds (the C library has its own).
- `rtl.h`: the C library's `size_t` in host builds.
- `mm/init.cpp`: the RAM probe uses the 0x80000000 alias (the same physical addresses as the 2BL's identity map; page
  0 cannot be mapped in a Linux process).
- `ke/cpu.cpp`: no segment reload in host builds.
- `ke/bugcheck.cpp`: the harness hook (host builds only).

## 5. Not covered
- Code that needs devices or the boot path: HAL (other than the PIC/RTC), IDE, file systems, the XBE loader, video,
  start-up (`startup.cpp`), the double-fault and NMI tasks (hardware task switches).
- Asynchronous interrupts at arbitrary instructions (delivery is synchronous, which keeps the tests deterministic).
