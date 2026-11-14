// The network stack's internal interface between its files (src/network/): ip.cpp (Ethernet, ARP, IPv4, ICMP, DHCP) and
// tcp.cpp (TCP), on the network card's driver (hardware/ethernet/ethernet.h). spec/oxos_internal/31_upload_server.md
// §2.
#pragma once
#include <network/network.h>
#include <hardware/ethernet/ethernet.h>

constexpr ULONG NET_FRAME = 1514;             // the largest Ethernet frame (no FCS)
constexpr ULONG NET_IP_HEADER = 20;           // no options
constexpr ULONG NET_IP_PAYLOAD = NET_FRAME - 14 - NET_IP_HEADER;
constexpr UCHAR NET_ICMP = 1, NET_TCP = 6, NET_UDP = 17;

namespace xk::network {

// Big-endian fields.
inline USHORT LoadBigEndian16(const UCHAR *p) {
    return (USHORT)(p[0] << 8 | p[1]);
}

inline ULONG LoadBigEndian32(const UCHAR *p) {
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

inline void StoreBigEndian16(PUCHAR p, USHORT v) {
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

inline void StoreBigEndian32(PUCHAR p, ULONG v) {
    StoreBigEndian16(p, (USHORT)(v >> 16));
    StoreBigEndian16(p + 2, (USHORT)v);
}

// --- ip.cpp

extern UCHAR IpAddress[4];                        // our address (zeros until DHCP or the fixed address)
void FrameReceived(const UCHAR *Frame, ULONG Length);
// The one's complement sum of Length bytes, added to Sum (not yet folded or inverted).
ULONG Checksum(const UCHAR *Data, ULONG Length, ULONG Sum);
USHORT FoldChecksum(ULONG Sum); // folded and inverted: the checksum field
// An IPv4 packet, built in a transmit buffer: IpPayload gives the place for its payload (NET_IP_PAYLOAD bytes;
// null while the card is busy), then SendIp adds the headers and sends it.
PUCHAR IpPayload();
void SendIp(const UCHAR *DestinationMac, const UCHAR *DestinationIp, UCHAR Protocol, ULONG Length);

// --- tcp.cpp

bool StartTcp(); // the connections' memory
void StopTcp();
void TcpReceived(const UCHAR *SourceMac, const UCHAR *SourceIp, const UCHAR *Segment, ULONG Length);
void PollTcp();  // what is due: data to send, retransmissions, time-outs

} // namespace xk::network
