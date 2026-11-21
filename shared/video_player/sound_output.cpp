// The video player's sound (shared/video_player/sound_output.h).
#include "sound_output.h"

#include <string.h>

#include <xboxkrnl/xboxkrnl.h>

namespace oxos::video_player {

namespace {

// The AC'97 controller's registers in memory space (spec/oxos_internal/17 §2): the primary codec's at 0x000, the bus-master channels at 0x100 + 0x10 n in
// the ICH layout (descriptor list base, current index, last valid index, status, control), PCM out at 0x110 and S/PDIF out at 0x170, global control 0x12C,
// global status 0x130, codec access semaphore 0x134.
constexpr ULONG AC97_BASE = 0xFEC00000;
constexpr ULONG AC97_SIZE = 0x200;
constexpr uint32_t PCM_OUT = 0x110;
constexpr uint32_t SPDIF_OUT = 0x170;
constexpr uint32_t BDBAR = 0x0;
constexpr uint32_t CIV = 0x4;
constexpr uint32_t LVI = 0x5;
constexpr uint32_t SR = 0x6;
constexpr uint32_t CR = 0xB;
constexpr uint8_t CR_RUN = 1;
constexpr uint8_t CR_RESET = 2;
constexpr uint16_t SR_HALTED = 1;
constexpr uint32_t GLOB_CNT = 0x12C;
constexpr uint32_t GLOB_STA = 0x130;
constexpr uint32_t CAS = 0x134;
constexpr uint32_t GLOB_CNT_COLD = 2; // the cold reset released
constexpr uint32_t GLOB_CNT_WARM = 4; // a warm reset (wakes a codec whose link is off)
constexpr uint32_t GLOB_STA_CODEC_READY = 0x100;
constexpr uint32_t CODEC_POWER = 0x26;
constexpr uint32_t CODEC_MASTER = 0x02;
constexpr uint32_t CODEC_PCM = 0x18;
// Descriptors queued but not played that the ring may hold: the one playing must not be written over.
constexpr uint32_t MAX_QUEUED = SOUND_DESCRIPTORS - 2;
constexpr uint32_t BUFFER_BYTES = SOUND_MAX_FRAMES * 4;

uint8_t Read8(const SOUND_OUTPUT &Output, uint32_t Register) {
    return Output.Registers[Register];
}

uint16_t Read16(const SOUND_OUTPUT &Output, uint32_t Register) {
    return *reinterpret_cast<volatile uint16_t *>(Output.Registers + Register);
}

uint32_t Read32(const SOUND_OUTPUT &Output, uint32_t Register) {
    return *reinterpret_cast<volatile uint32_t *>(Output.Registers + Register);
}

void Write8(SOUND_OUTPUT &Output, uint32_t Register, uint8_t Value) {
    Output.Registers[Register] = Value;
}

void Write16(SOUND_OUTPUT &Output, uint32_t Register, uint16_t Value) {
    *reinterpret_cast<volatile uint16_t *>(Output.Registers + Register) = Value;
}

void Write32(SOUND_OUTPUT &Output, uint32_t Register, uint32_t Value) {
    *reinterpret_cast<volatile uint32_t *>(Output.Registers + Register) = Value;
}

void WaitMs(uint32_t Ms) {
    LARGE_INTEGER delay;
    delay.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

// A codec register write: waits for the codec access semaphore (reading it claims it), then writes.
void WriteCodec(SOUND_OUTPUT &Output, uint32_t Register, uint16_t Value) {
    for (int i = 0; i < 1000 && (Read8(Output, CAS) & 1); i++) {
        KeStallExecutionProcessor(1);
    }
    Write16(Output, Register, Value);
}

void ResetChannel(SOUND_OUTPUT &Output, uint32_t Channel) {
    Write8(Output, Channel + CR, 0);
    Write8(Output, Channel + CR, CR_RESET);
    for (int i = 0; i < 1000 && (Read8(Output, Channel + CR) & CR_RESET); i++) {
        KeStallExecutionProcessor(1);
    }
    Write16(Output, Channel + SR, 0x1C); // clears the status bits
}

bool IsCodecReady(const SOUND_OUTPUT &Output) {
    return Read32(Output, GLOB_STA) & GLOB_STA_CODEC_READY;
}

bool WaitForCodec(const SOUND_OUTPUT &Output) {
    for (int ms = 0; ms < 500 && !IsCodecReady(Output); ms++) {
        WaitMs(1);
    }
    return IsCodecReady(Output);
}

// The codec as the stock sound library starts it (spec/oxos_internal/46 §3): the cold reset released only while it is still held (a pulse on a running codec
// resets it some milliseconds later, while its ready bit still reads set, and the volumes written meanwhile are lost), a warm reset for a released codec that
// is not ready, then its power and both volumes written, never read.
bool StartCodec(SOUND_OUTPUT &Output) {
    uint32_t control = Read32(Output, GLOB_CNT);
    if (!(control & GLOB_CNT_COLD)) {
        Write32(Output, GLOB_CNT, control | GLOB_CNT_COLD);
    } else if (!IsCodecReady(Output)) {
        Write32(Output, GLOB_CNT, control | GLOB_CNT_WARM);
        KeStallExecutionProcessor(10);
        Write32(Output, GLOB_CNT, control & ~GLOB_CNT_WARM);
    }
    if (!WaitForCodec(Output)) {
        return false;
    }
    WriteCodec(Output, CODEC_POWER, 0x0000);  // every section powered
    WriteCodec(Output, CODEC_MASTER, 0x0000); // 0 dB, not muted
    WriteCodec(Output, CODEC_PCM, 0x0808);    // 0 dB, not muted
    return true;
}

} // namespace

bool StartSoundOutput(SOUND_OUTPUT &Output) {
    memset(&Output, 0, sizeof(Output));
    Output.Registers = static_cast<volatile uint8_t *>(MmMapIoSpace(AC97_BASE, AC97_SIZE, PAGE_READWRITE | PAGE_NOCACHE));
    if (!Output.Registers) {
        return false;
    }
    auto memory = static_cast<uint8_t *>(MmAllocateContiguousMemoryEx(sizeof(SOUND_DESCRIPTOR) * SOUND_DESCRIPTORS + BUFFER_BYTES * SOUND_DESCRIPTORS, 0,
                                                                       0x03FFFFFF, 0, PAGE_READWRITE | PAGE_NOCACHE));
    if (!memory || !StartCodec(Output)) {
        if (memory) {
            MmFreeContiguousMemory(memory);
        }
        MmUnmapIoSpace(const_cast<uint8_t *>(Output.Registers), AC97_SIZE);
        Output.Registers = nullptr;
        return false;
    }
    Output.Descriptors = reinterpret_cast<SOUND_DESCRIPTOR *>(memory);
    Output.Buffers = reinterpret_cast<int16_t *>(memory + sizeof(SOUND_DESCRIPTOR) * SOUND_DESCRIPTORS);
    memset(Output.Descriptors, 0, sizeof(SOUND_DESCRIPTOR) * SOUND_DESCRIPTORS);
    ResetChannel(Output, PCM_OUT);
    ResetChannel(Output, SPDIF_OUT);
    uint32_t ring = MmGetPhysicalAddress(Output.Descriptors);
    Write32(Output, PCM_OUT + BDBAR, ring);
    Write32(Output, SPDIF_OUT + BDBAR, ring);
    return true;
}

uint32_t CountQueuedSound(const SOUND_OUTPUT &Output) {
    if (!Output.Registers || !Output.Filled) {
        return 0;
    }
    if (!Output.Running) {
        return Output.Filled;
    }
    if (Read16(Output, PCM_OUT + SR) & SR_HALTED) {
        return 0;
    }
    uint32_t next = Output.Filled % SOUND_DESCRIPTORS;
    uint32_t playing = Read8(Output, PCM_OUT + CIV) % SOUND_DESCRIPTORS;
    return (next - playing + SOUND_DESCRIPTORS) % SOUND_DESCRIPTORS;
}

bool QueueSound(SOUND_OUTPUT &Output, const int16_t *Frames, uint32_t FrameCount) {
    if (!Output.Registers || !FrameCount || FrameCount > SOUND_MAX_FRAMES || CountQueuedSound(Output) >= MAX_QUEUED) {
        return false;
    }
    uint32_t index = Output.Filled % SOUND_DESCRIPTORS;
    int16_t *buffer = Output.Buffers + (size_t)index * SOUND_MAX_FRAMES * 2;
    memcpy(buffer, Frames, (size_t)FrameCount * 4);
    Output.Descriptors[index].Address = MmGetPhysicalAddress(buffer);
    Output.Descriptors[index].Samples = static_cast<uint16_t>(FrameCount * 2);
    Output.Descriptors[index].Control = 0;
    bool halted = Output.Running && (Read16(Output, PCM_OUT + SR) & SR_HALTED);
    Write8(Output, PCM_OUT + LVI, static_cast<uint8_t>(index));
    Write8(Output, SPDIF_OUT + LVI, static_cast<uint8_t>(index));
    Output.Filled++;
    // After an underrun the outputs have stopped at the last descriptor: the new one starts them again.
    if (halted) {
        Write8(Output, PCM_OUT + CR, CR_RUN);
        Write8(Output, SPDIF_OUT + CR, CR_RUN);
    }
    return true;
}

void PlaySound(SOUND_OUTPUT &Output) {
    if (!Output.Registers || Output.Running) {
        return;
    }
    Write8(Output, PCM_OUT + CR, CR_RUN);
    Write8(Output, SPDIF_OUT + CR, CR_RUN);
    Output.Running = true;
}

void StopSoundOutput(SOUND_OUTPUT &Output) {
    if (!Output.Registers) {
        return;
    }
    ResetChannel(Output, PCM_OUT);
    ResetChannel(Output, SPDIF_OUT);
    Write32(Output, PCM_OUT + BDBAR, 0);
    Write32(Output, SPDIF_OUT + BDBAR, 0);
    MmFreeContiguousMemory(Output.Descriptors);
    MmUnmapIoSpace(const_cast<uint8_t *>(Output.Registers), AC97_SIZE);
    memset(&Output, 0, sizeof(Output));
}

} // namespace oxos::video_player
