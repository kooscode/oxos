// The chipset (src/hardware/chipset/; spec/stock/02_executive_init.md §3, spec/stock/06, spec/stock/12): PCI set-up and HAL phase 0 (chipset.cpp, init.cpp),
// the PIC, PIT and CMOS clock (interrupts_timers.cpp), SMBus and the SMC (smbus.cpp), the EEPROM (eeprom.cpp), string port I/O (port_io.cpp); the debug serial
// port is in serial.h.
#pragma once
#include <core/core.h>
#include <runtime/runtime.h>
#include <runtime/types.h>

typedef struct _PCI_SLOT_NUMBER {
    union {
        struct {
            ULONG DeviceNumber : 5;
            ULONG FunctionNumber : 3;
            ULONG Reserved : 24;
        } bits;
        ULONG AsULONG;
    } u;
} PCI_SLOT_NUMBER;

XBAPI void NTAPI HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber, PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

XBAPI ULONG NTAPI HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql);
XBAPI void NTAPI HalEnableSystemInterrupt(ULONG BusInterruptLevel, KINTERRUPT_MODE InterruptMode);
XBAPI void NTAPI HalDisableSystemInterrupt(ULONG BusInterruptLevel);

namespace xk::hardware::chipset {
void InitHalPhase0(bool coldBoot);
void InitPic();
void InitRtc();
void InitRtcLate();
void InitPit();
void InitPci();
void IdleVideoOverlay();   // the video overlay stopped (cold boot and quick reboot)
void PowerCycle();         // the console powered off and on through the SMC (smbus.cpp); any IRQL; does not return
NTSTATUS ResetConsole(ULONG Kind); // exported as oxos_console_reset (spec/oxos_internal/34 §7.3): 1 power cycle, 2 the SMC's reset
extern volatile LONG ConsoleStopping; // an extension's launch or reset (oxos_title_launch, oxos_console_reset) is under way: the first one wins
void InitAcpiPm();
void Pulse();
void DetectHardware();
} // namespace xk::hardware::chipset

// PIC: the effective mask is (disabled IRQs) | (IRQs whose IRQL <= current IRQL). See spec/stock/06 §1.
constexpr ULONG HAL_IRQ_BASE_VECTOR = 0x30;
namespace xk::hardware::chipset {
KIRQL GetIrqLevel(ULONG irq);            // IRQL an IRQ is dispatched at
void SetIrqLevel(ULONG irq, KIRQL irql); // a connected KINTERRUPT overrides the default
void ResetIrqLevel(ULONG irq);
void SetIrqlMask(KIRQL irql);
void SendEoi(ULONG irq);
bool IsSpuriousIrq(ULONG irq);
} // namespace xk::hardware::chipset

namespace xk::hardware::chipset {
// CMOS and the real-time clock (interrupts_timers.cpp); the signature that marks the clock as set.
UCHAR ReadCmos(UCHAR reg);
void WriteCmos(UCHAR reg, UCHAR v);
void QueryRealTimeClock(TIME_FIELDS *t);
void SetRealTimeClock(const TIME_FIELDS *t);
void WriteCmosSignature();
bool CmosSignatureValid();
} // namespace xk::hardware::chipset

namespace xk::hardware::chipset {
// Thin typed wrappers over HalReadWritePCISpace.
UCHAR ReadPci8(ULONG bus, ULONG dev, ULONG reg);
ULONG ReadPci32(ULONG bus, ULONG dev, ULONG reg);
void WritePci8(ULONG bus, ULONG dev, ULONG reg, UCHAR v);
void WritePci16(ULONG bus, ULONG dev, ULONG reg, USHORT v);
void WritePci32(ULONG bus, ULONG dev, ULONG reg, ULONG v);
} // namespace xk::hardware::chipset

// SMBus, the SMC and firmware return (smbus.cpp, spec/stock/12).
XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
namespace xk::hardware::chipset {
void InitSmcColdBoot();
void StartSmcEvents();
BOOLEAN ArmSmcEvents(BOOLEAN Dashboard);
void HandleSmcEvents(ULONG reason);
extern volatile ULONG BootFlags;
void SetBootLed(UCHAR Pattern); // the front LED's boot status (spec/oxos_internal/03 §6)
} // namespace xk::hardware::chipset

// Exported data: the console's hardware (init.cpp) and the EEPROM key the 2BL hands over
// (eeprom.cpp).
XBAPI XBOX_HARDWARE_INFO XboxHardwareInfo;
XBAPI UCHAR XboxEEPROMKey[16];

// String port I/O exports (port_io.cpp).
XBAPI void NTAPI READ_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);

// --- EEPROM settings and the security section (eeprom.cpp, spec/stock/12 §3)

XBAPI UCHAR XboxHDKey[16]; // the console's hard disk key, from the EEPROM's security section

namespace xk::hardware::chipset {
void InitEeprom(); // start-up: read the EEPROM, decrypt the security section, take the HD key and the game region
} // namespace xk::hardware::chipset
XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength);
XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength);
XBAPI NTSTATUS NTAPI ExReadWriteRefurbInfo(PULONG RefurbInfo, ULONG ValueLength, BOOLEAN DoWrite);
namespace xk::hardware::chipset {
extern volatile ULONG EepromSecurityValid; // 1 when the security section decrypted and authenticated
} // namespace xk::hardware::chipset
