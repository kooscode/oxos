// Boot splash (the reference's INIT-section animation is not reproduced): on cold boot a system thread fades in a 640x480 32 bpp picture from the vector scene
// that tools/mkbios.py stores in flash (spec/oxos_internal/00_boot_splash.md), or, without one, a drawn emblem and caption, and holds it for a few seconds
// while the controllers are read (X: the File Manager, B: safe mode). No animation and no sound since 2026-11-17 (user: "remove the boot animation and jingle,
// just fast fade the splash, keep it there for a while then move on"). The title launcher waits for it, turns the screen off and frees its memory before the
// title starts; the title turns the screen on with its first frame.
#include <boot_splash/boot_splash.h>
#include <diagnostics/diagnostics.h>
#include <core/core.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <runtime/runtime.h>
#include <hardware/chipset/chipset.h>
#include <hardware/video/video.h>

#include <gamepad/gamepad.h>
#include <graphics/graphics.h>
#include <graphics/vector_renderer.h>

namespace xk::boot_splash {

// The splash runs only during a cold boot: its code is boot-only (freed before the first title starts, spec/oxos_internal/18_boot_memory.md), except EndSplash,
// which every title start calls.
#pragma code_seg(".xkb")

constexpr ULONG SPLASH_W = 640, SPLASH_H = 480, SPLASH_PITCH = SPLASH_W * 4;
// The timeline, from the screen coming on (spec/oxos_internal/00 §2): a fast fade from black to the picture (a frame about every 40 ms), then the picture held
// until SPLASH_SHOW_MS, before the first title starts.
constexpr ULONG SPLASH_FRAME_MS = 40;
constexpr ULONG SPLASH_FADE_MS = 300;  // the fade from black
constexpr ULONG SPLASH_SHOW_MS = 4000; // the picture on screen (the fade included): the time to hold X or B

// "XKSPLASH", version 2 (1 was a PNG, no longer supported), length, CRC-32 (the scene's own checks are used instead), then the scene: tools/mkbios.py puts it
// in flash bank 1 of a 1 MiB image, else in the boot bank; the 2BL copies it into RAM for the kernel.
constexpr ULONG SPLASH_HEADER = 20;

static KEVENT SplashDone;                      // the splash has finished (at once without an AV pack)
static PKTHREAD SplashThreadObject;            // waited for before the title starts, so its stack is freed by then
static ULONG *SplashScreen, *SplashImage;      // the scanned-out frame; the picture
volatile ULONG SplashFrames;                   // frames shown (boot tests)
volatile ULONG SplashFullMs;                   // when the fade reached the full picture, after the screen came on (boot tests)
volatile ULONG SplashSource;                   // 1 = drawn emblem, 2 = the vector scene from flash (boot tests)
volatile ULONG SplashStatus; // the scene: 0 = drawn, else why not (header 0x100+, 0x200 + the renderer's check)
// The 2BL's copy of the scene in RAM (TakeSplashRecord), freed once it has been drawn.
ULONG SplashRamPhysical, SplashRamSize;

// The hint for the File Manager and the OXOS version and video encoder (VersionLabel), small and grey, centred at the bottom, inside the part of the picture a
// TV shows: [ Hold (X) for File Manager - v0.4.2/CONEXANT ].
static void DrawSplashLabel(ULONG *img) {
    static const char hint[] = "[ Hold  X  for File Manager - "; // the X drawn as the button (DrawText)
    char label[64];
    memcpy(label, hint, sizeof(hint));
    ULONG n = sizeof(hint) - 1;
    graphics::VersionLabel(label + n, sizeof(label) - n - 3);
    while (label[n])
        n++;
    label[n] = ' ';
    label[n + 1] = ']';
    label[n + 2] = 0;
    constexpr ULONG scale = 2, margin = 40;
    graphics::DrawCentredText(img, SPLASH_H - margin - 7 * scale, label, scale, 0x707070);
}

static LONG Abs(LONG v) {
    return v < 0 ? -v : v;
}

// The emblem, as the logo's X: two crossed bars in green, lighter towards the top, with a soft edge (coverage from the distance in pixels), and a glow around
// them.
static void DrawEmblem(ULONG *img) {
    const LONG cx = SPLASH_W / 2, cy = 200, half = 80, bar = 15, glow = 24;
    for (LONG y = cy - half - glow; y <= cy + half + glow; y++) {
        for (LONG x = cx - half - glow; x <= cx + half + glow; x++) {
            LONG dx = x - cx, dy = y - cy;
            LONG a = Abs(dx - dy), b = Abs(dx + dy), m = a < b ? a : b; // distance from a bar x sqrt(2)
            LONG along = Abs(dx) > Abs(dy) ? Abs(dx) : Abs(dy);         // how far out along it
            LONG edge = bar * 141 / 100, d = (m - edge) * 100 / 141;
            LONG out = along - half > d ? along - half : d; // distance outside the X, pixels
            LONG v = out <= 0 ? 255 : out <= 2 ? 255 - out * 100 : 0;
            LONG g = out < glow ? (glow - out) * 70 / glow : 0; // the glow
            if (!v && !g)
                continue;
            LONG shade = 150 + (cy + half - y) * 105 / (2 * half); // lighter towards the top
            LONG green = v * shade / 255 + g * (255 - v) / 255;
            img[y * SPLASH_W + x] = (ULONG)((green * 0x8B / 0xFF) << 16 | green << 8 | (green * 0x1F / 0xFF));
        }
    }
}

// A vector scene: its end picture (the green X and its glow; the scene's start picture is not used) into img. 0 = drawn.
static ULONG DrawVectorSplash(const UCHAR *Scene, ULONG Size, ULONG *img) {
    unsigned scratchSize;
    if (unsigned step = oxos::graphics::vector_renderer::Check(Scene, Size, &scratchSize))
        return 0x200 + step;
    auto scratch = MmAllocateContiguousMemoryEx(scratchSize, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (!scratch)
        return 0x103;
    oxos::graphics::vector_renderer::Render(Scene, Size, oxos::graphics::vector_renderer::END, (unsigned *)img, scratch);
    MmFreeContiguousMemory(scratch);
    SplashSource = 2;
    return 0;
}

// Draw the splash at Splash (Room bytes available after it); 0 = drawn.
static ULONG DecodeSplash(const UCHAR *Splash, ULONG Room, ULONG *img) {
    static const char magic[8] = {'X', 'K', 'S', 'P', 'L', 'A', 'S', 'H'};
    for (int i = 0; i < 8; i++) {
        if (Splash[i] != (UCHAR)magic[i])
            return 0x101; // no splash here
    }
    ULONG version = *(const ULONG *)(Splash + 8), length = *(const ULONG *)(Splash + 12);
    if (version != 2 || Room < SPLASH_HEADER || length > Room - SPLASH_HEADER)
        return 0x102;
    return DrawVectorSplash(Splash + SPLASH_HEADER, length, img);
}

// The 2BL's copy of the splash (boot/bl.cpp BlCopySplash): drawn from RAM, then its pages are freed. Without one (an older 2BL, or no splash in the image) the
// emblem is drawn; the kernel does not read the flash itself.
static ULONG LoadSplash(ULONG *img) {
    if (!SplashRamSize)
        return 0x100;
    ULONG status = DecodeSplash((const UCHAR *)(MM_SYSTEM_PHYSICAL_MAP | SplashRamPhysical), SplashRamSize, img);
    ULONG first = SplashRamPhysical >> PAGE_SHIFT, end = (SplashRamPhysical + SplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    SplashRamSize = 0;
    for (ULONG pfn = first; pfn < end; pfn++)
        memory::FreeContiguousPage(pfn);
    return status;
}

static void NTAPI SplashThread(PVOID) {
    memset(SplashScreen, 0, SPLASH_PITCH * SPLASH_H);
    SplashStatus = LoadSplash(SplashImage);
    XK_SERIAL("splash: image status %#lx\n", SplashStatus);
    if (SplashStatus) { // no scene: the emblem and the name
        memset(SplashImage, 0, SPLASH_PITCH * SPLASH_H);
        DrawEmblem(SplashImage);
        graphics::DrawCentredText(SplashImage, 330, "OXOS", 6, 0xA8B4A0);
        SplashSource = 1;
    }
    DrawSplashLabel(SplashImage);
    // The reference boot display (spec/stock/22 §7-8): device set-up, the AV pack's mode word, then the screen on.
    hardware::video::PrepareBootDisplay();
    graphics::ShowScreen(MmGetPhysicalAddress(SplashScreen));
    XK_SERIAL("splash: display mode set\n");
    {
        // The colour table, now that the encoder clocks the display: after the next vertical blank (PCRTC interrupt status bit 0; at most 100 ms).
        auto pcrtc = (volatile ULONG *)0xFD600100;
        *pcrtc = 1;
        for (ULONG ms = 0; ms < 100 && !(*pcrtc & 1); ms++)
            KeStallExecutionProcessor(1000);
        if (*pcrtc & 1) {
            hardware::video::LoadBootGamma();
            XK_SERIAL("splash: colour table loaded\n");
        } else {
            XK_SERIAL("splash: no vertical blank within 100 ms: colour table left alone\n");
        }
    }
    // Sleeps, reading the controllers about every 10 ms meanwhile (spec/oxos_internal/32 §2).
    auto sleepMs = [](ULONG ms) {
        for (ULONG left = ms; left;) {
            gamepad::PollControllers();
            ULONG step = left < 10 ? left : 10;
            LARGE_INTEGER t;
            t.QuadPart = -10000LL * step;
            KeDelayExecutionThread(KernelMode, FALSE, &t);
            left -= step;
        }
    };
    // Draws the picture at brightness level (0-256).
    auto draw = [&](ULONG level) {
        for (ULONG i = 0; i < SPLASH_W * SPLASH_H; i++) {
            ULONG p = SplashImage[i];
            if (!p)
                continue;
            ULONG out = 0;
            for (ULONG sh = 0; sh < 24; sh += 8) {
                out |= (((p >> sh & 0xFF) * level) >> 8) << sh;
            }
            SplashScreen[i] = out;
        }
        SplashFrames = SplashFrames + 1;
    };
    const ULONGLONG shown = KeQueryInterruptTime(); // 100 ns units
    auto sinceMs = [](ULONGLONG from100ns) {
        return (ULONG)((KeQueryInterruptTime() - from100ns) / 10000);
    };
    // 1. The fade from black.
    for (ULONG ms; (ms = sinceMs(shown)) < SPLASH_FADE_MS;) {
        draw(ms * 256 / SPLASH_FADE_MS);
        sleepMs(SPLASH_FADE_MS - ms < SPLASH_FRAME_MS ? SPLASH_FADE_MS - ms : SPLASH_FRAME_MS);
    }
    draw(256);
    SplashFullMs = sinceMs(shown);
    // 2. The picture held: the controllers are set up (it blocks for a few hundred milliseconds), then read until the splash ends: X opens the File Manager
    // (spec/oxos_internal/32), B starts in safe mode (spec/oxos_internal/34 §3.3).
    gamepad::StartWatchingControllers();
    if (ULONG ms = sinceMs(shown); ms < SPLASH_SHOW_MS)
        sleepMs(SPLASH_SHOW_MS - ms);
    XK_SERIAL("splash: shown\n");
    gamepad::StopWatchingControllers();
    KeSetEvent(&SplashDone, 0, FALSE);
}

// Cold boot, with an AV pack connected.
void StartSplash() {
    KeInitializeEvent(&SplashDone, NotificationEvent, TRUE);
    if (HalBootSMCVideoMode == 7)
        return;
    // Low memory, like the reference keeps its boot framebuffer (in the freed INIT section, spec/stock/05 §5): titles assume the top of RAM holds only a little
    // of the kernel's own memory when they start (an nxdk title clears a framebuffer at 0x3C00000 it never allocated).
    SplashScreen = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    SplashImage = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (!SplashScreen || !SplashImage) {
        if (SplashScreen)
            MmFreeContiguousMemory(SplashScreen);
        SplashScreen = nullptr;
        return;
    }
    KeResetEvent(&SplashDone);
    SplashThreadObject = core::CreateSystemThread(SplashThread, nullptr, 0, 8);
}

void WaitForSplash() {
    KeWaitForSingleObject(&SplashDone, Executive, KernelMode, FALSE, nullptr);
}

#pragma code_seg()

// The boot-only pages, keeping the tables of the console's video encoder (once; quick reboots find them freed).
static void HalpFreeBootPages() {
    ULONG_PTR start, end;
    hardware::video::VideoTablesInUse(&start, &end);
    memory::FreeBootPages(start, end);
}

// Wait for the splash to end, turn the screen off and free the splash's memory. StopSplash keeps the boot-only pages: the format countdown and
// the upload server take the screen and still run from them (spec/oxos_internal/30 §4, 31 §6). EndSplash, before a title starts, frees them too, so the title
// gets the whole budget (spec/stock/09 §7; the reference frees its boot animation's framebuffer, in its INIT section, too); the title's display-mode change and
// first frame turn the screen on again.
// KeepPicture (EndSplash): the scanned-out frame is not blanked or freed here but handed to the video code, which frees it at the next mode set that shows
// another frame buffer.
static void FinishSplash(bool KeepPicture) {
    if (!SplashScreen)
        return;
    KeWaitForSingleObject(&SplashDone, Executive, KernelMode, FALSE, nullptr);
    if (SplashThreadObject) { // until it has exited: the next switch back frees its stack
        KeWaitForSingleObject(SplashThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(SplashThreadObject);
        SplashThreadObject = nullptr;
    }
    // Screen off until the title shows its first frame: its graphics set-up writes the memory the splash is scanned out of (and, freed below, may reuse it),
    // which garbled the last splash frame on screen. A title's display-mode change leaves the screen off too and turns it on itself (option 9 with 0,
    // spec/stock/22).
    if (KeepPicture) {
        hardware::video::KeepScreenUntilNextMode(SplashScreen);
    } else {
        AvSendTVEncoderOption((PVOID)0xFD000000, 9, 1, nullptr);
        MmFreeContiguousMemory(SplashScreen);
    }
    MmFreeContiguousMemory(SplashImage);
    SplashScreen = SplashImage = nullptr;
}

void StopSplash() {
    FinishSplash(false);
}

void EndSplash(bool KeepPicture) {
    FinishSplash(KeepPicture);
    HalpFreeBootPages(); // the splash has ended: the boot-only code goes
}

// --- The 2BL's splash record (boot/bl.cpp: 'OXSP', physical address, size at physical 0x2380), taken on a cold boot
// before the memory manager starts, which keeps those pages until the splash has been decoded. Normal code: the memory manager asks on quick reboots too.

void TakeSplashRecord(bool QuickReboot) {
    SplashRamPhysical = SplashRamSize = 0;
    auto record = (volatile ULONG *)0x80002380;
    if (QuickReboot || record[0] != 0x5053584F)
        return;
    ULONG phys = record[1], size = record[2];
    record[0] = 0;
    if (size && size <= 0x40000 && !(phys & (PAGE_SIZE - 1)) && phys >= 0x100000 && phys + size <= 0x3000000) {
        SplashRamPhysical = phys;
        SplashRamSize = size;
    }
}

bool IsSplashPage(ULONG pfn) {
    return SplashRamSize && pfn >= (SplashRamPhysical >> PAGE_SHIFT) && pfn < ((SplashRamPhysical + SplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

} // namespace xk::boot_splash
