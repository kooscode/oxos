// PCI configuration access and the Xbox chipset set-up (spec/stock/02-executive-init.md §3.7-3.8).
#include <xk/serial.h>
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
    // Whole 32-bit configuration cycles only: a byte or word is read, or merged into its dword and written back.
    // Suspected on the first console runs (2026-10-21): byte-wide cycles through 0xCFD-0xCFF did not reach the MCPX
    // devices (the IDE controller stayed off and its configuration read as 0xFF); xemu accepts either. To confirm.
    ULONG flags = save_flags_cli();
    UCHAR *p = (UCHAR *)Buffer;
    for (ULONG i = 0; i < Length;) {
        ULONG reg = RegisterNumber + i, shift = (reg & 3) * 8;
        ULONG n = 4 - (reg & 3) < Length - i ? 4 - (reg & 3) : Length - i; // bytes in this dword
        outl(PCI_ADDRESS, PciAddress(BusNumber, SlotNumber, reg));
        ULONG v = inl(PCI_DATA);
        if (WritePCISpace) {
            for (ULONG k = 0; k < n; k++)
                v = (v & ~(0xFFu << (shift + 8 * k))) | (ULONG)p[i + k] << (shift + 8 * k);
            outl(PCI_ADDRESS, PciAddress(BusNumber, SlotNumber, reg));
            outl(PCI_DATA, v);
        } else {
            for (ULONG k = 0; k < n; k++)
                p[i + k] = (UCHAR)(v >> (shift + 8 * k));
        }
        i += n;
    }
    // As the reference (0x8001530C): cli ... sti around the cycles, so it always returns with interrupts enabled.
    (void)flags;
    sti();
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

// A function's whole configuration space read, edited and written back, as the reference does for each device it
// sets up (0x80063908): every register is rewritten with its current value (status bits that are write-one-to-clear
// are cleared), and the interrupt pin is written as well.
struct PciSpace {
    UCHAR b[0x100];
    void Set8(ULONG r, UCHAR v) { b[r] = v; }
    void Set16(ULONG r, USHORT v) { b[r] = (UCHAR)v, b[r + 1] = (UCHAR)(v >> 8); }
    void Set32(ULONG r, ULONG v) { for (int i = 0; i < 4; i++) b[r + i] = (UCHAR)(v >> (8 * i)); }
};
template <typename F> static void PciEditAll(ULONG bus, ULONG dev, F edit) {
    PciSpace s;
    HalReadWritePCISpace(bus, dev, 0, s.b, 0x100, FALSE);
    edit(s);
    HalReadWritePCISpace(bus, dev, 0, s.b, 0x100, TRUE);
}

// Bus 0 device numbers on the Xbox (MCPX + NV2A host bridge).
enum : ULONG {
    DEV_HOST = 0, DEV_LPC = 1, DEV_USB0 = 2, DEV_USB1 = 3, DEV_NIC = 4,
    DEV_APU = 5, DEV_AC97 = 6, DEV_IDE = 9, DEV_AGP = 0x1E,
};

// --- Bus masters left running (OXOS addition, spec/oxos-internal/10-dma-quiesce.md). A modchip menu (XeniumOS) or
// another OS that started this image without a full hardware reset can leave the USB host controllers, the NIC, the
// audio and IDE DMA engines or the GPU's FIFO running, writing into RAM the kernel is about to reuse: corruption that
// hangs the boot at varying places on hardware, never in xemu. Before anything here enables bus mastering: log the
// state each device arrives in, turn bus mastering off and stop or reset each engine.
static void HalpQuiesceBusMasters() {
    constexpr ULONG DEVS[] = {2, 3, 4, 5, 6, 9};
    for (ULONG d : DEVS) {
        ULONG cmd = HalpPciRead32(0, d, 4);
        [[maybe_unused]] ULONG bar0 = HalpPciRead32(0, d, 0x10);
        XK_SERIAL("pci: at entry 0:%lx.0 cmd %08lx bar0 %08lx\n", d, cmd, bar0);
        HalpPciWrite16(0, d, 4, (USHORT)(cmd & ~4u)); // bus master off
    }
    [[maybe_unused]] ULONG gpuCmd = HalpPciRead32(1, 0, 4);
    // Memory-mapped engines in 0xFEC00000-0xFEFFFFFF (USB 0xFED00000 / 0xFED08000, NIC 0xFEF00000): not mapped yet
    // (the 2BL maps only RAM, the NV2A and the flash), so map that 4 MB window uncached for a moment through the
    // page directory's self-map (entry 0x300: the directory at 0xC0300000).
    auto pd = (volatile ULONG *)0xC0300000;
    ULONG saved = pd[0x3FB];
    pd[0x3FB] = 0xFEC00000 | 0x9B; // present, writable, write-through, cache-disabled, 4 MB
    __asm__ volatile("invlpg (%0)" ::"r"(0xFEC00000) : "memory");
    auto mmio = [](ULONG a) -> volatile ULONG & { return *(volatile ULONG *)a; };
    static const ULONG OHCI[] = {2, 3};
    for (ULONG d : OHCI) { // OHCI: HcControl 0x04, HcCommandStatus 0x08, HcHCCA 0x18
        ULONG bar = HalpPciRead32(0, d, 0x10) & ~0xFFFu;
        if (bar < 0xFEC00000 || bar >= 0xFF000000 || !(HalpPciRead32(0, d, 4) & 2))
            continue;
        XK_SERIAL("pci: USB %lu at entry: control %08lx hcca %08lx\n", d - 2, mmio(bar + 4), mmio(bar + 0x18));
        mmio(bar + 0x14) = 0xC000007F; // HcInterruptDisable: all
        mmio(bar + 4) = 0;             // HcControl: USB reset state, every list off
        mmio(bar + 8) = 1;             // HcCommandStatus: host controller reset (clears HCCA and the list pointers)
        for (int i = 0; i < 100 && (mmio(bar + 8) & 1); i++)
            KeStallExecutionProcessor(1);
    }
    ULONG nic = HalpPciRead32(0, 4, 0x10) & ~0xFFFu;
    if (nic >= 0xFEC00000 && nic < 0xFF000000 && (HalpPciRead32(0, 4, 4) & 2)) {
        XK_SERIAL("pci: NIC at entry: tx ring %08lx rx ring %08lx tx %lx rx %lx\n", mmio(nic + 0x100),
                  mmio(nic + 0x104), mmio(nic + 0x84), mmio(nic + 0x94));
        mmio(nic + 0x84) = 0; // transmitter off
        mmio(nic + 0x94) = 0; // receiver off
        mmio(nic + 0x004) = 0; // interrupts masked
        mmio(nic + 0x144) = 0x14; // TxRxControl: reset
        KeStallExecutionProcessor(10);
        mmio(nic + 0x144) = 0x04;
        mmio(nic + 0x100) = 0;
        mmio(nic + 0x104) = 0;
    }
    pd[0x3FB] = saved;
    __asm__ volatile("invlpg (%0)" ::"r"(0xFEC00000) : "memory");
    // AC'97 bus-master channels (I/O BAR1; ICH layout: control at 0x0B, 0x1B, 0x2B, S/PDIF 0x7B): stop, then reset.
    static const USHORT AC97_CONTROL[] = {0x0B, 0x1B, 0x2B, 0x7B};
    ULONG aci = HalpPciRead32(0, 6, 0x14);
    if ((aci & 1) && (aci & ~3u) && (HalpPciRead32(0, 6, 4) & 1))
        for (USHORT c : AC97_CONTROL) {
            outb((USHORT)((aci & ~3u) + c), 0);
            outb((USHORT)((aci & ~3u) + c), 2);
        }
    // IDE bus master (I/O BAR4): stop.
    ULONG ide = HalpPciRead32(0, 9, 0x20);
    if ((ide & 1) && (ide & ~3u))
        outb((USHORT)(ide & ~3u), 0);
    // GPU: its FIFO and graphics engines reset (PMC 0x200 bits 8 and 12 off and on again; the NV2A registers are mapped).
    auto pmc = (volatile ULONG *)0xFD000200;
    ULONG enable = *pmc;
    // The FIFO's registers only while the FIFO is switched on (PMC bit 8): a switched-off GPU block does not answer
    // properly, and touching one froze consoles (2026-10-23).
    XK_SERIAL("pci: NV2A at entry: cmd %08lx PMC enable %08lx PFIFO caches %08lx pusher %08lx\n", gpuCmd, enable,
              (enable & 0x100) ? *(volatile ULONG *)0xFD002500 : 0, (enable & 0x100) ? *(volatile ULONG *)0xFD003220 : 0);
    *pmc = enable & ~0x1100u;
    *pmc = enable;
}

void HalpInitPci() {
    HalpQuiesceBusMasters();
    // MCPX ISA/LPC bridge.
    HalpPciWrite8(0, DEV_LPC, 0x6A, 0x03);
    HalpPciWrite32(0, DEV_LPC, 0x6C, 0x0E065491);
    HalpPciWrite32(0, DEV_LPC, 0x64, 0x00000B0C);
    PciOr8(0, DEV_LPC, 0x81, 0x08);

    // IDE: I/O + bus master, both channels in legacy mode, bus-master registers at 0xFF60, timings.
    PciEditAll(0, DEV_IDE, [](PciSpace &c) {
        c.b[4] |= 5;
        c.b[9] &= 0xFA;
        c.Set32(0x20, 0xFF61);
        c.Set32(0x50, 0x00000002);
        c.Set32(0x58, 0x20202020);
        c.Set32(0x60, 0xC0C0C0C0);
    });
    // The bus-master engine keeps its registers across a reset of its owner (a modchip menu or another BIOS started
    // this image): stop it, clear its status and table pointer before anything selects a device. A start bit left
    // set would let the first device to raise DMARQ (the DVD probe's commands) write into reused RAM.
    XK_SERIAL("pci: IDE bus master at entry: command %#x status %#x table %08lx\n", inb(0xFF60), inb(0xFF62),
              inl(0xFF64));
    outb(0xFF60, 0);
    outb(0xFF62, 0x06);
    outl(0xFF64, 0);
    XK_SERIAL("pci: IDE bus master after reset: command %#x status %#x\n", inb(0xFF60), inb(0xFF62));

    // NIC.
    PciEditAll(0, DEV_NIC, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set32(0x10, 0xFEF00000);
        c.Set32(0x14, 0x0000E001);
        c.Set8(0x3C, 4);
        c.Set8(0x3D, 0);
    });

    // USB host controllers.
    PciEditAll(0, DEV_USB0, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set32(0x10, 0xFED00000);
        c.Set8(0x3C, 1);
        c.Set8(0x3D, 0);
    });
    HalpPciWrite32(0, DEV_USB0, 0x50, 0x0000000F);
    PciEditAll(0, DEV_USB1, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set32(0x10, 0xFED08000);
        c.Set8(0x3C, 9);
        c.Set8(0x3D, 0);
    });
    HalpPciWrite32(0, DEV_USB1, 0x50, 0x00000030);

    // AC'97 and APU.
    PciEditAll(0, DEV_AC97, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set8(0x3C, 6);
        c.Set8(0x3D, 0);
        c.Set32(0x10, 0x0000D001);
        c.Set32(0x14, 0x0000D201);
        c.Set32(0x18, 0xFEC00000);
    });
    PciEditAll(0, DEV_APU, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set8(0x3C, 5);
        c.Set8(0x3D, 0);
        c.Set32(0x10, 0xFE800000);
    });

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
    PciEditAll(0, DEV_AGP, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set16(0x26, (USHORT)(0xF3F0 | ((XboxHardwareInfo.Flags & 8) << 7)));
        c.Set8(0x18, 0x00);
        c.Set8(0x19, 0x01);
        c.Set8(0x1A, 0x01);
        c.Set32(0x20, 0xFE70FD00);
        c.Set16(0x24, 0xF000);
    });

    // Host bridge, then NV2A (bus 1, device 0).
    HalpPciWrite32(0, DEV_HOST, 0x48, 0x00000114);
    PciEditAll(1, 0, [](PciSpace &c) {
        c.b[4] |= 7;
        c.Set8(0x3C, 3);
        c.Set8(0x3D, 1);
    });
    HalpPciWrite32(1, 0, 0x4C, 0x00000114);
#if XK_DEBUG_SERIAL
    static const UCHAR devs[] = {0, 1, 2, 3, 4, 5, 6, 9, 0x1E};
    for (UCHAR d : devs)
        XK_SERIAL("pci: 0:%x.0 ids %08lx cmd %08lx\n", d, HalpPciRead32(0, d, 0), HalpPciRead32(0, d, 4));
    XK_SERIAL("pci: 1:0.0 ids %08lx; dev1 0x80 %08lx 0x8c %08lx 0xc8 %08lx; ide 0x50 %08lx\n", HalpPciRead32(1, 0, 0),
              HalpPciRead32(0, 1, 0x80), HalpPciRead32(0, 1, 0x8C), HalpPciRead32(0, 1, 0xC8), HalpPciRead32(0, 9, 0x50));
    XK_SERIAL("pci: memory: host 0x84 %08lx, NV2A PFB 0x100200 %08lx 0x100204 %08lx, NVPLL %08lx, 0x68050C %08lx\n",
              HalpPciRead32(0, 0, 0x84), *(volatile ULONG *)0xFD100200, *(volatile ULONG *)0xFD100204,
              *(volatile ULONG *)0xFD680500, *(volatile ULONG *)0xFD68050C);
#endif
}

// ACPI power-management block at I/O 0x8000.
void HalpInitAcpiPm() {
    outw(0x8004, inw(0x8004) | 0x0001); // PM1 control: SCI enable
    outw(0x8022, inw(0x8022) | 0x0002);
    outw(0x8002, inw(0x8002) | 0x0001); // PM1 enable: timer
    outw(0x8028, inw(0x8028) | 0x0001);
}
