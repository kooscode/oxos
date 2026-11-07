// Shared helpers for the core runtime tests.
#pragma once
#include <gtest/gtest.h>

#include "host.h"
#include <functional>
#include <xk/arch.h>
#include <xk/hardware.h>
#include <xk/core.h>
#include <xk/memory.h>
#include <xk/objects.h>

// The page database (PTE array of the 0x80000000 map) as physical memory, independent of the kernel's mappings.
inline uint32_t PfnEntry(uint32_t pfn) {
    return ((uint32_t *)host::Phys(0x3FF0000))[pfn];
}

// Walk every colour list from the physical page database: no loops, entries free and of their colour, back links
// consistent, and the count matching MmAvailablePages. Returns a description of the first problem, or "".
std::string CheckPfnDatabase();

// Expect `statement` to bug-check with `code`; IRQL and the interrupt flag are put back afterwards.
#define EXPECT_BUGCHECK(code, statement)                                                                              \
    do {                                                                                                              \
        jmp_buf catcher_;                                                                                             \
        KIRQL irql_ = KeGetCurrentIrql();                                                                             \
        if (!setjmp(catcher_)) {                                                                                      \
            host::BugCheckCatcher = &catcher_;                                                                        \
            statement;                                                                                                \
            host::BugCheckCatcher = nullptr;                                                                          \
            ADD_FAILURE() << "no bug check from: " #statement;                                                       \
        } else {                                                                                                      \
            EXPECT_EQ(host::LastBugCheck[0], (uint32_t)(code)) << #statement;                                        \
            KiPcr.Irql = irql_;                                                                                       \
            HostRestoreFlags(0x200);                                                                                  \
        }                                                                                                             \
    } while (0)

// A kernel thread running `fn` (any priority 0-31); Join waits for it to terminate and drops the reference.
PKTHREAD Spawn(std::function<void()> fn, LONG priority = 8);
void Join(PKTHREAD thread);
// Wait helpers.
inline LARGE_INTEGER RelativeMs(LONGLONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    return t;
}

inline void SleepMs(LONGLONG ms) {
    LARGE_INTEGER t = RelativeMs(ms);
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}
