// Lazy FPU/SSE context switching (spec/07): each thread's FXSAVE area is the 0x210 bytes below its
// StackBase. CR0.TS is set whenever the running thread doesn't own the FPU, so its first FPU/SSE
// instruction raises #NM and the state is swapped here. Kernel C code is built without x87/SSE.
#include <xk/arch.h>
#include <xk/ke.h>

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
