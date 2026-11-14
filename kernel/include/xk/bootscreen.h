// The boot screen (src/bootscreen/): the splash (spec/oxos_internal/00_boot_splash.md) and the jingle
// (spec/oxos_internal/17_boot_jingle.md). Boot-only code, freed before the first title starts, except EndSplash
// and the text drawing.
#pragma once
#include <xk/types.h>

namespace xk::bootscreen {

// The boot sound (bootsound.cpp).
void StartBootSound();
void PlayBootSound();
void EndBootSound();

// The boot splash (splash.cpp).
void StartSplash();
void TakeSplashRecord(bool QuickReboot);
bool IsSplashPage(ULONG pfn);
// Until the splash and the jingle have ended (at once without them); its controller read is final then (cold boot).
void WaitForSplash();
void StopSplash();    // the splash ends; the boot-only pages stay
void EndSplash();     // the splash ends and the boot-only pages are freed (a title starts)
void StopBootSound(); // resident: the sound controller stopped at once (a fatal error on the idle thread)

// Text (text.cpp, resident: the fatal error and bug check screens draw with it after the boot-only code is freed):
// the OXOS 5x8 font, printable ASCII; a glyph is 5 x 8 pixels of Scale x Scale, 6 * Scale apart. Pitch is in pixels
// of 32 bits. An X with two spaces on either side ("Press  X  to") is drawn as the controller's X button: a bold blue
// X in a blue ring.
ULONG TextWidth(const char *Text, ULONG Scale);
void DrawText(ULONG *Image, ULONG Pitch, ULONG X, ULONG Y, const char *Text, ULONG Scale, ULONG Color);
void VersionLabel(char *Buffer, ULONG Size); // "v0.3.16/CONEXANT"

} // namespace xk::bootscreen
