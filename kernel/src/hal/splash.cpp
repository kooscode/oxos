// Boot splash (the reference's INIT-section animation is not reproduced): on cold boot a system thread fades in a
// 640x480 32 bpp picture: the PNG that tools/mkbios.py stores in flash bank 1 (spec/oxos-internal/00-boot-splash.md), or, without one, a
// drawn emblem and caption. The title launcher waits for it and frees its memory before the title starts, leaving the
// last frame on screen, as the reference does with its animation.
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>
#include <xk/rtl.h>

XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
XBAPI UCHAR HalBootSMCVideoMode;

constexpr ULONG SPLASH_W = 640, SPLASH_H = 480, SPLASH_PITCH = SPLASH_W * 4;
// ~1 s fade-in, running alongside the rest of boot; the picture then stays up until the title shows its own frame.
constexpr ULONG SPLASH_FADE_FRAMES = 24, SPLASH_FRAME_MS = 40;

static KEVENT HalpSplashDone;
static PKTHREAD HalpSplashThreadObject; // waited for before the title starts, so its stack is freed by then
static ULONG *HalpSplashScreen, *HalpSplashImage;
volatile ULONG HalpSplashFrames; // frames shown (boot tests)
volatile ULONG HalpSplashSource; // 1 = drawn emblem, 2 = flash image (boot tests)
volatile ULONG HalpSplashStatus; // flash image: 0 = shown, else why not (header 0x100+, HalpDecodePng step 1-9)

ULONG HalpPngScratchSize(ULONG Width, ULONG Height);
ULONG HalpDecodePng(const UCHAR *Png, ULONG Size, ULONG *Pixels, ULONG Width, ULONG Height, UCHAR *Scratch);

// "XKSPLASH", version 1, PNG length, CRC-32 (checked by the PNG's own Adler-32 instead), PNG: in flash bank 1 of a
// 1 MiB image, else (a 256 KiB image) the copy in the boot bank that the payload header at bank offset 0x6000 points
// to (words 6 and 7: offset and size, 0 when there is none).
constexpr ULONG SPLASH_FLASH = 0xFFF40000, SPLASH_FLASH_SIZE = 0x40000, SPLASH_HEADER = 20;
constexpr ULONG SPLASH_BOOT_BANK = 0xFFFC0000, SPLASH_PAYLOAD = 0x6000, SPLASH_PAYLOAD_MAGIC = 0x504B584F;

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

// Decode the splash at Splash (Room bytes available after it); 0 = shown.
static ULONG HalpDecodeFlashSplash(const UCHAR *Splash, ULONG Room, ULONG *img) {
    static const char magic[8] = {'X', 'K', 'S', 'P', 'L', 'A', 'S', 'H'};
    for (int i = 0; i < 8; i++)
        if (Splash[i] != (UCHAR)magic[i])
            return 0x101; // no splash here
    ULONG length = *(const ULONG *)(Splash + 12);
    if (*(const ULONG *)(Splash + 8) != 1 || Room < SPLASH_HEADER || length > Room - SPLASH_HEADER)
        return 0x102;
    ULONG scratchSize = HalpPngScratchSize(SPLASH_W, SPLASH_H);
    auto scratch = (UCHAR *)MmAllocateContiguousMemoryEx(scratchSize, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    ULONG status = scratch ? HalpDecodePng(Splash + SPLASH_HEADER, length, img, SPLASH_W, SPLASH_H, scratch) : 0x103;
    if (scratch)
        MmFreeContiguousMemory(scratch);
    return status;
}

static ULONG HalpLoadFlashSplash(ULONG *img) {
    auto flash = (const UCHAR *)MmMapIoSpace(SPLASH_FLASH, SPLASH_FLASH_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (!flash)
        return 0x100;
    ULONG status = HalpDecodeFlashSplash(flash, SPLASH_FLASH_SIZE, img);
    MmUnmapIoSpace((PVOID)flash, SPLASH_FLASH_SIZE);
    if (status != 0x101)
        return status;
    // No bank 1 splash (a 256 KiB chip mirrors the boot bank there): the boot bank's own copy.
    auto bank = (const UCHAR *)MmMapIoSpace(SPLASH_BOOT_BANK, SPLASH_FLASH_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (!bank)
        return 0x100;
    auto payload = (const ULONG *)(bank + SPLASH_PAYLOAD);
    ULONG offset = payload[6], size = payload[7];
    if (payload[0] == SPLASH_PAYLOAD_MAGIC && offset >= SPLASH_PAYLOAD + 0x40 && size >= SPLASH_HEADER &&
        offset < SPLASH_FLASH_SIZE && size <= SPLASH_FLASH_SIZE - offset)
        status = HalpDecodeFlashSplash(bank + offset, size, img);
    MmUnmapIoSpace((PVOID)bank, SPLASH_FLASH_SIZE);
    return status;
}

static void NTAPI HalpSplashThread(PVOID) {
    RtlZeroMemory(HalpSplashScreen, SPLASH_PITCH * SPLASH_H);
    HalpSplashStatus = HalpLoadFlashSplash(HalpSplashImage);
    if (!HalpSplashStatus) {
        HalpSplashSource = 2;
    } else {
        RtlZeroMemory(HalpSplashImage, SPLASH_PITCH * SPLASH_H);
        HalpDrawEmblem(HalpSplashImage);
        HalpDrawText(HalpSplashImage, "OPEN XBOX KERNEL", 350, 3, 0xE0E0E0);
        HalpSplashSource = 1;
    }
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
    KeSetEvent(&HalpSplashDone, 0, FALSE);
}

// Cold boot, with an AV pack connected.
void HalpStartSplash() {
    KeInitializeEvent(&HalpSplashDone, NotificationEvent, TRUE);
    if (HalBootSMCVideoMode == 7)
        return;
    // Low memory, like the reference keeps its boot framebuffer (in the freed INIT section, spec/stock/05 §5): titles
    // assume the top of RAM holds only a little of the kernel's own memory when they start (an nxdk title clears a
    // framebuffer at 0x3C00000 it never allocated).
    HalpSplashScreen = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE);
    HalpSplashImage = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (!HalpSplashScreen || !HalpSplashImage) {
        if (HalpSplashScreen)
            MmFreeContiguousMemory(HalpSplashScreen);
        HalpSplashScreen = nullptr;
        return;
    }
    KeResetEvent(&HalpSplashDone);
    HalpSplashThreadObject = KiCreateSystemThread(HalpSplashThread, nullptr, 0, 8);
}

// Before the title starts: wait for the splash and free its memory, but leave the scanout on its last frame. The
// reference does the same with its boot animation (its framebuffer is in the INIT section, freed before the title
// starts), so the title gets the whole budget; freed pages go to the end of the free lists, so the picture usually
// stays up until the title sets its own display mode (spec/stock/09 §7).
void HalpEndSplash() {
    if (!HalpSplashScreen)
        return;
    KeWaitForSingleObject(&HalpSplashDone, Executive, KernelMode, FALSE, nullptr);
    if (HalpSplashThreadObject) { // until it has exited: the next switch back frees its stack
        KeWaitForSingleObject(HalpSplashThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(HalpSplashThreadObject);
        HalpSplashThreadObject = nullptr;
    }
    MmFreeContiguousMemory(HalpSplashImage);
    MmFreeContiguousMemory(HalpSplashScreen);
    HalpSplashScreen = HalpSplashImage = nullptr;
}
