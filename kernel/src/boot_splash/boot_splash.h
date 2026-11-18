// The boot splash (src/boot_splash/, spec/oxos_internal/00_boot_splash.md). Boot-only code, freed before the first title starts, except EndSplash. The splash
// draws with the kernel's graphics (graphics/graphics.h).
#pragma once
#include <runtime/types.h>

namespace xk::boot_splash {

// The boot splash (splash.cpp).
void StartSplash();
void TakeSplashRecord(bool QuickReboot);
bool IsSplashPage(ULONG pfn);
// Until the splash has ended (at once without one); its controller read is final then (cold boot).
void WaitForSplash();
void StopSplash();    // the splash ends; the boot-only pages stay
// The splash ends and the boot-only pages are freed (a title starts). KeepPicture: its last picture stays on screen until the title's mode set (the boot
// video player, spec/oxos_internal/38 §2), so the display keeps its picture and its signal.
void EndSplash(bool KeepPicture = false);

} // namespace xk::boot_splash
