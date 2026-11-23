// The pads' state while titles run (pad_reports.cpp, spec/oxos_internal/22 §9, spec/oxos_internal/34 §7.3): read from the USB host controllers' done lists
// at their interrupts, before the title's own USB interrupt routine, for every title however it was built. Resident (unlike the rest of src/hardware/usb/,
// which is boot-only).
#pragma once
#include <runtime/types.h>
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#include <oxos_sdk.h>

namespace xk::hardware::usb {

constexpr ULONG PAD_SLOTS = 4; // pads tracked at once (OXOS_PAD_STATE::Port)

// Decodes one input report of an Xbox pad (20 bytes: 00 14, the digital buttons, a reserved byte, the analog buttons A B X Y BLACK WHITE LT RT, the sticks
// LX LY RX RY as 16-bit values) into State's buttons, analog values and sticks; false when Report is not one. The pad's answer to a title asking for its
// capabilities (XInputGetCapabilities) comes on the same done list with the same header, every supported button's bits set (00 14 FF 00 FF ...): it is
// told apart by the d-pad, which no pad reports as up and down, or left and right, at once (a false in-game reset in a game, 2026-11-23).
inline bool DecodePadReport(const UCHAR *Report, OXOS_PAD_STATE &State) {
    if (Report[0] != 0 || Report[1] != 0x14)
        return false;
    if ((Report[2] & 0x03) == 0x03 || (Report[2] & 0x0C) == 0x0C) // up + down, left + right
        return false;
    ULONG buttons = Report[2];
    for (ULONG i = 0; i < 8; i++) {
        State.Analog[i] = Report[4 + i];
        if (Report[4 + i] > 0x80)
            buttons |= 0x100u << i;
    }
    State.Buttons = buttons;
    State.ThumbLX = (SHORT)(Report[12] | Report[13] << 8);
    State.ThumbLY = (SHORT)(Report[14] | Report[15] << 8);
    State.ThumbRX = (SHORT)(Report[16] | Report[17] << 8);
    State.ThumbRY = (SHORT)(Report[18] | Report[19] << 8);
    return true;
}

void StartPadReports();               // every boot, after the memory manager: the page the reads go through
void ReadPadReports(ULONG Irq);       // at a USB controller's interrupt (IRQ 1 or 9), before the title's interrupt routine
ULONG TakePadChanges();               // the pads whose buttons changed since the last call (bit = OXOS_PAD_STATE::Port), cleared
BOOLEAN GetPadState(ULONG Port, OXOS_PAD_STATE &State); // the pad's last report: exported as oxos_pad_state; FALSE when none has been seen
void CheckPadLastResort();            // every 100 ms, from the clock interrupt: LT + RT + BACK + BLACK held 3 s on a pad powers the console off and on

} // namespace xk::hardware::usb
