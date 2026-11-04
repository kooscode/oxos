// In-game reset (spec/oxos-internal/22). src/arch/igr.S passes every controller state a title reads through
// XInputGetState; LT + RT + BACK + BLACK held together power-cycles the console (a cold boot: a netbooted console
// fetches its image again and starts the launcher, a mounted disc image is dropped).
#include <xk/serial.h>
#include <xk/types.h>

XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
XBAPI VOID NTAPI KeStallExecutionProcessor(ULONG MicroSeconds);
XBAPI void NTAPI HalpIgrReboot();

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
        HalpIgrReboot();
    }
}

// The in-game reset's reboot: a power cycle through the SMC (register 0x02 = 0x40, Cerbios's IGRCYCLE), so the next
// boot is a cold one. A reset (0x02 = 1, then port 0xCF9, HalReturnToFirmware 1) while a title is using the GPU left
// most of the X-codes' GPU and memory set-up undone at the next boot (29 settings at their power-on values, the RAM
// mis-sized and the kernel stopped, spec/oxos-internal/22 §5); port 0xCF9 alone does not reset the console at all.
// The SMBus is polled, so this works at any IRQL (XInputGetState may be called at DISPATCH_LEVEL). If the SMC does
// not cut the power within two seconds, the reference reset follows.
XBAPI void NTAPI HalpIgrReboot() {
    for (int i = 0; i < 5 && !NT_SUCCESS(HalWriteSMBusValue(0x20, 0x02, FALSE, 0x40)); i++) {
    }
    for (ULONG ms = 0; ms < 2000; ms++)
        KeStallExecutionProcessor(1000);
    HalReturnToFirmware(HalRebootRoutine);
}
