// The video player's sound (spec/oxos_internal/38 §4): 16-bit stereo at 48 kHz through the MCPX's AC'97 controller, its PCM and S/PDIF outputs played
// together from one ring of 32 buffer descriptors (the controller as spec/oxos_internal/17 §2 describes it). The ring is filled ahead by the caller and
// polled: no interrupt is connected, so nothing of the app is left behind in the kernel when it starts another title (nxdk's XAudio connects one and never
// disconnects it).
#pragma once

#include <stdint.h>

namespace oxos::video_player {

// The most frames one QueueSound takes: one MP2/MP3 frame (1152 samples) of the lowest sample rate (8 kHz) at 48 kHz.
constexpr uint32_t SOUND_MAX_FRAMES = 1152 * 6;
// The descriptors of the controller's ring.
constexpr uint32_t SOUND_DESCRIPTORS = 32;

// One buffer descriptor as the controller reads it.
struct SOUND_DESCRIPTOR {
    uint32_t Address;  // physical address of the samples
    uint16_t Samples;  // 16-bit samples (two a stereo frame)
    uint16_t Control;  // 0: no interrupt
};

// The controller while the player owns it.
struct SOUND_OUTPUT {
    volatile uint8_t *Registers;   // the controller's registers, mapped; null when not started
    SOUND_DESCRIPTOR *Descriptors; // the ring (contiguous memory)
    int16_t *Buffers;              // SOUND_DESCRIPTORS buffers of SOUND_MAX_FRAMES stereo frames (contiguous memory)
    uint32_t Filled;               // descriptors filled since the start
    bool Running;                  // the outputs were started
};

// Maps the controller, cold-resets its codec (unmuting what is muted), resets both outputs and sets up the ring. False
// when the controller or its memory cannot be had (the video then plays without sound).
bool StartSoundOutput(SOUND_OUTPUT &Output);

// The descriptors filled and not yet played to the end.
uint32_t CountQueuedSound(const SOUND_OUTPUT &Output);

// Adds FrameCount stereo frames (at most SOUND_MAX_FRAMES) to the ring; false when the ring is full. The outputs start with the first call after
// PlaySound.
bool QueueSound(SOUND_OUTPUT &Output, const int16_t *Frames, uint32_t FrameCount);

// Starts both outputs (after the first buffers are queued).
void PlaySound(SOUND_OUTPUT &Output);

// Stops both outputs, clears their ring addresses and frees the memory.
void StopSoundOutput(SOUND_OUTPUT &Output);

} // namespace oxos::video_player
