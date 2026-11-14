// In-game reset (src/in_game_reset/, spec/oxos_internal/22): LT + RT + BACK + BLACK held in a title power-cycles the
// console. xinput_hook.S watches XInputGetState and calls HalpIgrCheck (a C name: assembly calls it).
#pragma once
#include <runtime/types.h>

XBAPI void NTAPI HalpIgrCheck(const UCHAR *State);

namespace xk::in_game_reset {

// A power cycle through the SMC, so the next boot is a cold one; the reference reset if the SMC does not cut the power
// within two seconds. Works at any IRQL (the SMBus is polled).
void Reboot();

} // namespace xk::in_game_reset
