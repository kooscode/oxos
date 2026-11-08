// The network card for the kernel's network stack (spec/oxos_internal/31_upload_server.md §2): its rings in
// contiguous memory, frames in and out by polling, and the bring-up and shut-down xk/nic.h shares with the 2BL.
// Boot-only.
#include <xk/network_stack.h>
#include <xk/core.h>
#include <xk/hardware.h>
#include <xk/memory.h>
#include <xk/runtime.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")
// After the pragmas: the card's inline bring-up is boot-only code too (only the network stack uses it).
#include <xk/nic.h>

// The descriptors (RX_COUNT receive, then TX_COUNT transmit) in one uncached page: the card and the processor both
// change them. The frame buffers, NIC_BUFFER bytes for each descriptor, in ordinary cached memory: the MCPX's DMA
// keeps the processor's cache coherent, and an uncached buffer made every byte the stack reads a trip to RAM (the
// upload ran at a tenth of its speed). 64 receive buffers hold 64 full frames: more than the 64 KB a TCP sender may
// have in flight (tcp.cpp), so frames that arrive while the caller writes to the disk are not lost.
constexpr ULONG RX_COUNT = 64, TX_COUNT = 32;
constexpr ULONG BUFFERS_SIZE = (RX_COUNT + TX_COUNT) * NIC_BUFFER;
static_assert((RX_COUNT + TX_COUNT) * sizeof(NIC_DESCRIPTOR) <= PAGE_SIZE, "the descriptors fit one page");

UCHAR NetMac[6];
static PUCHAR NetpRing;          // the descriptors (uncached), or null while the card is off
static PUCHAR NetpBuffers;       // the frame buffers
static ULONG NetpRxHead;         // the next receive descriptor to look at
static ULONG NetpTxNext;         // the next transmit descriptor to use

static NIC_DESCRIPTOR *NetpRx() {
    return (NIC_DESCRIPTOR *)NetpRing;
}

static NIC_DESCRIPTOR *NetpTx() {
    return NetpRx() + RX_COUNT;
}

// Buffer n: receive buffers first, then transmit buffers.
static PUCHAR NetpBuffer(ULONG n) {
    return NetpBuffers + n * NIC_BUFFER;
}

ULONG NetpMilliseconds() {
    return (ULONG)(KeQueryInterruptTime() / 10000);
}

bool NetpCardStart(ULONG LinkWait) {
    if (!NetpRing) {
        NetpRing = (PUCHAR)MmAllocateContiguousMemoryEx(PAGE_SIZE, 0, 0x3FFFFFF, PAGE_SIZE,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
        NetpBuffers = (PUCHAR)MmAllocateContiguousMemoryEx(BUFFERS_SIZE, 0, 0x3FFFFFF, PAGE_SIZE, PAGE_READWRITE);
        if (!NetpRing || !NetpBuffers) {
            NetpCardStop();
            return false;
        }
        // The console's own address, so DHCP gives the server the address its titles get; a locally administered
        // one when the EEPROM has none.
        ULONG type, length;
        static const UCHAR fallback[6] = {0x02, 0x58, 0x42, 0x4F, 0x58, 0x31};
        if (!NT_SUCCESS(ExQueryNonVolatileSetting(0x101, &type, NetMac, sizeof(NetMac), &length)) ||
            !(NetMac[0] | NetMac[1] | NetMac[2] | NetMac[3] | NetMac[4] | NetMac[5]))
            RtlCopyMemory(NetMac, fallback, sizeof(NetMac));
    }
    RtlZeroMemory(NetpRing, PAGE_SIZE);
    for (ULONG i = 0; i < RX_COUNT; i++)
        NetpRx()[i] = {MmGetPhysicalAddress(NetpBuffer(i)), (USHORT)NIC_BUFFER, NIC_RX_AVAIL};
    NetpRxHead = NetpTxNext = 0;
    return NicStart(NetMac, MmGetPhysicalAddress(NetpRx()), RX_COUNT, MmGetPhysicalAddress(NetpTx()), TX_COUNT,
                    LinkWait);
}

void NetpCardStop() {
    if (NetpRing)
        NicStop();
    if (NetpRing)
        MmFreeContiguousMemory(NetpRing);
    if (NetpBuffers)
        MmFreeContiguousMemory(NetpBuffers);
    NetpRing = NetpBuffers = nullptr;
}

PUCHAR NetpFrameBuffer() {
    NIC_DESCRIPTOR &d = NetpTx()[NetpTxNext % TX_COUNT];
    for (ULONG t = NicNow(); (d.Flags & NIC_TX_VALID) && NicBefore(t, 2000);) {
        // Wait for the card to finish with this descriptor, at most 2 ms.
    }
    return (d.Flags & NIC_TX_VALID) ? nullptr : NetpBuffer(RX_COUNT + NetpTxNext % TX_COUNT);
}

void NetpFrameSend(ULONG Length) {
    PUCHAR buffer = NetpBuffer(RX_COUNT + NetpTxNext % TX_COUNT);
    for (; Length < 60; Length++) // the shortest Ethernet frame
        buffer[Length] = 0;
    NicTransmit(NetpTx()[NetpTxNext % TX_COUNT], MmGetPhysicalAddress(buffer), Length);
    NetpTxNext++;
}

ULONG NetpFramesIn() {
    ULONG n = 0;
    for (; n < RX_COUNT; n++) {
        NIC_DESCRIPTOR &d = NetpRx()[NetpRxHead];
        if (d.Flags & NIC_RX_AVAIL)
            break;
        ULONG length = d.Length & 0x3FFF;
        if ((d.Flags & NIC_RX_VALID) && !(d.Flags & NIC_RX_ERRORS) && length <= NIC_BUFFER)
            NetpFrameReceived(NetpBuffer(NetpRxHead), length);
        NicGiveBack(d);
        NetpRxHead = (NetpRxHead + 1) % RX_COUNT;
    }
    return n;
}
