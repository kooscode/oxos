// The network stack's internal interface between its files (src/network/): nic.cpp (the card, frames in and out),
// ip.cpp (Ethernet, ARP, IPv4, ICMP, DHCP) and tcp.cpp (TCP). spec/oxos_internal/31_upload_server.md §2.
#pragma once
#include <xk/network.h>

constexpr ULONG NET_FRAME = 1514;             // the largest Ethernet frame (no FCS)
constexpr ULONG NET_IP_HEADER = 20;           // no options
constexpr ULONG NET_IP_PAYLOAD = NET_FRAME - 14 - NET_IP_HEADER;
constexpr UCHAR NET_ICMP = 1, NET_TCP = 6, NET_UDP = 17;

// Big-endian fields.
inline USHORT NetGet16(const UCHAR *p) {
    return (USHORT)(p[0] << 8 | p[1]);
}

inline ULONG NetGet32(const UCHAR *p) {
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

inline void NetPut16(PUCHAR p, USHORT v) {
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

inline void NetPut32(PUCHAR p, ULONG v) {
    NetPut16(p, (USHORT)(v >> 16));
    NetPut16(p + 2, (USHORT)v);
}

// --- nic.cpp

extern UCHAR NetMac[6]; // the console's MAC address (EEPROM factory setting 0x101)
bool NetpCardStart(ULONG LinkWait);           // rings filled, card started; false without a link within LinkWait us
void NetpCardStop();
PUCHAR NetpFrameBuffer();                     // the next transmit buffer to build a frame in; null while busy
void NetpFrameSend(ULONG Length);             // sends the frame built in it
ULONG NetpFramesIn();                         // each received frame to NetpFrameReceived; how many
ULONG NetpMilliseconds();                     // a millisecond clock

// --- ip.cpp

extern UCHAR NetIp[4];                        // our address (zeros until DHCP or the fixed address)
void NetpFrameReceived(const UCHAR *Frame, ULONG Length);
// The one's complement sum of Length bytes, added to Sum (not yet folded or inverted).
ULONG NetpSum(const UCHAR *Data, ULONG Length, ULONG Sum);
USHORT NetpFold(ULONG Sum); // folded and inverted: the checksum field
// An IPv4 packet, built in a transmit buffer: NetpIpPayload gives the place for its payload (NET_IP_PAYLOAD bytes;
// null while the card is busy), then NetpSendIp adds the headers and sends it.
PUCHAR NetpIpPayload();
void NetpSendIp(const UCHAR *DestinationMac, const UCHAR *DestinationIp, UCHAR Protocol, ULONG Length);

// --- tcp.cpp

bool NetpTcpStart(); // the connections' memory
void NetpTcpStop();
void NetpTcpReceived(const UCHAR *SourceMac, const UCHAR *SourceIp, const UCHAR *Segment, ULONG Length);
void NetpTcpPoll();  // what is due: data to send, retransmissions, time-outs
