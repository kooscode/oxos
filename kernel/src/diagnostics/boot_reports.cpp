// Boot reports for the debug log (serial builds): the machine-check banks, DRAM refresh (spec/oxos_internal/20), the
// GPU clock, the X-code checks and snapshots, and the NV2A memory pads with the 2BL's calibration
// (spec/oxos_internal/27, 28). The boot loader's records are copied before the memory manager unmaps page 2.
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <core/core.h>
#include <hardware/chipset/serial.h>

namespace xk::diagnostics {

// DRAM refresh at kernel entry (spec/oxos_internal/20): PFB 0x100210 bit 31 must be set. A boot loader that leaves it
// clear lets RAM lose bits within seconds; it is set here before the kernel relies on memory. The boot loader's own
// fixes (OXOS's start.S and 2BL, records at physical 0x23D8 and 0x23D0) and this one are logged once the serial port
// is up (ReportRefresh).
static ULONG RefreshFoundOff = 0xFFFFFFFF; // the value found when refresh was off at entry (0xFFFFFFFF: it was on)
static ULONG RefreshBootFix[4];            // start.S's record ('RFS0', value), the 2BL's record ('RFEN', value)
// The boot loader's other records, copied before the memory manager unmaps page 2: the 2BL's GPU clock record
// (boot/bl.cpp, physical 0x23E0), start.S's snapshot right after the X-codes (serial builds, 0x2340), and the 2BL's
// X-code checks (BlCheckXcodeValues, 0x2400 and 0x2600).
static ULONG GpuClockRecord[5];
static ULONG XcodeEntryRecord[9];
static ULONG XcodeRecord[2][2 + 2 * 60];
#if XK_DEBUG_SERIAL
static ULONG XcodeSnapshot[2 + 2 * 40]; // the 2BL's snapshot of the memory set-up (serial builds, physical 0x2800)
#endif

static ULONG PadRecord[13]; // the 2BL's pad calibration record, copied before the memory manager unmaps page 2

void CheckDramRefresh() {
    auto refresh = (volatile ULONG *)0xFD100210; // NV2A registers, mapped by the boot loader's page directory
    if (!(*refresh & 0x80000000)) {
        RefreshFoundOff = *refresh;
        *refresh = 0x80000000;
    }
}

void TakeBootLoaderRecords(bool Quick) {
    for (int i = 0; i < 4; i++) // the boot loader's refresh records (boot/start.S at 0x23D8, boot/bl.cpp at 0x23D0)
        RefreshBootFix[i] = Quick ? 0 : ((volatile ULONG *)0x800023D0)[i < 2 ? i + 2 : i - 2];
    for (int i = 0; i < 9; i++) // boot/start.S's X-code snapshot at 0x2340 (serial builds)
        XcodeEntryRecord[i] = Quick ? 0 : ((volatile ULONG *)0x80002340)[i];
    if (!Quick)
        ((volatile ULONG *)0x80002340)[0] = 0; // a later boot loader without the snapshot leaves no stale record
    for (int i = 0; i < 5; i++) // boot/bl.cpp's GPU clock record at 0x23E0
        GpuClockRecord[i] = Quick ? 0 : ((volatile ULONG *)0x800023E0)[i];
    for (ULONG r = 0; r < 2; r++) {
        for (ULONG i = 0; i < sizeof(XcodeRecord[0]) / 4; i++)
            XcodeRecord[r][i] = Quick ? 0 : ((volatile ULONG *)(0x80002400 + 0x200 * r))[i];
    }
#if XK_DEBUG_SERIAL
    for (ULONG i = 0; i < sizeof(XcodeSnapshot) / 4; i++) // boot/bl.cpp BlSnapshotRegisters (serial builds)
        XcodeSnapshot[i] = Quick ? 0 : ((volatile ULONG *)0x80002800)[i];
    if (!Quick)
        ((volatile ULONG *)0x80002800)[0] = 0;
#endif
    for (int i = 0; i < 13; i++) // boot/bl.cpp BL_PAD_RESULT at physical 0x2300 (cold boot only; page 2 is free)
        PadRecord[i] = Quick ? 0 : ((volatile ULONG *)0x80002300)[i];
}

// Machine checks stay off, as in the reference (spec/stock/23 §1: CR4.MCE clear, no MCG_CTL or MCi_CTL write). The
// banks are only read: what they hold at boot (errors logged before the last reset) goes to the debug log.
ULONG MachineCheckBanks;

void ReportMachineCheckBanks(const char *When) {
    for (ULONG i = 0; i < MachineCheckBanks; i++) {
        ULONGLONG st = rdmsr(0x401 + 4 * i);
        if (st >> 63) {
            [[maybe_unused]] ULONGLONG addr = (st >> 58 & 1) ? rdmsr(0x402 + 4 * i) : 0;
            XK_SERIAL("*** machine check bank %lu (%s): status %08lx%08lx address %08lx%08lx\n", i, When,
                      (ULONG)(st >> 32), (ULONG)st, (ULONG)(addr >> 32), (ULONG)addr);
        }
    }
    if (MachineCheckBanks)
        XK_SERIAL("machine check: MCG_STATUS %08lx (%s)\n", (ULONG)rdmsr(0x17A), When);
}

// Whether DRAM refresh had to be switched on before or at kernel entry (debug log, spec/oxos_internal/20).
void ReportRefresh() {
    [[maybe_unused]] const ULONG *r = RefreshBootFix;
    if (r[0] == 0x30534652) // 'RFS0'
        XK_SERIAL("*** refresh: DRAM refresh was off after the X-codes (PFB 0x100210 %08lx); start.S set it\n", r[1]);
    if (r[2] == 0x4E454652) { // 'RFEN'
        XK_SERIAL("*** refresh: the 2BL found DRAM refresh off before the kernel (PFB 0x100210 %08lx) and set it\n",
                  r[3]);
    }
    if (RefreshFoundOff != 0xFFFFFFFF)
        XK_SERIAL("*** refresh: DRAM refresh was off at kernel entry (PFB 0x100210 %08lx); set\n", RefreshFoundOff);
    // The GPU core clock (boot/bl.cpp BlCheckGpuClock): NVPLL and the PLL select as the X-codes write them, unless the
    // 2BL had to put them back.
    [[maybe_unused]] const ULONG *g = GpuClockRecord;
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
    [[maybe_unused]] const ULONG *e = XcodeEntryRecord;
    if (e[0] == 0x53304358) { // 'XC0S'
        XK_SERIAL("xcodes at entry: 10b0 %08lx 10bc %08lx 10e8 %08lx nvpll %08lx select %08lx refresh %08lx "
                  "100200 %08lx 100204 %08lx\n", e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
    }
    // The 2BL's snapshot of the memory set-up the X-codes left, and EEPROM 0xC0-0xC3 (spec/oxos_internal/28 §4 G0):
    // NV2A offsets, PCI configuration addresses (bit 31) and EEPROM words (0xEE0000nn), four per line.
#if XK_DEBUG_SERIAL
    const ULONG *m = XcodeSnapshot;
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
        [[maybe_unused]] const ULONG *x = XcodeRecord[r];
        [[maybe_unused]] const char *when = r ? "before the kernel" : "at 2BL start";
        if (x[0] != 0x4B484358) // 'XCHK'
            continue;
        XK_SERIAL("xcodes: %lu settings differ %s\n", x[1], when);
        for (ULONG i = 0; i < x[1] && i < 60; i++)
            XK_SERIAL("xcodes: %s: %08lx reads %08lx\n", when, x[2 + 2 * i], x[3 + 2 * i]);
    }
}

// NV2A memory pad registers (debug log): the X-codes set them; read at two points to see whether they change.
void ReportNvPads([[maybe_unused]] const char *When) {
    [[maybe_unused]] auto nv = [](ULONG o) { return *(volatile ULONG *)(0xFD000000 + o); };
    XK_SERIAL("nv pads (%s): 1214 %08lx 122c %08lx 1230 %08lx 1234 %08lx 1238 %08lx 123c %08lx 1240 %08lx 1244 %08lx "
              "1248 %08lx 1250 %08lx\n", When, nv(0x1214), nv(0x122C), nv(0x1230), nv(0x1234), nv(0x1238), nv(0x123C),
              nv(0x1240), nv(0x1244), nv(0x1248), nv(0x1250));
    // The 2BL's pad calibration record (boot/bl.cpp BL_PAD_RESULT, physical 0x2300), printed once.
    static bool shown;
    [[maybe_unused]] auto r = PadRecord;
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

void ReportMachineChecksAtBoot() {
    ULONG a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    XK_SERIAL("cpu: signature %08lx, microcode revision %lu\n", a, core::MicrocodeRevision);
    if (!(d & (1u << 7)) || !(d & (1u << 14))) // no machine-check exception or architecture
        return;
    MachineCheckBanks = (ULONG)rdmsr(0x179) & 0xFF;
    ReportMachineCheckBanks("at boot");
}

} // namespace xk::diagnostics
