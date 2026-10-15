// Processor bring-up: GDT, TSSs, IDT and the processor control region (spec/01-boot.md §2).
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
constexpr ULONG FAULT_STACK_SIZE = 0x1000;

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
    KiGdt[KGDT_R0_CODE / 8] = MakeSegment(0, 0xBFFFF, 0x9B, 0xC);
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

void KiInitializeProcessor() {
    // The PCR must be filled before FS is loaded with a descriptor pointing at it.
    KiInitializePcr();
    post(post_code::Pcr);
    KiInitializeGdt();
    post(post_code::Gdt);
    KiInitializeIdt();
    post(post_code::Idt);
}
