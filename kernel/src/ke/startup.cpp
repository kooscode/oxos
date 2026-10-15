// Kernel start-up after the 2BL hand-off (spec/01-boot.md, spec/02-executive-init.md).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/idex.h>
#include <xk/rtl.h>
void HalpSmcInitColdBoot();
void HalpSmcStartEvents();

void KiInitializeDpcs();
extern "C" UCHAR KiBootStack[];
constexpr ULONG BOOT_STACK_SIZE = 0x3000;

// Second boot key handed over by the 2BL (likely the certificate key; spec 01 §1).
XK_PERSIST UCHAR KiCertKey[16];

// Quick reboot (spec/02 §1): flag, count, and the boot keys kept for the restart.
extern "C" XK_PERSIST volatile UCHAR KiQuickRebootFlag;
extern "C" XK_PERSIST volatile ULONG KiQuickRebootCount;
XK_PERSIST volatile UCHAR KiQuickRebootFlag;
XK_PERSIST volatile ULONG KiQuickRebootCount;
extern "C" XK_PERSIST UCHAR KiPersistedBootKeys[32];
XK_PERSIST UCHAR KiPersistedBootKeys[32];

// Boot progress, readable from the emulator monitor (tests/boot_test.py).
volatile ULONG KiBootStage;

static void KiSetStage(UCHAR code) {
    KiBootStage = code;
    post(code);
}

void XeStartTitleLauncher(); // spec/05: load and enter the title
void ExInitEeprom();          // spec/12: EEPROM shadow, HD key, game region
void AvInit();                // spec/12: AV pack, video encoder
void KiInitializeSystemTime(); // system time from the RTC

extern "C" void KiSystemStartup(const UCHAR *bootKeys) {
    KiSetStage(post_code::Entry);

    // Take the keys, then erase the 2BL image (0x6000 bytes at 0x80400000), as the stock kernel does. A quick
    // reboot keeps memory: the keys come from the persisted copy and nothing is erased.
    bool quick = KiQuickRebootFlag;
    RtlCopyMemory(XboxEEPROMKey, bootKeys, 16);
    RtlCopyMemory(KiCertKey, bootKeys + 16, 16);
    if (!quick) {
        RtlCopyMemory(KiPersistedBootKeys, bootKeys, 32);
        RtlZeroMemory((PVOID)0x80400000, 0x6000);
    }

    KiInitializeProcessor();

    KiSetStage(post_code::HalPhase0);
    HalInitSystemPhase0(/*coldBoot=*/!quick);
    if (!quick)
        HalpDetectHardware();

    MmInitSystem(quick);
    KiInitializeDpcs();
    KiInitializeTimers();
    KiInitializeScheduler(KiBootStack + BOOT_STACK_SIZE, KiBootStack);
    ObInitSystem();
    IoInitSystem();
    FsInitSystem();
    KiInitializeSystemTime();
    ExInitEeprom();
    AvInit();
    if (!quick)
        HalpSmcInitColdBoot();
    IdexChannelInit();
    HalEnableSystemInterrupt(0, Latched); // PIT clock
    HalpSmcStartEvents();

#if XK_BOOT_SELFTEST
    if (!quick)
        KiRunBootSelfTests();
#endif
    XeStartTitleLauncher();

    KfLowerIrql(DISPATCH_LEVEL);
    KiSetStage(post_code::Idle);
    KiIdleLoop();
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
