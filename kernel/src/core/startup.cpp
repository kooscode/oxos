// Kernel start-up after the 2BL hand-off (spec/stock/01_boot.md, spec/stock/02_executive_init.md).
#include <xk/core.h>
#include <xk/arch.h>
#include <xk/hardware.h>
#include <xk/memory.h>
#include <xk/storage.h>
#include <xk/runtime.h>
#include <xk/serial.h>
#include <xk/titles.h>
#include <xk/selftest.h>
#include <xk/bootscreen.h>
#include <xk/filesystems.h>
#if XK_STATEDUMP
#include "../../../tools/statedump/statedump.h"
#endif

// Open BIOS image (spec/stock/01 §1, §5): no EEPROM key, but HMAC states of the known EEPROM keys (from
// XboxDev/cromwell) that ExInitEeprom tries in turn. Kept across quick reboots, which re-enter with arg0 = 0.
constexpr ULONG XK_OPEN_BOOT_MAGIC = 0x534F584F, XK_KEY_STATES_MAGIC = 0x4B42584F; // 'OXOS', 'OXBK'
XK_PERSIST ULONG KiEepromKeyStates[8][10];
XK_PERSIST ULONG KiEepromKeyStateCount;

// The certificate key handed over by the 2BL (spec 01 §1): the per-title keys are derived from it (titles/loader.cpp).
XK_PERSIST UCHAR KiCertKey[16];

// Quick reboot (spec/stock/02 §1): flag, count, and the boot keys kept for the restart.
XK_PERSIST volatile UCHAR KiQuickRebootFlag;
XK_PERSIST volatile ULONG KiQuickRebootCount;
XK_PERSIST UCHAR KiPersistedBootKeys[32];

// Boot progress, readable from the emulator monitor (tests/boot_test.py).
volatile ULONG KiBootStage;

static ULONG KiPadRecord[13]; // the 2BL's pad calibration record, copied before the memory manager unmaps page 2

// The reports of the boot diagnostics and last-words sections below.
static void KiReportMachineChecksAtBoot();
static void KiReportNvPads(const char *When);
static void KiReportRefresh();
static void KiReportLastWords();

// --- Start-up marks (xk/serial.h). Off once the title starts, or once a fault record owns the CMOS bytes.

extern "C" volatile BOOLEAN XkBootMarksOn;
volatile BOOLEAN XkBootMarksOn;
static ULONG XkBootMarkCount, KiPrevMark, KiPrevMarkCount;
[[maybe_unused]] static ULONG KiPrevMarkCaller;
static bool KiPrevMarkFound, KiPrevStallFound;
static ULONG KiPrevStallEip, KiPrevStallIrql, KiPrevStallSeconds;
extern "C" volatile ULONG XkLastMarkTick;
volatile ULONG XkLastMarkTick;         // XkWatchdogTicks at the last mark (the clock interrupt's start-up watchdog)
static volatile ULONG XkWatchdogTicks; // clock interrupts the watchdog has seen

extern "C" void XkBootMark(const char *Format) {
    if (!XkBootMarksOn)
        return;
    ULONG flags = save_flags_cli(); // the index / data pairs must not interleave with another CMOS access
    if (XkBootMarkCount++ == 0)
        HalpCmosWrite(0x70, 'P');
    for (int i = 0; i < 4; i++)
        HalpCmosWrite((UCHAR)(0x71 + i), (UCHAR)((ULONG)Format >> (8 * i)));
    HalpCmosWrite(0x75, (UCHAR)XkBootMarkCount);
    HalpCmosWrite(0x76, (UCHAR)(XkBootMarkCount >> 8));
    if (XkLastMarkTick + 3000 <= XkWatchdogTicks)
        HalpCmosWrite(0x77, 0); // a stall record of an earlier pause is stale now
    XkLastMarkTick = XkWatchdogTicks;
    restore_flags(flags);
}

// Start-up watchdog (clock interrupt, all builds): no mark for 3 s while the marks are on means start-up is stuck.
// Each tick from then on leaves the interrupted EIP and IRQL in CMOS ('W' 0x77, EIP 0x78-0x7B, IRQL 0x7C, seconds
// since the last mark 0x7D): a CPU that idles shows the idle loop, one that spins shows where. No record at all
// means the clock interrupt stopped too.
extern "C" void XkBootMarkWatchdog(ULONG Eip, UCHAR Irql) {
    ULONG now = XkWatchdogTicks + 1;
    XkWatchdogTicks = now;
    if (!XkBootMarksOn || now - XkLastMarkTick < 3000)
        return;
    HalpCmosWrite(0x77, 'W');
    for (int i = 0; i < 4; i++)
        HalpCmosWrite((UCHAR)(0x78 + i), (UCHAR)(Eip >> (8 * i)));
    HalpCmosWrite(0x7C, Irql);
    ULONG s = (now - XkLastMarkTick) / 1000;
    HalpCmosWrite(0x7D, (UCHAR)(s > 255 ? 255 : s));
}

// The title is starting: the marks stop, and the record says start-up finished ('T').
extern "C" void XkBootMarksDone() {
    XkBootMarksOn = FALSE;
    HalpCmosWrite(0x70, 'T');
}

// --- Start-up (spec/stock/01 §2, spec/stock/02)

// Serial log of the start-up steps (SERIAL=1 builds, spec/oxos_internal/03_debug_serial.md).
#define KiBootStep(name) XK_SERIAL("boot: %s\n", name)

static void KiSetStage(UCHAR code) {
    KiBootStage = code;
    post(code);
}

// DRAM refresh at kernel entry (spec/oxos_internal/20): PFB 0x100210 bit 31 must be set. A boot loader that leaves it
// clear lets RAM lose bits within seconds; it is set here before the kernel relies on memory. The boot loader's own
// fixes (OXOS's start.S and 2BL, records at physical 0x23D8 and 0x23D0) and this one are logged once the serial port
// is up (KiReportRefresh).
static ULONG KiRefreshFoundOff = 0xFFFFFFFF; // the value found when refresh was off at entry (0xFFFFFFFF: it was on)
static ULONG KiRefreshBootFix[4];            // start.S's record ('RFS0', value), the 2BL's record ('RFEN', value)
// The boot loader's other records, copied before the memory manager unmaps page 2: the 2BL's GPU clock record
// (boot/bl.cpp, physical 0x23E0), start.S's snapshot right after the X-codes (serial builds, 0x2340), and the 2BL's
// X-code checks (BlCheckXcodeValues, 0x2400 and 0x2600).
static ULONG KiGpuClockRecord[5];
static ULONG KiXcodeEntryRecord[9];
static ULONG KiXcodeRecord[2][2 + 2 * 60];
#if XK_DEBUG_SERIAL
static ULONG KiXcodeSnapshot[2 + 2 * 40]; // the 2BL's snapshot of the memory set-up (serial builds, physical 0x2800)
#endif

extern "C" void KiSystemStartup(ULONG arg0, const UCHAR *bootKeys) {
    {
        auto refresh = (volatile ULONG *)0xFD100210; // NV2A registers, mapped by the boot loader's page directory
        if (!(*refresh & 0x80000000)) {
            KiRefreshFoundOff = *refresh;
            *refresh = 0x80000000;
        }
    }
    // The previous boot's start-up record, before this boot's first mark overwrites it.
    if (HalpCmosRead(0x70) == 'P') {
        KiPrevMarkFound = true;
        for (int i = 0; i < 4; i++)
            KiPrevMark |= (ULONG)HalpCmosRead((UCHAR)(0x71 + i)) << (8 * i);
        KiPrevMarkCount = HalpCmosRead(0x75) | HalpCmosRead(0x76) << 8;
        if (HalpCmosRead(0x77) == 'W') {
            KiPrevStallFound = true;
            for (int i = 0; i < 4; i++)
                KiPrevStallEip |= (ULONG)HalpCmosRead((UCHAR)(0x78 + i)) << (8 * i);
            KiPrevStallIrql = HalpCmosRead(0x7C);
            KiPrevStallSeconds = HalpCmosRead(0x7D);
        }
        HalpCmosWrite(0x77, 0);
    }
    if (HalpCmosRead(0x70) != 'X' && HalpCmosRead(0x70) != 'B')
        XkBootMarksOn = TRUE; // else the fault record stays until KiReportLastWords has shown it
    if (!KiQuickRebootFlag)
        XeReadNetbootTitle(); // a title delivered by netboot (titles/nettitle.cpp): before the bank's pages are reused
    KiSetStage(post_code::Entry);
#if XK_DEBUG_SERIAL
    HalpSerialInit();
    XK_SERIAL("\nOXOS kernel (" __DATE__ " " __TIME__ "): %s boot\n", KiQuickRebootFlag ? "quick" : "cold");
#endif

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

    // As the reference (KfLowerIrql(1) at 0x8001C838, before its executive init): the start-up runs at APC_LEVEL, and
    // with interrupts enabled from the PIC set-up on, so the clock, timers and DPCs run while the drivers start.
    // (The PIC is not set up yet: only the IRQL value changes here; HalpInitPic masks every line.)
    KiPcr.Irql = APC_LEVEL;
    KiSetStage(post_code::HalPhase0);
    KiBootStep("HAL phase 0");
    HalInitSystemPhase0(/*coldBoot=*/!quick);
    HalpSetBootLed(0x0A); // fast green blink: the kernel is starting (steady green once the title is entered)

    for (int i = 0; i < 4; i++) // the boot loader's refresh records (boot/start.S at 0x23D8, boot/bl.cpp at 0x23D0)
        KiRefreshBootFix[i] = quick ? 0 : ((volatile ULONG *)0x800023D0)[i < 2 ? i + 2 : i - 2];
    for (int i = 0; i < 9; i++) // boot/start.S's X-code snapshot at 0x2340 (serial builds)
        KiXcodeEntryRecord[i] = quick ? 0 : ((volatile ULONG *)0x80002340)[i];
    if (!quick)
        ((volatile ULONG *)0x80002340)[0] = 0; // a later boot loader without the snapshot leaves no stale record
    for (int i = 0; i < 5; i++) // boot/bl.cpp's GPU clock record at 0x23E0
        KiGpuClockRecord[i] = quick ? 0 : ((volatile ULONG *)0x800023E0)[i];
    for (ULONG r = 0; r < 2; r++) {
        for (ULONG i = 0; i < sizeof(KiXcodeRecord[0]) / 4; i++)
            KiXcodeRecord[r][i] = quick ? 0 : ((volatile ULONG *)(0x80002400 + 0x200 * r))[i];
    }
#if XK_DEBUG_SERIAL
    for (ULONG i = 0; i < sizeof(KiXcodeSnapshot) / 4; i++) // boot/bl.cpp BlSnapshotRegisters (serial builds)
        KiXcodeSnapshot[i] = quick ? 0 : ((volatile ULONG *)0x80002800)[i];
    if (!quick)
        ((volatile ULONG *)0x80002800)[0] = 0;
#endif
    for (int i = 0; i < 13; i++) // boot/bl.cpp BL_PAD_RESULT at physical 0x2300 (cold boot only; page 2 is free)
        KiPadRecord[i] = quick ? 0 : ((volatile ULONG *)0x80002300)[i];
    HalpTakeSplashRecord(quick); // boot/bl.cpp's splash record at 0x2380, before the memory manager
    KiBootStep("memory manager");
    MmInitSystem(quick);
    KiReportMachineChecksAtBoot();
    KiReportNvPads("after the memory manager");
    KiReportRefresh();
    KiReportLastWords();
    XK_SERIAL("cpu: PAT %08lx%08lx\n", (ULONG)(rdmsr(0x277) >> 32), (ULONG)rdmsr(0x277));
    {
        [[maybe_unused]] static const char *what[] = {"not run", "64 MB (64 MB + x reads back at x)", "more than 64 MB",
                                     "unclear"};
        [[maybe_unused]] ULONG top;
        HalReadWritePCISpace(0, 0, 0x84, &top, 4, FALSE);
        XK_SERIAL("ram: %s; GPU memory size %08lx -> %08lx (PFB 0x100200 %08lx); host top of memory %08lx\n",
                  what[MiRamProbe & 3], MiGpuSizeBefore, MiGpuSizeAfter, *(volatile ULONG *)0xFD100200, top);
    }
    if (!quick)
        HalpDetectHardware();
    KiInitializeDpcs();
    KiInitializeTimers();
    KiBootStep("scheduler");
    KiInitializeScheduler(KiBootStack + KI_BOOT_STACK_SIZE, KiBootStack);
    // The reference enables its system interrupts here (0x80031876, called at 0x80015907, after the memory manager
    // and the hardware detection, before the object and I/O managers): the clock (IRQ 0, latched) first. Its IRQ 8
    // (RTC, whose interrupts stay off) and IRQ 11 (SMBus) handlers have no counterpart here yet (our SMBus polls);
    // IRQ 12 (SMC events) is enabled by HalpSmcStartEvents below.
    HalEnableSystemInterrupt(0, Latched); // PIT clock
    XK_SERIAL("boot: clock interrupt on\n");
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
    // The splash (and its jingle) before the IDE set-up: a hard disk or DVD drive can take seconds to spin up after
    // power-on, and the drive waits run at this thread's IRQL, so the splash shows meanwhile (spec/oxos_internal/00).
    if (!quick)
        HalpStartSplash();
    XK_SERIAL("boot: splash started\n");
    KiBootStep("IDE");
    IdexChannelInit();
    XK_SERIAL("boot: IDE done\n");
    HalpSmcStartEvents();
    XK_SERIAL("boot: SMC events started\n");

#if XK_BOOT_SELFTEST
    if (!quick)
        KiRunBootSelfTests();
#endif
    KiReportNvPads("at the title launcher");
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
                HalpIgrReboot();
            }
        };
        KiCreateSystemThread(R::Run, nullptr, 0, 8);
    }
#endif
    KiBootStep("title launcher");
    XeStartTitleLauncher();

    KfRaiseIrql(DISPATCH_LEVEL); // the idle loop runs at DISPATCH_LEVEL
    KiSetStage(post_code::Idle);
#if XK_BOOT_SELFTEST
    if (!quick)
        KiReleaseBootSpinners();
#endif
    KiIdleLoop();
}

// --- Boot diagnostics for the debug log: machine-check banks, DRAM refresh, NV2A memory pads

// Machine checks stay off, as in the reference (spec/stock/23 §1: CR4.MCE clear, no MCG_CTL or MCi_CTL write). The
// banks are only read: what they hold at boot (errors logged before the last reset) goes to the debug log.
static ULONG KiMcaBanks;
static void KiReportMachineCheckBanks(const char *When) {
    for (ULONG i = 0; i < KiMcaBanks; i++) {
        ULONGLONG st = rdmsr(0x401 + 4 * i);
        if (st >> 63) {
            [[maybe_unused]] ULONGLONG addr = (st >> 58 & 1) ? rdmsr(0x402 + 4 * i) : 0;
            XK_SERIAL("*** machine check bank %lu (%s): status %08lx%08lx address %08lx%08lx\n", i, When,
                      (ULONG)(st >> 32), (ULONG)st, (ULONG)(addr >> 32), (ULONG)addr);
        }
    }
    if (KiMcaBanks)
        XK_SERIAL("machine check: MCG_STATUS %08lx (%s)\n", (ULONG)rdmsr(0x17A), When);
}

// Whether DRAM refresh had to be switched on before or at kernel entry (debug log, spec/oxos_internal/20).
static void KiReportRefresh() {
    [[maybe_unused]] const ULONG *r = KiRefreshBootFix;
    if (r[0] == 0x30534652) // 'RFS0'
        XK_SERIAL("*** refresh: DRAM refresh was off after the X-codes (PFB 0x100210 %08lx); start.S set it\n", r[1]);
    if (r[2] == 0x4E454652) { // 'RFEN'
        XK_SERIAL("*** refresh: the 2BL found DRAM refresh off before the kernel (PFB 0x100210 %08lx) and set it\n",
                  r[3]);
    }
    if (KiRefreshFoundOff != 0xFFFFFFFF)
        XK_SERIAL("*** refresh: DRAM refresh was off at kernel entry (PFB 0x100210 %08lx); set\n", KiRefreshFoundOff);
    // The GPU core clock (boot/bl.cpp BlCheckGpuClock): NVPLL and the PLL select as the X-codes write them, unless the
    // 2BL had to put them back.
    [[maybe_unused]] const ULONG *g = KiGpuClockRecord;
    if (g[0] == 0x4C50564E) { // 'NVPL'
        if (g[1] != 0x00011C01 || g[2] != 0x000A0400 || g[3] != 0x00011C01 || g[4] != 0x000A0400) {
            XK_SERIAL("*** gpu clock: NVPLL %08lx select %08lx at 2BL start, %08lx %08lx before the kernel; the 2BL "
                      "set 00011c01 000a0400\n", g[1], g[2], g[3], g[4]);
        } else {
            XK_SERIAL("gpu clock: NVPLL 00011c01 select 000a0400\n");
        }
    }
    // boot/start.S's snapshot of the settings that get lost on some cold boots, taken before any OXOS code ran
    // (spec/oxos_internal/27 §4).
    [[maybe_unused]] const ULONG *e = KiXcodeEntryRecord;
    if (e[0] == 0x53304358) { // 'XC0S'
        XK_SERIAL("xcodes at entry: 10b0 %08lx 10bc %08lx 10e8 %08lx nvpll %08lx select %08lx refresh %08lx "
                  "100200 %08lx 100204 %08lx\n", e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
    }
    // The 2BL's snapshot of the memory set-up the X-codes left, and EEPROM 0xC0-0xC3 (spec/oxos_internal/28 §4 G0):
    // NV2A offsets, PCI configuration addresses (bit 31) and EEPROM words (0xEE0000nn), four per line.
#if XK_DEBUG_SERIAL
    const ULONG *m = KiXcodeSnapshot;
    if (m[0] == 0x53363158) { // 'X16S'
        for (ULONG i = 0; i < m[1] && i < 40; i += 4) {
            XK_SERIAL("xcode snapshot:");
            for (ULONG j = i; j < i + 4 && j < m[1]; j++)
                XK_SERIAL(" %lx=%08lx", m[2 + 2 * j], m[3 + 2 * j]);
            XK_SERIAL("\n");
        }
    }
#endif
    // The 2BL's checks of the X-codes' settings (at its start, and before the kernel): the registers that did not hold
    // the value written.
    for (ULONG r = 0; r < 2; r++) {
        [[maybe_unused]] const ULONG *x = KiXcodeRecord[r];
        [[maybe_unused]] const char *when = r ? "before the kernel" : "at 2BL start";
        if (x[0] != 0x4B484358) // 'XCHK'
            continue;
        XK_SERIAL("xcodes: %lu settings differ %s\n", x[1], when);
        for (ULONG i = 0; i < x[1] && i < 60; i++)
            XK_SERIAL("xcodes: %s: %08lx reads %08lx\n", when, x[2 + 2 * i], x[3 + 2 * i]);
    }
}

// NV2A memory pad registers (debug log): the X-codes set them; read at two points to see whether they change.
static void KiReportNvPads([[maybe_unused]] const char *When) {
    [[maybe_unused]] auto nv = [](ULONG o) { return *(volatile ULONG *)(0xFD000000 + o); };
    XK_SERIAL("nv pads (%s): 1214 %08lx 122c %08lx 1230 %08lx 1234 %08lx 1238 %08lx 123c %08lx 1240 %08lx 1244 %08lx "
              "1248 %08lx 1250 %08lx\n", When, nv(0x1214), nv(0x122C), nv(0x1230), nv(0x1234), nv(0x1238), nv(0x123C),
              nv(0x1240), nv(0x1244), nv(0x1248), nv(0x1250));
    // The 2BL's pad calibration record (boot/bl.cpp BL_PAD_RESULT, physical 0x2300), printed once.
    static bool shown;
    [[maybe_unused]] auto r = KiPadRecord;
    // 'PADC' the table method, 'PADE' the EEPROM method (1.6), 'PADN' none.
    if (!shown && (r[0] == 0x43444150 || r[0] == 0x45444150 || r[0] == 0x4E444150)) {
        shown = true;
        if (r[0] == 0x4E444150) {
            XK_SERIAL("nv pads: the 2BL did not calibrate (no table in this image, or no EEPROM answer)\n");
        } else {
            XK_SERIAL("nv pads: 2BL calibration (%s) A %#lx B %#lx strap %08lx -> %08lx %08lx %08lx %08lx %08lx %08lx "
                      "%08lx %08lx %08lx\n", r[0] == 0x45444150 ? "EEPROM rows" : "table", r[1], r[2], r[3], r[4],
                      r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12]);
        }
    }
}

static void KiReportMachineChecksAtBoot() {
    ULONG a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    XK_SERIAL("cpu: signature %08lx, microcode revision %lu\n", a, KiMicrocodeRevision);
    if (!(d & (1u << 7)) || !(d & (1u << 14))) // no machine-check exception or architecture
        return;
    KiMcaBanks = (ULONG)rdmsr(0x179) & 0xFF;
    KiReportMachineCheckBanks("at boot");
}

// --- Last words: a record in CMOS that survives a power cycle (a serial build prints it at the next boot)
// CMOS bytes 0x70-0x7F (free; 0x10-0x6F and 0x80-0xFF hold the clock signature): 'X', vector, EIP, MCG_STATUS low,
// bank 1 status low and high.
static void KiCmosPut32(UCHAR at, ULONG v) {
    for (int i = 0; i < 4; i++)
        HalpCmosWrite((UCHAR)(at + i), (UCHAR)(v >> (8 * i)));
}

[[maybe_unused]] static ULONG KiCmosGet32(UCHAR at) {
    ULONG v = 0;
    for (int i = 0; i < 4; i++)
        v |= (ULONG)HalpCmosRead((UCHAR)(at + i)) << (8 * i);
    return v;
}

static void KiSaveLastWords(ULONG vector, ULONG eip) {
    XkBootMarksOn = FALSE;
    HalpCmosWrite(0x70, 'X');
    HalpCmosWrite(0x71, (UCHAR)vector);
    KiCmosPut32(0x72, eip);
    if (KiMcaBanks) {
        KiCmosPut32(0x76, (ULONG)rdmsr(0x17A));
        ULONGLONG st = rdmsr(0x405);
        KiCmosPut32(0x7A, (ULONG)st);
        HalpCmosWrite(0x7E, (UCHAR)(st >> 56));
    }
}

// Bug checks: 'B', the code (0x71), the first parameter (0x75) and the caller (0x79); for 0x1E the access violation's
// address (0x75) and the faulting eip (0x79).
extern "C" void KiSaveBugCheckLastWords(ULONG Code, ULONG P1, ULONG Caller) {
    XkBootMarksOn = FALSE;
    HalpCmosWrite(0x70, 'B');
    KiCmosPut32(0x71, Code);
    KiCmosPut32(0x75, P1);
    KiCmosPut32(0x79, Caller);
}

// For the NMI and double-fault tasks (core/processor.cpp): the same CMOS record, vector 2 or 8.
extern "C" void KiSaveFaultTaskLastWords(ULONG vector, ULONG eip) {
    KiSaveLastWords(vector, eip);
}

static void KiReportLastWords() {
    if (KiPrevMarkFound) {
        XK_SERIAL("*** previous boot stopped in start-up: last mark %08lx (mark %lu; the address of a log line's "
                  "format string in that boot's image)\n", KiPrevMark, KiPrevMarkCount);
    }
    if (KiPrevMarkFound) {
        XK_SERIAL("*** previous boot's last mark was set from a function called at %08lx (if that boot recorded it)\n",
                  KiPrevMarkCaller);
    }
    if (KiPrevStallFound) {
        XK_SERIAL("*** previous boot's start-up watchdog: %lu s after that mark the clock interrupt still ran, last "
                  "interrupted eip %08lx at IRQL %lu\n", KiPrevStallSeconds, KiPrevStallEip, KiPrevStallIrql);
    } else if (KiPrevMarkFound) {
        XK_SERIAL("*** previous boot's start-up watchdog: no record (the clock interrupt stopped, or start-up "
                  "had not stalled for 3 s)\n");
    }
    if (HalpCmosRead(0x70) == 'B') {
        if (KiCmosGet32(0x71) == 0x1E) {
            XK_SERIAL("*** previous boot ended in bug check 0x1e: unhandled exception at eip %08lx (an access "
                      "violation's address: %08lx)\n", KiCmosGet32(0x79), KiCmosGet32(0x75));
        } else {
            XK_SERIAL("*** previous boot ended in bug check %#lx (%#lx), called from %08lx\n", KiCmosGet32(0x71),
                      KiCmosGet32(0x75), KiCmosGet32(0x79));
        }
        HalpCmosWrite(0x70, 0);
        XkBootMarksOn = TRUE;
        return;
    }
    if (HalpCmosRead(0x70) != 'X')
        return;
    XK_SERIAL("*** previous boot ended in exception %u at eip %08lx; MCG_STATUS %08lx, bank 1 status %02x..%08lx\n",
              HalpCmosRead(0x71), KiCmosGet32(0x72), KiCmosGet32(0x76), HalpCmosRead(0x7E), KiCmosGet32(0x7A));
    HalpCmosWrite(0x70, 0);
    XkBootMarksOn = TRUE;
}

// --- Fatal paths

volatile ULONG KiLastExceptionVector = 0xFFFFFFFF;
volatile ULONG KiLastExceptionCode;
volatile ULONG KiLastExceptionEip;
extern "C" void KiUnexpectedException(ULONG vector, ULONG errorCode, ULONG eip) {
    KiSaveLastWords(vector, eip);
    XK_SERIAL("*** unexpected exception %lu, error code %#lx, eip %#lx\n", vector, errorCode, eip);
    if (vector == 18)
        KiReportMachineCheckBanks("#MC");
    KiLastExceptionVector = vector;
    KiLastExceptionCode = errorCode;
    KiLastExceptionEip = eip;
    post((UCHAR)(post_code::Exception | (vector & 0x1F)));
    for (;;) {
        cli();
        halt();
    }
}
