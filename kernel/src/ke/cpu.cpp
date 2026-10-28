// Processor bring-up: GDT, TSSs, IDT and the processor control region (spec/stock/01-boot.md §2).
// Selector layout matches the reference kernel:
//   0x08 code, 0x10 data, 0x18 main TSS, 0x20 FS -> KPCR, 0x28 double-fault TSS, 0x30 NMI TSS.
#include <xk/arch.h>
#include <xk/ke.h>

namespace {

struct [[gnu::packed]] SegmentDescriptor {
    USHORT LimitLow;
    USHORT BaseLow;
    UCHAR BaseMid;
    UCHAR Access;
    UCHAR LimitHighFlags; // limit[19:16] | flags << 4
    UCHAR BaseHigh;
};
XK_SIZE(SegmentDescriptor, 8);

struct [[gnu::packed]] GateDescriptor {
    USHORT OffsetLow;
    USHORT Selector;
    UCHAR Reserved;
    UCHAR Type;
    USHORT OffsetHigh;
};
XK_SIZE(GateDescriptor, 8);

struct [[gnu::packed]] DescriptorTableRegister {
    USHORT Limit;
    ULONG Base;
};

struct KTSS {
    ULONG Link, Esp0, Ss0, Esp1, Ss1, Esp2, Ss2, Cr3, Eip, EFlags;
    ULONG Eax, Ecx, Edx, Ebx, Esp, Ebp, Esi, Edi;
    ULONG Es, Cs, Ss, Ds, Fs, Gs, LdtSelector;
    USHORT Trap, IoMapBase;
};
XK_SIZE(KTSS, 0x68);

constexpr USHORT KGDT_R0_CODE = 0x08, KGDT_R0_DATA = 0x10, KGDT_TSS = 0x18, KGDT_R0_PCR = 0x20,
                 KGDT_DF_TSS = 0x28, KGDT_NMI_TSS = 0x30;
constexpr ULONG GDT_ENTRIES = 7;
constexpr ULONG IDT_ENTRIES = 64;
// Double-fault and NMI stacks: their handlers only record the fault in CMOS.
constexpr ULONG FAULT_STACK_SIZE = 0x400;

alignas(8) SegmentDescriptor KiGdt[GDT_ENTRIES];
alignas(8) GateDescriptor KiIdt[IDT_ENTRIES];
KTSS KiTss, KiDoubleFaultTss, KiNmiTss;
alignas(16) UCHAR KiDoubleFaultStack[FAULT_STACK_SIZE];
alignas(16) UCHAR KiNmiStack[FAULT_STACK_SIZE];

SegmentDescriptor MakeSegment(ULONG base, ULONG limit, UCHAR access, UCHAR flags) {
    return {(USHORT)(limit & 0xFFFF), (USHORT)(base & 0xFFFF), (UCHAR)(base >> 16), access,
            (UCHAR)(((limit >> 16) & 0x0F) | (flags << 4)), (UCHAR)(base >> 24)};
}

GateDescriptor MakeInterruptGate(PVOID handler) {
    ULONG off = (ULONG)handler;
    return {(USHORT)(off & 0xFFFF), KGDT_R0_CODE, 0, 0x8E, (USHORT)(off >> 16)};
}

GateDescriptor MakeTaskGate(USHORT tssSelector) { return {0, tssSelector, 0, 0x85, 0}; }

void InitFaultTss(KTSS &tss, void (*entry)(), UCHAR *stack) {
    tss = {};
    tss.Cr3 = read_cr3();
    tss.Eip = (ULONG)entry;
    tss.EFlags = 0x2;
    tss.Esp = tss.Esp0 = (ULONG)(stack + FAULT_STACK_SIZE);
    tss.Ss = tss.Ss0 = tss.Ds = tss.Es = KGDT_R0_DATA;
    tss.Cs = KGDT_R0_CODE;
    tss.Fs = KGDT_R0_PCR;
    tss.IoMapBase = sizeof(KTSS);
}

} // namespace


// The double-fault and NMI tasks (entry.S): the task switch saved the interrupted state in KiTss; record where it
// was in CMOS (reported at the next boot), so a fault cascade does not just stop the console without a trace.
extern "C" void KiSaveFaultTaskLastWords(ULONG vector, ULONG eip);

extern "C" void KiFaultTaskReport(ULONG Kind) { KiSaveFaultTaskLastWords(Kind, KiTss.Eip); }

extern "C" void KiDoubleFaultTask();
extern "C" void KiNmiTask();
extern "C" void KiIgnoreInterrupt();
extern "C" PVOID KiTrapStubs[32];
extern "C" PVOID KiIrqStubs[16];
extern "C" UCHAR KiBootStack[];

KPCR KiPcr;
KTHREAD KiIdleThread;

static void KiInitializeGdt() {
    constexpr UCHAR TSS_AVAILABLE = 0x89;
    KiGdt[0] = {};
#if XK_TRACE_IMPORTS
    KiGdt[KGDT_R0_CODE / 8] = MakeSegment(0, 0xFFFFF, 0x9B, 0xC); // 4 GB: the import-trace stubs run from pool (0xD0000000+)
#else
    // The reference's code-segment limit (spec/stock/23 §6.4): 0xBFFFFFFF, so a jump into the page tables, pool, the
    // MMIO and AGP windows or the flash is an immediate #GP instead of running whatever is there.
    KiGdt[KGDT_R0_CODE / 8] = MakeSegment(0, 0xBFFFF, 0x9B, 0xC);
#endif
    KiGdt[KGDT_R0_DATA / 8] = MakeSegment(0, 0xFFFFF, 0x93, 0xC);
    KiGdt[KGDT_TSS / 8] = MakeSegment((ULONG)&KiTss, sizeof(KTSS), TSS_AVAILABLE, 0);
    KiGdt[KGDT_R0_PCR / 8] = MakeSegment((ULONG)&KiPcr, 0x27C, 0x93, 0xC);
    KiGdt[KGDT_DF_TSS / 8] = MakeSegment((ULONG)&KiDoubleFaultTss, sizeof(KTSS), TSS_AVAILABLE, 0);
    KiGdt[KGDT_NMI_TSS / 8] = MakeSegment((ULONG)&KiNmiTss, sizeof(KTSS), TSS_AVAILABLE, 0);

    KiTss = {};
    KiTss.Ss0 = KGDT_R0_DATA;
    KiTss.Cr3 = read_cr3();
    KiTss.IoMapBase = sizeof(KTSS);
    InitFaultTss(KiDoubleFaultTss, KiDoubleFaultTask, KiDoubleFaultStack);
    InitFaultTss(KiNmiTss, KiNmiTask, KiNmiStack);

#ifdef XK_HOST_TEST
    return; // host unit tests run in user mode: no segment reload (the TSS fields above are still filled)
#endif
    DescriptorTableRegister gdtr = {sizeof(KiGdt) - 1, (ULONG)KiGdt};
    __asm__ volatile("lgdt %0\n\t"
                     "ljmp $0x08, $1f\n"
                     "1:\n\t"
                     "movw $0x10, %%ax\n\t"
                     "movw %%ax, %%ds\n\t"
                     "movw %%ax, %%es\n\t"
                     "movw %%ax, %%ss\n\t"
                     "movw $0x20, %%ax\n\t"
                     "movw %%ax, %%fs\n\t"
                     "xorw %%ax, %%ax\n\t"
                     "movw %%ax, %%gs\n\t"
                     "movw $0x18, %%ax\n\t"
                     "ltr %%ax"
                     :
                     : "m"(gdtr)
                     : "eax", "memory");
}

static void KiInitializeIdt() {
    for (ULONG v = 0; v < 32; v++)
        KiIdt[v] = MakeInterruptGate(KiTrapStubs[v]);
    KiIdt[2] = MakeTaskGate(KGDT_NMI_TSS);
    KiIdt[8] = MakeTaskGate(KGDT_DF_TSS);
    for (ULONG v = 32; v < 48; v++)
        KiIdt[v] = MakeInterruptGate((PVOID)KiIgnoreInterrupt);
    for (ULONG irq = 0; irq < 16; irq++)
        KiIdt[0x30 + irq] = MakeInterruptGate(KiIrqStubs[irq]);

    DescriptorTableRegister idtr = {sizeof(KiIdt) - 1, (ULONG)KiIdt};
    __asm__ volatile("lidt %0" ::"m"(idtr) : "memory");
}

static void KiInitializePcr() {
    constexpr ULONG BOOT_STACK_SIZE = 0x3000;
    KiPcr = {};
    KiPcr.NtTib.ExceptionList = (PVOID)0xFFFFFFFF;
    KiPcr.NtTib.StackBase = KiBootStack + BOOT_STACK_SIZE - NPX_FRAME_LENGTH;
    KiPcr.NtTib.StackLimit = KiBootStack;
    KiPcr.NtTib.Self = &KiPcr.NtTib;
    KiPcr.SelfPcr = &KiPcr;
    KiPcr.Prcb = &KiPcr.PrcbData;
    KiPcr.Irql = HIGH_LEVEL;
    KiPcr.PrcbData.CurrentThread = &KiIdleThread;
    KiPcr.PrcbData.IdleThread = &KiIdleThread;
}

// The page attribute table as the retail 2BLs leave it (spec/stock/21 §1): PA0 WB, PA1 WC, PA2 UC-, PA3 UC, repeated.
// With it a PTE's PWT bit means write-combined, which is what PAGE_WRITECOMBINE (PTE_PWT) and the 0xF0000000 window
// rely on; the power-on table makes PWT write-through, i.e. cached: the CPU then keeps stale copies of memory the GPU
// writes (fences, push-buffer state) and a title waits for it forever. Set here too so that any 2BL works; the
// change follows Intel's procedure (caches off and flushed around the write, then a TLB flush).
static void KiInitializePat() {
    constexpr ULONGLONG RETAIL_PAT = 0x0007010600070106ULL;
    ULONG a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & (1u << 16)) || rdmsr(0x277) == RETAIL_PAT)
        return;
    ULONG flags = save_flags_cli();
    ULONG cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0\n\twbinvd" ::"r"(cr0 | 0x40000000) : "memory"); // CD
    wrmsr(0x277, RETAIL_PAT);
    __asm__ volatile("wbinvd\n\tmov %%cr3, %%eax\n\tmov %%eax, %%cr3" ::: "eax", "memory");
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    restore_flags(flags);
}

// CPU microcode (spec/stock/01 §2 step 3): the reference loads an update at start-up when cpuid(1) reports 0x686 or
// 0x68A. Ours are Intel's published updates for those steppings on platform 4 (generated into ucode.inc by
// tools/mkucode.py); one is loaded when both the signature and the platform (MSR 0x17 bits 50-52) match. Only at a
// cold boot: a quick reboot does not reset the processor, so the update stays loaded, and the table is boot-only
// memory (spec/oxos-internal/18-boot-memory.md).
#ifndef XK_HOST_TEST
alignas(16) XK_BOOT_DATA static const ULONG KiMicrocode[][512] = {
#include "ucode.inc"
};
#endif
ULONG KiMicrocodeRevision; // after the load (MSR 0x8B high half); 0 = none reported

extern "C" volatile UCHAR KiQuickRebootFlag;
static void KiLoadMicrocode() {
#ifndef XK_HOST_TEST
    ULONG a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    ULONG platform = (ULONG)(rdmsr(0x17) >> 50) & 7;
    for (const auto &update : KiMicrocode) {
        if (KiQuickRebootFlag) // loaded at the cold boot (the table's pages are free memory now)
            break;
        if (update[3] != a || !(update[6] & (1u << platform)))
            continue;
        wrmsr(0x79, (ULONGLONG)(ULONG)&update[12]); // IA32_BIOS_UPDT_TRIG <- the update data, after the 48-byte header
        break;
    }
    wrmsr(0x8B, 0);
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1) : "memory");
    KiMicrocodeRevision = (ULONG)(rdmsr(0x8B) >> 32);
#endif
}

void KiInitializeProcessor() {
    KiLoadMicrocode();
    KiInitializePat();
    // The PCR must be filled before FS is loaded with a descriptor pointing at it.
    KiInitializePcr();
    post(post_code::Pcr);
    KiInitializeGdt();
    post(post_code::Gdt);
    KiInitializeIdt();
    post(post_code::Idt);
}
