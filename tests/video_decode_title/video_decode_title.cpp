// Video decode benchmark (spec/oxos_internal/38 §4.1): how fast the console decodes the boot video format. Every .mpg file in the XBE's folder (D:) is read
// into memory and split into its video and sound streams, then timed in four passes:
//   1. video decoded only (libmpeg2, MMX);
//   2. video decoded and turned into 32-bit RGB (libmpeg2's converter, MMX);
//   3. the sound decoded (minimp3: MP2 to 16-bit PCM);
//   4. as 2, then shown: the middle 640x480 of each frame copied by the CPU into the frame buffer (640x480, 32 bits). This pass runs for every file at the
//      end, after SDL is closed (the screen is then the frame buffer alone, no text). Copying into an SDL texture instead cost 27 ms a frame at 848x480 (its
//      GPU renderer rearranges the pixels into the GPU's own texture layout), and nxdk's memcpy (a byte at a time) 21 ms: the rows are copied with SSE
//      streaming stores instead.
// Each line goes to the debug log (DbgPrint: the serial port of a SERIAL=1 kernel), the screen and D:\video_decode_results.txt. When done the title starts
// E:\ftp\ftp.xbe (the OXOS FTP server as netboot puts it there) if it exists, so the results can be fetched and the next build started without a power cycle.
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

#include <string>
#include <vector>

#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "sdl_screen.h"
#include <video_player/mpeg_program_stream.h>

extern "C" {
#include <mpeg2.h>
#include <mpeg2convert.h>
}

// minimp3's SIMD code needs SSE2, which the console's Pentium III does not have (clang's Windows target looks like MSVC, which minimp3 takes for SSE2).
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

namespace {

// --- Results

// D:\video_decode_results.txt while the passes run; closed at the end.
FILE *ResultsFile;

// One result line: the debug log, the screen and the results file.
void Report(const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    DbgPrint("video_decode: %s\n", line);
    if (sdl_screen::IsOpen()) {
        sdl_screen::Print("%s\n", line);
        sdl_screen::Present();
    }
    if (ResultsFile) {
        fprintf(ResultsFile, "%s\n", line);
        fflush(ResultsFile);
    }
}

// --- Time

// The performance counter's ticks per millisecond, read once.
double TicksPerMs;

double ReadMs() {
    return (double)KeQueryPerformanceCounter() / TicksPerMs;
}

// A number with one decimal, as text: nxdk's printf has no floating-point conversions (%f prints nothing and skips no argument).
struct DECIMAL {
    char Text[24];
};

DECIMAL FormatDecimal(double value) {
    DECIMAL decimal;
    long tenths = (long)(value * 10.0 + (value < 0 ? -0.5 : 0.5));
    snprintf(decimal.Text, sizeof(decimal.Text), "%s%ld.%ld", tenths < 0 ? "-" : "", labs(tenths) / 10, labs(tenths) % 10);
    return decimal;
}

// --- The file

// A program stream's two streams, each as one run of bytes: the first video stream (0xE0) and the first MPEG audio stream (0xC0).
struct STREAMS {
    std::vector<uint8_t> Video;
    std::vector<uint8_t> Audio;
};

// Reads a whole file; false if it cannot be read.
bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &contents) {
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    contents.resize(size > 0 ? size : 0);
    size_t read = contents.empty() ? 0 : fread(contents.data(), 1, contents.size(), file);
    fclose(file);
    return read == contents.size();
}

// --- Video passes

enum class VIDEO_PASS { DECODE, CONVERT, SHOW };

// What one video pass measured.
struct VIDEO_TIMES {
    int Frames = 0;          // frames that came out of the decoder
    int Width = 0;           // the picture's size, from the sequence header
    int Height = 0;
    double FrameRate = 0;    // frames per second the stream asks for
    double TotalMs = 0;      // the whole pass
    double SlowestMs = 0;    // the longest time between two frames coming out of the decoder
    double CopyMs = 0;       // SHOW: copying the frames into the frame buffer, all together
};

// The frame buffer of the SHOW passes: 640x480, 32 bits a pixel (XVideoSetMode, after SDL is closed).
constexpr int SCREEN_WIDTH = 640;
constexpr int SCREEN_HEIGHT = 480;
uint32_t *FrameBuffer;

// Copies one row of pixels into the frame buffer, 16 bytes at a time with streaming stores (the frame buffer is not cached: a store that skips the cache
// writes whole lines at once). The frame buffer's rows are 16-byte aligned; the source may not be. A row's width is a multiple of 4 pixels.
void CopyRow(uint32_t *to, const uint8_t *from, int pixels) {
    for (int i = 0; i < pixels; i += 4) {
        _mm_stream_ps(reinterpret_cast<float *>(to + i), _mm_loadu_ps(reinterpret_cast<const float *>(from + i * 4)));
    }
}

// Shows one RGB frame: its middle 640x480 (the middle 4:3 of a 16:9 picture at 480 lines; a larger picture is cut, not scaled, in this test) copied into the
// frame buffer.
void ShowFrame(const uint8_t *rgb, int width, int height, VIDEO_TIMES &times) {
    double start = ReadMs();
    int copyWidth = width < SCREEN_WIDTH ? width : SCREEN_WIDTH;
    int copyHeight = height < SCREEN_HEIGHT ? height : SCREEN_HEIGHT;
    const uint8_t *from = rgb + ((size_t)((height - copyHeight) / 2) * width + (width - copyWidth) / 2) * 4;
    for (int y = 0; y < copyHeight; y++) {
        CopyRow(FrameBuffer + (size_t)y * SCREEN_WIDTH, from + (size_t)y * width * 4, copyWidth);
    }
    _mm_sfence();
    times.CopyMs += ReadMs() - start;
}

// Decodes the whole video stream once.
VIDEO_TIMES TimeVideoPass(const std::vector<uint8_t> &video, VIDEO_PASS pass) {
    VIDEO_TIMES times;
    mpeg2dec_t *decoder = mpeg2_init();
    const mpeg2_info_t *info = mpeg2_info(decoder);
    mpeg2_buffer(decoder, const_cast<uint8_t *>(video.data()), const_cast<uint8_t *>(video.data() + video.size()));
    double start = ReadMs();
    double lastFrame = start;
    bool fed = true;
    while (fed) {
        mpeg2_state_t state = mpeg2_parse(decoder);
        if (state == STATE_BUFFER) {
            fed = false;
        } else if (state == STATE_SEQUENCE) {
            times.Width = info->sequence->width;
            times.Height = info->sequence->height;
            times.FrameRate = info->sequence->frame_period ? 27000000.0 / info->sequence->frame_period : 0;
            if (pass != VIDEO_PASS::DECODE) {
                mpeg2_convert(decoder, mpeg2convert_rgb32, nullptr);
            }
        } else if ((state == STATE_SLICE || state == STATE_END || state == STATE_INVALID_END) && info->display_fbuf) {
            if (pass == VIDEO_PASS::SHOW) {
                ShowFrame(info->display_fbuf->buf[0], times.Width, times.Height, times);
            }
            double now = ReadMs();
            if (now - lastFrame > times.SlowestMs) {
                times.SlowestMs = now - lastFrame;
            }
            lastFrame = now;
            times.Frames++;
        }
    }
    times.TotalMs = ReadMs() - start;
    mpeg2_close(decoder);
    return times;
}

const char *NamePass(VIDEO_PASS pass) {
    if (pass == VIDEO_PASS::DECODE) {
        return "decode";
    }
    return pass == VIDEO_PASS::CONVERT ? "+rgb" : "+rgb+show";
}

void ReportVideoPass(VIDEO_PASS pass, const VIDEO_TIMES &times) {
    double perFrame = times.Frames ? times.TotalMs / times.Frames : 0;
    char copy[48] = "";
    if (pass == VIDEO_PASS::SHOW && times.Frames) {
        snprintf(copy, sizeof(copy), " copy %s", FormatDecimal(times.CopyMs / times.Frames).Text);
    }
    Report(" %-10s %3d fr %5ld ms %5s ms/fr %5s fps max %s%s", NamePass(pass), times.Frames, (long)times.TotalMs, FormatDecimal(perFrame).Text,
           FormatDecimal(perFrame > 0 ? 1000.0 / perFrame : 0).Text, FormatDecimal(times.SlowestMs).Text, copy);
}

// --- Sound pass

void TimeSoundPass(const std::vector<uint8_t> &audio) {
    if (audio.empty()) {
        Report("  sound: none");
        return;
    }
    static mp3dec_t decoder;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&decoder);
    size_t at = 0;
    long samples = 0;
    int rate = 0;
    int channels = 0;
    int layer = 0;
    double start = ReadMs();
    while (at < audio.size()) {
        mp3dec_frame_info_t frame;
        int decoded = mp3dec_decode_frame(&decoder, audio.data() + at, (int)(audio.size() - at), pcm, &frame);
        if (frame.frame_bytes == 0) {
            break;
        }
        at += frame.frame_bytes;
        samples += decoded;
        if (decoded) {
            rate = frame.hz;
            channels = frame.channels;
            layer = frame.layer;
        }
    }
    double ms = ReadMs() - start;
    double seconds = rate ? (double)samples / rate : 0;
    Report(" sound: MP%d %d Hz %d ch, %s s in %ld ms = %s%% CPU", layer, rate, channels, FormatDecimal(seconds).Text, (long)ms,
           FormatDecimal(seconds > 0 ? ms / (seconds * 10.0) : 0).Text);
}

// --- The files

std::vector<std::string> ListVideoFiles() {
    std::vector<std::string> names;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA("D:\\*.mpg", &found);
    if (search == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        names.push_back(found.cFileName);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return names;
}

// Reads a file and splits it; false if it cannot be read.
bool ReadStreams(const std::string &name, STREAMS &streams, double &splitMs) {
    if (!ReadWholeFile("D:\\" + name, streams.Video)) {
        Report("%s: cannot be read", name.c_str());
        return false;
    }
    double start = ReadMs();
    oxos::video_player::SplitProgramStream(streams.Video, streams.Audio);
    splitMs = ReadMs() - start;
    return true;
}

void TimeFile(const std::string &name) {
    STREAMS streams;
    double splitMs;
    if (!ReadStreams(name, streams, splitMs)) {
        return;
    }
    Report("%s: %u KB video, %u KB sound, split %s ms", name.c_str(), (unsigned)(streams.Video.size() / 1024), (unsigned)(streams.Audio.size() / 1024),
           FormatDecimal(splitMs).Text);
    VIDEO_TIMES first = TimeVideoPass(streams.Video, VIDEO_PASS::DECODE);
    Report(" %dx%d at %s fps", first.Width, first.Height, FormatDecimal(first.FrameRate).Text);
    ReportVideoPass(VIDEO_PASS::DECODE, first);
    ReportVideoPass(VIDEO_PASS::CONVERT, TimeVideoPass(streams.Video, VIDEO_PASS::CONVERT));
    TimeSoundPass(streams.Audio);
}

void ShowFile(const std::string &name) {
    STREAMS streams;
    double splitMs;
    if (!ReadStreams(name, streams, splitMs)) {
        return;
    }
    Report("%s shown:", name.c_str());
    ReportVideoPass(VIDEO_PASS::SHOW, TimeVideoPass(streams.Video, VIDEO_PASS::SHOW));
}

} // namespace

int main() {
    TicksPerMs = (double)KeQueryPerformanceFrequency() / 1000.0;
    sdl_screen::Open(false);
    ResultsFile = fopen("D:\\video_decode_results.txt", "w");
    uint32_t accel = mpeg2_accel(MPEG2_ACCEL_DETECT);
    unsigned int features;
    __asm__ volatile("cpuid" : "=d"(features) : "a"(1) : "ebx", "ecx");
    Report("OXOS video decode test: libmpeg2 accel %08x, CPUID 1 edx %08x", (unsigned)accel, features);
    std::vector<std::string> names = ListVideoFiles();
    if (names.empty()) {
        Report("no .mpg files next to the XBE");
    }
    for (const std::string &name : names) {
        TimeFile(name);
    }
    sdl_screen::Close();
    XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, REFRESH_DEFAULT);
    FrameBuffer = reinterpret_cast<uint32_t *>(XVideoGetFB());
    memset(FrameBuffer, 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 4);
    for (const std::string &name : names) {
        ShowFile(name);
    }
    Report("done");
    if (ResultsFile) {
        fclose(ResultsFile);
    }
    Sleep(5000);
    nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    if (GetFileAttributesA("E:\\ftp\\ftp.xbe") != INVALID_FILE_ATTRIBUTES) {
        XLaunchXBE("E:\\ftp\\ftp.xbe");
    }
    sdl_screen::Open(false);
    sdl_screen::ShowForever();
}
