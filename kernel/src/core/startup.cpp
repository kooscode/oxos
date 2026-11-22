// Kernel start-up after the 2BL hand-off (spec/stock/01_boot.md, spec/stock/02_executive_init.md).
#include <core/core.h>
#include <core/scheduler.h> // StartHangDumps (serial builds)
#include <arch/arch.h>
#include <hardware/chipset/chipset.h>
#include <hardware/video/video.h>
#include <diagnostics/diagnostics.h>
#include <in_game_reset/in_game_reset.h>
#include <memory/memory.h>
#include <storage/storage.h>
#include <runtime/runtime.h>
#include <hardware/chipset/serial.h>
#include <titles/titles.h>
#include <boot_splash/boot_splash.h>
#include <filesystems/filesystems.h>
#if XK_BOOT_SELFTEST
#include <kernel_self_tests.h> // tests/kernel_self_tests (SELFTEST=1 builds only)
#endif
#if XK_STATEDUMP
#include "../../../tools/statedump/statedump.h"
#endif

// The quick reboot's state (spec/stock/02 §1: flag, count, and the boot keys kept for the restart), which the assembly in arch/ reads: outside the namespace, C
// names.
XK_PERSIST volatile UCHAR KiQuickRebootFlag;
XK_PERSIST volatile ULONG KiQuickRebootCount;
XK_PERSIST UCHAR KiPersistedBootKeys[32];

namespace xk::core {

// Open BIOS image (spec/stock/01 §1, §5): no EEPROM key, but HMAC states of the known EEPROM keys (from XboxDev/cromwell) that hardware::chipset::InitEeprom
// tries in turn. Kept across quick reboots, which re-enter with arg0 = 0.
constexpr ULONG XK_OPEN_BOOT_MAGIC = 0x534F584F, XK_KEY_STATES_MAGIC = 0x4B42584F; // 'OXOS', 'OXBK'
XK_PERSIST ULONG EepromKeyStates[8][10];
XK_PERSIST ULONG EepromKeyStateCount;

// The certificate key handed over by the 2BL (spec 01 §1): the per-title keys are derived from it (titles/title_loader.cpp).
XK_PERSIST UCHAR CertKey[16];

// Boot progress, readable from the emulator monitor (tests/boot_test.py).
volatile ULONG BootStage;

// Same version as the reference kernel so titles that test it behave identically.
XBAPI XBOX_KRNL_VERSION XboxKrnlVersion = {1, 0, 4627, 1};

// --- Start-up (spec/stock/01 §2, spec/stock/02)

// Serial log of the start-up steps (SERIAL=1 builds, spec/oxos_internal/03_debug_serial.md).
#define KiBootStep(name) XK_SERIAL("boot: %s\n", name)

static void SetStage(UCHAR code) {
    BootStage = code;
    post(code);
}

extern "C" void KiSystemStartup(ULONG arg0, const UCHAR *bootKeys) {
    diagnostics::CheckDramRefresh();
    diagnostics::TakePreviousBootMarks();
    if (!KiQuickRebootFlag)
        // A title delivered by netboot (titles/title_netboot.cpp): before the bank's pages are reused.
        titles::ReadNetbootTitle();
    SetStage(post_code::Entry);
#if XK_DEBUG_SERIAL
    hardware::chipset::InitSerial();
    XK_SERIAL("\nOXOS kernel (" __DATE__ " " __TIME__ "): %s boot\n", KiQuickRebootFlag ? "quick" : "cold");
#endif

    // Take the keys, then erase the 2BL image (0x6000 bytes at 0x80400000), as the stock kernel does. A quick reboot keeps memory: the keys come from the
    // persisted copy and nothing is erased.
    bool quick = KiQuickRebootFlag;
    memcpy(XboxEEPROMKey, bootKeys, 16);
    memcpy(CertKey, bootKeys + 16, 16);
    if (!quick) {
        memcpy(KiPersistedBootKeys, bootKeys, 32);
        EepromKeyStateCount = 0;
        auto ext = (const ULONG *)(bootKeys + 32);
        if (arg0 == XK_OPEN_BOOT_MAGIC && ext[0] == XK_KEY_STATES_MAGIC) {
            ULONG n = ext[1] < 8 ? ext[1] : 8;
            memcpy(EepromKeyStates, ext + 2, n * sizeof(EepromKeyStates[0]));
            EepromKeyStateCount = n;
        }
        memset((PVOID)0x80400000, 0, 0x6000);
    }

    KiBootStep("processor");
    InitializeProcessor();

    // As the reference (KfLowerIrql(1) at 0x8001C838, before its executive init): the start-up runs at APC_LEVEL, and with interrupts enabled from the PIC
    // set-up on, so the clock, timers and DPCs run while the drivers start. (The PIC is not set up yet: only the IRQL value changes here;
    // hardware::chipset::InitPic masks every line.)
    KiPcr.Irql = APC_LEVEL;
    SetStage(post_code::HalPhase0);
    KiBootStep("HAL phase 0");
    hardware::chipset::InitHalPhase0(/*coldBoot=*/!quick);
    // Fast green blink: the kernel is starting (steady green once the title is entered).
    hardware::chipset::SetBootLed(0x0A);

    diagnostics::TakeBootLoaderRecords(quick);
    boot_splash::TakeSplashRecord(quick); // boot/bl.cpp's splash record at 0x2380, before the memory manager
    KiBootStep("memory manager");
    memory::InitMemoryManager(quick);
    diagnostics::ReportMachineChecksAtBoot(quick);
    diagnostics::ReportNvPads("after the memory manager");
    diagnostics::ReportRefresh();
    diagnostics::ReportPreviousBootMarks();
    diagnostics::ReportLastWords();
    XK_SERIAL("cpu: PAT %08lx%08lx\n", (ULONG)(rdmsr(0x277) >> 32), (ULONG)rdmsr(0x277));
    {
        [[maybe_unused]] static const char *what[] = {"not run", "64 MB (64 MB + x reads back at x)", "more than 64 MB", "unclear"};
        [[maybe_unused]] ULONG top;
        HalReadWritePCISpace(0, 0, 0x84, &top, 4, FALSE);
        XK_SERIAL("ram: %s; GPU memory size %08lx -> %08lx (PFB 0x100200 %08lx); host top of memory %08lx\n", what[memory::RamProbe & 3], memory::GpuSizeBefore,
                  memory::GpuSizeAfter,
                  *(volatile ULONG *)0xFD100200, top);
    }
    if (!quick)
        hardware::chipset::DetectHardware();
    InitializeDpcs();
    InitializeTimers();
    KiBootStep("scheduler");
    InitializeScheduler(KiBootStack + KI_BOOT_STACK_SIZE, KiBootStack);
    // The reference enables its system interrupts here (0x80031876, called at 0x80015907, after the memory manager and the hardware detection, before the
    // object and I/O managers): the clock (IRQ 0, latched) first. Its IRQ 8 (RTC, whose interrupts stay off) and IRQ 11 (SMBus) handlers have no counterpart
    // here yet (our SMBus polls); IRQ 12 (SMC events) is enabled by hardware::chipset::StartSmcEvents below.
    HalEnableSystemInterrupt(0, Latched); // PIT clock
    XK_SERIAL("boot: clock interrupt on\n");
    KiBootStep("object manager");
    objects::InitObjectManager();
    KiBootStep("I/O manager");
    io::InitIoSystem();
    filesystems::InitFileSystems();
    InitializeSystemTime();
    KiBootStep("EEPROM");
    hardware::chipset::InitEeprom();
    KiBootStep("AV");
    hardware::video::InitVideo();
    if (!quick)
        hardware::chipset::InitSmcColdBoot();
    // The splash before the IDE set-up: a hard disk or DVD drive can take seconds to spin up after power-on, and the drive waits run at this
    // thread's IRQL, so the splash shows meanwhile (spec/oxos_internal/00).
    if (!quick)
        boot_splash::StartSplash();
    XK_SERIAL("boot: splash started\n");
    KiBootStep("IDE");
    hardware::ide::InitIdeChannel();
    XK_SERIAL("boot: IDE done\n");
    hardware::chipset::StartSmcEvents();
    XK_SERIAL("boot: SMC events started\n");

#if XK_BOOT_SELFTEST
    if (!quick)
        KiRunBootSelfTests();
#endif
    diagnostics::ReportNvPads("at the title launcher");
#if XK_STATEDUMP
    {
        // The machine-state probe (tools/statedump, make STATEDUMP=1), where a reference dump is taken.
        struct P {
            static void Out(const char *Line) {
                XK_SERIAL("%s", Line);
            }
        };
        statedump::Dump(P::Out, "OXOS (end of start-up)");
    }
#endif
#if XK_REBOOT_TEST
    { // debug (make REBOOT_TEST=N): a reboot N seconds into every boot (also inside a title), through the in-game reset
        struct R {
            static void NTAPI Run(PVOID) {
                LARGE_INTEGER t;
                t.QuadPart = -10000000LL * XK_REBOOT_TEST;
                KeDelayExecutionThread(KernelMode, FALSE, &t);
                XK_SERIAL("reboot test: the in-game reset's reboot\n");
                in_game_reset::Reboot();
            }
        };
        CreateSystemThread(R::Run, nullptr, 0, 8);
    }
#endif
#if XK_DEBUG_SERIAL
    StartHangDumps(); // serial builds: the hang dump's own thread (interrupts.cpp)
#endif
    KiBootStep("title launcher");
    titles::StartTitleLauncher();

    KfRaiseIrql(DISPATCH_LEVEL); // the idle loop runs at DISPATCH_LEVEL
    SetStage(post_code::Idle);
#if XK_BOOT_SELFTEST
    if (!quick)
        KiReleaseBootSpinners();
#endif
    IdleLoop();
}

// --- Fatal paths

volatile ULONG LastExceptionVector = 0xFFFFFFFF;
volatile ULONG LastExceptionCode;
volatile ULONG LastExceptionEip;
extern "C" void UnexpectedException(ULONG vector, ULONG errorCode, ULONG eip) {
    diagnostics::SaveExceptionLastWords(vector, eip);
    XK_SERIAL("*** unexpected exception %lu, error code %#lx, eip %#lx\n", vector, errorCode, eip);
    if (vector == 18)
        diagnostics::ReportMachineCheckBanks("#MC");
    LastExceptionVector = vector;
    LastExceptionCode = errorCode;
    LastExceptionEip = eip;
    post((UCHAR)(post_code::Exception | (vector & 0x1F)));
    for (;;) {
        cli();
        halt();
    }
}

} // namespace xk::core
