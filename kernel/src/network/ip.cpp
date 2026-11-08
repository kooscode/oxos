// Ethernet, ARP, IPv4, ICMP echo and DHCP for the kernel's network stack, and its entry points
// (spec/oxos_internal/31_upload_server.md §2). Boot-only.
//
// Only what a server on the local network needs: ARP answers requests for our address (we never ask: every reply
// goes to the MAC address the request came from, a router's for a browser on another subnet), IPv4 without options
// or fragments, ping, and a DHCP client without renewals (the server runs for minutes).
#include <xk/network_stack.h>
#include <xk/runtime.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

constexpr USHORT ETHERTYPE_IP = 0x0800, ETHERTYPE_ARP = 0x0806;
constexpr USHORT DHCP_SERVER_PORT = 67, DHCP_CLIENT_PORT = 68;
constexpr UCHAR DHCP_DISCOVER = 1, DHCP_OFFER = 2, DHCP_REQUEST = 3, DHCP_ACK = 5, DHCP_NAK = 6;
constexpr ULONG DHCP_RETRY_MS = 4000, DHCP_GIVE_UP_MS = 30000;
constexpr ULONG LINK_CHECK_MS = 2000;

static const UCHAR NetpBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const UCHAR NetpBroadcastIp[4] = {255, 255, 255, 255};
static const UCHAR NetpFixedIp[4] = {XK_NETBOOT_IP}; // make NETBOOT_IP=, the netboot address

UCHAR NetIp[4];
static NET_STATE NetpState;
static PUCHAR NetpFrame;  // the transmit buffer the packet being built is in (NetpIpPayload, NetpSendIp)
static USHORT NetpIpId;   // the IP identification of the next packet

// DHCP: the transaction, the offered address and its server, what was last sent and when.
static ULONG NetpDhcpXid;
static UCHAR NetpOffered[4], NetpDhcpServer[4];
static UCHAR NetpDhcpSent;              // DHCP_DISCOVER or DHCP_REQUEST
static ULONG NetpDhcpSentMs, NetpDhcpStartMs, NetpLinkCheckMs;

// --- Checksums and sending

ULONG NetpSum(const UCHAR *Data, ULONG Length, ULONG Sum) {
    for (ULONG i = 0; i + 1 < Length; i += 2)
        Sum += (ULONG)Data[i] << 8 | Data[i + 1];
    if (Length & 1)
        Sum += (ULONG)Data[Length - 1] << 8;
    return Sum;
}

USHORT NetpFold(ULONG Sum) {
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (USHORT)~Sum;
}

PUCHAR NetpIpPayload() {
    NetpFrame = NetpFrameBuffer();
    return NetpFrame ? NetpFrame + 14 + NET_IP_HEADER : nullptr;
}

void NetpSendIp(const UCHAR *DestinationMac, const UCHAR *DestinationIp, UCHAR Protocol, ULONG Length) {
    PUCHAR e = NetpFrame, h = NetpFrame + 14;
    RtlCopyMemory(e, DestinationMac, 6);
    RtlCopyMemory(e + 6, NetMac, 6);
    NetPut16(e + 12, ETHERTYPE_IP);
    RtlZeroMemory(h, NET_IP_HEADER);
    h[0] = 0x45; // IPv4, 20-byte header
    NetPut16(h + 2, (USHORT)(NET_IP_HEADER + Length));
    NetPut16(h + 4, NetpIpId++);
    NetPut16(h + 6, 0x4000); // don't fragment
    h[8] = 64;               // time to live
    h[9] = Protocol;
    RtlCopyMemory(h + 12, NetIp, 4);
    RtlCopyMemory(h + 16, DestinationIp, 4);
    NetPut16(h + 10, NetpFold(NetpSum(h, NET_IP_HEADER, 0)));
    NetpFrameSend(14 + NET_IP_HEADER + Length);
}

// --- ARP: a request for our address gets a reply

static void NetpArpReceived(const UCHAR *a, ULONG Length) {
    bool request = Length >= 28 && NetGet16(a) == 1 && NetGet16(a + 2) == ETHERTYPE_IP && a[4] == 6 && a[5] == 4 &&
                   NetGet16(a + 6) == 1;
    if (!request || NetpState < NetLeased || RtlCompareMemory(a + 24, NetIp, 4) != 4)
        return;
    PUCHAR reply = NetpFrameBuffer();
    if (!reply)
        return;
    RtlCopyMemory(reply, a + 8, 6); // to the asker
    RtlCopyMemory(reply + 6, NetMac, 6);
    NetPut16(reply + 12, ETHERTYPE_ARP);
    PUCHAR r = reply + 14;
    RtlCopyMemory(r, a, 6); // hardware and protocol types and sizes, as asked
    NetPut16(r + 6, 2);     // reply
    RtlCopyMemory(r + 8, NetMac, 6);
    RtlCopyMemory(r + 14, NetIp, 4);
    RtlCopyMemory(r + 18, a + 8, 10); // the asker's MAC and IP address
    NetpFrameSend(14 + 28);
}

// --- ICMP: an echo request gets its echo reply

static void NetpIcmpReceived(const UCHAR *SourceMac, const UCHAR *SourceIp, const UCHAR *m, ULONG Length) {
    if (Length < 8 || m[0] != 8 || Length > NET_IP_PAYLOAD || NetpFold(NetpSum(m, Length, 0)))
        return;
    PUCHAR r = NetpIpPayload();
    if (!r)
        return;
    RtlCopyMemory(r, m, Length);
    r[0] = 0; // echo reply
    NetPut16(r + 2, 0);
    NetPut16(r + 2, NetpFold(NetpSum(r, Length, 0)));
    NetpSendIp(SourceMac, SourceIp, NET_ICMP, Length);
}

// --- DHCP

// A DISCOVER or a REQUEST (for the offered address), broadcast from 0.0.0.0.
static void NetpDhcpSend(UCHAR Type) {
    NetpDhcpSent = Type;
    NetpDhcpSentMs = NetpMilliseconds();
    PUCHAR u = NetpIpPayload();
    if (!u)
        return; // sent again after DHCP_RETRY_MS
    PUCHAR b = u + 8;
    ULONG length = 236 + 4;
    RtlZeroMemory(b, 300);
    b[0] = 1; // a request
    b[1] = 1; // Ethernet
    b[2] = 6;
    NetPut32(b + 4, NetpDhcpXid);
    NetPut16(b + 10, 0x8000); // answer by broadcast: we have no address yet
    RtlCopyMemory(b + 28, NetMac, 6);
    NetPut32(b + 236, 0x63825363); // the DHCP magic cookie
    PUCHAR o = b + length;
    auto option = [&](UCHAR code, const UCHAR *value, UCHAR size) {
        o[0] = code;
        o[1] = size;
        RtlCopyMemory(o + 2, value, size);
        o += 2 + size;
    };
    option(53, &Type, 1);
    static const UCHAR wanted[] = {1, 3, 6}; // subnet mask, router, DNS servers
    option(55, wanted, sizeof(wanted));
    static const UCHAR name[] = {'O', 'X', 'O', 'S'};
    option(12, name, sizeof(name));
    if (Type == DHCP_REQUEST) {
        option(50, NetpOffered, 4);
        option(54, NetpDhcpServer, 4);
    }
    *o++ = 255; // end
    length = (ULONG)(o - b);
    if (length < 300) // BOOTP's minimum
        length = 300;
    NetPut16(u, DHCP_CLIENT_PORT);
    NetPut16(u + 2, DHCP_SERVER_PORT);
    NetPut16(u + 4, (USHORT)(8 + length));
    NetPut16(u + 6, 0); // no UDP checksum
    UCHAR none[4] = {};
    RtlCopyMemory(NetIp, none, 4);
    NetpSendIp(NetpBroadcastMac, NetpBroadcastIp, NET_UDP, 8 + length);
}

// The value of option Code in the options at o (Length bytes), or null.
static const UCHAR *NetpDhcpOption(const UCHAR *o, ULONG Length, UCHAR Code, UCHAR Size) {
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

static void NetpDhcpReceived(const UCHAR *b, ULONG Length) {
    if (NetpState != NetAsking || Length < 240 || b[0] != 2 || NetGet32(b + 4) != NetpDhcpXid ||
        NetGet32(b + 236) != 0x63825363)
        return;
    const UCHAR *type = NetpDhcpOption(b + 240, Length - 240, 53, 1);
    const UCHAR *server = NetpDhcpOption(b + 240, Length - 240, 54, 4);
    if (!type)
        return;
    if (*type == DHCP_OFFER && NetpDhcpSent == DHCP_DISCOVER && server) {
        RtlCopyMemory(NetpOffered, b + 16, 4);
        RtlCopyMemory(NetpDhcpServer, server, 4);
        NetpDhcpSend(DHCP_REQUEST);
    } else if (*type == DHCP_ACK && NetpDhcpSent == DHCP_REQUEST) {
        RtlCopyMemory(NetIp, b + 16, 4);
        NetpState = NetLeased;
    } else if (*type == DHCP_NAK) {
        NetpDhcpSend(DHCP_DISCOVER);
    }
}

// DHCP's timers: a lost DISCOVER or REQUEST is sent again; no answer in 30 s: the fixed address.
static void NetpDhcpPoll() {
    ULONG now = NetpMilliseconds();
    if (now - NetpDhcpStartMs >= DHCP_GIVE_UP_MS) {
        RtlCopyMemory(NetIp, NetpFixedIp, 4);
        NetpState = NetFixed;
    } else if (now - NetpDhcpSentMs >= DHCP_RETRY_MS) {
        NetpDhcpSend(NetpDhcpSent == DHCP_REQUEST ? DHCP_REQUEST : DHCP_DISCOVER);
    }
}

// --- Frames in

void NetpFrameReceived(const UCHAR *Frame, ULONG Length) {
    if (Length < 14)
        return;
    if (NetGet16(Frame + 12) == ETHERTYPE_ARP)
        return NetpArpReceived(Frame + 14, Length - 14);
    if (NetGet16(Frame + 12) != ETHERTYPE_IP || Length < 14 + NET_IP_HEADER)
        return;
    const UCHAR *h = Frame + 14;
    ULONG headerLength = (h[0] & 0x0F) * 4, total = NetGet16(h + 2);
    // IPv4, a whole packet (no fragments), its header intact.
    if ((h[0] >> 4) != 4 || headerLength < NET_IP_HEADER || total < headerLength || 14 + total > Length ||
        (NetGet16(h + 6) & 0x3FFF) || NetpFold(NetpSum(h, headerLength, 0)))
        return;
    bool forUs = NetpState >= NetLeased && RtlCompareMemory(h + 16, NetIp, 4) == 4;
    bool broadcast = RtlCompareMemory(h + 16, NetpBroadcastIp, 4) == 4;
    const UCHAR *p = h + headerLength;
    ULONG length = total - headerLength;
    if (h[9] == NET_UDP && length >= 8 && NetGet16(p + 2) == DHCP_CLIENT_PORT) // DHCP comes before we have an address
        NetpDhcpReceived(p + 8, length - 8);
    else if (h[9] == NET_ICMP && forUs)
        NetpIcmpReceived(Frame + 6, h + 12, p, length);
    else if (h[9] == NET_TCP && forUs && !broadcast)
        NetpTcpReceived(Frame + 6, h + 12, p, length);
}

// --- Entry points

// Starts the card; with a link, DHCP starts asking. Without one, NetPoll tries again every LINK_CHECK_MS.
static void NetpTryLink() {
    NetpLinkCheckMs = NetpMilliseconds();
    if (!NetpCardStart(0))
        return;
    NetpState = NetAsking;
    NetpDhcpStartMs = NetpMilliseconds();
    NetpDhcpXid = NetpDhcpStartMs ^ (ULONG)NetMac[5] << 24 ^ 0x4F584F53;
    NetpDhcpSend(DHCP_DISCOVER);
}

bool NetStart() {
    if (!NetpTcpStart())
        return false;
    NetpState = NetNoLink;
    NetpTryLink();
    return true;
}

void NetStop() {
    NetpCardStop();
    NetpTcpStop();
    NetpState = NetNoLink;
}

bool NetPoll() {
    if (NetpState == NetNoLink) {
        if (NetpMilliseconds() - NetpLinkCheckMs >= LINK_CHECK_MS)
            NetpTryLink();
        if (NetpState == NetNoLink)
            return false;
    }
    ULONG frames = NetpFramesIn();
    if (NetpState == NetAsking)
        NetpDhcpPoll();
    NetpTcpPoll();
    return frames != 0;
}

NET_STATE NetState() {
    return NetpState;
}

void NetAddressText(char *Text, ULONG Size) {
    if (NetpState < NetLeased) {
        if (Size)
            Text[0] = 0;
        return;
    }
    RtlSnprintf(Text, Size, "%u.%u.%u.%u", NetIp[0], NetIp[1], NetIp[2], NetIp[3]);
}
