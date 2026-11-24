// Video (src/hardware/video/): the AV pack, the video encoders (Conexant, Focus, Xcalibur) and display modes, and the boot display (video.cpp; spec/stock/12
// §4, spec/stock/17-20, spec/oxos_internal/02); the NV2A graphics engine's set-up at a cold boot (nv2a.cpp, spec/oxos_internal/24) with its default channel
// context (gpu_context.h).
#pragma once
#include <core/core.h>
#include <runtime/types.h>

// The AV exports (video.cpp) and the SMC's AV pack code read at boot (7 = none).
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch, ULONG FrameBuffer);
XBAPI void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
XBAPI UCHAR HalBootSMCVideoMode;

namespace xk::hardware::video {

// The encoder and the boot display (video.cpp).
void InitVideo();
ULONG BootModeWord();
void PrepareBootDisplay();
void LoadBootGamma();
void VideoTablesInUse(ULONG_PTR *Start, ULONG_PTR *End);
void SmcSetOutputs(bool Enable);
// A frame buffer on screen that its owner has handed over (the boot splash's, kept for the boot video player): freed by the next AvSetDisplayMode that
// scans out another one.
void KeepScreenUntilNextMode(PVOID Screen);
// Before each title: moves the saved display (a dashboard's persisted last picture) below 16 MB, out of the way of the title's allocations.
void MoveSavedDisplay();
extern volatile UCHAR EncoderAddress;
extern volatile ULONG ScreenWidth;       // the frame's width as the CRTC scans it out (CR01), from the last mode set; 0 before (video.cpp)
extern volatile bool ScreenInterlaced;   // the CRTC scans out fields (CR39 not 0xFF: 1080i), from the last mode set (video.cpp)
extern volatile ULONG EncoderProgrammed; // diagnostics: the last mode word whose encoder registers were written
extern volatile ULONG EncoderMismatches; // diagnostics: registers that read back differently after that mode set
extern volatile ULONG XcShadow[0x100];   // the Xcalibur's registers as last written

// The NV2A graphics engine (nv2a.cpp): cold boot, after the GPU reset.
void InitGraphicsEngine();

} // namespace xk::hardware::video
