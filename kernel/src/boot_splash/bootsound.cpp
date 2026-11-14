// The boot jingle's player (spec/oxos_internal/17_boot_jingle.md): on cold boot a system thread synthesizes the jingle (jingle.cpp) into a buffer and plays it
// through the AC'97 controller (hardware/audio/), starting with the splash's fade-in. The title launcher waits for the jingle to end, and the controller is
// stopped and its memory freed before the title starts.
#include <boot_splash/boot_splash.h>
#include <diagnostics/diagnostics.h>
#include <core/core.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <hardware/chipset/chipset.h>
#include <hardware/audio/audio.h>

#include <boot_splash/jingle.h>

namespace xk::boot_splash {

// Boot-only code (cold boot, with the splash): freed before the first title starts (spec/oxos_internal/18).
#pragma code_seg(".xkb")

// For the boot tests: 1 synthesizing, 2 playing, 3 played to the end, 5 played with gaps (synthesis fell behind); 0x10 no codec, 0x11 no memory. The
// controller's descriptor at the end, and the jingle's last one.
volatile ULONG BootSoundStatus, BootSoundPosition, BootSoundLast;
// Descriptors synthesized before playback started, the synthesis time (ms), and the times the controller ran out of samples.
volatile ULONG BootSoundAhead, BootSoundSynthMs, BootSoundUnderruns;
volatile ULONG BootSoundBuffer, BootSoundBytes; // the samples' physical address and size, while playing
volatile ULONG BootSoundOn = 1;                 // boot tests may clear it (0: no jingle)

namespace {

constexpr ULONG DESCRIPTORS = AUDIO_DESCRIPTORS_MAX, FRAMES_PER_DESCRIPTOR = 10800; // 32 x 0.225 s
static_assert(DESCRIPTORS * FRAMES_PER_DESCRIPTOR >= 7000 * 48, "the descriptor list holds the jingle and some silence");
static_assert(2 * FRAMES_PER_DESCRIPTOR <= 0xFFFF, "a descriptor's sample count is 16 bits");
// Synthesis runs in slices of 50 ms (48 frames a millisecond), and while playing stays 600 ms ahead of the controller.
constexpr ULONG SLICE_FRAMES = 2400, LEAD_MS = 600;

KEVENT BootSoundGo, BootSoundDone; // the splash's fade-in has begun; the thread has finished
PKTHREAD BootSoundThreadObject;    // waited for before the title starts, so its stack is freed by then

void Sleep(ULONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

void NTAPI BootSoundThread(PVOID) {
    const ULONG frames = jingle::Frames(), stateSize = jingle::StateSize();
    const ULONG pcmBytes = DESCRIPTORS * FRAMES_PER_DESCRIPTOR * 4;
    // Low memory, as the splash: titles assume the top of RAM is theirs.
    auto pcm = (SHORT *)MmAllocateContiguousMemoryEx(pcmBytes + DESCRIPTORS * sizeof(AUDIO_BUFFER_DESCRIPTOR), 0, 0xFFFFFF, 0,
                                                     PAGE_READWRITE | PAGE_WRITECOMBINE);
    auto state = (jingle::State *)MmAllocateContiguousMemoryEx(stateSize, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    bool mapped = hardware::audio::MapController();
    BootSoundStatus = 1;
    if (!pcm || !state || !mapped || !hardware::audio::StartCodec()) {
        BootSoundStatus = !pcm || !state || !mapped ? 0x11 : 0x10;
        XK_SERIAL("bootsound: not played\n");
    } else {
        // Every descriptor is valid from the start, over a buffer of silence that synthesis fills in ahead of the controller. The MCPX does not stop at the
        // last valid index (it carries on around the list), so the player keeps time itself: 48 frames a millisecond from the start.
        for (ULONG i = 0; i < pcmBytes / 4; i++)
            ((ULONG *)pcm)[i] = 0;
        auto list = (AUDIO_BUFFER_DESCRIPTOR *)((UCHAR *)pcm + pcmBytes);
        for (ULONG i = 0; i < DESCRIPTORS; i++) {
            list[i].Address = MmGetPhysicalAddress(pcm + i * FRAMES_PER_DESCRIPTOR * 2);
            list[i].Samples = (USHORT)(FRAMES_PER_DESCRIPTOR * 2);
            list[i].Flags = i + 1 == DESCRIPTORS ? AUDIO_DESCRIPTOR_SILENCE_AFTER : 0;
        }
        hardware::audio::SetDescriptors(MmGetPhysicalAddress(list), DESCRIPTORS);

        ULONGLONG t0 = KeQueryInterruptTime();
        jingle::Init(state);
        ULONG rendered = 0;
        auto renderSlice = [&] { // 50 ms of sound: short enough not to hold up the splash's fade
            ULONG n = frames - rendered < SLICE_FRAMES ? frames - rendered : SLICE_FRAMES;
            jingle::Render(state, pcm + rendered * 2, n);
            rendered += n;
        };
        // Until the splash's fade-in starts (at most 15 s): synthesize in the time the splash leaves free (a lower priority than its thread, so its picture
        // comes up as soon as it would without the jingle).
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
        BootSoundAhead = rendered / 48;
        hardware::audio::StartOutputs();
        ULONGLONG start = KeQueryInterruptTime();
        auto playedMs = [&] { return (ULONG)((KeQueryInterruptTime() - start) / 10000); };
        BootSoundStatus = 2;
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
        BootSoundUnderruns = underruns;
        BootSoundBuffer = MmGetPhysicalAddress(pcm);
        BootSoundBytes = frames * 4;
        BootSoundSynthMs = (ULONG)((KeQueryInterruptTime() - t0) / 10000) | 0x80000000; // never 0 once done
        // The rest of the jingle plays out.
        ULONG lengthMs = frames / 48 + 40;
        while (playedMs() < lengthMs)
            Sleep(10);
        BootSoundPosition = hardware::audio::CurrentDescriptor();
        BootSoundLast = (frames - 1) / FRAMES_PER_DESCRIPTOR;
        BootSoundStatus = underruns ? 5 : 3;
        XK_SERIAL("bootsound: done (synthesis %lu ms, %lu ms ahead at the start, %lu underruns, buffer %u, the " "jingle's last %lu)\n",
                  BootSoundSynthMs & 0x7FFFFFFF, BootSoundAhead, underruns, hardware::audio::CurrentDescriptor(), BootSoundLast);
        BootSoundBuffer = 0;
        hardware::audio::StopOutputs();
    }
    hardware::audio::UnmapController();
    if (state)
        MmFreeContiguousMemory(state);
    if (pcm)
        MmFreeContiguousMemory(pcm);
    KeSetEvent(&BootSoundDone, 0, FALSE);
}

} // namespace

// Cold boot, with the splash: start synthesizing.
void StartBootSound() {
    KeInitializeEvent(&BootSoundGo, NotificationEvent, FALSE);
    KeInitializeEvent(&BootSoundDone, NotificationEvent, TRUE);
    if (!BootSoundOn)
        return;
    KeResetEvent(&BootSoundDone);
    BootSoundThreadObject = core::CreateSystemThread(BootSoundThread, nullptr, 0, 7); // below the splash
    if (!BootSoundThreadObject)
        KeSetEvent(&BootSoundDone, 0, FALSE);
}

// The splash's fade-in begins: play.
void PlayBootSound() {
    KeSetEvent(&BootSoundGo, 0, FALSE);
}

// Before the title starts: the jingle has ended, the controller is stopped and the memory freed.
void EndBootSound() {
    KeSetEvent(&BootSoundGo, 0, FALSE); // no splash fade: play now (or finish)
    KeWaitForSingleObject(&BootSoundDone, Executive, KernelMode, FALSE, nullptr);
    if (BootSoundThreadObject) {
        KeWaitForSingleObject(BootSoundThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(BootSoundThreadObject);
        BootSoundThreadObject = nullptr;
    }
}

// Resident: a fatal error on the idle thread (user_interface/error_screens.cpp) stops the sound at once, as the jingle's thread no longer runs to finish it;
// left running, the controller replays its buffers in a loop.
#pragma code_seg()

void StopBootSound() {
    hardware::audio::StopNow();
}

} // namespace xk::boot_splash
