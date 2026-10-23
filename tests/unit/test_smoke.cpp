// The harness itself: the kernel booted, the test runs on a kernel thread at PASSIVE_LEVEL, time advances.
#include <gtest/gtest.h>

#include "host.h"
#include <xk/ke.h>
#include <xk/mm.h>

TEST(Harness, RunsOnAKernelThreadAtPassiveLevel) {
    EXPECT_EQ(KeGetCurrentIrql(), PASSIVE_LEVEL);
    EXPECT_NE(KeGetCurrentThread(), nullptr);
    EXPECT_TRUE(host::InterruptsEnabled());
}

TEST(Harness, ClockAdvances) {
    ULONG before = KeTickCount;
    host::AdvanceTime(10);
    EXPECT_EQ(KeTickCount - before, 10u);
}

TEST(Harness, DelayExecutionSleepsOnFakeTime) {
    uint64_t t0 = host::NowMs();
    LARGE_INTEGER d;
    d.QuadPart = -10000LL * 50; // 50 ms
    EXPECT_EQ(KeDelayExecutionThread(KernelMode, FALSE, &d), STATUS_SUCCESS);
    EXPECT_GE(host::NowMs() - t0, 50u);
}
