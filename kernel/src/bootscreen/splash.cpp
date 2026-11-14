// Boot splash (the reference's INIT-section animation is not reproduced): on cold boot a system thread fades in a
// 640x480 32 bpp picture from the vector scene that tools/mkbios.py stores in flash
// (spec/oxos_internal/00_boot_splash.md), drawn twice (the start and the end of a cross-fade), or, without one, a drawn
// emblem and caption. The title launcher waits for it, turns the screen off and frees its memory before the title
// starts; the title turns the screen on with its first frame.
#include <xk/bootscreen.h>
#include <xk/serial.h>
#include <xk/core.h>
#include <xk/memory.h>
#include <xk/objects.h>
#include <xk/runtime.h>
#include <xk/hardware.h>

#include <xk/gamepad.h>
#include <xk/jingle.h>
#include <xk/vsplash.h>

namespace xk::bootscreen {

// The splash runs only during a cold boot: its code is boot-only (freed before the first title starts,
// spec/oxos_internal/18_boot_memory.md), except EndSplash, which every title start calls.
#pragma code_seg(".xkb")

constexpr ULONG SPLASH_W = 640, SPLASH_H = 480, SPLASH_PITCH = SPLASH_W * 4;
// The timeline, from the screen coming on (spec/oxos_internal/00 §2): a short fade from black to the start picture
// (the metal logo; the emblem has only the one picture), which stays until SPLASH_LOGO_MS;
// then the jingle starts and the picture cross-fades to the end picture (the green X and its glow), reaching it as the
// jingle's build-up lands on its final chord (jingle::ClimaxMs after its start); the end picture then stays
// until the jingle's final chord has died away (jingle::LengthMs), before the first title starts. A frame about
// every 40 ms while anything changes.
constexpr ULONG SPLASH_FRAME_MS = 40;
constexpr ULONG SPLASH_INTRO_MS = 500;  // the fade from black
constexpr ULONG SPLASH_LOGO_MS = 1000;  // the start picture alone (the fade included), before the jingle

// "XKSPLASH", version 2 (1 was a PNG, no longer supported), length, CRC-32 (the scene's own checks are used instead),
// then the scene: tools/mkbios.py puts it in flash bank 1 of a 1 MiB image, else in the boot bank; the 2BL
// copies it into RAM for the kernel.
constexpr ULONG SPLASH_HEADER = 20;

static KEVENT SplashDone;                      // the splash has finished (at once without an AV pack)
static PKTHREAD SplashThreadObject;            // waited for before the title starts, so its stack is freed by then
static ULONG *SplashScreen, *SplashImage;  // the scanned-out frame; the end picture
static ULONG *HalpSplashStart;                     // a vector splash's start picture; else none
volatile ULONG SplashFrames;                   // frames shown (boot tests)
volatile ULONG SplashFadeMs, SplashFullMs; // the fade's planned length, and when full intensity was drawn
volatile ULONG SplashSource;                   // 1 = drawn emblem, 2 = the vector scene from flash (boot tests)
volatile ULONG SplashStatus; // the scene: 0 = drawn, else why not (header 0x100+, 0x200 + vsplash::Check's step)
// The 2BL's copy of the scene in RAM (TakeSplashRecord), freed once it has been drawn.
ULONG SplashRamPhysical, SplashRamSize;

// Text centred on the screen's width.
static void DrawSplashText(ULONG *img, const char *text, ULONG y, ULONG scale, ULONG color) {
    DrawText(img, SPLASH_W, (SPLASH_W - TextWidth(text, scale)) / 2, y, text, scale, color);
}

// The hint for the File Manager and the OXOS version and video encoder (VersionLabel), small and grey, centred at the
// bottom, inside the part of the picture a TV shows: [ Hold (X) for File Manager - v0.4.2/CONEXANT ].
static void DrawSplashLabel(ULONG *img) {
    static const char hint[] = "[ Hold  X  for File Manager - "; // the X drawn as the button (DrawText)
    char label[64];
    RtlCopyMemory(label, hint, sizeof(hint));
    ULONG n = sizeof(hint) - 1;
    VersionLabel(label + n, sizeof(label) - n - 3);
    while (label[n])
        n++;
    label[n] = ' ';
    label[n + 1] = ']';
    label[n + 2] = 0;
    constexpr ULONG scale = 2, margin = 40;
    DrawSplashText(img, label, SPLASH_H - margin - 7 * scale, scale, 0x707070);
}

static LONG Abs(LONG v) {
    return v < 0 ? -v : v;
}

// The emblem, as the logo's X: two crossed bars in green, lighter towards the top, with a soft edge (coverage from
// the distance in pixels), and a glow around them.
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

// A vector scene: the end picture into img, the start picture into HalpSplashStart (when there is memory for it; else
// only the end picture is faded in). 0 = drawn.
static ULONG DrawVectorSplash(const UCHAR *Scene, ULONG Size, ULONG *img) {
    unsigned scratchSize;
    if (unsigned step = vsplash::Check(Scene, Size, &scratchSize))
        return 0x200 + step;
    auto scratch = MmAllocateContiguousMemoryEx(scratchSize, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (!scratch)
        return 0x103;
    vsplash::Render(Scene, Size, vsplash::END, (unsigned *)img, scratch);
    HalpSplashStart = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (HalpSplashStart)
        vsplash::Render(Scene, Size, vsplash::START, (unsigned *)HalpSplashStart, scratch);
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

// The 2BL's copy of the splash (boot/bl.cpp BlCopySplash): drawn from RAM, then its pages are freed. Without one (an
// older 2BL, or no splash in the image) the emblem is drawn; the kernel does not read the flash itself.
static ULONG LoadSplash(ULONG *img) {
    if (!SplashRamSize)
        return 0x100;
    ULONG status = DecodeSplash((const UCHAR *)(MM_SYSTEM_PHYSICAL_MAP | SplashRamPhysical), SplashRamSize,
                                img);
    ULONG first = SplashRamPhysical >> PAGE_SHIFT,
          end = (SplashRamPhysical + SplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    SplashRamSize = 0;
    for (ULONG pfn = first; pfn < end; pfn++)
        memory::FreeContiguousPage(pfn);
    return status;
}

static void NTAPI SplashThread(PVOID) {
    RtlZeroMemory(SplashScreen, SPLASH_PITCH * SPLASH_H);
    SplashStatus = LoadSplash(SplashImage);
    XK_SERIAL("splash: image status %#lx\n", SplashStatus);
    if (SplashStatus) { // no scene: the emblem and the name
        RtlZeroMemory(SplashImage, SPLASH_PITCH * SPLASH_H);
        DrawEmblem(SplashImage);
        DrawSplashText(SplashImage, "OXOS", 330, 6, 0xA8B4A0);
        SplashSource = 1;
    }
    DrawSplashLabel(SplashImage);
    if (HalpSplashStart)
        DrawSplashLabel(HalpSplashStart);
    // The reference boot display (spec/stock/22 §7-8): device set-up, the AV pack's mode word, then the screen on.
    hardware::PrepareBootDisplay();
    AvSetDisplayMode((PVOID)0xFD000000, 0, hardware::BootModeWord(), 0x1E, SPLASH_PITCH,
                     MmGetPhysicalAddress(SplashScreen));
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
            hardware::LoadBootGamma();
            XK_SERIAL("splash: colour table loaded\n");
        } else {
            XK_SERIAL("splash: no vertical blank within 100 ms: colour table left alone\n");
        }
    }
    const ULONG climaxMs = jingle::ClimaxMs();
    const ULONG *from = HalpSplashStart ? HalpSplashStart : SplashImage; // the start picture
    SplashFadeMs = climaxMs;
    const ULONG endMs = jingle::LengthMs(); // the end picture stays until the jingle has died away
    XK_SERIAL("splash: logo for %lu ms, then the jingle and a cross-fade of %lu ms, held to %lu ms\n", SPLASH_LOGO_MS,
              climaxMs, endMs);
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
    // Draws the mix of the start and end pictures (mix 0-256) at brightness level (0-256).
    auto draw = [&](ULONG level, ULONG mix) {
        for (ULONG i = 0; i < SPLASH_W * SPLASH_H; i++) {
            ULONG p = SplashImage[i], q = from[i];
            if (!p && !q)
                continue;
            ULONG out = 0;
            for (ULONG sh = 0; sh < 24; sh += 8) {
                ULONG c = ((q >> sh & 0xFF) * (256 - mix) + (p >> sh & 0xFF) * mix) >> 8;
                out |= (c * level >> 8) << sh;
            }
            SplashScreen[i] = out;
        }
        SplashFrames = SplashFrames + 1;
    };
    // 1. The fade from black to the start picture, which then stays until SPLASH_LOGO_MS.
    const ULONGLONG shown = KeQueryInterruptTime(); // 100 ns units
    auto sinceMs = [](ULONGLONG from100ns) { return (ULONG)((KeQueryInterruptTime() - from100ns) / 10000); };
    for (ULONG ms; (ms = sinceMs(shown)) < SPLASH_INTRO_MS;) {
        draw(ms * 256 / SPLASH_INTRO_MS, 0);
        sleepMs(SPLASH_INTRO_MS - ms < SPLASH_FRAME_MS ? SPLASH_INTRO_MS - ms : SPLASH_FRAME_MS);
    }
    draw(256, 0);
    // The controllers are set up while the start picture stands still (it blocks for a few hundred milliseconds),
    // then read until the jingle ends: X opens the File Manager (spec/oxos_internal/32).
    gamepad::StartWatchingControllers();
    if (ULONG ms = sinceMs(shown); ms < SPLASH_LOGO_MS)
        sleepMs(SPLASH_LOGO_MS - ms);
    // 2. The jingle, and the cross-fade to the end picture (smoothstep: eases in and out), there at the climax.
    PlayBootSound();
    const ULONGLONG jingleStart = KeQueryInterruptTime();
    XK_SERIAL("splash: jingle started\n");
    for (;;) {
        ULONG ms = sinceMs(jingleStart);
        if (ms >= climaxMs) {
            draw(256, 256);
            SplashFullMs = ms;
            break;
        }
        ULONG u = ms * 256 / climaxMs;
        draw(256, u * u * (768 - 2 * u) / 65536);
        sleepMs(climaxMs - ms < SPLASH_FRAME_MS ? climaxMs - ms : SPLASH_FRAME_MS);
    }
    XK_SERIAL("splash: faded in\n");
    // 3. The end picture, held.
    if (ULONG ms = sinceMs(jingleStart); ms < endMs)
        sleepMs(endMs - ms);
    XK_SERIAL("splash: shown\n");
    gamepad::StopWatchingControllers();
    KeSetEvent(&SplashDone, 0, FALSE);
}

// Cold boot, with an AV pack connected.
void StartSplash() {
    KeInitializeEvent(&SplashDone, NotificationEvent, TRUE);
    if (HalBootSMCVideoMode == 7)
        return;
    // Low memory, like the reference keeps its boot framebuffer (in the freed INIT section, spec/stock/05 §5): titles
    // assume the top of RAM holds only a little of the kernel's own memory when they start (an nxdk title clears a
    // framebuffer at 0x3C00000 it never allocated).
    SplashScreen = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0,
                                                         PAGE_READWRITE | PAGE_WRITECOMBINE);
    SplashImage = (ULONG *)MmAllocateContiguousMemoryEx(SPLASH_PITCH * SPLASH_H, 0, 0xFFFFFF, 0, PAGE_READWRITE);
    if (!SplashScreen || !SplashImage) {
        if (SplashScreen)
            MmFreeContiguousMemory(SplashScreen);
        SplashScreen = nullptr;
        return;
    }
    KeResetEvent(&SplashDone);
    StartBootSound(); // synthesizing before the splash thread needs it
    SplashThreadObject = core::CreateSystemThread(SplashThread, nullptr, 0, 8);
}

void WaitForSplash() {
    KeWaitForSingleObject(&SplashDone, Executive, KernelMode, FALSE, nullptr);
}

#pragma code_seg()

// The boot-only pages, keeping the tables of the console's video encoder (once; quick reboots find them freed).
static void HalpFreeBootPages() {
    ULONG_PTR start, end;
    hardware::VideoTablesInUse(&start, &end);
    memory::FreeBootPages(start, end);
}

// Wait for the splash and the jingle to end, turn the screen off and free the splash's memory. StopSplash keeps
// the boot-only pages: the format countdown and the upload server take the screen and still run from them
// (spec/oxos_internal/30 §4, 31 §6). EndSplash, before a title starts, frees them too, so the title gets the whole
// budget (spec/stock/09 §7; the reference frees its boot animation's framebuffer, in its INIT section, too); the
// title's display-mode change and first frame turn the screen on again.
void StopSplash() {
    if (!SplashScreen)
        return;
    KeWaitForSingleObject(&SplashDone, Executive, KernelMode, FALSE, nullptr);
    if (SplashThreadObject) { // until it has exited: the next switch back frees its stack
        KeWaitForSingleObject(SplashThreadObject, Executive, KernelMode, FALSE, nullptr);
        ObfDereferenceObject(SplashThreadObject);
        SplashThreadObject = nullptr;
    }
    EndBootSound(); // the jingle has ended and the audio controller is stopped
    // Screen off until the title shows its first frame: its graphics set-up writes the memory the splash is scanned
    // out of (and, freed below, may reuse it), which garbled the last splash frame on screen. A title's display-mode
    // change leaves the screen off too and turns it on itself (option 9 with 0, spec/stock/22).
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 1, nullptr);
    if (HalpSplashStart)
        MmFreeContiguousMemory(HalpSplashStart);
    MmFreeContiguousMemory(SplashImage);
    MmFreeContiguousMemory(SplashScreen);
    SplashScreen = SplashImage = HalpSplashStart = nullptr;
}

void EndSplash() {
    StopSplash();
    HalpFreeBootPages(); // the splash and the jingle have ended: the boot-only code goes
}

// --- The 2BL's splash record (boot/bl.cpp: 'OXSP', physical address, size at physical 0x2380), taken on a cold boot
// before the memory manager starts, which keeps those pages until the splash has been decoded. Normal code: the memory
// manager asks on quick reboots too.

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
    return SplashRamSize && pfn >= (SplashRamPhysical >> PAGE_SHIFT) &&
           pfn < ((SplashRamPhysical + SplashRamSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

} // namespace xk::bootscreen
