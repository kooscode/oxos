// Hardware abstraction layer (spec/stock/02-executive-init.md §3, spec/stock/06).
#pragma once
#include <xk/ke.h>
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

// Boot flags byte (origin not yet traced, spec 03); bit 0 suppresses the 0x80CF pulse.
extern UCHAR HalBootFlags;

void HalInitSystemPhase0(bool coldBoot);
void HalpInitPic();
void HalpInitRtc();
void HalpInitRtcLate();
void HalpInitPit();
void HalpInitPci();
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

// CMOS access (shared by RTC set-up and the RTC self-test).
UCHAR HalpCmosRead(UCHAR reg);
void HalpCmosWrite(UCHAR reg, UCHAR v);

// Thin typed wrappers over HalReadWritePCISpace.
UCHAR HalpPciRead8(ULONG bus, ULONG dev, ULONG reg);
ULONG HalpPciRead32(ULONG bus, ULONG dev, ULONG reg);
void HalpPciWrite8(ULONG bus, ULONG dev, ULONG reg, UCHAR v);
void HalpPciWrite16(ULONG bus, ULONG dev, ULONG reg, USHORT v);
void HalpPciWrite32(ULONG bus, ULONG dev, ULONG reg, ULONG v);
