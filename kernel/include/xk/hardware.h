// Hardware (spec/stock/02_executive_init.md §3, spec/stock/06, spec/stock/12): the HAL exports, the chipset, SMBus/SMC,
// video, the EEPROM.
#pragma once
#include <xk/core.h>
#include <xk/runtime.h>
#include <xk/types.h>

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

XBAPI void NTAPI HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber, PVOID Buffer,
                                      ULONG Length, BOOLEAN WritePCISpace);

XBAPI ULONG NTAPI HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql);
XBAPI void NTAPI HalEnableSystemInterrupt(ULONG BusInterruptLevel, KINTERRUPT_MODE InterruptMode);
XBAPI void NTAPI HalDisableSystemInterrupt(ULONG BusInterruptLevel);
XBAPI void FASTCALL HalRequestSoftwareInterrupt(KIRQL RequestIrql);
XBAPI void FASTCALL HalClearSoftwareInterrupt(KIRQL RequestIrql);

namespace xk::hardware {
void InitHalPhase0(bool coldBoot);
void InitPic();
void InitRtc();
void InitRtcLate();
void InitPit();
void InitPci();
void IdleVideoOverlay();   // the video overlay stopped (cold boot and quick reboot)
void InitGraphicsEngine(); // hardware/graphics.cpp: cold boot, after the GPU reset
void InitAcpiPm();
void Pulse();
void DetectHardware();
} // namespace xk::hardware

// PIC: the effective mask is (disabled IRQs) | (IRQs whose IRQL <= current IRQL). See spec/stock/06 §1.
constexpr ULONG HAL_IRQ_BASE_VECTOR = 0x30;
namespace xk::hardware {
KIRQL GetIrqLevel(ULONG irq);            // IRQL an IRQ is dispatched at
void SetIrqLevel(ULONG irq, KIRQL irql); // a connected KINTERRUPT overrides the default
void ResetIrqLevel(ULONG irq);
void SetIrqlMask(KIRQL irql);
void SendEoi(ULONG irq);
bool IsSpuriousIrq(ULONG irq);
} // namespace xk::hardware

namespace xk::hardware {
// CMOS and the real-time clock (hardware/interrupts_timers.cpp); the signature that marks the clock as set.
UCHAR ReadCmos(UCHAR reg);
void WriteCmos(UCHAR reg, UCHAR v);
void QueryRealTimeClock(TIME_FIELDS *t);
void SetRealTimeClock(const TIME_FIELDS *t);
void WriteCmosSignature();
bool CmosSignatureValid();
} // namespace xk::hardware

namespace xk::hardware {
// Thin typed wrappers over HalReadWritePCISpace.
UCHAR ReadPci8(ULONG bus, ULONG dev, ULONG reg);
ULONG ReadPci32(ULONG bus, ULONG dev, ULONG reg);
void WritePci8(ULONG bus, ULONG dev, ULONG reg, UCHAR v);
void WritePci16(ULONG bus, ULONG dev, ULONG reg, USHORT v);
void WritePci32(ULONG bus, ULONG dev, ULONG reg, ULONG v);
} // namespace xk::hardware

namespace xk::hardware {
// AV: the video encoder, display modes and the boot display (hardware/video.cpp, spec/stock/17).
void InitVideo();
} // namespace xk::hardware
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
XBAPI void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
namespace xk::hardware {
ULONG BootModeWord();
void PrepareBootDisplay();
void LoadBootGamma();
void VideoTablesInUse(ULONG_PTR *Start, ULONG_PTR *End);
void SmcSetOutputs(bool Enable);
extern volatile UCHAR EncoderAddress;
} // namespace xk::hardware
XBAPI UCHAR HalBootSMCVideoMode; // the SMC's AV pack code read at boot (7 = none)
namespace xk::hardware {
extern volatile ULONG EncoderProgrammed; // diagnostics: the last mode word whose encoder registers were written
extern volatile ULONG EncoderMismatches; // diagnostics: registers that read back differently after that mode set
extern volatile ULONG XcShadow[0x100];   // the Xcalibur's registers as last written
} // namespace xk::hardware

// SMBus, the SMC and firmware return (hardware/smbus.cpp, spec/stock/12); the in-game reset
// (hardware/in_game_reset.cpp).
XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue,
                                       PULONG DataValue);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue,
                                        ULONG DataValue);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
namespace xk::hardware {
void InitSmcColdBoot();
void StartSmcEvents();
BOOLEAN ArmSmcEvents(BOOLEAN Dashboard);
void HandleSmcEvents(ULONG reason);
extern volatile ULONG BootFlags;
} // namespace xk::hardware
XBAPI void NTAPI InGameResetReboot();
namespace xk::hardware {
void SetBootLed(UCHAR Pattern); // the front LED's boot status (spec/oxos_internal/03 §6)
} // namespace xk::hardware

// The Ethernet PHY (hardware/ethernet.cpp).
XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2);
XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN Verify);

// String port I/O exports (runtime/runtime.cpp).
XBAPI void NTAPI READ_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI READ_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
XBAPI void NTAPI WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count);

// --- EEPROM settings and the security section (hardware/eeprom.cpp, spec/stock/12 §3)

XBAPI UCHAR XboxHDKey[16]; // the console's hard disk key, from the EEPROM's security section

namespace xk::hardware {
void InitEeprom(); // start-up: read the EEPROM, decrypt the security section, take the HD key and the game region
} // namespace xk::hardware
XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                               PULONG ResultLength);
XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength);
XBAPI NTSTATUS NTAPI ExReadWriteRefurbInfo(PULONG RefurbInfo, ULONG ValueLength, BOOLEAN DoWrite);
namespace xk::hardware {
extern volatile ULONG EepromSecurityValid; // 1 when the security section decrypted and authenticated
} // namespace xk::hardware
