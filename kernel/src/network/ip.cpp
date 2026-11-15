// Ethernet, ARP, IPv4, ICMP echo and DHCP for the kernel's network stack, and its entry points (spec/oxos_internal/31_upload_server.md §2). Boot-only.
//
// Only what a server on the local network needs: ARP answers requests for our address (we never ask: every reply goes to the MAC address the request came from,
// a router's for a browser on another subnet), IPv4 without options or fragments, ping, and a DHCP client without renewals (the server runs for minutes).
#include <network/network_stack.h>
#include <runtime/runtime.h>

namespace xk::network {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

constexpr USHORT ETHERTYPE_IP = 0x0800, ETHERTYPE_ARP = 0x0806;
constexpr USHORT DHCP_SERVER_PORT = 67, DHCP_CLIENT_PORT = 68;
constexpr UCHAR DHCP_DISCOVER = 1, DHCP_OFFER = 2, DHCP_REQUEST = 3, DHCP_ACK = 5, DHCP_NAK = 6;
constexpr ULONG DHCP_RETRY_MS = 4000, DHCP_GIVE_UP_MS = 30000;
constexpr ULONG LINK_CHECK_MS = 2000;

static const UCHAR BroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UCHAR BroadcastIp[4] = {255, 255, 255, 255};
static const UCHAR FixedIp[4] = {XK_NETBOOT_IP}; // make NETBOOT_IP=, the netboot address

UCHAR IpAddress[4];
static NET_STATE AddressState;
static PUCHAR BuildingFrame; // the transmit buffer the packet being built is in (IpPayload, SendIp)
static USHORT NextIpId;      // the IP identification of the next packet

// DHCP: the transaction, the offered address and its server, what was last sent and when.
static ULONG DhcpXid;
static UCHAR OfferedIp[4], DhcpServer[4];
static UCHAR DhcpSent;              // DHCP_DISCOVER or DHCP_REQUEST
static ULONG DhcpSentMs, DhcpStartMs, LinkCheckMs;

// --- Checksums and sending

ULONG Checksum(const UCHAR *Data, ULONG Length, ULONG Sum) {
    for (ULONG i = 0; i + 1 < Length; i += 2)
        Sum += (ULONG)Data[i] << 8 | Data[i + 1];
    if (Length & 1)
        Sum += (ULONG)Data[Length - 1] << 8;
    return Sum;
}

USHORT FoldChecksum(ULONG Sum) {
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (USHORT)~Sum;
}

PUCHAR IpPayload() {
    BuildingFrame = hardware::ethernet::NextFrameBuffer();
    return BuildingFrame ? BuildingFrame + 14 + NET_IP_HEADER : nullptr;
}

void SendIp(const UCHAR *DestinationMac, const UCHAR *DestinationIp, UCHAR Protocol, ULONG Length) {
    PUCHAR e = BuildingFrame, h = BuildingFrame + 14;
    memcpy(e, DestinationMac, 6);
    memcpy(e + 6, hardware::ethernet::MacAddress, 6);
    StoreBigEndian16(e + 12, ETHERTYPE_IP);
    memset(h, 0, NET_IP_HEADER);
    h[0] = 0x45; // IPv4, 20-byte header
    StoreBigEndian16(h + 2, (USHORT)(NET_IP_HEADER + Length));
    StoreBigEndian16(h + 4, NextIpId++);
    StoreBigEndian16(h + 6, 0x4000); // don't fragment
    h[8] = 64;                       // time to live
    h[9] = Protocol;
    memcpy(h + 12, IpAddress, 4);
    memcpy(h + 16, DestinationIp, 4);
    StoreBigEndian16(h + 10, FoldChecksum(Checksum(h, NET_IP_HEADER, 0)));
    hardware::ethernet::SendFrame(14 + NET_IP_HEADER + Length);
}

// --- ARP: a request for our address gets a reply

static void ArpReceived(const UCHAR *a, ULONG Length) {
    bool request = Length >= 28 && LoadBigEndian16(a) == 1 && LoadBigEndian16(a + 2) == ETHERTYPE_IP && a[4] == 6 && a[5] == 4 && LoadBigEndian16(a + 6) == 1;
    if (!request || AddressState < NetLeased || memcmp(a + 24, IpAddress, 4) != 0)
        return;
    PUCHAR reply = hardware::ethernet::NextFrameBuffer();
    if (!reply)
        return;
    memcpy(reply, a + 8, 6); // to the asker
    memcpy(reply + 6, hardware::ethernet::MacAddress, 6);
    StoreBigEndian16(reply + 12, ETHERTYPE_ARP);
    PUCHAR r = reply + 14;
    memcpy(r, a, 6);     // hardware and protocol types and sizes, as asked
    StoreBigEndian16(r + 6, 2); // reply
    memcpy(r + 8, hardware::ethernet::MacAddress, 6);
    memcpy(r + 14, IpAddress, 4);
    memcpy(r + 18, a + 8, 10); // the asker's MAC and IP address
    hardware::ethernet::SendFrame(14 + 28);
}

// --- ICMP: an echo request gets its echo reply

static void IcmpReceived(const UCHAR *SourceMac, const UCHAR *SourceIp, const UCHAR *m, ULONG Length) {
    if (Length < 8 || m[0] != 8 || Length > NET_IP_PAYLOAD || FoldChecksum(Checksum(m, Length, 0)))
        return;
    PUCHAR r = IpPayload();
    if (!r)
        return;
    memcpy(r, m, Length);
    r[0] = 0; // echo reply
    StoreBigEndian16(r + 2, 0);
    StoreBigEndian16(r + 2, FoldChecksum(Checksum(r, Length, 0)));
    SendIp(SourceMac, SourceIp, NET_ICMP, Length);
}

// --- DHCP

// A DISCOVER or a REQUEST (for the offered address), broadcast from 0.0.0.0.
static void SendDhcp(UCHAR Type) {
    DhcpSent = Type;
    DhcpSentMs = hardware::ethernet::Milliseconds();
    PUCHAR u = IpPayload();
    if (!u)
        return; // sent again after DHCP_RETRY_MS
    PUCHAR b = u + 8;
    ULONG length = 236 + 4;
    memset(b, 0, 300);
    b[0] = 1; // a request
    b[1] = 1; // Ethernet
    b[2] = 6;
    StoreBigEndian32(b + 4, DhcpXid);
    StoreBigEndian16(b + 10, 0x8000); // answer by broadcast: we have no address yet
    memcpy(b + 28, hardware::ethernet::MacAddress, 6);
    StoreBigEndian32(b + 236, 0x63825363); // the DHCP magic cookie
    PUCHAR o = b + length;
    auto option = [&](UCHAR code, const UCHAR *value, UCHAR size) {
        o[0] = code;
        o[1] = size;
        memcpy(o + 2, value, size);
        o += 2 + size;
    };
    option(53, &Type, 1);
    static const UCHAR wanted[] = {1, 3, 6}; // subnet mask, router, DNS servers
    option(55, wanted, sizeof(wanted));
    static const UCHAR name[] = {'O', 'X', 'O', 'S'};
    option(12, name, sizeof(name));
    if (Type == DHCP_REQUEST) {
        option(50, OfferedIp, 4);
        option(54, DhcpServer, 4);
    }
    *o++ = 255; // end
    length = (ULONG)(o - b);
    if (length < 300) // BOOTP's minimum
        length = 300;
    StoreBigEndian16(u, DHCP_CLIENT_PORT);
    StoreBigEndian16(u + 2, DHCP_SERVER_PORT);
    StoreBigEndian16(u + 4, (USHORT)(8 + length));
    StoreBigEndian16(u + 6, 0); // no UDP checksum
    UCHAR none[4] = {};
    memcpy(IpAddress, none, 4);
    SendIp(BroadcastMac, BroadcastIp, NET_UDP, 8 + length);
}

// The value of option Code in the options at o (Length bytes), or null.
static const UCHAR *DhcpOption(const UCHAR *o, ULONG Length, UCHAR Code, UCHAR Size) {
    for (ULONG i = 0; i < Length && o[i] != 255;) {
        if (o[i] == 0) { // padding
            i++;
            continue;
        }
        if (i + 1 >= Length || i + 2 + o[i + 1] > Length)
            return nullptr;
        if (o[i] == Code && o[i + 1] >= Size)
            return o + i + 2;
        i += 2 + o[i + 1];
    }
    return nullptr;
}

static void DhcpReceived(const UCHAR *b, ULONG Length) {
    if (AddressState != NetAsking || Length < 240 || b[0] != 2 || LoadBigEndian32(b + 4) != DhcpXid || LoadBigEndian32(b + 236) != 0x63825363)
        return;
    const UCHAR *type = DhcpOption(b + 240, Length - 240, 53, 1);
    const UCHAR *server = DhcpOption(b + 240, Length - 240, 54, 4);
    if (!type)
        return;
    if (*type == DHCP_OFFER && DhcpSent == DHCP_DISCOVER && server) {
        memcpy(OfferedIp, b + 16, 4);
        memcpy(DhcpServer, server, 4);
        SendDhcp(DHCP_REQUEST);
    } else if (*type == DHCP_ACK && DhcpSent == DHCP_REQUEST) {
        memcpy(IpAddress, b + 16, 4);
        AddressState = NetLeased;
    } else if (*type == DHCP_NAK) {
        SendDhcp(DHCP_DISCOVER);
    }
}

// DHCP's timers: a lost DISCOVER or REQUEST is sent again; no answer in 30 s: the fixed address.
static void PollDhcp() {
    ULONG now = hardware::ethernet::Milliseconds();
    if (now - DhcpStartMs >= DHCP_GIVE_UP_MS) {
        memcpy(IpAddress, FixedIp, 4);
        AddressState = NetFixed;
    } else if (now - DhcpSentMs >= DHCP_RETRY_MS) {
        SendDhcp(DhcpSent == DHCP_REQUEST ? DHCP_REQUEST : DHCP_DISCOVER);
    }
}

// --- Frames in

void FrameReceived(const UCHAR *Frame, ULONG Length) {
    if (Length < 14)
        return;
    if (LoadBigEndian16(Frame + 12) == ETHERTYPE_ARP)
        return ArpReceived(Frame + 14, Length - 14);
    if (LoadBigEndian16(Frame + 12) != ETHERTYPE_IP || Length < 14 + NET_IP_HEADER)
        return;
    const UCHAR *h = Frame + 14;
    ULONG headerLength = (h[0] & 0x0F) * 4, total = LoadBigEndian16(h + 2);
    // IPv4, a whole packet (no fragments), its header intact.
    if ((h[0] >> 4) != 4 || headerLength < NET_IP_HEADER || total < headerLength || 14 + total > Length || (LoadBigEndian16(h + 6) & 0x3FFF) ||
        FoldChecksum(Checksum(h, headerLength, 0)))
        return;
    bool forUs = AddressState >= NetLeased && memcmp(h + 16, IpAddress, 4) == 0;
    bool broadcast = memcmp(h + 16, BroadcastIp, 4) == 0;
    const UCHAR *p = h + headerLength;
    ULONG length = total - headerLength;
    // DHCP comes before we have an address.
    if (h[9] == NET_UDP && length >= 8 && LoadBigEndian16(p + 2) == DHCP_CLIENT_PORT)
        DhcpReceived(p + 8, length - 8);
    else if (h[9] == NET_ICMP && forUs)
        IcmpReceived(Frame + 6, h + 12, p, length);
    else if (h[9] == NET_TCP && forUs && !broadcast)
        TcpReceived(Frame + 6, h + 12, p, length);
}

// --- Entry points

// Starts the card; with a link, DHCP starts asking. Without one, PollNetwork tries again every LINK_CHECK_MS.
static void TryLink() {
    LinkCheckMs = hardware::ethernet::Milliseconds();
    if (!hardware::ethernet::StartCard(0))
        return;
    AddressState = NetAsking;
    DhcpStartMs = hardware::ethernet::Milliseconds();
    DhcpXid = DhcpStartMs ^ (ULONG)hardware::ethernet::MacAddress[5] << 24 ^ 0x4F584F53;
    SendDhcp(DHCP_DISCOVER);
}

bool StartNetwork() {
    if (!StartTcp())
        return false;
    AddressState = NetNoLink;
    TryLink();
    return true;
}

void StopNetwork() {
    hardware::ethernet::StopCard();
    StopTcp();
    AddressState = NetNoLink;
}

bool PollNetwork() {
    if (AddressState == NetNoLink) {
        if (hardware::ethernet::Milliseconds() - LinkCheckMs >= LINK_CHECK_MS)
            TryLink();
        if (AddressState == NetNoLink)
            return false;
    }
    ULONG frames = hardware::ethernet::ReceiveFrames();
    if (AddressState == NetAsking)
        PollDhcp();
    PollTcp();
    return frames != 0;
}

NET_STATE NetworkState() {
    return AddressState;
}

void IpAddressText(char *Text, ULONG Size) {
    if (AddressState < NetLeased) {
        if (Size)
            Text[0] = 0;
        return;
    }
    snprintf(Text, Size, "%u.%u.%u.%u", IpAddress[0], IpAddress[1], IpAddress[2], IpAddress[3]);
}

} // namespace xk::network
