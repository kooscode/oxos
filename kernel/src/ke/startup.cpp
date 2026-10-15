// Kernel start-up after the 2BL hand-off (spec/01-boot.md, spec/02-executive-init.md).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/rtl.h>

// Second boot key handed over by the 2BL (likely the certificate key; spec 01 §1).
UCHAR KiCertKey[16];

// Boot progress, readable from the emulator monitor (tests/m1-boot.py).
volatile ULONG KiBootStage;

static void KiSetStage(UCHAR code) {
    KiBootStage = code;
    post(code);
}

extern "C" void KiSystemStartup(const UCHAR *bootKeys) {
    KiSetStage(post_code::Entry);

    // Take the keys, then erase the 2BL image (0x6000 bytes at 0x80400000), as the stock kernel does.
    RtlCopyMemory(XboxEEPROMKey, bootKeys, 16);
    RtlCopyMemory(KiCertKey, bootKeys + 16, 16);
    RtlZeroMemory((PVOID)0x80400000, 0x6000);

    KiInitializeProcessor();

    KiSetStage(post_code::HalPhase0);
    HalInitSystemPhase0(/*coldBoot=*/true);
    HalpDetectHardware();

    // Milestone 1 stops here: clock running, idle at DISPATCH_LEVEL.
    HalpUnmaskIrq(0);
    KfLowerIrql(DISPATCH_LEVEL);
    KiSetStage(post_code::Idle);
    KiIdleLoop();
}

void KiIdleLoop() {
    for (;;) {
        sti();
        halt();
    }
}

// --- Fatal paths

volatile ULONG KiLastExceptionVector = 0xFFFFFFFF;
volatile ULONG KiLastExceptionCode;
volatile ULONG KiLastExceptionEip;

extern "C" void KiUnexpectedException(ULONG vector, ULONG errorCode, ULONG eip) {
    KiLastExceptionVector = vector;
    KiLastExceptionCode = errorCode;
    KiLastExceptionEip = eip;
    post((UCHAR)(post_code::Exception | (vector & 0x1F)));
    for (;;) {
        cli();
        halt();
    }
}
