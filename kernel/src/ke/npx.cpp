// Lazy FPU/SSE context switching (spec/07): each thread's FXSAVE area is the 0x210 bytes below its
// StackBase. CR0.TS is set whenever the running thread doesn't own the FPU, so its first FPU/SSE
// instruction raises #NM and the state is swapped here. Kernel C code is built without x87/SSE.
#include <xk/arch.h>
#include <xk/ke.h>
#include <xk/mm.h>

#define Prcb (KiPcr.PrcbData)

constexpr ULONG CR0_EM = 1u << 2, CR0_TS = 1u << 3;

static inline ULONG ReadCr0() {
    ULONG v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline void WriteCr0(ULONG v) { __asm__ volatile("mov %0, %%cr0" ::"r"(v) : "memory"); }

static UCHAR *NpxArea(PKTHREAD t) { return (UCHAR *)t->StackBase - NPX_FRAME_LENGTH; }

void KiInitializeNpxArea(PKTHREAD t) {
    UCHAR *area = NpxArea(t);
    for (ULONG i = 0; i < NPX_FRAME_LENGTH; i++)
        area[i] = 0;
    *(USHORT *)area = 0x027F;         // FCW
    *(ULONG *)(area + 0x18) = 0x1F80; // MXCSR
    t->NpxState = NPX_STATE_NOT_LOADED;
}

// Before switching to `next`: trap its first FPU use unless it already owns the FPU.
void KiNpxPrepareSwitch(PKTHREAD next) {
    ULONG cr0 = ReadCr0();
    ULONG want = next == Prcb.NpxThread ? cr0 & ~CR0_TS : cr0 | CR0_TS;
    if (want != cr0)
        WriteCr0(want);
}

// A terminating thread gives up the FPU so its save area can be reused.
void KiNpxRelease(PKTHREAD t) {
    if (Prcb.NpxThread == t) {
        Prcb.NpxThread = nullptr;
        WriteCr0(ReadCr0() | CR0_TS);
    }
}

// #NM handler (interrupt gate: interrupts disabled).
extern "C" void KiNpxNotAvailable() {
    WriteCr0(ReadCr0() & ~(CR0_TS | CR0_EM));
    PKTHREAD current = Prcb.CurrentThread;
    if (current->NpxState == NPX_STATE_LOADED)
        return;
    if (PKTHREAD owner = Prcb.NpxThread) {
        __asm__ volatile("fxsave %0" : "=m"(*(UCHAR(*)[512])NpxArea(owner)));
        owner->NpxState = NPX_STATE_NOT_LOADED;
    }
    __asm__ volatile("fxrstor %0" ::"m"(*(UCHAR(*)[512])NpxArea(current)));
    current->NpxState = NPX_STATE_LOADED;
    Prcb.NpxThread = current;
}

void KiInitializeNpx() { WriteCr0(ReadCr0() | CR0_TS); }

// Kernel code that wants the FPU (DPCs, drivers) brackets it with these. The caller's 0x18-byte block holds
// {flags, ..., +8 save buffer, +0x14 thread}; the FPU state is kept in an FXSAVE buffer from pool.
struct KFLOATING_SAVE_ {
    UCHAR Flags, Irql, Reserved[6];
    PVOID Buffer;
    ULONG Reserved2[2];
    PKTHREAD Thread;
};
static_assert(sizeof(KFLOATING_SAVE_) == 0x18);

XBAPI NTSTATUS NTAPI KeSaveFloatingPointState(KFLOATING_SAVE_ *FloatSave) {
    auto buf = (UCHAR *)ExAllocatePoolWithTag(NPX_FRAME_LENGTH + 16, ' XPN');
    if (!buf)
        return (NTSTATUS)0xC000009A;
    FloatSave->Buffer = buf;
    FloatSave->Thread = KeGetCurrentThread();
    FloatSave->Flags = 1;
    auto area = (UCHAR *)(((ULONG)buf + 15) & ~15u);
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    WriteCr0(ReadCr0() & ~CR0_TS);
    PKTHREAD current = Prcb.CurrentThread;
    if (Prcb.NpxThread && Prcb.NpxThread != current) { // put the owner's state back where it belongs
        __asm__ volatile("fxsave %0" : "=m"(*(UCHAR(*)[512])NpxArea(Prcb.NpxThread)));
        Prcb.NpxThread->NpxState = NPX_STATE_NOT_LOADED;
        Prcb.NpxThread = nullptr;
    }
    if (Prcb.NpxThread == current)
        __asm__ volatile("fxsave %0" : "=m"(*(UCHAR(*)[512])area));
    else
        for (ULONG i = 0; i < 512; i++)
            area[i] = NpxArea(current)[i];
    // Fresh state for the caller: the current thread now owns the FPU with defaults.
    __asm__ volatile("fninit");
    ULONG mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
    current->NpxState = NPX_STATE_LOADED;
    Prcb.NpxThread = current;
    __builtin_ia32_writeeflags_u32(flags);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI KeRestoreFloatingPointState(KFLOATING_SAVE_ *FloatSave) {
    auto buf = (UCHAR *)FloatSave->Buffer;
    if (!buf)
        return STATUS_SUCCESS;
    auto area = (UCHAR *)(((ULONG)buf + 15) & ~15u);
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    WriteCr0(ReadCr0() & ~CR0_TS);
    PKTHREAD current = Prcb.CurrentThread;
    __asm__ volatile("fxrstor %0" ::"m"(*(UCHAR(*)[512])area));
    current->NpxState = NPX_STATE_LOADED;
    Prcb.NpxThread = current;
    __builtin_ia32_writeeflags_u32(flags);
    ExFreePool(buf);
    FloatSave->Buffer = nullptr;
    return STATUS_SUCCESS;
}
