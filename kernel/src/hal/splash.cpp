// Boot splash (our own artwork; the reference's INIT-section animation is not reproduced): on cold boot a system
// thread shows an emblem and caption fading in on a 640x480 32 bpp screen. The title launcher waits for it and
// releases it before entering the title.
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
XBAPI UCHAR HalBootSMCVideoMode;

constexpr ULONG SPLASH_W = 640, SPLASH_H = 480, SPLASH_PITCH = SPLASH_W * 4;
constexpr ULONG SPLASH_FADE_FRAMES = 24, SPLASH_FRAME_MS = 40, SPLASH_HOLD_MS = 600;

static KEVENT HalpSplashDone;
static ULONG *HalpSplashScreen, *HalpSplashImage;
volatile ULONG HalpSplashFrames; // frames shown (boot tests)

// 5x7 glyphs, one byte per row (bit 4 = leftmost column), for the caption's letters.
static const struct {
    char C;
    UCHAR Rows[7];
} HalpGlyphs[] = {
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
};

static void HalpDrawText(ULONG *img, const char *text, ULONG y, ULONG scale, ULONG color) {
    ULONG n = 0;
    while (text[n])
        n++;
    ULONG x = (SPLASH_W - n * 6 * scale + scale) / 2;
    for (ULONG i = 0; i < n; i++, x += 6 * scale)
        for (auto &g : HalpGlyphs) {
            if (g.C != text[i])
                continue;
            for (ULONG r = 0; r < 7; r++)
                for (ULONG c = 0; c < 5; c++)
                    if (g.Rows[r] & (0x10 >> c))
                        for (ULONG dy = 0; dy < scale; dy++)
                            for (ULONG dx = 0; dx < scale; dx++)
                                img[(y + r * scale + dy) * SPLASH_W + x + c * scale + dx] = color;
        }
}

static LONG HalpAbs(LONG v) { return v < 0 ? -v : v; }

// The emblem: a ring and two crossed bars, green, with a soft edge (coverage from a distance in pixels).
static void HalpDrawEmblem(ULONG *img) {
    const LONG cx = SPLASH_W / 2, cy = 200, outer = 110, inner = 96, bar = 15;
    for (LONG y = cy - outer - 2; y <= cy + outer + 2; y++)
        for (LONG x = cx - outer - 2; x <= cx + outer + 2; x++) {
            LONG dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            // Ring: inside [inner, outer]; bars: |dx - dy| or |dx + dy| below bar*sqrt(2), within the ring.
            LONG ring = d2 <= outer * outer && d2 >= inner * inner ? 255 : 0;
            LONG cross = 0;
            if (d2 < (inner - 10) * (inner - 10)) {
                LONG a = HalpAbs(dx - dy), b = HalpAbs(dx + dy), m = a < b ? a : b; // distance * sqrt(2)
                LONG edge = bar * 141 / 100;
                cross = m <= edge ? 255 : m <= edge + 3 ? 255 - (m - edge) * 80 : 0;
            }
            LONG v = ring > cross ? ring : cross;
            if (!v)
                continue;
            LONG shade = 140 + (cy + outer - y) * 115 / (2 * outer); // lighter towards the top
            img[y * SPLASH_W + x] = (ULONG)((v * shade / 255 * 0x40 / 0xFF) << 16 | (v * shade / 255) << 8 |
                                            (v * shade / 255 * 0x30 / 0xFF));
        }
}

static void NTAPI HalpSplashThread(PVOID) {
    RtlZeroMemory(HalpSplashImage, SPLASH_PITCH * SPLASH_H);
    RtlZeroMemory(HalpSplashScreen, SPLASH_PITCH * SPLASH_H);
    HalpDrawEmblem(HalpSplashImage);
    HalpDrawText(HalpSplashImage, "OPEN XBOX KERNEL", 350, 3, 0xE0E0E0);
    AvSetDisplayMode((PVOID)0xFD000000, 0, 0, 0x1E, SPLASH_PITCH, MmGetPhysicalAddress(HalpSplashScreen));
    for (ULONG f = 1; f <= SPLASH_FADE_FRAMES; f++) {
        for (ULONG i = 0; i < SPLASH_W * SPLASH_H; i++) {
            ULONG p = HalpSplashImage[i];
            if (!p)
                continue;
            ULONG r = (p >> 16 & 0xFF) * f / SPLASH_FADE_FRAMES, g = (p >> 8 & 0xFF) * f / SPLASH_FADE_FRAMES,
                  b = (p & 0xFF) * f / SPLASH_FADE_FRAMES;
            HalpSplashScreen[i] = r << 16 | g << 8 | b;
        }
        HalpSplashFrames = f;
        LARGE_INTEGER t;
        t.QuadPart = -10000LL * SPLASH_FRAME_MS;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * SPLASH_HOLD_MS;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
    KeSetEvent(&HalpSplashDone, 0, FALSE);
}

// Cold boot, with an AV pack connected.
void HalpStartSplash() {
    KeInitializeEvent(&HalpSplashDone, NotificationEvent, TRUE);
    if (HalBootSMCVideoMode == 7)
        return;
    HalpSplashScreen = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0x3FFFFFF, 0x1000,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE);
    HalpSplashImage = (ULONG *)MmAllocateContiguousMemory(SPLASH_PITCH * SPLASH_H);
    if (!HalpSplashScreen || !HalpSplashImage)
        return;
    KeResetEvent(&HalpSplashDone);
    KiCreateSystemThread(HalpSplashThread, nullptr, 0, 8);
}

// Before the title starts: wait for the splash, blank the screen (the title sets its own mode) and free it.
void HalpEndSplash() {
    if (!HalpSplashScreen)
        return;
    KeWaitForSingleObject(&HalpSplashDone, Executive, KernelMode, FALSE, nullptr);
    RtlZeroMemory(HalpSplashScreen, SPLASH_PITCH * SPLASH_H);
    auto crtc = (volatile UCHAR *)(0xFD000000 + 0x6013D4);
    crtc[0] = 0x28; // screen off until the title sets its mode (both AvSetDisplayMode paths turn it back on)
    crtc[1] = 0;
    MmFreeContiguousMemory(HalpSplashScreen);
    if (HalpSplashImage)
        MmFreeContiguousMemory(HalpSplashImage);
    HalpSplashScreen = HalpSplashImage = nullptr;
}
