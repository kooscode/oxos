// The controllers read during the boot splash and on the No Dashboard Found screen (src/gamepad/,
// spec/oxos_internal/32_file_manager_button.md): X opens the OXOS File Manager. Boot-only.
#pragma once
#include <xk/types.h>

void GamepadWatchStart(); // the controllers found and set up again (blocks for a few hundred milliseconds)
void GamepadWatchPoll();  // without waiting: the reports that arrived since the last call
void GamepadWatchStop();  // the USB controller reset and its memory freed, before the title's driver takes it
// Reads the controllers until X is pressed; with none found, looks again every 3 s (one plugged in later).
void GamepadWaitForX();

extern volatile ULONG GamepadXPressed; // 1 once X was seen pressed (read by the title launcher and the tests)
extern volatile ULONG GamepadPadsFound;    // the controllers found (read by the boot tests)
