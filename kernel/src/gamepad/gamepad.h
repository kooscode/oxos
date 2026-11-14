// The gamepads read during the boot splash and on the No Dashboard Found screen (src/gamepad/, on src/hardware/usb/,
// spec/oxos_internal/32_file_manager_button.md): X opens the OXOS File Manager. Boot-only.
#pragma once
#include <runtime/types.h>

namespace xk::gamepad {

void StartWatchingControllers(); // the controllers found and set up again (blocks for a few hundred milliseconds)
void PollControllers();          // without waiting: the reports that arrived since the last call
void StopWatchingControllers();  // the USB controller reset and its memory freed, before the title's driver takes it
// Reads the controllers until X is pressed; with none found, looks again every 3 s (one plugged in later).
void WaitForXButton();

extern volatile ULONG XButtonPressed;   // 1 once X was seen pressed (read by the title launcher and the tests)
extern volatile ULONG BButtonPressed;   // 1 once B was seen pressed: safe mode (spec/oxos_internal/34 §3.3)
extern volatile ULONG ControllersFound; // the controllers found (read by the boot tests)

} // namespace xk::gamepad
