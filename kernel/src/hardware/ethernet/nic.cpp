// The network card for the kernel's network stack (spec/oxos_internal/31_upload_server.md §2): its rings in contiguous memory, frames in and out by polling,
// and the bring-up and shut-down hardware/ethernet/nic.h shares with the 2BL. Boot-only.
#include <network/network_stack.h>
#include <core/core.h>
#include <hardware/chipset/chipset.h>
#include <memory/memory.h>
#include <runtime/runtime.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")
// After the pragmas: the card's inline bring-up is boot-only code too (only the network stack uses it).
#include <hardware/ethernet/nic.h>

namespace xk::hardware::ethernet {

// The descriptors (RX_COUNT receive, then TX_COUNT transmit) in one uncached page: the card and the processor both change them. The frame buffers, NIC_BUFFER
// bytes for each descriptor, in ordinary cached memory: the MCPX's DMA keeps the processor's cache coherent, and an uncached buffer made every byte the stack
// reads a trip to RAM (the upload ran at a tenth of its speed). 64 receive buffers hold 64 full frames: more than the 64 KB a TCP sender may have in flight
// (tcp.cpp), so frames that arrive while the caller writes to the disk are not lost.
constexpr ULONG RX_COUNT = 64, TX_COUNT = 32;
constexpr ULONG BUFFERS_SIZE = (RX_COUNT + TX_COUNT) * NIC_BUFFER;
static_assert((RX_COUNT + TX_COUNT) * sizeof(NIC_DESCRIPTOR) <= PAGE_SIZE, "the descriptors fit one page");

UCHAR MacAddress[6];
static PUCHAR DescriptorRing; // the descriptors (uncached), or null while the card is off
static PUCHAR FrameBuffers;   // the frame buffers
static ULONG ReceiveHead;     // the next receive descriptor to look at
static ULONG TransmitNext;    // the next transmit descriptor to use

static NIC_DESCRIPTOR *ReceiveDescriptor() {
    return (NIC_DESCRIPTOR *)DescriptorRing;
}

static NIC_DESCRIPTOR *TransmitDescriptor() {
    return ReceiveDescriptor() + RX_COUNT;
}

// Buffer n: receive buffers first, then transmit buffers.
static PUCHAR FrameBufferAt(ULONG n) {
    return FrameBuffers + n * NIC_BUFFER;
}

ULONG Milliseconds() {
    return (ULONG)(KeQueryInterruptTime() / 10000);
}

bool StartCard(ULONG LinkWait) {
    if (!DescriptorRing) {
        DescriptorRing = (PUCHAR)MmAllocateContiguousMemoryEx(PAGE_SIZE, 0, 0x3FFFFFF, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        FrameBuffers = (PUCHAR)MmAllocateContiguousMemoryEx(BUFFERS_SIZE, 0, 0x3FFFFFF, PAGE_SIZE, PAGE_READWRITE);
        if (!DescriptorRing || !FrameBuffers) {
            StopCard();
            return false;
        }
        // The console's own address, so DHCP gives the server the address its titles get; a locally administered one when the EEPROM has none.
        ULONG type, length;
        static const UCHAR fallback[6] = {0x02, 0x58, 0x42, 0x4F, 0x58, 0x31};
        if (!NT_SUCCESS(ExQueryNonVolatileSetting(0x101, &type, MacAddress, sizeof(MacAddress), &length)) ||
            !(MacAddress[0] | MacAddress[1] | MacAddress[2] | MacAddress[3] | MacAddress[4] | MacAddress[5]))
            memcpy(MacAddress, fallback, sizeof(MacAddress));
    }
    memset(DescriptorRing, 0, PAGE_SIZE);
    for (ULONG i = 0; i < RX_COUNT; i++)
        ReceiveDescriptor()[i] = {MmGetPhysicalAddress(FrameBufferAt(i)), (USHORT)NIC_BUFFER, NIC_RX_AVAIL};
    ReceiveHead = TransmitNext = 0;
    return ProgramCard(MacAddress, MmGetPhysicalAddress(ReceiveDescriptor()), RX_COUNT, MmGetPhysicalAddress(TransmitDescriptor()), TX_COUNT, LinkWait);
}

void StopCard() {
    if (DescriptorRing)
        ResetCard();
    if (DescriptorRing)
        MmFreeContiguousMemory(DescriptorRing);
    if (FrameBuffers)
        MmFreeContiguousMemory(FrameBuffers);
    DescriptorRing = FrameBuffers = nullptr;
}

PUCHAR NextFrameBuffer() {
    NIC_DESCRIPTOR &d = TransmitDescriptor()[TransmitNext % TX_COUNT];
    for (ULONG t = CardTicks(); (d.Flags & NIC_TX_VALID) && WithinMicroseconds(t, 2000);) {
        // Wait for the card to finish with this descriptor, at most 2 ms.
    }
    return (d.Flags & NIC_TX_VALID) ? nullptr : FrameBufferAt(RX_COUNT + TransmitNext % TX_COUNT);
}

void SendFrame(ULONG Length) {
    PUCHAR buffer = FrameBufferAt(RX_COUNT + TransmitNext % TX_COUNT);
    for (; Length < 60; Length++) // the shortest Ethernet frame
        buffer[Length] = 0;
    QueueTransmit(TransmitDescriptor()[TransmitNext % TX_COUNT], MmGetPhysicalAddress(buffer), Length);
    TransmitNext++;
}

ULONG ReceiveFrames() {
    ULONG n = 0;
    for (; n < RX_COUNT; n++) {
        NIC_DESCRIPTOR &d = ReceiveDescriptor()[ReceiveHead];
        if (d.Flags & NIC_RX_AVAIL)
            break;
        ULONG length = d.Length & 0x3FFF;
        if ((d.Flags & NIC_RX_VALID) && !(d.Flags & NIC_RX_ERRORS) && length <= NIC_BUFFER)
            network::FrameReceived(FrameBufferAt(ReceiveHead), length);
        ReturnReceiveDescriptor(d);
        ReceiveHead = (ReceiveHead + 1) % RX_COUNT;
    }
    return n;
}

} // namespace xk::hardware::ethernet
