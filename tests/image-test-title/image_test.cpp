// Image test title (nxdk, open source): the simplest title that shows the kernel, a title and video all work.
// It sets a video mode, loads a PNG from its disc (D:\picture.png) and decodes it on the console with libpng (file
// I/O, zlib, memory allocation), shows it, and then keeps a status panel and the front LED changing once a second, so
// a frozen console is obvious (README.md in this folder). The same state is kept in XkImageTest for automated runs
// (found in RAM by its signature).
#include <hal/debug.h>
#include <hal/video.h>
extern "C" {
#include <hal/led.h> // no C++ linkage guard of its own
}
#include <png.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

constexpr unsigned int Width = 640, Height = 480;
constexpr unsigned int PictureHeight = 330; // the picture's band; the status lines go below it
constexpr int TextX = 30, TextY = 340;
constexpr unsigned int TextLines = 4, LinePitch = 17; // debugPrint: a 16-pixel font, 17-pixel lines
constexpr unsigned int XboxGreen = 0x0052B043;       // nxdk's debugPrint draws white; the band is recoloured

enum class PictureStatus : unsigned int { Loading = 0, Shown = 1, Missing = 2, Undecodable = 3, OutOfMemory = 4 };

const char *Describe(PictureStatus s) {
    switch (s) {
    case PictureStatus::Shown: return "shown";
    case PictureStatus::Missing: return "D:\\picture.png not found";
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
    unsigned int PngStatus; // a PictureStatus
    unsigned int PngWidth, PngHeight, PngMs;
    unsigned int FreePages;
} XkImageTest = {{'X', 'K', 'I', 'M', 'G', 'T', 'S', '1'}};

namespace {

// Decode the PNG and draw it scaled (keeping its aspect) and centred into the picture band of the framebuffer.
PictureStatus ShowPicture(const char *path) {
    DWORD start = GetTickCount();
    std::size_t size;
    auto file = LoadWholeFile(path, size);
    if (!file)
        return PictureStatus::Missing;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, file.get(), size))
        return PictureStatus::Undecodable;
    image.format = PNG_FORMAT_BGRA; // the framebuffer's byte order (32 bpp: B, G, R, A)
    std::unique_ptr<unsigned char[]> pixels(new (std::nothrow) unsigned char[PNG_IMAGE_SIZE(image)]);
    if (!pixels) {
        png_image_free(&image);
        return PictureStatus::OutOfMemory;
    }
    if (!png_image_finish_read(&image, nullptr, pixels.get(), 0, nullptr))
        return PictureStatus::Undecodable;
    file.reset();
    const unsigned int w = image.width, h = image.height;
    unsigned int dw = Width, dh = static_cast<unsigned int>(static_cast<unsigned long long>(h) * Width / w);
    if (dh > PictureHeight) {
        dh = PictureHeight;
        dw = static_cast<unsigned int>(static_cast<unsigned long long>(w) * PictureHeight / h);
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
    XkImageTest.PngWidth = w;
    XkImageTest.PngHeight = h;
    XkImageTest.PngMs = GetTickCount() - start;
    return PictureStatus::Shown;
}

unsigned int FreePages() {
    MM_STATISTICS s{};
    s.Length = sizeof(s);
    return NT_SUCCESS(MmQueryStatistics(&s)) ? s.AvailablePages : 0;
}

void RecolourText() {
    auto band = reinterpret_cast<unsigned int *>(XVideoGetFB()) + TextY * Width;
    for (unsigned int i = 0; i < TextLines * LinePitch * Width; i++)
        if ((band[i] & 0x00FFFFFF) == 0x00FFFFFF)
            band[i] = XboxGreen;
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
    auto status = ShowPicture("D:\\picture.png");
    XkImageTest.PngStatus = static_cast<unsigned int>(status);

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
        debugPrint("alive %u s, %u frames (%u per s)             \n", static_cast<unsigned int>(seconds), frames,
                   seconds ? frames / static_cast<unsigned int>(seconds) : 0);
        debugPrint("free memory %u KB                            \n", XkImageTest.FreePages * 4);
        if (status == PictureStatus::Shown)
            debugPrint("picture %ux%u from D:\\picture.png in %u ms       \n", XkImageTest.PngWidth,
                       XkImageTest.PngHeight, XkImageTest.PngMs);
        else
            debugPrint("picture: %s                                  \n", Describe(status));
        RecolourText();
    }
}
