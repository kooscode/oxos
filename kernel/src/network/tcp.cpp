// TCP for the kernel's network stack: a server on port 80 with NET_TCP_CONNECTIONS connections
// (spec/oxos_internal/31_upload_server.md §2). Boot-only.
//
// Kept simple, as one HTTP request per connection on the local network allows:
// - receiving: in-order data only. A segment past a gap is dropped and the expected one asked for again (a duplicate
//   ACK). One ACK per connection after each batch of received frames (PollTcp), not one per segment: a full window
//   is some 45 segments, and an ACK each would queue behind the card's transmit ring. The window is the receive
//   buffer's free space, at most 64 KB less one byte (the field's limit), so a sender never has more in flight than
//   the card's receive ring holds (network/nic.cpp).
// - sending: what is queued stays in the send buffer until acknowledged. Nothing acknowledged for 1 s: everything
//   from the oldest unacknowledged byte is sent again ("go back N"), 8 times at most, then the connection is reset.
// - ending: our FIN after the queued data; the connection ends once both sides' FINs are acknowledged, or 2 s after
//   ours with no FIN from the peer. Nothing heard for 30 s: reset.
#include <xk/network_stack.h>
#include <xk/memory.h>
#include <xk/runtime.h>

namespace xk::network {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

constexpr USHORT HTTP_PORT = 80;
constexpr ULONG RX_SIZE = 0xFFFF, TX_SIZE = 0x10000; // per connection; RX_SIZE: the largest window without scaling
constexpr USHORT MSS = 1460;
constexpr ULONG RETRY_MS = 1000, RETRIES = 8, CLOSE_MS = 2000, IDLE_MS = 30000;
constexpr UCHAR FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10;

enum TCP_STATE : UCHAR { TcpFree, TcpSynReceived, TcpEstablished };

struct NETP_CONNECTION {
    TCP_STATE State;
    bool Accepted;    // handed to the caller (AcceptTcp)
    bool FinQueued;   // CloseTcp: a FIN follows the queued data
    bool FinSent;     // our FIN is in SndNxt
    bool PeerFin;     // the peer's FIN has arrived
    bool AckDue;      // data or a FIN arrived since our last segment: PollTcp acknowledges it
    UCHAR Generation; // part of the id: a new connection in this slot gets a new one
    UCHAR PeerMac[6], PeerIp[4];
    USHORT PeerPort, PeerMss;
    ULONG SndUna, SndNxt; // the oldest unacknowledged and the next sequence number we send
    ULONG SndWindow;      // the peer's window
    ULONG RcvNxt;         // the next sequence number we expect
    ULONG Advertised;     // the window in our last segment
    PUCHAR Rx;            // RX_SIZE bytes: received, not yet read (a ring: RxStart, RxCount)
    ULONG RxStart, RxCount;
    PUCHAR Tx;            // TX_SIZE bytes: from SndUna on, sent or not (TxCount bytes)
    ULONG TxCount;
    ULONG SentMs, Retries; // the oldest unacknowledged data's last sending, and how often it went again
    ULONG HeardMs, FinAckedMs;
};

static NETP_CONNECTION *Connections; // NET_TCP_CONNECTIONS of them, and their buffers after them

// --- Connections and ids

static LONG ConnectionId(ULONG Slot) {
    return (LONG)(Slot | (ULONG)Connections[Slot].Generation << 8);
}

// The open connection Id names, or null.
static NETP_CONNECTION *ConnectionOf(LONG Id) {
    ULONG slot = (ULONG)Id & 0xFF;
    if (Id < 0 || slot >= NET_TCP_CONNECTIONS || !Connections)
        return nullptr;
    NETP_CONNECTION *c = &Connections[slot];
    return c->State != TcpFree && c->Generation == (UCHAR)(Id >> 8) ? c : nullptr;
}

static void FreeConnection(NETP_CONNECTION *c) {
    c->State = TcpFree;
    c->Generation++;
}

static ULONG ReceiveWindow(const NETP_CONNECTION *c) {
    return RX_SIZE - c->RxCount;
}

// Received bytes into the receive ring, and out of it, in at most two copies each (where the ring wraps).
static void RingPut(NETP_CONNECTION *c, const UCHAR *Data, ULONG Length) {
    ULONG tail = (c->RxStart + c->RxCount) % RX_SIZE, first = Length < RX_SIZE - tail ? Length : RX_SIZE - tail;
    RtlCopyMemory(c->Rx + tail, Data, first);
    RtlCopyMemory(c->Rx, Data + first, Length - first);
    c->RxCount += Length;
}

static void RingTake(NETP_CONNECTION *c, PUCHAR Out, ULONG Length) {
    ULONG first = Length < RX_SIZE - c->RxStart ? Length : RX_SIZE - c->RxStart;
    RtlCopyMemory(Out, c->Rx + c->RxStart, first);
    RtlCopyMemory(Out + first, c->Rx, Length - first);
    c->RxStart = (c->RxStart + Length) % RX_SIZE;
    c->RxCount -= Length;
}

bool StartTcp() {
    ULONG size = NET_TCP_CONNECTIONS * (sizeof(NETP_CONNECTION) + RX_SIZE + TX_SIZE);
    Connections = (NETP_CONNECTION *)MmAllocateSystemMemory(size, PAGE_READWRITE);
    if (!Connections)
        return false;
    RtlZeroMemory(Connections, NET_TCP_CONNECTIONS * sizeof(NETP_CONNECTION));
    PUCHAR buffers = (PUCHAR)(Connections + NET_TCP_CONNECTIONS);
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
        Connections[i].Rx = buffers + i * (RX_SIZE + TX_SIZE);
        Connections[i].Tx = Connections[i].Rx + RX_SIZE;
    }
    return true;
}

void StopTcp() {
    if (!Connections)
        return;
    MmFreeSystemMemory(Connections, NET_TCP_CONNECTIONS * (sizeof(NETP_CONNECTION) + RX_SIZE + TX_SIZE));
    Connections = nullptr;
}

// --- Segments out

// One segment with Flags, sequence number Seq and Length bytes of Data, to the connection's peer (or, for a reset
// of an unknown connection, to the given peer).
static void SendSegment(const UCHAR *Mac, const UCHAR *Ip, USHORT PeerPort, ULONG Seq, ULONG Ack, UCHAR Flags,
                        ULONG Window, const UCHAR *Data, ULONG Length) {
    PUCHAR t = IpPayload();
    if (!t)
        return; // the card is busy: the retransmission timer sends it again
    ULONG header = (Flags & SYN) ? 24 : 20; // a SYN carries our MSS
    StoreBigEndian16(t, HTTP_PORT);
    StoreBigEndian16(t + 2, PeerPort);
    StoreBigEndian32(t + 4, Seq);
    StoreBigEndian32(t + 8, Ack);
    t[12] = (UCHAR)(header / 4 << 4);
    t[13] = Flags;
    StoreBigEndian16(t + 14, (USHORT)(Window > 0xFFFF ? 0xFFFF : Window));
    StoreBigEndian16(t + 16, 0);
    StoreBigEndian16(t + 18, 0);
    if (Flags & SYN) {
        t[20] = 2; // MSS option
        t[21] = 4;
        StoreBigEndian16(t + 22, MSS);
    }
    if (Length)
        RtlCopyMemory(t + header, Data, Length);
    // The checksum covers a pseudo-header: both addresses, the protocol and the segment's length.
    UCHAR pseudo[12];
    RtlCopyMemory(pseudo, IpAddress, 4);
    RtlCopyMemory(pseudo + 4, Ip, 4);
    pseudo[8] = 0;
    pseudo[9] = NET_TCP;
    StoreBigEndian16(pseudo + 10, (USHORT)(header + Length));
    StoreBigEndian16(t + 16, FoldChecksum(Checksum(t, header + Length, Checksum(pseudo, 12, 0))));
    SendIp(Mac, Ip, NET_TCP, header + Length);
}

static void SendOnConnection(NETP_CONNECTION *c, ULONG Seq, UCHAR Flags, const UCHAR *Data, ULONG Length) {
    c->Advertised = ReceiveWindow(c);
    c->AckDue = false; // every segment carries our ACK
    SendSegment(c->PeerMac, c->PeerIp, c->PeerPort, Seq, c->RcvNxt, Flags, c->Advertised, Data, Length);
}

static void SendAck(NETP_CONNECTION *c) {
    SendOnConnection(c, c->SndNxt, ACK, nullptr, 0);
}

static void ResetConnection(NETP_CONNECTION *c) {
    SendOnConnection(c, c->SndNxt, RST | ACK, nullptr, 0);
    FreeConnection(c);
}

// Sends what the peer's window allows of the queued data, then our FIN once everything has gone out.
static void TransmitConnection(NETP_CONNECTION *c) {
    if (c->State != TcpEstablished)
        return;
    ULONG window = c->SndWindow ? c->SndWindow : 1; // a closed window gets a one-byte probe
    ULONG mss = c->PeerMss < MSS ? c->PeerMss : MSS;
    ULONG inFlight = c->SndNxt - c->SndUna - (c->FinSent ? 1 : 0);
    while (inFlight < c->TxCount && inFlight < window) {
        ULONG n = c->TxCount - inFlight;
        if (n > mss)
            n = mss;
        if (n > window - inFlight)
            n = window - inFlight;
        if (inFlight == 0)
            c->SentMs = Milliseconds();
        SendOnConnection(c, c->SndNxt, ACK | PSH, c->Tx + inFlight, n);
        c->SndNxt += n;
        inFlight += n;
    }
    if (c->FinQueued && !c->FinSent && inFlight == c->TxCount) {
        if (c->SndNxt == c->SndUna)
            c->SentMs = Milliseconds();
        SendOnConnection(c, c->SndNxt, FIN | ACK, nullptr, 0);
        c->SndNxt++;
        c->FinSent = true;
    }
}

// --- Segments in

// A segment for a connection we do not have: a SYN opens one, anything but a reset is answered with a reset.
static void NewConnection(const UCHAR *Mac, const UCHAR *Ip, const UCHAR *t, ULONG Length, ULONG HeaderLength) {
    UCHAR flags = t[13];
    USHORT port = LoadBigEndian16(t);
    ULONG seq = LoadBigEndian32(t + 4);
    if (flags & RST)
        return;
    NETP_CONNECTION *c = nullptr;
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS && !c; i++) {
        if (Connections[i].State == TcpFree)
            c = &Connections[i];
    }
    if (!(flags & SYN) || (flags & ACK) || !c || LoadBigEndian16(t + 2) != HTTP_PORT) {
        ULONG ack = (flags & ACK) ? LoadBigEndian32(t + 8) : 0;
        SendSegment(Mac, Ip, port, ack, seq + Length - HeaderLength + ((flags & SYN) ? 1 : 0), RST | ACK, 0, nullptr,
                    0);
        return;
    }
    UCHAR generation = c->Generation;
    PUCHAR rx = c->Rx, tx = c->Tx;
    RtlZeroMemory(c, sizeof(*c));
    c->Generation = generation;
    c->Rx = rx;
    c->Tx = tx;
    c->State = TcpSynReceived;
    RtlCopyMemory(c->PeerMac, Mac, 6);
    RtlCopyMemory(c->PeerIp, Ip, 4);
    c->PeerPort = port;
    c->PeerMss = 536; // the default without the option
    for (ULONG i = 20; i + 1 < HeaderLength;) { // options: MSS
        UCHAR kind = t[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (kind == 2 && t[i + 1] == 4 && i + 4 <= HeaderLength)
            c->PeerMss = LoadBigEndian16(t + i + 2);
        i += t[i + 1] ? t[i + 1] : 1;
    }
    c->RcvNxt = seq + 1;
    c->SndUna = c->SndNxt = Milliseconds() * 2654435761u; // the initial sequence number
    c->SndWindow = LoadBigEndian16(t + 14);
    c->HeardMs = c->SentMs = Milliseconds();
    SendOnConnection(c, c->SndNxt, SYN | ACK, nullptr, 0);
    c->SndNxt++;
}

// The acknowledgement in a segment: acknowledged data leaves the send buffer.
static void Acknowledged(NETP_CONNECTION *c, ULONG Ack, ULONG Window) {
    ULONG acked = Ack - c->SndUna;
    if (acked == 0 || acked > c->SndNxt - c->SndUna) // nothing new, or more than we sent
        return;
    if (c->State == TcpSynReceived) { // our SYN, the first acknowledged byte
        c->State = TcpEstablished;
        acked--;
        c->SndUna++;
    }
    bool finAcked = c->FinSent && Ack == c->SndNxt;
    ULONG data = acked - (finAcked ? 1 : 0);
    if (data > c->TxCount)
        data = c->TxCount;
    RtlMoveMemory(c->Tx, c->Tx + data, c->TxCount - data);
    c->TxCount -= data;
    c->SndUna = Ack;
    c->SndWindow = Window;
    c->Retries = 0;
    c->SentMs = Milliseconds();
    if (finAcked)
        c->FinAckedMs = Milliseconds();
}

void TcpReceived(const UCHAR *SourceMac, const UCHAR *SourceIp, const UCHAR *t, ULONG Length) {
    if (!Connections || Length < 20)
        return;
    ULONG headerLength = (t[12] >> 4) * 4;
    UCHAR pseudo[12];
    RtlCopyMemory(pseudo, SourceIp, 4);
    RtlCopyMemory(pseudo + 4, IpAddress, 4);
    pseudo[8] = 0;
    pseudo[9] = NET_TCP;
    StoreBigEndian16(pseudo + 10, (USHORT)Length);
    if (headerLength < 20 || headerLength > Length || FoldChecksum(Checksum(t, Length, Checksum(pseudo, 12, 0))))
        return;
    USHORT port = LoadBigEndian16(t);
    NETP_CONNECTION *c = nullptr;
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS && !c; i++) {
        NETP_CONNECTION *k = &Connections[i];
        if (k->State != TcpFree && k->PeerPort == port && RtlCompareMemory(k->PeerIp, SourceIp, 4) == 4)
            c = k;
    }
    if (!c)
        return NewConnection(SourceMac, SourceIp, t, Length, headerLength);
    UCHAR flags = t[13];
    ULONG seq = LoadBigEndian32(t + 4), dataLength = Length - headerLength;
    c->HeardMs = Milliseconds();
    if (flags & RST) {
        FreeConnection(c);
        return;
    }
    if (flags & SYN) { // our SYN-ACK was lost: send it again
        if (c->State == TcpSynReceived)
            SendOnConnection(c, c->SndUna, SYN | ACK, nullptr, 0);
        return;
    }
    if (flags & ACK)
        Acknowledged(c, LoadBigEndian32(t + 8), LoadBigEndian16(t + 14));
    if (c->State != TcpEstablished)
        return;
    bool acknowledge = dataLength || (flags & FIN);
    if (seq == c->RcvNxt && dataLength) { // in order: as much as the buffer holds
        ULONG n = dataLength < ReceiveWindow(c) ? dataLength : ReceiveWindow(c);
        RingPut(c, t + headerLength, n);
        c->RcvNxt += n;
        dataLength = n;
    }
    if ((flags & FIN) && seq + dataLength == c->RcvNxt && !c->PeerFin) {
        c->RcvNxt++;
        c->PeerFin = true;
    }
    if (acknowledge)
        c->AckDue = true; // a duplicate ACK when the segment was out of order
    TransmitConnection(c);
}

// --- Timers

void PollTcp() {
    if (!Connections)
        return;
    ULONG now = Milliseconds();
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
        NETP_CONNECTION *c = &Connections[i];
        if (c->State == TcpFree)
            continue;
        if (now - c->HeardMs >= IDLE_MS) {
            ResetConnection(c);
            continue;
        }
        bool finAcked = c->FinSent && c->SndUna == c->SndNxt;
        if (finAcked && (c->PeerFin || now - c->FinAckedMs >= CLOSE_MS)) {
            FreeConnection(c);
            continue;
        }
        if (c->SndUna != c->SndNxt && now - c->SentMs >= RETRY_MS) { // nothing acknowledged for a while
            if (++c->Retries > RETRIES) {
                ResetConnection(c);
                continue;
            }
            c->SentMs = now;
            if (c->State == TcpSynReceived) {
                SendOnConnection(c, c->SndUna, SYN | ACK, nullptr, 0);
                continue;
            }
            c->SndNxt = c->SndUna; // go back: everything from the oldest unacknowledged byte again
            c->FinSent = false;
        }
        TransmitConnection(c);
        if (c->AckDue) // nothing went out to carry it
            SendAck(c);
    }
}

// --- The caller's side

LONG AcceptTcp() {
    for (ULONG i = 0; Connections && i < NET_TCP_CONNECTIONS; i++) {
        NETP_CONNECTION *c = &Connections[i];
        if (c->State == TcpEstablished && !c->Accepted) {
            c->Accepted = true;
            return ConnectionId(i);
        }
    }
    return -1;
}

bool IsTcpOpen(LONG Id) {
    return ConnectionOf(Id) != nullptr;
}

ULONG ReceiveTcp(LONG Id, PVOID Buffer, ULONG Size) {
    NETP_CONNECTION *c = ConnectionOf(Id);
    if (!c)
        return 0;
    ULONG n = Size < c->RxCount ? Size : c->RxCount;
    RingTake(c, (PUCHAR)Buffer, n);
    // The window has grown by two segments or more since we last told the peer: tell it now (a window update), or it
    // keeps sending into the smaller window it knows, or waits for its own timer when that one had closed.
    if (n && ReceiveWindow(c) >= c->Advertised + 2 * MSS)
        SendAck(c);
    return n;
}

bool TcpPeerClosed(LONG Id) {
    NETP_CONNECTION *c = ConnectionOf(Id);
    return !c || (c->PeerFin && !c->RxCount);
}

ULONG SendTcp(LONG Id, const void *Data, ULONG Size) {
    NETP_CONNECTION *c = ConnectionOf(Id);
    if (!c || c->FinQueued)
        return 0;
    ULONG n = Size < TX_SIZE - c->TxCount ? Size : TX_SIZE - c->TxCount;
    RtlCopyMemory(c->Tx + c->TxCount, Data, n);
    c->TxCount += n;
    TransmitConnection(c);
    return n;
}

bool TcpAllSent(LONG Id) {
    NETP_CONNECTION *c = ConnectionOf(Id);
    return !c || c->TxCount == 0;
}

void CloseTcp(LONG Id) {
    NETP_CONNECTION *c = ConnectionOf(Id);
    if (!c)
        return;
    c->FinQueued = true;
    TransmitConnection(c);
}

} // namespace xk::network
