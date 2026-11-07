// The boot screen (src/bootscreen/): the splash (spec/oxos_internal/00_boot_splash.md) and the jingle
// (spec/oxos_internal/17_boot_jingle.md). Boot-only code, freed before the first title starts, except HalpEndSplash.
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
