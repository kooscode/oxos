// Hardware abstraction layer (spec/02-executive-init.md §3).
#pragma once
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

// PIC helpers
void HalpUnmaskIrq(ULONG irq);
void HalpEoi(ULONG irq);

// Thin typed wrappers over HalReadWritePCISpace.
UCHAR HalpPciRead8(ULONG bus, ULONG dev, ULONG reg);
ULONG HalpPciRead32(ULONG bus, ULONG dev, ULONG reg);
void HalpPciWrite8(ULONG bus, ULONG dev, ULONG reg, UCHAR v);
void HalpPciWrite16(ULONG bus, ULONG dev, ULONG reg, USHORT v);
void HalpPciWrite32(ULONG bus, ULONG dev, ULONG reg, ULONG v);
