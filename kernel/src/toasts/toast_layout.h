// A toast's box and where it goes (spec/oxos_internal/36 §3): its size from its text, its place in the current video mode, and its picture's colours in YUY2.
// No kernel calls, so the host unit tests run it unchanged (tests/unit/test_toasts.cpp).
#pragma once
#include <runtime/types.h>
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#include <oxos_sdk.h>

namespace xk::toasts {

// The box at 480-line size: the font at scale 2 (12 pixels a character, 16 high), a 3-pixel frame. The overlay scales it to the mode's height.
constexpr ULONG TOAST_SCALE = 2, TOAST_CHAR_WIDTH = 6 * TOAST_SCALE, TOAST_LINE_HEIGHT = 20, TOAST_FRAME = 3;
constexpr ULONG TOAST_PADDING_X = 16, TOAST_PADDING_TOP = 15, TOAST_PADDING_BOTTOM = 9;
constexpr ULONG TOAST_WIDTH_STEP = 32; // the overlay's row pitch must be a multiple of 64 bytes on a console (2026-11-23): 32 YUY2 pixels
constexpr ULONG TOAST_COLOR_FRAME = 0x8BE31F, TOAST_COLOR_BACKGROUND = 0x000000, TOAST_COLOR_TEXT = 0xF4FFE6; // the boot splash's green and pale white

// A box: its size in pixels at 480-line size.
struct TOAST_BOX {
    ULONG Width;
    ULONG Height;
};

// Where the overlay shows it: its top left and size, in the mode's pixels.
struct TOAST_PLACEMENT {
    ULONG X, Y;
    ULONG Width, Height;
};

// The box for Text (one or two lines, '\n' between them): the longest line and the padding, rounded up to TOAST_WIDTH_STEP.
inline TOAST_BOX MeasureToast(const char *Text) {
    ULONG longest = 0, line = 0, lines = 1;
    for (const char *p = Text; *p; p++) {
        if (*p == '\n') {
            lines++;
            line = 0;
            continue;
        }
        if (++line > longest)
            longest = line;
    }
    ULONG width = longest * TOAST_CHAR_WIDTH + 2 * TOAST_PADDING_X;
    width = (width + TOAST_WIDTH_STEP - 1) / TOAST_WIDTH_STEP * TOAST_WIDTH_STEP;
    return {width, TOAST_PADDING_TOP + lines * TOAST_LINE_HEIGHT + TOAST_PADDING_BOTTOM};
}

// Box placed at Place in a ModeWidth x ModeHeight mode, scaled by ModeHeight / 480 so the text is the same size on every screen: 10% of the height from the top
// or the bottom, 6% of the width from the sides.
inline TOAST_PLACEMENT PlaceToast(TOAST_BOX Box, OXOS_TOAST_PLACE Place, ULONG ModeWidth, ULONG ModeHeight) {
    ULONG width = Box.Width * ModeHeight / 480, height = Box.Height * ModeHeight / 480;
    ULONG marginX = ModeWidth * 6 / 100, marginY = ModeHeight / 10;
    ULONG column = (ULONG)Place % 3, row = (ULONG)Place / 3;
    ULONG x = column == 0 ? marginX : column == 1 ? (ModeWidth - width) / 2 : ModeWidth - marginX - width;
    ULONG y = row == 0 ? marginY : row == 1 ? (ModeHeight - height) / 2 : ModeHeight - marginY - height;
    return {x, y, width, height};
}

// Two pixels (X8R8G8B8) as one YUY2 dword (Y0 U Y1 V, BT.601 studio range): the overlay's format.
inline ULONG ConvertToYUY2(ULONG Left, ULONG Right) {
    LONG y[2], u = 0, v = 0;
    const ULONG pixels[2] = {Left, Right};
    for (ULONG i = 0; i < 2; i++) {
        LONG r = (pixels[i] >> 16) & 0xFF, g = (pixels[i] >> 8) & 0xFF, b = pixels[i] & 0xFF;
        y[i] = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    }
    return (ULONG)y[0] | (ULONG)(u / 2) << 8 | (ULONG)y[1] << 16 | (ULONG)(v / 2) << 24;
}

} // namespace xk::toasts
