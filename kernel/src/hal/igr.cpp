// In-game reset (spec/oxos-internal/22). src/arch/igr.S passes every controller state a title reads through
// XInputGetState; LT + RT + BACK + BLACK held together reboots the console (a cold reboot: a netbooted console
// fetches its image again and starts the launcher, a mounted disc image is dropped).
#include <xk/serial.h>
#include <xk/types.h>

XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);

namespace {
// Cerbios's button mask (spec/cerbios/08 §2): bits 0-7 the digital buttons, bit 8 + i analog button i
// (A B X Y BLACK WHITE LT RT) held fully down (0xFF).
constexpr ULONG IGR_BACK = 0x0020, IGR_BLACK = 0x1000, IGR_LT = 0x4000, IGR_RT = 0x8000;
constexpr ULONG IGR_REBOOT = IGR_LT | IGR_RT | IGR_BACK | IGR_BLACK; // 0xD020, Cerbios's IGRFULL default
constexpr ULONG HalRebootRoutine = 1;

bool HalpIgrSeen;
} // namespace

// State: XINPUT_STATE (packet number, then the gamepad: buttons u16 at +4, 8 analog bytes at +6).
XBAPI void NTAPI HalpIgrCheck(const UCHAR *State) {
    if (!HalpIgrSeen) {
        HalpIgrSeen = true;
        XK_SERIAL("igr: the title reads its controllers (XInputGetState)\n");
    }
    ULONG mask = State[4];
    for (ULONG i = 0; i < 8; i++)
        if (State[6 + i] == 0xFF)
            mask |= 0x100u << i;
    if ((mask & IGR_REBOOT) == IGR_REBOOT) {
        XK_SERIAL("igr: LT RT BACK BLACK: reboot\n");
        HalReturnToFirmware(HalRebootRoutine);
    }
}
