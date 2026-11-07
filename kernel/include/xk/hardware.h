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

void HalInitSystemPhase0(bool coldBoot);
void HalpInitPic();
void HalpInitRtc();
void HalpInitRtcLate();
void HalpInitPit();
void HalpInitPci();
void HalpIdleVideoOverlay(); // the video overlay stopped (cold boot and quick reboot)
void HalpInitGraphicsEngine(); // hardware/graphics.cpp: cold boot, after the GPU reset
void HalpInitAcpiPm();
void HalpPulse();
void HalpDetectHardware();

// PIC: the effective mask is (disabled IRQs) | (IRQs whose IRQL <= current IRQL). See spec/stock/06 §1.
constexpr ULONG HAL_IRQ_BASE_VECTOR = 0x30;
KIRQL HalpGetIrqLevel(ULONG irq);            // IRQL an IRQ is dispatched at
void HalpSetIrqLevel(ULONG irq, KIRQL irql); // a connected KINTERRUPT overrides the default
void HalpResetIrqLevel(ULONG irq);
void HalpSetIrqlMask(KIRQL irql);
void HalpEoi(ULONG irq);
bool HalpIsSpuriousIrq(ULONG irq);

// CMOS and the real-time clock (hardware/interrupts_timers.cpp); the signature that marks the clock as set.
UCHAR HalpCmosRead(UCHAR reg);
void HalpCmosWrite(UCHAR reg, UCHAR v);
void HalpQueryRealTimeClock(TIME_FIELDS *t);
void HalpSetRealTimeClock(const TIME_FIELDS *t);
void HalpWriteCmosSignature();
bool HalpCmosSignatureValid();

// Thin typed wrappers over HalReadWritePCISpace.
UCHAR HalpPciRead8(ULONG bus, ULONG dev, ULONG reg);
ULONG HalpPciRead32(ULONG bus, ULONG dev, ULONG reg);
void HalpPciWrite8(ULONG bus, ULONG dev, ULONG reg, UCHAR v);
void HalpPciWrite16(ULONG bus, ULONG dev, ULONG reg, USHORT v);
void HalpPciWrite32(ULONG bus, ULONG dev, ULONG reg, ULONG v);

// AV: the video encoder, display modes and the boot display (hardware/video.cpp, spec/stock/17).
void AvInit();
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
XBAPI void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
ULONG AvBootModeWord();
void AvPrepareBootDisplay();
void AvLoadBootGamma();
void AvTablesInUse(ULONG_PTR *Start, ULONG_PTR *End);
void AvpSmcSetOutputs(bool Enable);
extern volatile UCHAR AvpEncoderAddress;
XBAPI UCHAR HalBootSMCVideoMode; // the SMC's AV pack code read at boot (7 = none)
extern volatile ULONG AvpEncoderProgrammed;  // diagnostics: the last mode word whose encoder registers were written
extern volatile ULONG AvpEncoderMismatches;  // diagnostics: registers that read back differently after that mode set
extern volatile ULONG AvpXcShadow[0x100];    // the Xcalibur's registers as last written

// SMBus, the SMC and firmware return (hardware/smbus.cpp, spec/stock/12); the in-game reset
// (hardware/in_game_reset.cpp).
XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue,
                                       PULONG DataValue);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue,
                                        ULONG DataValue);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
void HalpSmcInitColdBoot();
void HalpSmcStartEvents();
BOOLEAN HalpSmcArmEvents(BOOLEAN Dashboard);
void HalpSmcHandleEvents(ULONG reason);
extern volatile ULONG HalpBootFlags;
XBAPI void NTAPI HalpIgrReboot();
extern "C" void HalpSetBootLed(UCHAR Pattern); // the front LED's boot status (spec/oxos_internal/03 §6)

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

void ExInitEeprom(); // start-up: read the EEPROM, decrypt the security section, take the HD key and the game region
XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                               PULONG ResultLength);
XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength);
XBAPI NTSTATUS NTAPI ExReadWriteRefurbInfo(PULONG RefurbInfo, ULONG ValueLength, BOOLEAN DoWrite);
extern volatile ULONG ExpEepromSecurityValid; // 1 when the security section decrypted and authenticated
