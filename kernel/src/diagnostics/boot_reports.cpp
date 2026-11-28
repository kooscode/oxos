// Boot reports for the debug log (serial builds): the CPU, and the problems a boot found: machine-check errors, DRAM refresh that was off
// (spec/oxos_internal/20), a GPU clock the 2BL had to put back (spec/oxos_internal/26). The boot loader's records are copied before the memory manager unmaps
// page 2; its other records (X-code read-back, pad calibration) stay there for a debugger at the GDB kernel's boot stop.
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <core/core.h>

namespace xk::diagnostics {

// DRAM refresh at kernel entry (spec/oxos_internal/20): PFB 0x100210 bit 31 must be set. A boot loader that leaves it clear lets RAM lose bits within seconds;
// it is set here before the kernel relies on memory. The boot loader's own fixes (OXOS's start.S and 2BL, records at physical 0x23D8 and 0x23D0) and this one
// are logged once the serial port is up (ReportRefresh).
static ULONG RefreshFoundOff = 0xFFFFFFFF; // the value found when refresh was off at entry (0xFFFFFFFF: it was on)
// The boot loader's records, copied before the memory manager unmaps page 2, only for the serial log's reports: a kernel without the serial port keeps none
// (they would take resident memory titles count on, spec/oxos_internal/18).
#if XK_DEBUG_SERIAL
static ULONG RefreshBootFix[4]; // start.S's record ('RFS0', value), the 2BL's record ('RFEN', value)
static ULONG GpuClockRecord[5]; // the 2BL's GPU clock record (boot/bl.cpp, physical 0x23E0)
#endif

void CheckDramRefresh() {
    auto refresh = (volatile ULONG *)0xFD100210; // NV2A registers, mapped by the boot loader's page directory
    if (!(*refresh & 0x80000000)) {
        RefreshFoundOff = *refresh;
        *refresh = 0x80000000;
    }
}

void TakeBootLoaderRecords([[maybe_unused]] bool Quick) {
#if XK_DEBUG_SERIAL
    for (int i = 0; i < 4; i++) // the boot loader's refresh records (boot/start.S at 0x23D8, boot/bl.cpp at 0x23D0)
        RefreshBootFix[i] = Quick ? 0 : ((volatile ULONG *)0x800023D0)[i < 2 ? i + 2 : i - 2];
    for (int i = 0; i < 5; i++) // boot/bl.cpp's GPU clock record at 0x23E0
        GpuClockRecord[i] = Quick ? 0 : ((volatile ULONG *)0x800023E0)[i];
#endif
}

// Machine checks stay off, as in the reference (spec/stock/23 §1: CR4.MCE clear, no MCG_CTL or MCi_CTL write). What the banks hold at boot goes to the debug
// log; at a cold boot they are then cleared (spec/oxos_internal/14), so a bank that holds something at a later boot is a new error since then.
ULONG MachineCheckBanks;

void ReportMachineCheckBanks(const char *When) {
    for (ULONG i = 0; i < MachineCheckBanks; i++) {
        ULONGLONG st = rdmsr(0x401 + 4 * i);
        if (st >> 63) {
            [[maybe_unused]] ULONGLONG addr = (st >> 58 & 1) ? rdmsr(0x402 + 4 * i) : 0;
            XK_SERIAL("*** machine check bank %lu (%s): status %08lx%08lx address %08lx%08lx\n", i, When, (ULONG)(st >> 32), (ULONG)st, (ULONG)(addr >> 32),
                      (ULONG)addr);
        }
    }
#if XK_DEBUG_SERIAL
    if (MachineCheckBanks && rdmsr(0x17A))
        XK_SERIAL("*** machine check: MCG_STATUS %08lx (%s)\n", (ULONG)rdmsr(0x17A), When);
#endif
}

// Whether DRAM refresh had to be switched on before or at kernel entry (debug log, spec/oxos_internal/20).
void ReportRefresh() {
    if (RefreshFoundOff != 0xFFFFFFFF)
        XK_SERIAL("*** refresh: DRAM refresh was off at kernel entry (PFB 0x100210 %08lx); set\n", RefreshFoundOff);
#if XK_DEBUG_SERIAL
    const ULONG *r = RefreshBootFix;
    if (r[0] == 0x30534652) // 'RFS0'
        XK_SERIAL("*** refresh: DRAM refresh was off after the X-codes (PFB 0x100210 %08lx); start.S set it\n", r[1]);
    if (r[2] == 0x4E454652) { // 'RFEN'
        XK_SERIAL("*** refresh: the 2BL found DRAM refresh off before the kernel (PFB 0x100210 %08lx) and set it\n", r[3]);
    }
    // The GPU core clock (boot/bl.cpp BlCheckGpuClock): NVPLL and the PLL select as the X-codes write them, unless the 2BL had to put them back.
    const ULONG *g = GpuClockRecord;
    if (g[0] == 0x4C50564E && (g[1] != 0x00011C01 || g[2] != 0x000A0400 || g[3] != 0x00011C01 || g[4] != 0x000A0400)) { // 'NVPL'
        XK_SERIAL("*** gpu clock: NVPLL %08lx select %08lx at 2BL start, %08lx %08lx before the kernel; the 2BL " "set 00011c01 000a0400\n", g[1], g[2],
                  g[3], g[4]);
    }
#endif
}

void ReportMachineChecksAtBoot(bool Quick) {
    ULONG a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    XK_SERIAL("cpu: signature %08lx, microcode revision %lu\n", a, core::MicrocodeRevision);
    if (!(d & (1u << 7)) || !(d & (1u << 14))) // no machine-check exception or architecture
        return;
    MachineCheckBanks = (ULONG)rdmsr(0x179) & 0xFF;
    ReportMachineCheckBanks("at boot");
    // A cold boot's banks hold the CPU's power-on contents or what the boot ROM's start left (a different error code at every power-on, the same on good and
    // bad boots, never one added by the kernel): cleared, they no longer come back at every quick reboot. Writing 0 is the only write MCi_STATUS takes.
    if (!Quick) {
        for (ULONG i = 0; i < MachineCheckBanks; i++)
            wrmsr(0x401 + 4 * i, 0);
    }
}

} // namespace xk::diagnostics
