// The boot splash (src/boot_splash/): the splash (spec/oxos_internal/00_boot_splash.md) and the jingle (spec/oxos_internal/17_boot_jingle.md). Boot-only code,
// freed before the first title starts, except EndSplash and StopBootSound. The splash draws with the kernel's graphics (graphics/graphics.h).
#pragma once
#include <runtime/types.h>

namespace xk::boot_splash {

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

} // namespace xk::boot_splash
