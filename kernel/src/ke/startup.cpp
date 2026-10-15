// Kernel start-up after the 2BL hand-off (spec/01-boot.md, spec/02-executive-init.md).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/rtl.h>

void KiInitializeDpcs();

// Second boot key handed over by the 2BL (likely the certificate key; spec 01 §1).
UCHAR KiCertKey[16];

// Boot progress, readable from the emulator monitor (tests/boot_test.py).
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

    KiInitializeDpcs();
    KiInitializeTimers();
    HalEnableSystemInterrupt(0, Latched); // PIT clock

#if XK_BOOT_SELFTEST
    KiRunBootSelfTests();
#endif

    KfLowerIrql(DISPATCH_LEVEL);
    KiSetStage(post_code::Idle);
    KiIdleLoop();
}

// Runs at DISPATCH_LEVEL. Like the reference KiIdleLoop, it drains DPCs itself because lowering
// IRQL never happens here (spec/01 §3).
void KiIdleLoop() {
    for (;;) {
        cli();
        if (!IsListEmpty(&KiPcr.PrcbData.DpcListHead)) {
            HalClearSoftwareInterrupt(DISPATCH_LEVEL);
            sti();
            KiRetireDpcList();
            continue;
        }
        // sti's one-instruction shadow makes "sti; hlt" race-free against the wake-up interrupt.
        __asm__ volatile("sti\n\thlt" ::: "memory");
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
