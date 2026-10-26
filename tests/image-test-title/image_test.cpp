// Image test title (nxdk, open source): the simplest title that shows the kernel, a title and video all work.
// It sets a video mode, loads a JPEG from its disc (D:\picture.jpg) and decodes it on the console with libjpeg
// (file I/O, memory allocation), shows it, and then keeps a status panel and the front LED changing once a second, so
// a frozen console is obvious (README.md in this folder). The same state is kept in XkImageTest for automated runs
// (found in RAM by its signature).
#include <hal/debug.h>
#include <hal/video.h>
extern "C" {
#include <hal/led.h> // no C++ linkage guard of its own
}
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>

#include <jpeglib.h> // after <cstdio>: it uses FILE

namespace {

constexpr unsigned int Width = 640, Height = 480;
constexpr unsigned int PictureHeight = 330; // the picture's band; the status lines go below it
constexpr int TextX = 30, TextY = 340;
constexpr const char *PicturePath = "D:\\picture.jpg";

enum class PictureStatus : unsigned int { Loading = 0, Shown = 1, Missing = 2, Undecodable = 3, OutOfMemory = 4 };

const char *Describe(PictureStatus s) {
    switch (s) {
    case PictureStatus::Shown: return "shown";
    case PictureStatus::Missing: return "not found";
    case PictureStatus::Undecodable: return "decode failed";
    case PictureStatus::OutOfMemory: return "out of memory";
    default: return "loading";
    }
}

struct FileCloser {
    void operator()(std::FILE *f) const { std::fclose(f); }
};

// A whole file in memory; empty if it cannot be opened or read.
std::unique_ptr<unsigned char[]> LoadWholeFile(const char *path, std::size_t &size) {
    size = 0;
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path, "rb"));
    if (!f)
        return nullptr;
    std::fseek(f.get(), 0, SEEK_END);
    long n = std::ftell(f.get());
    std::fseek(f.get(), 0, SEEK_SET);
    if (n <= 0)
        return nullptr;
    std::unique_ptr<unsigned char[]> data(new (std::nothrow) unsigned char[n]);
    if (!data || std::fread(data.get(), 1, static_cast<std::size_t>(n), f.get()) != static_cast<std::size_t>(n))
        return nullptr;
    size = static_cast<std::size_t>(n);
    return data;
}

} // namespace

// The state, for automated runs: found in RAM by its signature. The layout is fixed (tests read it).
volatile struct {
    char Magic[8];          // "XKIMGTS1"
    unsigned int Self;      // this block's virtual address
    unsigned int Seconds;   // seconds since the title started (from its own tick count)
    unsigned int Frames;    // vertical blanks waited for
    unsigned int Status; // a PictureStatus
    unsigned int PictureWidth, PictureHeight, PictureMs;
    unsigned int FreePages;
    unsigned int Widescreen; // 1 when the console's video setting is widescreen (the picture is drawn 3/4 as wide)
} XkImageTest = {{'X', 'K', 'I', 'M', 'G', 'T', 'S', '1'}};

namespace {

// libjpeg reports a fatal error by calling error_exit, which must not return: it jumps back to DecodeJpeg's setjmp.
// DecodeJpeg holds no objects with destructors, so the jump skips nothing.
struct JpegError {
    jpeg_error_mgr Manager; // first: libjpeg passes &Manager as cinfo->err
    std::jmp_buf Jump;
};

void JpegErrorExit(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegError *>(cinfo->err)->Jump, 1);
}

// Decodes a JPEG into 32-bit pixels in the framebuffer's byte order (B, G, R, x). The pixels are allocated with
// new[] and handed to *pixels as soon as they exist, so the caller frees them on every path.
PictureStatus DecodeJpeg(const unsigned char *data, std::size_t size, unsigned char **pixels, unsigned int &width,
                         unsigned int &height) {
    jpeg_decompress_struct cinfo;
    JpegError error;
    cinfo.err = jpeg_std_error(&error.Manager);
    error.Manager.error_exit = JpegErrorExit;
    if (setjmp(error.Jump)) {
        jpeg_destroy_decompress(&cinfo);
        return PictureStatus::Undecodable;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return PictureStatus::Undecodable;
    }
    cinfo.out_color_space = JCS_EXT_BGRX;
    jpeg_start_decompress(&cinfo);
    width = cinfo.output_width;
    height = cinfo.output_height;
    *pixels = new (std::nothrow) unsigned char[static_cast<std::size_t>(width) * height * 4];
    if (!*pixels) {
        jpeg_destroy_decompress(&cinfo);
        return PictureStatus::OutOfMemory;
    }
    while (cinfo.output_scanline < height) {
        JSAMPROW row = *pixels + static_cast<std::size_t>(cinfo.output_scanline) * width * 4;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return PictureStatus::Shown;
}

// Decode the JPEG and draw it scaled (keeping its aspect on the TV) and centred into the picture band of the
// framebuffer. In widescreen the TV stretches the 640-pixel width over 16:9 instead of 4:3, so every framebuffer
// pixel shows 4/3 as wide: the picture is drawn 3/4 as wide to come out right.
PictureStatus ShowPicture(const char *path, bool widescreen) {
    DWORD start = GetTickCount();
    std::size_t size;
    auto file = LoadWholeFile(path, size);
    if (!file)
        return PictureStatus::Missing;
    unsigned char *decoded = nullptr;
    unsigned int w = 0, h = 0;
    PictureStatus status = DecodeJpeg(file.get(), size, &decoded, w, h);
    std::unique_ptr<unsigned char[]> pixels(decoded);
    if (status != PictureStatus::Shown)
        return status;
    file.reset();
    const unsigned long long pixelW = widescreen ? 4 : 1, pixelH = widescreen ? 3 : 1; // a pixel's shape on the TV
    unsigned int dw = Width, dh = static_cast<unsigned int>(h * Width * pixelW / (w * pixelH));
    if (dh > PictureHeight) {
        dh = PictureHeight;
        dw = static_cast<unsigned int>(w * PictureHeight * pixelH / (h * pixelW));
    }
    const unsigned int x0 = (Width - dw) / 2, y0 = (PictureHeight - dh) / 2;
    auto fb = reinterpret_cast<unsigned int *>(XVideoGetFB());
    auto src = reinterpret_cast<const unsigned int *>(pixels.get());
    for (unsigned int y = 0; y < dh; y++) {
        const unsigned int *row = src + static_cast<unsigned long long>(y * h / dh) * w;
        unsigned int *dst = fb + (y0 + y) * Width + x0;
        for (unsigned int x = 0; x < dw; x++)
            dst[x] = row[static_cast<unsigned long long>(x) * w / dw];
    }
    XkImageTest.PictureWidth = w;
    XkImageTest.PictureHeight = h;
    XkImageTest.PictureMs = GetTickCount() - start;
    return PictureStatus::Shown;
}

unsigned int FreePages() {
    MM_STATISTICS s{};
    s.Length = sizeof(s);
    return NT_SUCCESS(MmQueryStatistics(&s)) ? s.AvailablePages : 0;
}

constexpr std::array<std::array<XLEDColor, 4>, 4> LedPatterns{{
    {XLED_GREEN, XLED_GREEN, XLED_GREEN, XLED_GREEN},
    {XLED_ORANGE, XLED_ORANGE, XLED_ORANGE, XLED_ORANGE},
    {XLED_GREEN, XLED_OFF, XLED_GREEN, XLED_OFF},
    {XLED_RED, XLED_GREEN, XLED_RED, XLED_GREEN},
}};

} // namespace

int main() {
    XkImageTest.Self = reinterpret_cast<unsigned int>(&XkImageTest);
    XVideoSetMode(Width, Height, 32, REFRESH_DEFAULT);
    std::memset(XVideoGetFB(), 0, Width * Height * 4);
    debugPrint(""); // debugPrint learns the screen size on its first call; until then debugMoveCursor is ignored
    const bool widescreen = XVideoGetEncoderSettings() & VIDEO_WIDESCREEN;
    XkImageTest.Widescreen = widescreen;
    auto status = ShowPicture(PicturePath, widescreen);
    XkImageTest.Status = static_cast<unsigned int>(status);

    const DWORD start = GetTickCount();
    DWORD lastSecond = static_cast<DWORD>(-1);
    for (;;) {
        XVideoWaitForVBlank();
        XkImageTest.Frames = XkImageTest.Frames + 1;
        const DWORD seconds = (GetTickCount() - start) / 1000;
        if (seconds == lastSecond)
            continue;
        lastSecond = seconds;
        XkImageTest.Seconds = seconds;
        XkImageTest.FreePages = FreePages();
        const auto &leds = LedPatterns[seconds % LedPatterns.size()];
        XSetCustomLED(leds[0], leds[1], leds[2], leds[3]);
        const unsigned int frames = XkImageTest.Frames;
        debugMoveCursor(TextX, TextY);
        debugPrint("OXOS image test - kernel %u.%u.%u.%u            \n", XboxKrnlVersion.Major, XboxKrnlVersion.Minor,
                   XboxKrnlVersion.Build, XboxKrnlVersion.Qfe & 0x7FFF);
        debugPrint("Uptime: %u s, %u frames (%u per s)             \n", static_cast<unsigned int>(seconds), frames,
                   seconds ? frames / static_cast<unsigned int>(seconds) : 0);
        debugPrint("Free memory %u KB                            \n", XkImageTest.FreePages * 4);
        if (status == PictureStatus::Shown)
            debugPrint("Image: %s                                        \n", PicturePath);
        else
            debugPrint("Image: %s (%s)                                   \n", PicturePath, Describe(status));
    }
}
