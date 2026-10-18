// Kernel start-up after the 2BL hand-off (spec/stock/01-boot.md, spec/stock/02-executive-init.md).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/idex.h>
#include <xk/rtl.h>
#include <xk/serial.h>
void HalpSmcInitColdBoot();
void HalpSmcStartEvents();
void HalpStartSplash();

void KiInitializeDpcs();
extern "C" UCHAR KiBootStack[];
constexpr ULONG BOOT_STACK_SIZE = 0x3000;

// Second boot key handed over by the 2BL (likely the certificate key; spec 01 §1).
XK_PERSIST UCHAR KiCertKey[16];

// Quick reboot (spec/stock/02 §1): flag, count, and the boot keys kept for the restart.
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

void XeStartTitleLauncher(); // spec/stock/05: load and enter the title
void ExInitEeprom();          // spec/stock/12: EEPROM shadow, HD key, game region
void AvInit();                // spec/stock/12: AV pack, video encoder
void KiInitializeSystemTime(); // system time from the RTC

// MTRRs as the boot loader left them (we do not program our own): default type, then 8 base/mask pairs, each MSR
// as {low, high}.
volatile ULONG KiBootMtrr[2 + 32];

// Open BIOS image (spec/stock/01 §1, §5): no EEPROM key, but HMAC states of the known EEPROM keys (from XboxDev/cromwell)
// that ExInitEeprom tries in turn. Kept across quick reboots, which re-enter with arg0 = 0.
constexpr ULONG XK_OPEN_BOOT_MAGIC = 0x534F584F, XK_KEY_STATES_MAGIC = 0x4B42584F; // 'OXOS', 'OXBK'
XK_PERSIST ULONG KiEepromKeyStates[8][10];
XK_PERSIST ULONG KiEepromKeyStateCount;

// Serial log of the start-up steps (SERIAL=1 builds, spec/stock/18).
#define KiBootStep(name) XK_SERIAL("boot: %s\n", name)

extern "C" void KiSystemStartup(ULONG arg0, const UCHAR *bootKeys) {
    KiSetStage(post_code::Entry);
#if XK_DEBUG_SERIAL
    HalpSerialInit();
    XK_SERIAL("\nOXOS kernel (" __DATE__ " " __TIME__ "): %s boot\n", KiQuickRebootFlag ? "quick" : "cold");
#endif
    for (ULONG i = 0; i < 17; i++) {
        ULONGLONG v = rdmsr(i ? 0x200 + i - 1 : 0x2FF);
        KiBootMtrr[2 * i] = (ULONG)v;
        KiBootMtrr[2 * i + 1] = (ULONG)(v >> 32);
    }

    // Take the keys, then erase the 2BL image (0x6000 bytes at 0x80400000), as the stock kernel does. A quick
    // reboot keeps memory: the keys come from the persisted copy and nothing is erased.
    bool quick = KiQuickRebootFlag;
    RtlCopyMemory(XboxEEPROMKey, bootKeys, 16);
    RtlCopyMemory(KiCertKey, bootKeys + 16, 16);
    if (!quick) {
        RtlCopyMemory(KiPersistedBootKeys, bootKeys, 32);
        KiEepromKeyStateCount = 0;
        auto ext = (const ULONG *)(bootKeys + 32);
        if (arg0 == XK_OPEN_BOOT_MAGIC && ext[0] == XK_KEY_STATES_MAGIC) {
            ULONG n = ext[1] < 8 ? ext[1] : 8;
            RtlCopyMemory(KiEepromKeyStates, ext + 2, n * sizeof(KiEepromKeyStates[0]));
            KiEepromKeyStateCount = n;
        }
        RtlZeroMemory((PVOID)0x80400000, 0x6000);
    }

    KiBootStep("processor");
    KiInitializeProcessor();

    KiSetStage(post_code::HalPhase0);
    KiBootStep("HAL phase 0");
    HalInitSystemPhase0(/*coldBoot=*/!quick);
    KiBootStep("memory manager");
    MmInitSystem(quick);
    if (!quick)
        HalpDetectHardware();
    KiInitializeDpcs();
    KiInitializeTimers();
    KiBootStep("scheduler");
    KiInitializeScheduler(KiBootStack + BOOT_STACK_SIZE, KiBootStack);
    KiBootStep("object manager");
    ObInitSystem();
    KiBootStep("I/O manager");
    IoInitSystem();
    FsInitSystem();
    KiInitializeSystemTime();
    KiBootStep("EEPROM");
    ExInitEeprom();
    KiBootStep("AV");
    AvInit();
    if (!quick)
        HalpSmcInitColdBoot();
    KiBootStep("IDE");
    IdexChannelInit();
    HalEnableSystemInterrupt(0, Latched); // PIT clock
    HalpSmcStartEvents();
    if (!quick)
        HalpStartSplash();

#if XK_BOOT_SELFTEST
    if (!quick)
        KiRunBootSelfTests();
#endif
    KiBootStep("title launcher");
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
    XK_SERIAL("*** unexpected exception %lu, error code %#lx, eip %#lx\n", vector, errorCode, eip);
    KiLastExceptionVector = vector;
    KiLastExceptionCode = errorCode;
    KiLastExceptionEip = eip;
    post((UCHAR)(post_code::Exception | (vector & 0x1F)));
    for (;;) {
        cli();
        halt();
    }
}
