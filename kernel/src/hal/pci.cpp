// PCI configuration access and the Xbox chipset set-up (spec/02-executive-init.md §3.7-3.8).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

constexpr USHORT PCI_ADDRESS = 0xCF8, PCI_DATA = 0xCFC;

static ULONG PciAddress(ULONG bus, ULONG slot, ULONG reg) {
    PCI_SLOT_NUMBER s;
    s.u.AsULONG = slot;
    return 0x80000000 | (bus << 16) | (s.u.bits.DeviceNumber << 11) | (s.u.bits.FunctionNumber << 8) | (reg & 0xFC);
}

XBAPI void NTAPI HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber, PVOID Buffer,
                                      ULONG Length, BOOLEAN WritePCISpace) {
    ULONG flags = save_flags_cli();
    UCHAR *p = (UCHAR *)Buffer;
    for (ULONG i = 0; i < Length; i++) {
        ULONG reg = RegisterNumber + i;
        outl(PCI_ADDRESS, PciAddress(BusNumber, SlotNumber, reg));
        USHORT port = PCI_DATA + (reg & 3);
        if (WritePCISpace)
            outb(port, p[i]);
        else
            p[i] = inb(port);
    }
    restore_flags(flags);
}

UCHAR HalpPciRead8(ULONG bus, ULONG dev, ULONG reg) {
    UCHAR v;
    HalReadWritePCISpace(bus, dev, reg, &v, 1, FALSE);
    return v;
}
ULONG HalpPciRead32(ULONG bus, ULONG dev, ULONG reg) {
    ULONG v;
    HalReadWritePCISpace(bus, dev, reg, &v, 4, FALSE);
    return v;
}
void HalpPciWrite8(ULONG bus, ULONG dev, ULONG reg, UCHAR v) { HalReadWritePCISpace(bus, dev, reg, &v, 1, TRUE); }
void HalpPciWrite16(ULONG bus, ULONG dev, ULONG reg, USHORT v) { HalReadWritePCISpace(bus, dev, reg, &v, 2, TRUE); }
void HalpPciWrite32(ULONG bus, ULONG dev, ULONG reg, ULONG v) { HalReadWritePCISpace(bus, dev, reg, &v, 4, TRUE); }

static void PciOr8(ULONG bus, ULONG dev, ULONG reg, UCHAR bits) {
    HalpPciWrite8(bus, dev, reg, HalpPciRead8(bus, dev, reg) | bits);
}

// Enable a function's decoders (command |= mask) and assign its interrupt line.
static void PciEnable(ULONG bus, ULONG dev, UCHAR commandBits, UCHAR intLine) {
    PciOr8(bus, dev, 0x04, commandBits);
    HalpPciWrite8(bus, dev, 0x3C, intLine);
}

// Bus 0 device numbers on the Xbox (MCPX + NV2A host bridge).
enum : ULONG {
    DEV_HOST = 0, DEV_LPC = 1, DEV_USB0 = 2, DEV_USB1 = 3, DEV_NIC = 4,
    DEV_APU = 5, DEV_AC97 = 6, DEV_IDE = 9, DEV_AGP = 0x1E,
};

void HalpInitPci() {
    // MCPX ISA/LPC bridge.
    HalpPciWrite8(0, DEV_LPC, 0x6A, 0x03);
    HalpPciWrite32(0, DEV_LPC, 0x6C, 0x0E065491);
    HalpPciWrite32(0, DEV_LPC, 0x64, 0x00000B0C);
    PciOr8(0, DEV_LPC, 0x81, 0x08);

    // IDE: I/O + bus master, both channels in legacy mode, bus-master registers at 0xFF60.
    PciOr8(0, DEV_IDE, 0x04, 0x05);
    HalpPciWrite8(0, DEV_IDE, 0x09, HalpPciRead8(0, DEV_IDE, 0x09) & 0xFA);
    HalpPciWrite32(0, DEV_IDE, 0x20, 0xFF61);
    HalpPciWrite32(0, DEV_IDE, 0x50, 0x00000002);
    HalpPciWrite32(0, DEV_IDE, 0x58, 0x20202020);
    HalpPciWrite32(0, DEV_IDE, 0x60, 0xC0C0C0C0);

    // NIC.
    PciEnable(0, DEV_NIC, 0x07, 4);
    HalpPciWrite32(0, DEV_NIC, 0x10, 0xFEF00000);
    HalpPciWrite32(0, DEV_NIC, 0x14, 0x0000E001);

    // USB host controllers.
    PciEnable(0, DEV_USB0, 0x07, 1);
    HalpPciWrite32(0, DEV_USB0, 0x10, 0xFED00000);
    HalpPciWrite32(0, DEV_USB0, 0x50, 0x0000000F);
    PciEnable(0, DEV_USB1, 0x07, 9);
    HalpPciWrite32(0, DEV_USB1, 0x10, 0xFED08000);
    HalpPciWrite32(0, DEV_USB1, 0x50, 0x00000030);

    // AC'97 and APU.
    PciEnable(0, DEV_AC97, 0x07, 6);
    HalpPciWrite32(0, DEV_AC97, 0x10, 0x0000D001);
    HalpPciWrite32(0, DEV_AC97, 0x14, 0x0000D201);
    HalpPciWrite32(0, DEV_AC97, 0x18, 0xFEC00000);
    PciEnable(0, DEV_APU, 0x07, 5);
    HalpPciWrite32(0, DEV_APU, 0x10, 0xFE800000);

    // LPC bridge register 0x8C: set bit 27, clear bit 26; then MCPX I/O 0x80CD = 8.
    ULONG r8c = HalpPciRead32(0, DEV_LPC, 0x8C);
    HalpPciWrite32(0, DEV_LPC, 0x8C, (r8c & ~(1u << 26)) | (1u << 27));
    outb(0x80CD, 0x08);

    // AC'97 register 0x4E |= 0x0101.
    USHORT r4e;
    HalReadWritePCISpace(0, DEV_AC97, 0x4E, &r4e, 2, FALSE);
    HalpPciWrite16(0, DEV_AC97, 0x4E, r4e | 0x0101);

    // AGP bridge: bus 0 -> 1, memory window 0xFD000000-0xFE7FFFFF, prefetch window 0xF0000000-0xF3FFFFFF
    // (limit extended when hardware flag 0x08 is set).
    PciOr8(0, DEV_AGP, 0x04, 0x07);
    HalpPciWrite8(0, DEV_AGP, 0x18, 0x00);
    HalpPciWrite8(0, DEV_AGP, 0x19, 0x01);
    HalpPciWrite8(0, DEV_AGP, 0x1A, 0x01);
    HalpPciWrite32(0, DEV_AGP, 0x20, 0xFE70FD00);
    HalpPciWrite16(0, DEV_AGP, 0x24, 0xF000);
    HalpPciWrite16(0, DEV_AGP, 0x26, (USHORT)(0xF3F0 | ((XboxHardwareInfo.Flags & 8) << 7)));

    // Host bridge, then NV2A (bus 1, device 0).
    HalpPciWrite32(0, DEV_HOST, 0x48, 0x00000114);
    PciEnable(1, 0, 0x07, 3);
    HalpPciWrite8(1, 0, 0x3D, 0x01);
    HalpPciWrite32(1, 0, 0x4C, 0x00000114);
}

// ACPI power-management block at I/O 0x8000.
void HalpInitAcpiPm() {
    outw(0x8004, inw(0x8004) | 0x0001); // PM1 control: SCI enable
    outw(0x8022, inw(0x8022) | 0x0002);
    outw(0x8002, inw(0x8002) | 0x0001); // PM1 enable: timer
    outw(0x8028, inw(0x8028) | 0x0001);
}
