// Hardware-breakpoint tracing for real consoles (OXOS addition, network-log builds; spec/oxos-internal/03 §9).
// tools/netboot/netboot.py --dr0 ... --steps N writes a configuration block into the image it serves, at bank offset
// 0x6020 (free space after the payload header). The 2BL leaves the downloaded bank at physical 0x800000; the kernel
// copies the block at entry, programs DR0-DR3 / DR7 once its IDT is up, and its #DB handler reports each breakpoint
// hit and, from the first hit of DR0, single-steps the CPU for N instructions, sending the address of every one (8
// per line). Nothing in the kernel's layout depends on the configuration, so moving a breakpoint needs no rebuild.
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/serial.h>

XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);

#if XK_NETLOG
struct XK_DEBUG_CONFIG {
    ULONG Magic; // 'DBGC'
    ULONG Dr[4];
    ULONG Dr7;
    ULONG Steps; // single steps from the first DR0 hit (0: report hits only)
    ULONG Flags; // bit 0: DR1 starts a second stepping run
};
static_assert(sizeof(XK_DEBUG_CONFIG) == 32);
constexpr ULONG DEBUG_CONFIG_MAGIC = 0x43474244, PAYLOAD_MAGIC = 0x504B584F;

extern "C" volatile BOOLEAN XkBootMarksOn; // ke/startup.cpp: the step record takes over CMOS 0x70-0x7F
static XK_DEBUG_CONFIG KiDbg;
volatile ULONG KiDbgConfigured; // read by the network log: keep the NIC (step output) until a title reprograms it
static bool KiDbgArmed, KiDbgStepping, KiDbgSecondRun;
static ULONG KiDbgStepsLeft, KiDbgHits, KiDbgRing[8], KiDbgRingUsed;

// At kernel entry, before anything can reuse the downloaded bank's pages (still mapped by the 2BL at 0x80000000).
extern "C" void KiReadDebugConfig() {
    auto bank = (const ULONG *)0x80800000;
    auto c = (const XK_DEBUG_CONFIG *)0x80806020;
    if (bank[0x6000 / 4] == PAYLOAD_MAGIC && c->Magic == DEBUG_CONFIG_MAGIC)
        KiDbg = *c;
}

// Once the IDT and the network log are up.
extern "C" void KiArmDebugRegisters() {
    if (KiDbg.Magic != DEBUG_CONFIG_MAGIC)
        return;
    XK_SERIAL("dbg: DR0 %08lx DR1 %08lx DR2 %08lx DR3 %08lx DR7 %08lx, %lu steps from the first DR0 hit\n", KiDbg.Dr[0],
              KiDbg.Dr[1], KiDbg.Dr[2], KiDbg.Dr[3], KiDbg.Dr7, KiDbg.Steps);
    __asm__ volatile("mov %0, %%dr0" ::"r"(KiDbg.Dr[0]));
    __asm__ volatile("mov %0, %%dr1" ::"r"(KiDbg.Dr[1]));
    __asm__ volatile("mov %0, %%dr2" ::"r"(KiDbg.Dr[2]));
    __asm__ volatile("mov %0, %%dr3" ::"r"(KiDbg.Dr[3]));
    __asm__ volatile("mov %0, %%dr6" ::"r"(0u));
    __asm__ volatile("mov %0, %%dr7" ::"r"(KiDbg.Dr7));
    KiDbgArmed = true;
    KiDbgConfigured = 1;
    KiDbgStepsLeft = KiDbg.Steps;
}

static void KiDbgFlush() {
    if (!KiDbgRingUsed)
        return;
    char line[120];
    int n = RtlSnprintf(line, sizeof(line), "ss:");
    for (ULONG i = 0; i < KiDbgRingUsed; i++)
        n += RtlSnprintf(line + n, sizeof(line) - n, " %lx", KiDbgRing[i]);
    line[n++] = '\n';
    void HalpNetLogWrite(const char *Text, ULONG Length);
    HalpNetLogWrite(line, (ULONG)n);
    KiDbgRingUsed = 0;
}

// The last step's state also goes to CMOS 0x70-0x7F (survives a freeze and a power cycle, needs no NIC): 'S', EIP,
// EDI, ECX (4 bytes each) and the step count (16 bits). The next boot reports them (KiReportLastWords).
UCHAR HalpCmosRead(UCHAR reg);
void HalpCmosWrite(UCHAR reg, UCHAR v);
static ULONG KiDbgSteps;
static void KiDbgCmosPut(UCHAR at, ULONG v, ULONG bytes) {
    for (ULONG i = 0; i < bytes; i++)
        HalpCmosWrite((UCHAR)(at + i), (UCHAR)(v >> (8 * i)));
}
static void KiDbgCmosStep(ULONG eip, ULONG edi, ULONG ecx, ULONG) {
    if (KiDbgSteps == 0) {
        XkBootMarksOn = FALSE;
        HalpCmosWrite(0x70, 'S');
    }
    KiDbgCmosPut(0x71, eip, 4);
    KiDbgCmosPut(0x75, edi, 4);
    KiDbgCmosPut(0x79, ecx, 4);
    KiDbgSteps++;
    KiDbgCmosPut(0x7D, KiDbgSteps, 2);
}

// From KiDispatchTrap for vector 1: TRUE when the debug exception was ours (the frame is resumed as edited).
bool KiDebugTrap(ULONG *eip, ULONG *eflags, ULONG edi, ULONG ecx, ULONG ebp) {
    if (!KiDbgArmed)
        return false;
    ULONG dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" ::"r"(0u));
    if (dr6 & 0xF) { // a breakpoint
        KiDbgHits++;
        if (KiDbgHits <= 64) {
            KiDbgFlush();
            XK_SERIAL("dbg: breakpoint %s%s%s%s at eip %08lx (hit %lu)\n", dr6 & 1 ? "0" : "", dr6 & 2 ? "1" : "",
                      dr6 & 4 ? "2" : "", dr6 & 8 ? "3" : "", *eip, KiDbgHits);
        }
        // Flags bit 0: a DR1 hit starts a second run of Steps (the first run's timing reaches it).
        if ((dr6 & 2) && (KiDbg.Flags & 1) && !KiDbgStepping && !KiDbgSecondRun) {
            KiDbgSecondRun = true;
            KiDbgStepsLeft = KiDbg.Steps;
        }
        if ((dr6 & 3) && !KiDbgStepping && KiDbgStepsLeft) {
            KiDbgStepping = true;
            *eflags |= 0x100; // TF: step from here
        }
        *eflags |= 0x10000; // RF: run the breakpointed instruction
        return true;
    }
    if (dr6 & 0x4000) { // single step
        if (!KiDbgStepping) { // a thread switched out while stepping kept TF in its saved flags: just stop it
            *eflags &= ~0x100u;
            return true;
        }
        KiDbgCmosStep(*eip, edi, ecx, ebp);
        KiDbgRing[KiDbgRingUsed++] = *eip;
        if (KiDbgRingUsed == 8)
            KiDbgFlush();
        if (--KiDbgStepsLeft == 0) {
            KiDbgFlush();
            XK_SERIAL("dbg: stepping done\n");
            KiDbgStepping = false;
            *eflags &= ~0x100u;
        } else {
            *eflags |= 0x100;
        }
        return true;
    }
    return false;
}
#endif
