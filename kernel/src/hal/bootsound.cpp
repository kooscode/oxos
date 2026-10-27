// The boot jingle's player (spec/oxos-internal/17-boot-jingle.md): on cold boot a system thread synthesizes the
// jingle (hal/jingle.cpp) into a buffer and plays it through the MCPX AC'97 controller's PCM and S/PDIF outputs,
// starting with the splash's fade-in. The APU is not used. The title launcher waits for the jingle to end, and the
// controller is stopped and its memory freed before the title starts.
#include <xk/serial.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>

#include "jingle.h"

// Boot-only code (cold boot, with the splash): freed before the first title starts (spec/oxos-internal/18).
#pragma code_seg(".xkb")

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);

// For the boot tests: 1 synthesizing, 2 playing, 3 played to the end, 5 played with gaps (synthesis fell behind);
// 0x10 no codec, 0x11 no memory. The controller's descriptor at the end, and the jingle's last one.
volatile ULONG HalpBootSoundStatus, HalpBootSoundPosition, HalpBootSoundLast;
// Descriptors synthesized before playback started, the synthesis time (ms), and the times the controller ran out
// of samples.
volatile ULONG HalpBootSoundAhead, HalpBootSoundSynthMs, HalpBootSoundUnderruns;
volatile ULONG HalpBootSoundBuffer, HalpBootSoundBytes; // the samples' physical address and size, while playing

namespace {

// The AC'97 controller's registers in memory space (PCI BAR2): the primary codec's registers at 0x000, the
// bus-master channels at 0x100 + 0x10 n in the ICH layout (descriptor list base, current index, last valid index,
// status, position, prefetch index, control), PCM out at 0x110 and S/PDIF out at 0x170, global control 0x12C and
// global status 0x130.
constexpr ULONG AC97_BASE = 0xFEC00000, AC97_SIZE = 0x200;
constexpr ULONG PCM_OUT = 0x110, SPDIF_OUT = 0x170;
constexpr ULONG BDBAR = 0x0, CIV = 0x4, LVI = 0x5, SR = 0x6, CR = 0xB;
constexpr UCHAR CR_RUN = 1, CR_RESET = 2;
constexpr ULONG GLOB_CNT = 0x12C, GLOB_STA = 0x130, CAS = 0x134;
constexpr ULONG GLOB_CNT_COLD = 2, GLOB_STA_CODEC_READY = 0x100;
constexpr USHORT CODEC_MASTER = 0x02, CODEC_PCM = 0x18, VOLUME_MUTE = 0x8000;

struct Descriptor { // a buffer descriptor: physical address, 16-bit sample count, flags
    ULONG Address;
    USHORT Samples;
    USHORT Flags;
};
constexpr ULONG DESCRIPTORS = 32, FRAMES_PER_DESCRIPTOR = 10800; // 32 x 0.225 s (32: the controller's limit)
static_assert(DESCRIPTORS * FRAMES_PER_DESCRIPTOR >= 7000 * 48, "the descriptor list holds the jingle and some silence");
static_assert(2 * FRAMES_PER_DESCRIPTOR <= 0xFFFF, "a descriptor's sample count is 16 bits");
constexpr ULONG SLICE_FRAMES = 2400, LEAD_MS = 600;

volatile UCHAR *Ac97;
KEVENT BootSoundGo, BootSoundDone;
PKTHREAD BootSoundThreadObject;

UCHAR Reg8(ULONG r) { return Ac97[r]; }
USHORT Reg16(ULONG r) { return *(volatile USHORT *)(Ac97 + r); }
ULONG Reg32(ULONG r) { return *(volatile ULONG *)(Ac97 + r); }
void Set8(ULONG r, UCHAR v) { Ac97[r] = v; }
void Set16(ULONG r, USHORT v) { *(volatile USHORT *)(Ac97 + r) = v; }
void Set32(ULONG r, ULONG v) { *(volatile ULONG *)(Ac97 + r) = v; }

void Sleep(ULONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

// A codec register write: wait for the codec access semaphore (reading it claims it), then write.
void CodecWrite(USHORT reg, USHORT v) {
    for (ULONG i = 0; i < 1000 && (Reg8(CAS) & 1); i++)
        KeStallExecutionProcessor(1);
    Set16(reg, v);
}

void ResetChannel(ULONG ch) {
    Set8(ch + CR, 0);
    Set8(ch + CR, CR_RESET);
    for (ULONG i = 0; i < 1000 && (Reg8(ch + CR) & CR_RESET); i++)
        KeStallExecutionProcessor(1);
    Set16(ch + SR, 0x1C); // clear the status bits
}

// The codec out of reset and both output channels idle; false when no codec answers.
bool StartCodec() {
    Set32(GLOB_CNT, Reg32(GLOB_CNT) & ~GLOB_CNT_COLD); // cold reset
    KeStallExecutionProcessor(10);
    Set32(GLOB_CNT, Reg32(GLOB_CNT) | GLOB_CNT_COLD);
    ULONG ms = 0;
    for (; ms < 500 && !(Reg32(GLOB_STA) & GLOB_STA_CODEC_READY); ms++)
        Sleep(1);
    if (!(Reg32(GLOB_STA) & GLOB_STA_CODEC_READY)) {
        XK_SERIAL("bootsound: no codec (global status %#lx)\n", Reg32(GLOB_STA));
        return false;
    }
    USHORT master = Reg16(CODEC_MASTER), pcm = Reg16(CODEC_PCM);
    XK_SERIAL("bootsound: codec ready after %lu ms, master %#x pcm %#x\n", ms, master, pcm);
    // Unmute only what is muted: 0 dB master, 0 dB PCM.
    if (master & VOLUME_MUTE)
        CodecWrite(CODEC_MASTER, 0x0000);
    if (pcm & VOLUME_MUTE)
        CodecWrite(CODEC_PCM, 0x0808);
    ResetChannel(PCM_OUT);
    ResetChannel(SPDIF_OUT);
    return true;
}

void NTAPI BootSoundThread(PVOID) {
    const ULONG frames = jingle::Frames(), stateSize = jingle::StateSize();
    const ULONG pcmBytes = DESCRIPTORS * FRAMES_PER_DESCRIPTOR * 4;
    // Low memory, as the splash: titles assume the top of RAM is theirs.
    auto pcm = (SHORT *)MmAllocateContiguousMemoryEx(pcmBytes + DESCRIPTORS * sizeof(Descriptor), 0, 0xFFFFFF, 0,
                                                     PAGE_READWRITE | PAGE_WRITECOMBINE);
    auto state = (jingle::State *)MmAllocateContiguousMemoryEx(stateSize, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    Ac97 = (volatile UCHAR *)MmMapIoSpace(AC97_BASE, AC97_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    HalpBootSoundStatus = 1;
    if (!pcm || !state || !Ac97 || !StartCodec()) {
        HalpBootSoundStatus = !pcm || !state || !Ac97 ? 0x11 : 0x10;
        XK_SERIAL("bootsound: not played\n");
    } else {
        // Every descriptor is valid from the start, over a buffer of silence that synthesis fills in ahead of the
        // controller. The MCPX does not stop at the last valid index (it carries on around the list), so the
        // player keeps time itself: 48 frames a millisecond from the start.
        for (ULONG i = 0; i < pcmBytes / 4; i++)
            ((ULONG *)pcm)[i] = 0;
        auto list = (Descriptor *)((UCHAR *)pcm + pcmBytes);
        for (ULONG i = 0; i < DESCRIPTORS; i++) {
            list[i].Address = MmGetPhysicalAddress(pcm + i * FRAMES_PER_DESCRIPTOR * 2);
            list[i].Samples = (USHORT)(FRAMES_PER_DESCRIPTOR * 2);
            list[i].Flags = i + 1 == DESCRIPTORS ? 0x4000 : 0; // no interrupts; the last: silence after it
        }
        ULONG listPhys = MmGetPhysicalAddress(list);
        Set32(PCM_OUT + BDBAR, listPhys);
        Set32(SPDIF_OUT + BDBAR, listPhys);
        Set8(PCM_OUT + LVI, DESCRIPTORS - 1);
        Set8(SPDIF_OUT + LVI, DESCRIPTORS - 1);

        ULONGLONG t0 = KeQueryInterruptTime();
        jingle::Init(state);
        ULONG rendered = 0;
        auto renderSlice = [&] { // 50 ms of sound: short enough not to hold up the splash's fade
            ULONG n = frames - rendered < SLICE_FRAMES ? frames - rendered : SLICE_FRAMES;
            jingle::Render(state, pcm + rendered * 2, n);
            rendered += n;
        };
        // Until the splash's fade-in starts (at most 15 s): synthesize in the time the splash leaves free (a lower
        // priority than its thread, so its picture comes up as soon as it would without the jingle).
        LARGE_INTEGER zero, limit;
        zero.QuadPart = 0;
        limit.QuadPart = -10000LL * 15000;
        while (rendered < frames &&
               KeWaitForSingleObject(&BootSoundGo, Executive, KernelMode, FALSE, &zero) != STATUS_SUCCESS)
            renderSlice();
        if (rendered == frames)
            KeWaitForSingleObject(&BootSoundGo, Executive, KernelMode, FALSE, &limit);
        // Playing: above the splash, keeping LEAD_MS ahead of the controller.
        KeSetPriorityThread(KeGetCurrentThread(), 9);
        if (!rendered)
            renderSlice();
        HalpBootSoundAhead = rendered / 48;
        Set8(PCM_OUT + CR, CR_RUN);
        Set8(SPDIF_OUT + CR, CR_RUN);
        ULONGLONG start = KeQueryInterruptTime();
        auto playedMs = [&] { return (ULONG)((KeQueryInterruptTime() - start) / 10000); };
        HalpBootSoundStatus = 2;
        XK_SERIAL("bootsound: playing (%lu ms synthesized)\n", rendered / 48);
        ULONG underruns = 0;
        while (rendered < frames) {
            ULONG played = playedMs() * 48;
            if (rendered < played)
                underruns++;
            if (rendered < played + LEAD_MS * 48)
                renderSlice();
            else
                Sleep(10);
        }
        HalpBootSoundUnderruns = underruns;
        HalpBootSoundBuffer = MmGetPhysicalAddress(pcm);
        HalpBootSoundBytes = frames * 4;
        HalpBootSoundSynthMs = (ULONG)((KeQueryInterruptTime() - t0) / 10000) | 0x80000000; // never 0 once done
        // The rest of the jingle plays out.
        ULONG lengthMs = frames / 48 + 40;
        while (playedMs() < lengthMs)
            Sleep(10);
        HalpBootSoundPosition = Reg8(PCM_OUT + CIV);
        HalpBootSoundLast = (frames - 1) / FRAMES_PER_DESCRIPTOR;
        HalpBootSoundStatus = underruns ? 5 : 3;
        XK_SERIAL("bootsound: done (synthesis %lu ms, %lu ms ahead at the start, %lu underruns, buffer %u, the "
                  "jingle's last %lu)\n",
                  HalpBootSoundSynthMs & 0x7FFFFFFF, HalpBootSoundAhead, underruns, Reg8(PCM_OUT + CIV),
                  HalpBootSoundLast);
        HalpBootSoundBuffer = 0;
        ResetChannel(PCM_OUT);
        ResetChannel(SPDIF_OUT);
        Set32(PCM_OUT + BDBAR, 0);
        Set32(SPDIF_OUT + BDBAR, 0);
    }
    if (Ac97)
        MmUnmapIoSpace((PVOID)Ac97, AC97_SIZE);
    Ac97 = nullptr;
    if (state)
        MmFreeContiguousMemory(state);
    if (pcm)
        MmFreeContiguousMemory(pcm);
    KeSetEvent(&BootSoundDone, 0, FALSE);
}

} // namespace

volatile ULONG HalpBootSoundOn = 1; // boot tests may clear it (0: no jingle)

// Cold boot, with the splash: start synthesizing.
void HalpStartBootSound() {
    KeInitializeEvent(&BootSoundGo, NotificationEvent, FALSE);
    KeInitializeEvent(&BootSoundDone, NotificationEvent, TRUE);
    if (!HalpBootSoundOn)
        return;
    KeResetEvent(&BootSoundDone);
    BootSoundThreadObject = KiCreateSystemThread(BootSoundThread, nullptr, 0, 7); // below the splash
    if (!BootSoundThreadObject)
        KeSetEvent(&BootSoundDone, 0, FALSE);
}

// The splash's fade-in begins: play.
void HalpBootSoundGo() { KeSetEvent(&BootSoundGo, 0, FALSE); }

// Before the title starts: the jingle has ended, the controller is stopped and the memory freed.
void HalpEndBootSound() {
    KeSetEvent(&BootSoundGo, 0, FALSE); // no splash fade: play now (or finish)
    KeWaitForSingleObject(&BootSoundDone, Executive, KernelMode, FALSE, nullptr);
    if (BootSoundThreadObject) {
        KeWaitForSingleObject(BootSoundThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(BootSoundThreadObject);
        BootSoundThreadObject = nullptr;
    }
}
