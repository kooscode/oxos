// Audio (src/hardware/audio/): the MCPX's AC'97 controller and its codec (ac97.cpp), played through the PCM and
// S/PDIF outputs together from one list of buffer descriptors. The APU is not used. Boot-only, for the boot jingle
// (boot_splash/bootsound.cpp), except StopNow.
#pragma once
#include <runtime/types.h>

// A buffer descriptor: a buffer's physical address, its 16-bit sample count, and flags.
struct AUDIO_BUFFER_DESCRIPTOR {
    ULONG Address;
    USHORT Samples;
    USHORT Flags;
};
constexpr ULONG AUDIO_DESCRIPTORS_MAX = 32;               // the controller's limit
constexpr USHORT AUDIO_DESCRIPTOR_SILENCE_AFTER = 0x4000; // no interrupts; silence after this buffer

namespace xk::hardware::audio {

bool MapController();   // the controller's registers mapped; false without address space
void UnmapController(); // nothing when they are not mapped
// The codec out of reset, master and PCM unmuted, both outputs idle; false when no codec answers.
bool StartCodec();
// Both outputs take the list at ListPhysical (Count descriptors, every one valid); then StartOutputs runs them.
void SetDescriptors(ULONG ListPhysical, ULONG Count);
void StartOutputs();
UCHAR CurrentDescriptor(); // the PCM output's current descriptor
void StopOutputs();        // both outputs reset and their lists cleared

// Resident: both outputs stopped at once (a fatal error on the idle thread, where the player no longer runs); left
// running, the controller replays its buffers in a loop.
void StopNow();

} // namespace xk::hardware::audio
