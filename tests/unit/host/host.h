// Host unit-test harness API (tests/unit/README.md): the fake CPU, MMU and devices the kernel's core sources run on.
#pragma once
#include <csetjmp>
#include <cstdint>

namespace host {

constexpr uint32_t PHYS_SIZE = 64u << 20; // "physical RAM"

// Physical memory as the harness sees it (not through the kernel's page tables).
uint8_t *Phys(uint32_t pa);

// Fake CPU state.
extern volatile uint32_t Cr0, Cr2, Cr3, Cr4;
bool InterruptsEnabled();

// Interrupts: raise an IRQ line (delivered when unmasked in the fake PIC and interrupts are on).
void RaiseIrq(unsigned irq);
// Fake time: advance by `ms` milliseconds, one clock interrupt (IRQ 0) per millisecond, delivered as they fall due
// (the caller must be able to take interrupts, i.e. run with them enabled).
void AdvanceTime(uint32_t ms);
uint64_t NowMs();
// Move the fake time on without clock interrupts (the ACPI PM timer jumps; KeTickCount does not).
void SkipTime(uint64_t ms);
// Asynchronous clock interrupts at any kernel instruction, every `periodUs` microseconds of real time (0: off), as the
// PIT does on hardware; statistics: taken at once, left pending (IF off), skipped (outside kernel code).
void SetAsyncInterrupts(uint32_t periodUs);
extern volatile uint64_t AsyncDelivered, AsyncPended, AsyncSkipped;
// Number of clock interrupts the kernel has taken.
extern volatile uint64_t ClockInterrupts;

// Idle clock ticks in the current test (reset by the test listener; the deadlock guard trips at 600 s).
extern volatile uint64_t IdleTicksThisTest;

// Fake MMU statistics and control.
extern volatile uint64_t PageFills, TlbFlushes;
// Drop every cached translation (as a CR3 reload does).
void FlushTlb();
// True if the harness's last fault was a genuine kernel page fault (an unmapped or write-protected access).
extern volatile uint32_t KernelFaultAddress;

// Bug checks: a test sets BugCheckCatcher to catch the next one (see EXPECT_BUGCHECK in kernel_test.h).
extern jmp_buf *BugCheckCatcher;
extern volatile uint32_t LastBugCheck[5];

// Kernel log lines (XK_SERIAL) captured since the last call; printed by failing tests.
const char *TakeLog();

// Boot the kernel core (MM, scheduler, object manager) and run `body` on a kernel thread at PASSIVE_LEVEL;
// returns its result. The main thread becomes the idle thread.
int BootAndRun(int (*body)(void *), void *context);

} // namespace host
