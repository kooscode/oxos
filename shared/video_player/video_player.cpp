// The video player (shared/video_player/video_player.h).
#include "video_player.h"
#include "mpeg_program_stream.h"
#include "sound_output.h"

#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

#include <vector>

#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

extern "C" {
#include <mpeg2.h>
#include <mpeg2convert.h>
}

// minimp3's SIMD code needs SSE2, which the console's Pentium III does not have (clang's Windows target looks like MSVC, which minimp3 takes for SSE2).
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

namespace oxos::video_player {

namespace {

// --- Time

// The performance counter's ticks per millisecond.
double TicksPerMs;

double ReadMs() {
    if (TicksPerMs == 0) {
        TicksPerMs = static_cast<double>(KeQueryPerformanceFrequency()) / 1000.0;
    }
    return static_cast<double>(KeQueryPerformanceCounter()) / TicksPerMs;
}

// --- The screen
// The NV2A's raster position (PCRTC, 0xFD600808): the scanline it is sending; it falls back to 0 when a new frame starts.
constexpr uintptr_t CRTC_RASTER = 0xFD600808;
constexpr uint32_t RASTER_LINE_MASK = 0x7FF;
// The longest wait for a new frame (60 Hz: 16.7 ms), and the number of waits that may see no new frame before the raster is taken as not readable.
constexpr double FRAME_START_TIMEOUT_MS = 20;
constexpr int RASTER_TIMEOUTS_ALLOWED = 3;

// The frame buffer (XVideoSetMode), SCREEN_WIDTH x SCREEN_HEIGHT pixels of 32 bits; null until the screen is set up.
uint32_t *FrameBuffer;
// When the screen was switched on (ReadMs).
double ScreenOnMs;
// Waits for a new screen frame that saw none; at RASTER_TIMEOUTS_ALLOWED the frames are shown without waiting (xemu's raster does not move).
int RasterTimeouts;

// Where the screen's pixels come from in a decoded picture: its middle 4:3, scaled by picking the nearest pixel.
struct SCREEN_MAP {
    int Width = 0;                  // the picture's size
    int Height = 0;
    bool Direct = false;            // the middle 4:3 is 640x480: rows are copied as they are
    int Left = 0;                   // Direct: the first column copied
    std::vector<int> Columns;       // the picture's column for each screen column
    std::vector<int> Rows;          // the picture's row for each screen row
};

void SetUpScreenMap(SCREEN_MAP &Map, int Width, int Height) {
    Map.Width = Width;
    Map.Height = Height;
    int cropWidth = Width;
    int cropHeight = Height;
    if (Width * 3 > Height * 4) {
        cropWidth = Height * 4 / 3;
    } else {
        cropHeight = Width * 3 / 4;
    }
    int left = (Width - cropWidth) / 2;
    int top = (Height - cropHeight) / 2;
    Map.Direct = cropWidth == SCREEN_WIDTH && cropHeight == SCREEN_HEIGHT && (left % 4) == 0;
    Map.Left = left;
    Map.Columns.resize(SCREEN_WIDTH);
    Map.Rows.resize(SCREEN_HEIGHT);
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        Map.Columns[x] = left + x * cropWidth / SCREEN_WIDTH;
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        Map.Rows[y] = top + y * cropHeight / SCREEN_HEIGHT;
    }
}

// Copies Pixels 32-bit pixels (a multiple of 4) into the frame buffer with streaming stores, which write whole lines to the uncached frame buffer at once.
void CopyRow(uint32_t *To, const uint32_t *From, int Pixels) {
    for (int i = 0; i < Pixels; i += 4) {
        _mm_stream_ps(reinterpret_cast<float *>(To + i), _mm_loadu_ps(reinterpret_cast<const float *>(From + i)));
    }
}

void ClearScreen() {
    const __m128 black = _mm_setzero_ps();
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i += 4) {
        _mm_stream_ps(reinterpret_cast<float *>(FrameBuffer + i), black);
    }
    _mm_sfence();
}

// Waits until the screen starts a new frame (the raster falls back), so the copy, faster than the raster, is never overtaken by it: no tearing.
void WaitForFrameStart() {
    if (RasterTimeouts >= RASTER_TIMEOUTS_ALLOWED) {
        return;
    }
    volatile uint32_t *raster = reinterpret_cast<volatile uint32_t *>(CRTC_RASTER);
    uint32_t last = *raster & RASTER_LINE_MASK;
    double until = ReadMs() + FRAME_START_TIMEOUT_MS;
    while (ReadMs() < until) {
        uint32_t line = *raster & RASTER_LINE_MASK;
        if (line < last) {
            return;
        }
        last = line;
    }
    RasterTimeouts++;
}

void ShowPicture(const SCREEN_MAP &Map, const uint8_t *Rgb) {
    auto picture = reinterpret_cast<const uint32_t *>(Rgb);
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        uint32_t *to = FrameBuffer + y * SCREEN_WIDTH;
        const uint32_t *from = picture + Map.Rows[y] * Map.Width;
        if (Map.Direct) {
            CopyRow(to, from + Map.Left, SCREEN_WIDTH);
            continue;
        }
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            to[x] = from[Map.Columns[x]];
        }
    }
    _mm_sfence();
}

// The display mode words of 640x480 screens (nxdk's table, hal/video.c): every AV pack, region and refresh rate, interlaced and 480p.
constexpr ULONG SCREEN_MODES[] = {0x44030307, 0x0401010B, 0x04010101, 0x04010103, 0x60030307, 0x2001010B, 0x20010101,
                                  0x20010103, 0x48030314, 0x08010119, 0x0801010D, 0x88070701, 0xC0060601};
// AvSendTVEncoderOption: the current display mode word (0 when none is set); A8R8G8B8 for AvSetDisplayMode.
constexpr ULONG AV_QUERY_MODE = 0x0D;
constexpr ULONG FORMAT_A8R8G8B8 = 0x12;

// Draws Picture (or black) into the frame buffer.
void FillScreen(const uint32_t *Picture) {
    if (!Picture) {
        ClearScreen();
        return;
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        CopyRow(FrameBuffer + y * SCREEN_WIDTH, Picture + y * SCREEN_WIDTH, SCREEN_WIDTH);
    }
    _mm_sfence();
}

// Keeps the display mode already set when it is a 640x480 one (the boot splash's): only the frame buffer changes, so the video encoder is not set up again
// and a TV or an HDMI adapter keeps its signal (spec/oxos_internal/38 §4). Picture is drawn before the switch. False when another mode is set.
bool TakeOverScreen(const uint32_t *Picture) {
    ULONG mode = 0;
    AvSendTVEncoderOption(reinterpret_cast<PVOID>(VIDEO_BASE), AV_QUERY_MODE, 0, &mode);
    bool known = false;
    for (ULONG screenMode : SCREEN_MODES) {
        known = known || mode == screenMode;
    }
    if (!known) {
        return false;
    }
    auto memory = MmAllocateContiguousMemoryEx(SCREEN_WIDTH * SCREEN_HEIGHT * 4, 0, 0x7FFFFFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!memory) {
        return false;
    }
    FrameBuffer = static_cast<uint32_t *>(memory);
    FillScreen(Picture);
    ULONG step = 0;
    do {
        step = AvSetDisplayMode(reinterpret_cast<PVOID>(VIDEO_BASE), step, mode, FORMAT_A8R8G8B8, SCREEN_WIDTH * 4,
                                reinterpret_cast<ULONG>(memory) & 0x7FFFFFFF);
    } while (step);
    DbgPrint("video player: display mode %08lx kept\n", mode);
    return true;
}

// Sets up the 640x480 screen showing Picture (null: black) and switches the picture on, once.
void SetUpScreen(const uint32_t *Picture) {
    if (FrameBuffer) {
        FillScreen(Picture);
        return;
    }
    if (!TakeOverScreen(Picture)) {
        XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, REFRESH_DEFAULT);
        FrameBuffer = reinterpret_cast<uint32_t *>(XVideoGetFB());
        FillScreen(Picture);
    }
    XVideoSetVideoEnable(TRUE);
    ScreenOnMs = ReadMs();
}

// --- The sound

constexpr uint32_t OUTPUT_RATE = 48000;
// Descriptors kept queued ahead of the one playing: about 0.3 s at 48 kHz (MP2 frames are 24 ms).
constexpr uint32_t SOUND_AHEAD = 12;

// The sound stream while it is decoded: minimp3 one frame at a time, converted to 48 kHz stereo.
struct SOUND_DECODER {
    const std::vector<uint8_t> *Stream = nullptr; // the MPEG audio stream
    size_t At = 0;                                // the next frame's offset in it
    bool Ended = false;                           // no frame left
    uint32_t Position = 0;                        // the next output frame's place in the decoded frame, in 1/65536 frames
    mp3dec_t Decoder;
    mp3d_sample_t Decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t Converted[SOUND_MAX_FRAMES * 2];
};

// Decodes the next frame of the stream into Converted (48 kHz stereo, the nearest decoded sample for each); the number of frames, 0 at the end.
uint32_t DecodeSoundFrame(SOUND_DECODER &Sound) {
    while (!Sound.Ended) {
        mp3dec_frame_info_t info;
        size_t left = Sound.Stream->size() - Sound.At;
        int samples = left ? mp3dec_decode_frame(&Sound.Decoder, Sound.Stream->data() + Sound.At, static_cast<int>(left), Sound.Decoded, &info) : 0;
        if (!left || info.frame_bytes == 0) {
            Sound.Ended = true;
            break;
        }
        Sound.At += info.frame_bytes;
        if (samples <= 0 || info.hz <= 0) {
            continue;
        }
        uint32_t step = (static_cast<uint32_t>(info.hz) << 16) / OUTPUT_RATE;
        uint32_t frames = 0;
        while ((Sound.Position >> 16) < static_cast<uint32_t>(samples) && frames < SOUND_MAX_FRAMES) {
            uint32_t i = Sound.Position >> 16;
            int16_t left16 = Sound.Decoded[i * info.channels];
            Sound.Converted[frames * 2] = left16;
            Sound.Converted[frames * 2 + 1] = info.channels > 1 ? Sound.Decoded[i * info.channels + 1] : left16;
            frames++;
            Sound.Position += step;
        }
        Sound.Position -= static_cast<uint32_t>(samples) << 16;
        if (frames) {
            return frames;
        }
    }
    return 0;
}

// Keeps SOUND_AHEAD descriptors queued while the stream lasts.
void TopUpSound(SOUND_OUTPUT &Output, SOUND_DECODER &Sound) {
    while (Output.Registers && !Sound.Ended && CountQueuedSound(Output) < SOUND_AHEAD) {
        uint32_t frames = DecodeSoundFrame(Sound);
        if (frames) {
            QueueSound(Output, Sound.Converted, frames);
        }
    }
}

// --- The file

bool ReadWholeFile(const char *Path, std::vector<uint8_t> &Contents) {
    FILE *file = fopen(Path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bool ok = size > 0 && static_cast<unsigned long>(size) <= MAX_FILE_BYTES;
    if (ok) {
        Contents.resize(static_cast<size_t>(size));
        ok = fread(Contents.data(), 1, Contents.size(), file) == Contents.size();
    }
    fclose(file);
    return ok;
}

// A sequence end code, fed to the decoder after the stream so it gives out its last picture also when the file has none.
const uint8_t SEQUENCE_END[] = {0x00, 0x00, 0x01, 0xB7};

} // namespace

void ShowStillPicture(const uint32_t *Picture) {
    SetUpScreen(Picture);
}

const char *DescribePlayResult(PLAY_RESULT Result) {
    switch (Result) {
    case PLAY_RESULT::ENDED:
        return "played to its end";
    case PLAY_RESULT::STOPPED:
        return "stopped";
    case PLAY_RESULT::UNREADABLE:
        return "missing, empty, too large or unreadable";
    case PLAY_RESULT::NO_VIDEO:
        return "no MPEG-1 or MPEG-2 video in it";
    }
    return "?";
}

PLAY_RESULT PlayVideoFile(const char *Path, uint32_t LeadInMs, bool (*ShouldStop)(void *Context), void *Context) {
    // ShouldStop's answer, kept once it is true (it reports a button press once).
    bool stopped = false;
    auto stopRequested = [&]() {
        stopped = stopped || (ShouldStop && ShouldStop(Context));
        return stopped;
    };
    std::vector<uint8_t> video;
    if (!ReadWholeFile(Path, video)) {
        return PLAY_RESULT::UNREADABLE;
    }
    std::vector<uint8_t> soundStream;
    SplitProgramStream(video, soundStream);
    if (video.empty()) {
        return PLAY_RESULT::NO_VIDEO;
    }
    if (!FrameBuffer) {
        SetUpScreen(nullptr);
    }
    RasterTimeouts = 0;
    // What is on the screen stays while the display locks onto the picture.
    double readyMs = ReadMs();
    while (ReadMs() < ScreenOnMs + LeadInMs && !stopRequested()) {
        Sleep(1);
    }
    DbgPrint("video player: ready %d ms after the screen came on, first frame after %d ms\n", static_cast<int>(readyMs - ScreenOnMs),
             static_cast<int>(ReadMs() - ScreenOnMs));
    if (stopped) {
        ClearScreen();
        return PLAY_RESULT::STOPPED;
    }

    SOUND_OUTPUT output = {};
    static SOUND_DECODER sound;
    sound = SOUND_DECODER();
    sound.Stream = &soundStream;
    sound.Ended = soundStream.empty();
    mp3dec_init(&sound.Decoder);
    if (!sound.Ended && StartSoundOutput(output)) {
        TopUpSound(output, sound);
    }

    mpeg2dec_t *decoder = mpeg2_init();
    const mpeg2_info_t *info = mpeg2_info(decoder);
    mpeg2_buffer(decoder, video.data(), video.data() + video.size());
    SCREEN_MAP map;
    double periodMs = 40;
    double startMs = 0;
    int shown = 0;
    int late = 0; // frames shown more than a frame time after their time
    bool endFed = false;
    bool decoding = true;
    PLAY_RESULT result = PLAY_RESULT::ENDED;
    while (decoding) {
        mpeg2_state_t state = mpeg2_parse(decoder);
        if (state == STATE_BUFFER) {
            if (endFed) {
                decoding = false;
            } else {
                endFed = true;
                mpeg2_buffer(decoder, const_cast<uint8_t *>(SEQUENCE_END), const_cast<uint8_t *>(SEQUENCE_END) + sizeof(SEQUENCE_END));
            }
            continue;
        }
        if (state == STATE_SEQUENCE) {
            mpeg2_convert(decoder, mpeg2convert_rgb32, nullptr);
            SetUpScreenMap(map, static_cast<int>(info->sequence->width), static_cast<int>(info->sequence->height));
            if (info->sequence->frame_period) {
                periodMs = info->sequence->frame_period / 27000.0;
            }
            continue;
        }
        if ((state != STATE_SLICE && state != STATE_END && state != STATE_INVALID_END) || !info->display_fbuf || !map.Width) {
            continue;
        }
        if (!shown) {
            PlaySound(output);
            startMs = ReadMs();
        }
        double dueMs = startMs + shown * periodMs;
        while (ReadMs() < dueMs && !stopRequested()) {
            TopUpSound(output, sound);
            Sleep(1);
        }
        if (stopRequested()) {
            result = PLAY_RESULT::STOPPED;
            break;
        }
        if (ReadMs() > dueMs + periodMs) {
            late++;
        }
        WaitForFrameStart();
        ShowPicture(map, info->display_fbuf->buf[0]);
        shown++;
        TopUpSound(output, sound);
    }
    mpeg2_close(decoder);
    if (!shown && result == PLAY_RESULT::ENDED) {
        result = PLAY_RESULT::NO_VIDEO;
    }
    // The sound may last a little longer than the pictures: it plays out, for at most 2 s.
    double soundEndMs = ReadMs() + 2000;
    while (result == PLAY_RESULT::ENDED && CountQueuedSound(output) && ReadMs() < soundEndMs && !stopRequested()) {
        TopUpSound(output, sound);
        Sleep(1);
    }
    const char *timing = RasterTimeouts >= RASTER_TIMEOUTS_ALLOWED ? "no vertical blank wait" : "shown at the vertical blank";
    DbgPrint("video player: %dx%d, %d frames shown, %d late, %s, %s\n", map.Width, map.Height, shown, late, timing,
             output.Registers ? "with sound" : "no sound");
    StopSoundOutput(output);
    ClearScreen();
    return result;
}

} // namespace oxos::video_player
