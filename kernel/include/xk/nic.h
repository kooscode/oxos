// The MCPX's network card (NVIDIA nForce Ethernet), polled: its registers, descriptors and the bring-up and shut-down
// sequences, shared by the 2BL's netboot (boot/netboot.h) and the kernel's network stack (src/network/nic.cpp;
// spec/oxos_internal/09_netboot.md, 31_upload_server.md). Header-only with plain C++ types, so the 2BL (built without
// the kernel headers) can include it.
//
// Both see the card the same way: its registers at 0xFEF00000 (physical in the 2BL; an uncached identity mapping in
// the kernel), PCI configuration through ports 0xCF8/0xCFC, time from the ACPI PM timer at I/O 0x8008. Only the
// rings differ: the caller fills them and passes their physical addresses. The sequence follows XboxDev/cromwell's
// etherboot forcedeth order; the descriptors follow nxdk's nvnetdrv (MIT): a descriptor handed to the card has flags
// bit 15 (available) set; the card clears it and sets bit 0 (valid) and the length; the driver gives it back and
// writes TxRxControl bit 1 (get).
#pragma once

typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned long ULONG;

constexpr ULONG NIC_REGISTERS = 0xFEF00000; // BAR0, as the kernel's xk::hardware::InitPci assigns it
constexpr ULONG NIC_PCI_DEVICE = 4;         // bus 0, device 4, function 0
constexpr ULONG NIC_BUFFER = 2048;          // bytes per frame buffer (a frame is at most 1514)

enum : ULONG {
    NvIrqStatus = 0x000, NvIrqMask = 0x004, NvSetup6 = 0x008, NvPollingInterval = 0x00C, NvMisc1 = 0x080,
    NvTxControl = 0x084, NvTxStatus = 0x088, NvPacketFilter = 0x08C, NvOffload = 0x090, NvRxControl = 0x094,
    NvRxStatus = 0x098, NvRandomSeed = 0x09C, NvSetup1 = 0x0A0, NvSetup2 = 0x0A4, NvMacA = 0x0A8, NvMacB = 0x0AC,
    NvMcastA = 0x0B0, NvMcastB = 0x0B4, NvMcastMaskA = 0x0B8, NvMcastMaskB = 0x0BC, NvTxRing = 0x100,
    NvRxRing = 0x104, NvRingSizes = 0x108, NvTxUnknown = 0x10C, NvLinkSpeed = 0x110, NvSetup5 = 0x130,
    NvSetup3 = 0x134, NvTxRxControl = 0x144, NvMiiStatus = 0x180, NvSetup4 = 0x184, NvAdapterControl = 0x188,
    NvMiiSpeed = 0x18C, NvMiiControl = 0x190, NvMiiData = 0x194, NvWakeUpFlags = 0x200, NvPowerState = 0x26C,
};

// A ring entry: the buffer's physical address, the length (transmit: bytes - 1; receive: the buffer's size, then
// the frame's length in bits 0-13) and the flags.
struct NIC_DESCRIPTOR {
    ULONG Buffer;
    USHORT Length;
    USHORT Flags;
};
constexpr USHORT NIC_TX_VALID = 1u << 15, NIC_TX_FLAGS = (1u << 0) | (1u << 8) | (1u << 15);
constexpr USHORT NIC_RX_AVAIL = 1u << 15, NIC_RX_VALID = 1u << 0, NIC_RX_ERRORS = 0x1F80; // errors 1-4, CRC, overflow
constexpr ULONG NIC_TXRX_KICK = 1, NIC_TXRX_GET = 2, NIC_TXRX_BIT2 = 4, NIC_TXRX_RESET = 0x10;

namespace xk::network {

inline volatile ULONG &CardRegister(ULONG Offset) {
    return *(volatile ULONG *)(NIC_REGISTERS + Offset);
}

// --- Ports, time, PCI

inline ULONG CardIn32(USHORT Port) {
    ULONG v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(Port));
    return v;
}

inline void CardOut32(USHORT Port, ULONG Value) {
    __asm__ volatile("outl %0, %1" ::"a"(Value), "Nd"(Port));
}

// The ACPI PM timer: 3.375 MHz, free-running.
inline ULONG CardTicks() {
    return CardIn32(0x8008);
}

// Less than Microseconds have passed since Start (a CardTicks value).
inline bool WithinMicroseconds(ULONG Start, ULONG Microseconds) {
    return CardTicks() - Start < Microseconds * 27 / 8;
}

inline void StallMicroseconds(ULONG Microseconds) {
    for (ULONG t = CardTicks(); WithinMicroseconds(t, Microseconds);) {
        // Spin until Microseconds have passed.
    }
}

inline ULONG ReadCardPci(ULONG Register) {
    CardOut32(0xCF8, 0x80000000 | NIC_PCI_DEVICE << 11 | Register);
    return CardIn32(0xCFC);
}

inline void WriteCardPci(ULONG Register, ULONG Value) {
    CardOut32(0xCF8, 0x80000000 | NIC_PCI_DEVICE << 11 | Register);
    CardOut32(0xCFC, Value);
}

// --- PHY 1 over MII (as kernel/src/hardware/ethernet.cpp)

inline bool WaitForMii() {
    for (ULONG t = CardTicks(); WithinMicroseconds(t, 10000);) {
        if (!(CardRegister(NvMiiControl) & 0x8000))
            return true;
    }
    return false;
}

inline ULONG ReadMii(ULONG Register) {
    WaitForMii();
    CardRegister(NvMiiControl) = Register | 0x20;
    WaitForMii();
    return CardRegister(NvMiiData);
}

// The link is up and auto-negotiation has finished.
inline bool IsLinkUp() {
    return (ReadMii(1) & 0x24) == 0x24;
}

// --- Bring-up and shut-down

// Starts the card with the console's Mac and the caller's rings: RxCount receive descriptors at RxRing and TxCount
// transmit descriptors at TxRing (physical addresses), already filled (receive: buffer, NIC_BUFFER, NIC_RX_AVAIL;
// transmit: all zero). Waits up to LinkWait microseconds for the link (auto-negotiation takes about 2-3 s after
// power-on); false without a link, or without the card.
inline bool ProgramCard(const UCHAR *Mac, ULONG RxRing, ULONG RxCount, ULONG TxRing, ULONG TxCount, ULONG LinkWait) {
    WriteCardPci(0x10, NIC_REGISTERS);
    WriteCardPci(0x14, 0xE001);
    WriteCardPci(0x04, ReadCardPci(0x04) | 7); // I/O, memory, bus master
    ULONG ids = ReadCardPci(0x00);
    if (ids == 0xFFFFFFFF || !ids)
        return false;
    auto nv = [](ULONG offset) -> volatile ULONG & { return CardRegister(offset); };
    nv(NvMcastA) = 1;
    nv(NvMcastB) = 0;
    nv(NvMcastMaskA) = 0;
    nv(NvMcastMaskB) = 0;
    nv(NvPacketFilter) = 0;
    nv(NvAdapterControl) = 0;
    nv(NvLinkSpeed) = 0;
    nv(NvTxUnknown) = 0;
    nv(NvTxRxControl) = NIC_TXRX_BIT2 | NIC_TXRX_RESET;
    (void)nv(NvTxRxControl);
    StallMicroseconds(4);
    nv(NvTxRxControl) = NIC_TXRX_BIT2;
    nv(NvSetup6) = 0;
    nv(NvMacA) = (ULONG)Mac[0] | Mac[1] << 8 | Mac[2] << 16 | (ULONG)Mac[3] << 24; // byte order as nxdk
    nv(NvMacB) = (ULONG)Mac[4] | Mac[5] << 8;
    nv(NvSetup3) = 0x200010;
    nv(NvTxRxControl) = 0;
    (void)nv(NvTxRxControl);
    nv(NvTxRxControl) = NIC_TXRX_GET;
    for (ULONG t = CardTicks(); !(nv(NvSetup5) & 1u << 31) && WithinMicroseconds(t, 50000);) {
        // Wait for the transmit/receive engine to start (bit 31), at most 50 ms.
    }
    nv(NvSetup4) = 0;
    nv(NvMiiSpeed) = (1u << 8) | 5;
    bool link = false;
    for (ULONG t = CardTicks(); !(link = IsLinkUp()) && WithinMicroseconds(t, LinkWait);)
        StallMicroseconds(10000);
    if (!link)
        return false;
    ULONG common = ReadMii(4) & ReadMii(5);
    bool fast = common & 0x180, full = fast ? (common & 0x100) : (common & 0x40);
    nv(NvMisc1) = 0x3B0F3C | (full ? 0 : 2);
    nv(NvTxStatus) = nv(NvTxStatus);
    nv(NvPacketFilter) = 0x7F0020; // always + our address (broadcasts included), as nxdk
    nv(NvOffload) = 0x5EE;
    nv(NvRxStatus) = nv(NvRxStatus);
    nv(NvRandomSeed) = 0x7F00 | (CardTicks() & 0xFF);
    nv(NvSetup1) = 0x16070F;
    nv(NvSetup2) = 0x16;
    nv(NvPollingInterval) = 970;
    nv(NvSetup6) = 3;
    nv(NvAdapterControl) = (1u << 24) | 0x4000;
    nv(NvSetup4) = 8;
    nv(NvWakeUpFlags) = 0x7770;
    nv(NvRxRing) = RxRing;
    nv(NvTxRing) = TxRing;
    nv(NvRingSizes) = ((RxCount - 1) << 16) | (TxCount - 1);
    ULONG power = nv(NvPowerState);
    if (!(power & 0x8000))
        nv(NvPowerState) = power | 0x8000;
    StallMicroseconds(10);
    nv(NvPowerState) = nv(NvPowerState) | 0x100;
    nv(NvAdapterControl) = 0x100000;
    nv(NvIrqMask) = 0;
    nv(NvIrqStatus) = 0x1FF;
    nv(NvMiiStatus) = 0xF;
    nv(NvIrqStatus) = 0x1FF;
    nv(NvLinkSpeed) = 0x10000 | (fast ? 100 : 1000); // forcedeth: 100 = 100 Mbit/s, 1000 = 10 Mbit/s
    nv(NvTxControl) = 1;
    nv(NvRxControl) = 1;
    nv(NvTxRxControl) = NIC_TXRX_KICK | NIC_TXRX_GET;
    (void)nv(NvTxControl);
    return true;
}

// Leaves the card quiet: no DMA into memory that is about to be reused. A title's own driver sets it up again.
inline void ResetCard() {
    auto nv = [](ULONG offset) -> volatile ULONG & { return CardRegister(offset); };
    nv(NvRxControl) = 0;
    nv(NvTxControl) = 0;
    StallMicroseconds(100);
    nv(NvTxRxControl) = NIC_TXRX_BIT2 | NIC_TXRX_RESET;
    (void)nv(NvTxRxControl);
    StallMicroseconds(10);
    nv(NvTxRxControl) = NIC_TXRX_BIT2;
    nv(NvRxRing) = 0;
    nv(NvTxRing) = 0;
    nv(NvAdapterControl) = 0;
    nv(NvIrqMask) = 0;
    nv(NvIrqStatus) = 0x1FF;
}

// Hands a filled transmit buffer (physical address, Length bytes, at least 60) to descriptor d and starts sending.
inline void QueueTransmit(NIC_DESCRIPTOR &d, ULONG Buffer, ULONG Length) {
    d.Buffer = Buffer;
    d.Length = (USHORT)(Length - 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d.Flags = NIC_TX_FLAGS;
    CardRegister(NvTxRxControl) = NIC_TXRX_KICK;
    (void)CardRegister(NvTxRxControl);
}

// Gives receive descriptor d back to the card, for a buffer of NIC_BUFFER bytes.
inline void ReturnReceiveDescriptor(NIC_DESCRIPTOR &d) {
    d.Length = NIC_BUFFER;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d.Flags = NIC_RX_AVAIL;
    CardRegister(NvTxRxControl) = NIC_TXRX_GET;
}

} // namespace xk::network
