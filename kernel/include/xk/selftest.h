// Boot self-tests (kernel/selftest/, built only with SELFTEST=1). Each subsystem's tests start when the ones they
// depend on are done; tests/boot_test.py reads the results over the emulator monitor.
#pragma once
#include <xk/types.h>

// Start-up runs the self-tests and, once it has become the idle loop, releases the threads waiting on it
// (selftest.cpp).
void KiRunBootSelfTests();
void KiReleaseBootSpinners();

// Each subsystem's tests (selftest/<subsystem>.cpp).
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
