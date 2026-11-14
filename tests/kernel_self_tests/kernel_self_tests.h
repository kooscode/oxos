// The kernel's boot self-tests (tests/kernel_self_tests/), compiled into the kernel only with SELFTEST=1 (make, make
// test): they call kernel internals from inside the running kernel. Each subsystem's tests start when the ones they
// depend on are done; tests/boot_test.py reads the results over the emulator monitor. The kernel includes this
// header only in SELFTEST=1 builds, for the calls that start the tests (src/core/startup.cpp, src/titles/loader.cpp).
#pragma once
#include <xk/types.h>

// Start-up runs the self-tests and, once it has become the idle loop, releases the threads waiting on it
// (kernel_self_tests.cpp).
void KiRunBootSelfTests();
void KiReleaseBootSpinners();

// Each subsystem's tests (<subsystem>.cpp in this folder).
void KiStartIoSelfTests();
void KiStartIdexSelfTests();
void KiStartFATXSelfTests();
void KiStartDvdSelfTests();
void KiStartHalSelfTests();
void KiStartRtlSelfTests();
void KiStartSehSelfTests();
void KiStartXcSelfTests();
void KiStartMiscSelfTests();

// Set to 1 when a subsystem's tests have finished; the next subsystem's tests wait for them.
extern volatile ULONG KiTestObDone;
extern volatile ULONG KiTestMmDone;
extern volatile ULONG KiTestIoDone;
extern volatile ULONG KiTestXcDone;
extern volatile ULONG KiTestIdexDone;
extern volatile ULONG KiTestFATXDone;
extern volatile ULONG KiTestDvdDone;
extern volatile ULONG KiTestHalDone;
extern volatile ULONG KiTestRtlDone;
extern volatile ULONG KiTestSehDone;
