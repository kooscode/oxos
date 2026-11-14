// The network card (MCPX nForce Ethernet, src/hardware/ethernet/): the driver the kernel's network stack (src/network/) sends and receives frames through
// (nic.cpp), and the PHY exports titles call (phy.cpp). The card's registers and bring-up are in nic.h, shared with the 2BL's netboot.
#pragma once
#include <runtime/types.h>

// The Ethernet PHY over the card's MII management interface (phy.cpp).
XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2);
XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN Verify);

namespace xk::hardware::ethernet {

// The card for the network stack (nic.cpp): frames in and out, polled. Each received frame goes to network::FrameReceived.

extern UCHAR MacAddress[6];     // the console's MAC address (EEPROM factory setting 0x101)
bool StartCard(ULONG LinkWait); // rings filled, card started; false without a link within LinkWait us
void StopCard();
PUCHAR NextFrameBuffer();     // the next transmit buffer to build a frame in; null while busy
void SendFrame(ULONG Length); // sends the frame built in it
ULONG ReceiveFrames();        // each received frame to FrameReceived; how many
ULONG Milliseconds();         // a millisecond clock

} // namespace xk::hardware::ethernet
