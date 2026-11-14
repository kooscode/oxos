// The MCPX's AC'97 controller (spec/oxos_internal/17_boot_jingle.md §2): its codec started, and its PCM and S/PDIF
// outputs played together from one list of buffer descriptors. Boot-only, except StopNow.
#include <hardware/audio/audio.h>
#include <hardware/chipset/serial.h>
#include <core/core.h>
#include <memory/memory.h>

namespace xk::hardware::audio {

#pragma code_seg(".xkb")

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

volatile UCHAR *Ac97; // the controller's registers, while mapped

UCHAR Reg8(ULONG r) {
    return Ac97[r];
}

USHORT Reg16(ULONG r) {
    return *(volatile USHORT *)(Ac97 + r);
}

ULONG Reg32(ULONG r) {
    return *(volatile ULONG *)(Ac97 + r);
}

void Set8(ULONG r, UCHAR v) {
    Ac97[r] = v;
}

void Set16(ULONG r, USHORT v) {
    *(volatile USHORT *)(Ac97 + r) = v;
}

void Set32(ULONG r, ULONG v) {
    *(volatile ULONG *)(Ac97 + r) = v;
}

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

} // namespace

bool StartCodec() {
    Set32(GLOB_CNT, Reg32(GLOB_CNT) & ~GLOB_CNT_COLD); // cold reset
    KeStallExecutionProcessor(10);
    Set32(GLOB_CNT, Reg32(GLOB_CNT) | GLOB_CNT_COLD);
    ULONG ms = 0;
    for (; ms < 500 && !(Reg32(GLOB_STA) & GLOB_STA_CODEC_READY); ms++)
        Sleep(1);
    if (!(Reg32(GLOB_STA) & GLOB_STA_CODEC_READY)) {
        XK_SERIAL("audio: no codec (global status %#lx)\n", Reg32(GLOB_STA));
        return false;
    }
    USHORT master = Reg16(CODEC_MASTER), pcm = Reg16(CODEC_PCM);
    XK_SERIAL("audio: codec ready after %lu ms, master %#x pcm %#x\n", ms, master, pcm);
    // Unmute only what is muted: 0 dB master, 0 dB PCM.
    if (master & VOLUME_MUTE)
        CodecWrite(CODEC_MASTER, 0x0000);
    if (pcm & VOLUME_MUTE)
        CodecWrite(CODEC_PCM, 0x0808);
    ResetChannel(PCM_OUT);
    ResetChannel(SPDIF_OUT);
    return true;
}

bool MapController() {
    Ac97 = (volatile UCHAR *)MmMapIoSpace(AC97_BASE, AC97_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    return Ac97 != nullptr;
}

void UnmapController() {
    if (Ac97)
        MmUnmapIoSpace((PVOID)Ac97, AC97_SIZE);
    Ac97 = nullptr;
}

void SetDescriptors(ULONG ListPhysical, ULONG Count) {
    Set32(PCM_OUT + BDBAR, ListPhysical);
    Set32(SPDIF_OUT + BDBAR, ListPhysical);
    Set8(PCM_OUT + LVI, (UCHAR)(Count - 1));
    Set8(SPDIF_OUT + LVI, (UCHAR)(Count - 1));
}

void StartOutputs() {
    Set8(PCM_OUT + CR, CR_RUN);
    Set8(SPDIF_OUT + CR, CR_RUN);
}

UCHAR CurrentDescriptor() {
    return Reg8(PCM_OUT + CIV);
}

void StopOutputs() {
    ResetChannel(PCM_OUT);
    ResetChannel(SPDIF_OUT);
    Set32(PCM_OUT + BDBAR, 0);
    Set32(SPDIF_OUT + BDBAR, 0);
}

#pragma code_seg()

void StopNow() {
    if (Ac97) {
        Ac97[PCM_OUT + CR] = 0;
        Ac97[SPDIF_OUT + CR] = 0;
    }
}

} // namespace xk::hardware::audio
