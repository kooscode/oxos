// The kernel's own screens' frame buffer and shapes (spec/oxos_internal/29_error_screens.md): a 640x480 X8R8G8B8 frame buffer of its own, shown in the boot
// display mode, with the OXOS version at the top; rectangles, centred text, the title and seven-segment digits.
#include <graphics/graphics.h>
#include <hardware/chipset/chipset.h>
#include <hardware/video/video.h>
#include <memory/memory.h>
#include <runtime/runtime.h>
#include <storage/storage.h>

namespace xk::graphics {

// Seven-segment digits, our own layout: bit 0 top, 1 upper right, 2 lower right, 3 bottom, 4 lower left, 5 upper left, 6 middle.
static const UCHAR DigitSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void FillRectangle(ULONG *Screen, ULONG X, ULONG Y, ULONG Width, ULONG Height, ULONG Color) {
    for (ULONG j = Y; j < Y + Height; j++) {
        for (ULONG i = X; i < X + Width; i++)
            Screen[j * SCREEN_WIDTH + i] = Color;
    }
}

void ClearRectangle(ULONG *Screen, ULONG X, ULONG Y, ULONG Width, ULONG Height) {
    for (ULONG j = Y; j < Y + Height; j++)
        RtlZeroMemory(Screen + j * SCREEN_WIDTH + X, Width * 4);
}

void DrawLargeDigit(ULONG *Screen, ULONG X, ULONG Y, ULONG Digit) {
    const ULONG w = 60, h = 110, t = 12, half = (h - t) / 2;
    UCHAR s = DigitSegments[Digit % 10];
    if (s & 0x01)
        FillRectangle(Screen, X, Y, w, t, COLOR_DIGIT);
    if (s & 0x02)
        FillRectangle(Screen, X + w - t, Y, t, half + t, COLOR_DIGIT);
    if (s & 0x04)
        FillRectangle(Screen, X + w - t, Y + half, t, half + t, COLOR_DIGIT);
    if (s & 0x08)
        FillRectangle(Screen, X, Y + h - t, w, t, COLOR_DIGIT);
    if (s & 0x10)
        FillRectangle(Screen, X, Y + half, t, half + t, COLOR_DIGIT);
    if (s & 0x20)
        FillRectangle(Screen, X, Y, t, half + t, COLOR_DIGIT);
    if (s & 0x40)
        FillRectangle(Screen, X, Y + half, w, t, COLOR_DIGIT);
}

void DrawCentredText(ULONG *Screen, ULONG Y, const char *Text, ULONG Scale, ULONG Color) {
    ULONG width = TextWidth(Text, Scale);
    DrawText(Screen, SCREEN_WIDTH, width < SCREEN_WIDTH ? (SCREEN_WIDTH - width) / 2 : 0, Y, Text, Scale, Color);
}

void StartScreen(ULONG *Screen) {
    RtlZeroMemory(Screen, SCREEN_WIDTH * SCREEN_HEIGHT * 4);
    char header[48] = "OXOS ";
    VersionLabel(header + 5, sizeof(header) - 5);
    DrawCentredText(Screen, 36, header, 2, COLOR_GREY);
}

void DrawScreenTitle(ULONG *Screen, ULONG Y, const char *Title) {
    bool large = TextWidth(Title, 3) <= SCREEN_WIDTH - 2 * SCREEN_LEFT;
    DrawCentredText(Screen, large ? Y : Y + 4, Title, large ? 3 : 2, COLOR_WHITE);
}

// --- Opening and closing a screen: the fatal error's, and those that stay up while the kernel works (the format
// countdown, the File Manager: drawn once, then only the parts that change; taken down before the boot goes on).

ULONG *OpenScreen() {
    if (HalBootSMCVideoMode == 7)
        return nullptr;
    auto screen = (ULONG *)MmAllocateContiguousMemoryEx(SCREEN_WIDTH * SCREEN_HEIGHT * 4, 0, 0x3FFFFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (screen)
        StartScreen(screen);
    return screen;
}

void ShowScreen(ULONG Physical) {
    AvSetDisplayMode((PVOID)0xFD000000, 0, hardware::video::BootModeWord(), 0x1E, SCREEN_WIDTH * 4, Physical);
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 0, nullptr);
}

void CloseScreen(ULONG **Screen) {
    if (!*Screen)
        return;
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 1, nullptr);
    MmFreeContiguousMemory(*Screen);
    *Screen = nullptr;
}

} // namespace xk::graphics
