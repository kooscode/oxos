// The boot screen (src/bootscreen/): the splash (spec/oxos_internal/00_boot_splash.md) and the jingle
// (spec/oxos_internal/17_boot_jingle.md). Boot-only code, freed before the first title starts, except HalpEndSplash
// and the text drawing.
#pragma once
#include <xk/types.h>

// The boot sound (bootsound.cpp).
void HalpStartBootSound();
void HalpBootSoundGo();
void HalpEndBootSound();

// The boot splash (splash.cpp).
void HalpStartSplash();
void HalpTakeSplashRecord(bool QuickReboot);
bool HalpIsSplashPage(ULONG pfn);
void HalpEndSplash();
void HalpStopBootSound(); // resident: the sound controller stopped at once (a fatal error on the idle thread)

// Text (text.cpp, resident: the fatal error and bug check screens draw with it after the boot-only code is freed):
// the OXOS 5x8 font, printable ASCII; a glyph is 5 x 8 pixels of Scale x Scale, 6 * Scale apart. Pitch is in pixels
// of 32 bits.
ULONG XkTextWidth(const char *Text, ULONG Scale);
void XkDrawText(ULONG *Image, ULONG Pitch, ULONG X, ULONG Y, const char *Text, ULONG Scale, ULONG Color);
void XkVersionLabel(char *Buffer, ULONG Size); // "v0.3.16/CONEXANT"
