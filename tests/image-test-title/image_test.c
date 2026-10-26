// Image test title (nxdk, open source): the simplest title that shows the kernel, a title and video all work.
// It sets a video mode, loads a PNG from its disc (D:\picture.png) and decodes it on the console with libpng (file I/O,
// zlib, memory allocation), shows it, and then keeps a status panel and the front LED changing once a second, so a
// frozen console is obvious (README.md in this folder). The same state is kept in XkImageTest for automated runs
// (found in RAM by its signature).
#include <hal/debug.h>
#include <hal/led.h>
#include <hal/video.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define WIDTH 640
#define HEIGHT 480
#define PICTURE_H 330 // the picture's band; the status lines go below it
#define TEXT_X 30
#define TEXT_Y 340
#define TEXT_LINES 4
#define XBOX_GREEN 0x0052B043 // the text colour (nxdk's debugPrint draws white; the band is recoloured after each update)

volatile struct {
    char Magic[8]; // "XKIMGTS1"
    unsigned int Self;      // this block's virtual address
    unsigned int Seconds;   // seconds since the title started (from its own tick count)
    unsigned int Frames;    // vertical blanks waited for
    unsigned int PngStatus; // 0 not loaded yet, 1 shown, 2 file missing, 3 decode failed, 4 out of memory
    unsigned int PngWidth, PngHeight, PngMs;
    unsigned int FreePages;
} XkImageTest = {{'X', 'K', 'I', 'M', 'G', 'T', 'S', '1'}};

static const char *PngStatusText(unsigned int s) {
    switch (s) {
    case 1: return "shown";
    case 2: return "D:\\picture.png not found";
    case 3: return "decode failed";
    case 4: return "out of memory";
    default: return "loading";
    }
}

// Read a whole file; NULL if it cannot be opened or read.
static unsigned char *LoadWholeFile(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

// Decode the PNG and draw it scaled (keeping its aspect) and centred into the picture band of the framebuffer.
static unsigned int ShowPng(const char *path) {
    size_t size;
    DWORD start = GetTickCount();
    unsigned char *file = LoadWholeFile(path, &size);
    if (!file)
        return 2;
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, file, size)) {
        free(file);
        return 3;
    }
    image.format = PNG_FORMAT_BGRA; // the framebuffer's byte order (32 bpp: B, G, R, A)
    unsigned char *pixels = malloc(PNG_IMAGE_SIZE(image));
    if (!pixels) {
        png_image_free(&image);
        free(file);
        return 4;
    }
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        free(pixels);
        free(file);
        return 3;
    }
    free(file);
    unsigned int w = image.width, h = image.height;
    unsigned int dw = WIDTH, dh = (unsigned int)((unsigned long long)h * WIDTH / w);
    if (dh > PICTURE_H) {
        dh = PICTURE_H;
        dw = (unsigned int)((unsigned long long)w * PICTURE_H / h);
    }
    unsigned int x0 = (WIDTH - dw) / 2, y0 = (PICTURE_H - dh) / 2;
    unsigned int *fb = (unsigned int *)XVideoGetFB();
    for (unsigned int y = 0; y < dh; y++) {
        const unsigned int *src = (const unsigned int *)pixels + (unsigned long long)(y * h / dh) * w;
        unsigned int *dst = fb + (y0 + y) * WIDTH + x0;
        for (unsigned int x = 0; x < dw; x++)
            dst[x] = src[(unsigned long long)x * w / dw];
    }
    free(pixels);
    XkImageTest.PngWidth = w;
    XkImageTest.PngHeight = h;
    XkImageTest.PngMs = GetTickCount() - start;
    return 1;
}

static unsigned int FreePages(void) {
    MM_STATISTICS s;
    memset(&s, 0, sizeof(s));
    s.Length = sizeof(s);
    return NT_SUCCESS(MmQueryStatistics(&s)) ? s.AvailablePages : 0;
}

int main(void) {
    XkImageTest.Self = (unsigned int)&XkImageTest;
    XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT);
    memset(XVideoGetFB(), 0, WIDTH * HEIGHT * 4);
    debugPrint(""); // debugPrint learns the screen size on its first call; until then debugMoveCursor is ignored
    XkImageTest.PngStatus = ShowPng("D:\\picture.png");

    static const XLEDColor pattern[4][4] = {
        {XLED_GREEN, XLED_GREEN, XLED_GREEN, XLED_GREEN},
        {XLED_ORANGE, XLED_ORANGE, XLED_ORANGE, XLED_ORANGE},
        {XLED_GREEN, XLED_OFF, XLED_GREEN, XLED_OFF},
        {XLED_RED, XLED_GREEN, XLED_RED, XLED_GREEN},
    };
    DWORD start = GetTickCount(), lastSecond = (DWORD)-1;
    for (;;) {
        XVideoWaitForVBlank();
        XkImageTest.Frames++;
        DWORD seconds = (GetTickCount() - start) / 1000;
        if (seconds == lastSecond)
            continue;
        lastSecond = seconds;
        XkImageTest.Seconds = seconds;
        XkImageTest.FreePages = FreePages();
        XSetCustomLED(pattern[seconds % 4][0], pattern[seconds % 4][1], pattern[seconds % 4][2],
                      pattern[seconds % 4][3]);
        debugMoveCursor(TEXT_X, TEXT_Y);
        debugPrint("OXOS image test - kernel %u.%u.%u.%u            \n", XboxKrnlVersion.Major,
                   XboxKrnlVersion.Minor, XboxKrnlVersion.Build, XboxKrnlVersion.Qfe & 0x7FFF);
        debugPrint("alive %u s, %u frames (%u per s)             \n", (unsigned int)seconds, XkImageTest.Frames,
                   seconds ? XkImageTest.Frames / (unsigned int)seconds : 0);
        debugPrint("free memory %u KB                            \n", XkImageTest.FreePages * 4);
        if (XkImageTest.PngStatus == 1)
            debugPrint("picture %ux%u from D:\\picture.png in %u ms       \n", XkImageTest.PngWidth,
                       XkImageTest.PngHeight, XkImageTest.PngMs);
        else
            debugPrint("picture: %s                                  \n", PngStatusText(XkImageTest.PngStatus));
        unsigned int *band = (unsigned int *)XVideoGetFB() + TEXT_Y * WIDTH;
        for (unsigned int i = 0; i < TEXT_LINES * 17 * WIDTH; i++) // debugPrint: 16-pixel font, 17-pixel lines
            if ((band[i] & 0x00FFFFFF) == 0x00FFFFFF)
                band[i] = XBOX_GREEN;
    }
    return 0;
}
