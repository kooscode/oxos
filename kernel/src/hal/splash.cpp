// Boot splash (the reference's INIT-section animation is not reproduced): on cold boot a system thread fades in a
// 640x480 32 bpp picture: the PNG that tools/mkbios.py stores in flash bank 1 (spec/oxos-internal/00-boot-splash.md), or, without one, a
// drawn emblem and caption. The title launcher waits for it, turns the screen off and frees its memory before the title
// starts; the title turns the screen on with its first frame.
#include <xk/serial.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>
#include <xk/rtl.h>

#include "jingle.h"

// The splash runs only during a cold boot: its code is boot-only (freed before the first title starts, spec/oxos-internal/
// 18-boot-memory.md), except HalpEndSplash, which every title start calls.
#pragma code_seg(".xkb")

XBAPI void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
XBAPI UCHAR HalBootSMCVideoMode;

constexpr ULONG SPLASH_W = 640, SPLASH_H = 480, SPLASH_PITCH = SPLASH_W * 4;
// The fade-in follows the clock from the jingle's start and reaches full intensity as the jingle's build-up lands on
// its final chord (jingle::ClimaxMs), a frame about every 40 ms; the picture then stays up until the title shows its
// own frame.
constexpr ULONG SPLASH_FRAME_MS = 40;
// How long the splash stays, from the start of the fade, before the first title (dashboard or game) starts: a cold
// boot shows it for a few seconds, as the retail boot animation, also when the title is ready sooner.
constexpr ULONG SPLASH_SHOWN_MS = 5500;

static KEVENT HalpSplashDone;
static PKTHREAD HalpSplashThreadObject; // waited for before the title starts, so its stack is freed by then
static ULONG *HalpSplashScreen, *HalpSplashImage;
volatile ULONG HalpSplashFrames; // frames shown (boot tests)
volatile ULONG HalpSplashFadeMs, HalpSplashFullMs; // the fade's planned length, and when full intensity was drawn
volatile ULONG HalpSplashSource; // 1 = drawn emblem, 2 = flash image (boot tests)
volatile ULONG HalpSplashStatus; // flash image: 0 = shown, else why not (header 0x100+, HalpDecodePng step 1-9)

ULONG HalpPngScratchSize(ULONG Width, ULONG Height);
ULONG HalpDecodePng(const UCHAR *Png, ULONG Size, ULONG *Pixels, ULONG Width, ULONG Height, UCHAR *Scratch);

// "XKSPLASH", version 1, PNG length, CRC-32 (checked by the PNG's own Adler-32 instead), PNG: tools/mkbios.py puts it
// in flash bank 1 of a 1 MiB image, else in the boot bank; the 2BL copies it into RAM for the kernel.
constexpr ULONG SPLASH_HEADER = 20;

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
static ULONG HalpDecodeSplash(const UCHAR *Splash, ULONG Room, ULONG *img) {
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

// The 2BL's copy of the splash (boot/bl.cpp BlCopySplash): decoded from RAM, then its pages are freed. Without one (an
// older 2BL, or no splash in the image) the emblem is drawn; the kernel does not read the flash itself.
extern ULONG HalpSplashRamPhys, HalpSplashRamSize;
static ULONG HalpLoadSplash(ULONG *img) {
    if (!HalpSplashRamSize)
        return 0x100;
    ULONG status = HalpDecodeSplash((const UCHAR *)(MM_SYSTEM_PHYSICAL_MAP | HalpSplashRamPhys), HalpSplashRamSize, img);
    ULONG first = HalpSplashRamPhys >> PAGE_SHIFT, end = (HalpSplashRamPhys + HalpSplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    HalpSplashRamSize = 0;
    for (ULONG pfn = first; pfn < end; pfn++)
        MiFreeContiguousPage(pfn);
    return status;
}

void HalpStartBootSound();
void HalpBootSoundGo();
void HalpEndBootSound();

ULONG AvBootModeWord();
void AvPrepareBootDisplay();
void AvLoadBootGamma();


static void NTAPI HalpSplashThread(PVOID) {
    RtlZeroMemory(HalpSplashScreen, SPLASH_PITCH * SPLASH_H);
    HalpSplashStatus = HalpLoadSplash(HalpSplashImage);
    XK_SERIAL("splash: image status %#lx\n", HalpSplashStatus);
    if (!HalpSplashStatus) {
        HalpSplashSource = 2;
    } else {
        RtlZeroMemory(HalpSplashImage, SPLASH_PITCH * SPLASH_H);
        HalpDrawEmblem(HalpSplashImage);
        HalpDrawText(HalpSplashImage, "OPEN XBOX KERNEL", 350, 3, 0xE0E0E0);
        HalpSplashSource = 1;
    }
    // The reference boot display (spec/stock/22 §7-8): device set-up, the AV pack's mode word, then the screen on.
    AvPrepareBootDisplay();
    AvSetDisplayMode((PVOID)0xFD000000, 0, AvBootModeWord(), 0x1E, SPLASH_PITCH, MmGetPhysicalAddress(HalpSplashScreen));
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 0, nullptr);
    XK_SERIAL("splash: display mode set\n");
    {
        // The colour table, now that the encoder clocks the display: after the next vertical blank (PCRTC interrupt
        // status bit 0; at most 100 ms).
        auto pcrtc = (volatile ULONG *)0xFD600100;
        *pcrtc = 1;
        for (ULONG ms = 0; ms < 100 && !(*pcrtc & 1); ms++)
            KeStallExecutionProcessor(1000);
        if (*pcrtc & 1) {
            AvLoadBootGamma();
            XK_SERIAL("splash: colour table loaded\n");
        } else {
            XK_SERIAL("splash: no vertical blank within 100 ms: colour table left alone\n");
        }
    }
    HalpBootSoundGo(); // the jingle starts with the fade-in
    const ULONG fadeMs = jingle::ClimaxMs();
    const ULONGLONG start = KeQueryInterruptTime(); // 100 ns units
    auto elapsedMs = [&] { return (ULONG)((KeQueryInterruptTime() - start) / 10000); };
    HalpSplashFadeMs = fadeMs;
    for (;;) {
        ULONG ms = elapsedMs(), level = ms >= fadeMs ? 256 : ms * 256 / fadeMs; // intensity, 256 = full
        for (ULONG i = 0; i < SPLASH_W * SPLASH_H; i++) {
            ULONG p = HalpSplashImage[i];
            if (!p)
                continue;
            ULONG r = (p >> 16 & 0xFF) * level >> 8, g = (p >> 8 & 0xFF) * level >> 8, b = (p & 0xFF) * level >> 8;
            HalpSplashScreen[i] = r << 16 | g << 8 | b;
        }
        HalpSplashFrames = HalpSplashFrames + 1;
        if (level == 256) {
            HalpSplashFullMs = ms;
            break;
        }
        // The next frame in 40 ms, or at the full-intensity time if that comes first.
        LARGE_INTEGER t;
        t.QuadPart = -10000LL * (fadeMs - ms < SPLASH_FRAME_MS ? fadeMs - ms : SPLASH_FRAME_MS);
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    XK_SERIAL("splash: faded in\n");
    if (ULONG ms = elapsedMs(); ms < SPLASH_SHOWN_MS) {
        LARGE_INTEGER t;
        t.QuadPart = -10000LL * (SPLASH_SHOWN_MS - ms);
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    XK_SERIAL("splash: shown\n");
    KeSetEvent(&HalpSplashDone, 0, FALSE);
}

#pragma code_seg()
#pragma code_seg(".xkb")

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
    HalpStartBootSound(); // synthesizing before the splash thread needs it
    HalpSplashThreadObject = KiCreateSystemThread(HalpSplashThread, nullptr, 0, 8);
}

#pragma code_seg()

// The boot-only pages, keeping the tables of the console's video encoder (once; quick reboots find them freed).
void AvTablesInUse(ULONG_PTR *Start, ULONG_PTR *End);
static void HalpFreeBootPages() {
    ULONG_PTR start, end;
    AvTablesInUse(&start, &end);
    MmFreeBootPages(start, end);
}

// Before the title starts: wait for the splash, turn the screen off and free the splash's memory, so the title gets
// the whole budget (spec/stock/09 §7). The reference frees its boot animation's framebuffer too (it is in the INIT
// section); the title's display-mode change and first frame turn the screen on again.
void HalpEndSplash() {
    if (!HalpSplashScreen) {
        HalpFreeBootPages();
        return;
    }
    KeWaitForSingleObject(&HalpSplashDone, Executive, KernelMode, FALSE, nullptr);
    if (HalpSplashThreadObject) { // until it has exited: the next switch back frees its stack
        KeWaitForSingleObject(HalpSplashThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(HalpSplashThreadObject);
        HalpSplashThreadObject = nullptr;
    }
    HalpEndBootSound(); // the jingle has ended and the audio controller is stopped
    // Screen off until the title shows its first frame: its graphics set-up writes the memory the splash is scanned
    // out of (and, freed below, may reuse it), which garbled the last splash frame on screen. A title's display-mode
    // change leaves the screen off too and turns it on itself (option 9 with 0, spec/stock/22).
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 1, nullptr);
    MmFreeContiguousMemory(HalpSplashImage);
    MmFreeContiguousMemory(HalpSplashScreen);
    HalpSplashScreen = HalpSplashImage = nullptr;
    HalpFreeBootPages(); // the splash and the jingle have ended: their code goes
}

// --- The 2BL's splash record (boot/bl.cpp: 'OXSP', physical address, size at physical 0x2380), taken on a cold boot
// before the memory manager starts, which keeps those pages until the splash has been decoded. Normal code: the memory
// manager asks on quick reboots too.
ULONG HalpSplashRamPhys, HalpSplashRamSize;

void HalpTakeSplashRecord(bool QuickReboot) {
    HalpSplashRamPhys = HalpSplashRamSize = 0;
    auto record = (volatile ULONG *)0x80002380;
    if (QuickReboot || record[0] != 0x5053584F)
        return;
    ULONG phys = record[1], size = record[2];
    record[0] = 0;
    if (size && size <= 0x40000 && !(phys & (PAGE_SIZE - 1)) && phys >= 0x100000 && phys + size <= 0x3000000)
        HalpSplashRamPhys = phys, HalpSplashRamSize = size;
}

bool HalpIsSplashPage(ULONG pfn) {
    return HalpSplashRamSize && pfn >= (HalpSplashRamPhys >> PAGE_SHIFT) &&
           pfn < ((HalpSplashRamPhys + HalpSplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
}
