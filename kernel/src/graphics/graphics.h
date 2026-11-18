// The kernel's graphics (src/graphics/): the OXOS font and text (text.cpp), and the frame buffer and shapes of the kernel's own screens (screen.cpp): the error
// screens, the format countdown and the File Manager's screen draw with them, and the boot splash with the text. Resident: those screens can come up after the
// boot-only code is freed. The vector renderer the splash draws its scene with is shared/graphics/vector_renderer.h (oxos::graphics).
#pragma once
#include <runtime/types.h>

namespace xk::graphics {

// --- Text (text.cpp): the OXOS 5x8 font, printable ASCII; a glyph is 5 x 8 pixels of Scale x Scale, 6 * Scale apart.
// Pitch is in pixels of 32 bits. An X with two spaces on either side ("Press  X  to") is drawn as the controller's X
// button: a bold blue X in a blue ring.

ULONG TextWidth(const char *Text, ULONG Scale);
void DrawText(ULONG *Image, ULONG Pitch, ULONG X, ULONG Y, const char *Text, ULONG Scale, ULONG Color);
void VersionLabel(char *Buffer, ULONG Size); // "v0.3.16/CONEXANT"

// --- Screens (screen.cpp): 640x480 X8R8G8B8. The left edge of text and the spacing of its lines (scale 2), the
// characters a line holds between the margins, and the colours.

constexpr ULONG SCREEN_WIDTH = 640, SCREEN_HEIGHT = 480, SCREEN_LEFT = 48, SCREEN_LINE = 20;
constexpr ULONG SCREEN_LINE_CHARS = 45;
constexpr ULONG COLOR_WHITE = 0xFFFFFF, COLOR_TEXT = 0xC8C8C8, COLOR_GREY = 0x808080;
constexpr ULONG COLOR_DIGIT = 0xFFFFFFFF; // the large digits' white (the boot tests look for this value)

// A new black screen with "OXOS v0.3.16/CONEXANT" at the top (StartScreen), or null without an AV pack or memory; ShowScreen puts it on the TV.
ULONG *OpenScreen();
void StartScreen(ULONG *Screen); // black, with the version at the top
void ShowScreen(ULONG Physical); // the frame buffer at Physical on the TV, in the boot display mode
void CloseScreen(ULONG **Screen); // screen off (the boot goes on), memory back; nothing when *Screen is null

void FillRectangle(ULONG *Screen, ULONG X, ULONG Y, ULONG Width, ULONG Height, ULONG Color);
void ClearRectangle(ULONG *Screen, ULONG X, ULONG Y, ULONG Width, ULONG Height); // black
void DrawCentredText(ULONG *Screen, ULONG Y, const char *Text, ULONG Scale, ULONG Color);
void DrawScreenTitle(ULONG *Screen, ULONG Y, const char *Title); // centred, large when it fits between the margins
void DrawLargeDigit(ULONG *Screen, ULONG X, ULONG Y, ULONG Digit); // a seven-segment digit, 60 x 110 pixels

} // namespace xk::graphics
