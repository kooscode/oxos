// Network boot in the 2BL (NETBOOT=1 images; spec/oxos_internal/09_netboot.md; PC side: tools/netboot/netboot.py).
// Included by bl.cpp after its PCI / port helpers. Before the kernel in flash is unpacked, the 2BL brings up the NIC,
// asks the network for a boot image (UDP broadcasts to port 5141) and, if a PC answers, downloads a 256 KiB open image
// into RAM and boots the kernel and key block from it instead. No answer, no link, or a bad download: the flash image
// boots as usual.
//
// Runs with paging off (physical addresses), interrupts off, polled. The card's registers, bring-up and shut-down are
// shared with the kernel's network stack (xk/nic.h); here: the rings, the frames and the protocol.

#include <xk/nic.h>

namespace netboot {

constexpr ULONG RING = 0x00300000;                    // RX_COUNT receive descriptors, then 2 transmit descriptors
constexpr ULONG RX_BUFFERS = 0x00301000, TX_BUFFERS = 0x00320000;
constexpr ULONG IMAGE = 0x00800000;                   // the downloaded bank
constexpr ULONG RX_COUNT = 32, BUFFER = NIC_BUFFER, BANK_SIZE = 0x40000, CHUNK = 1024;
// A download is one bank or more (a kernel too large for one bank, a netbooted title's files after it), at most
// 4 MiB: 0x800000-0xBFFFFF, free RAM while the 2BL runs (the kernel keeps a title's pages: titles/nettitle.cpp).
constexpr ULONG MAX_IMAGE = 0x400000;

// The protocol (tools/netboot/netboot.py): its UDP port, the packet magic and the operations.
constexpr unsigned short PORT = 5141;
constexpr ULONG MAGIC = 0x424E584F;                   // 'OXNB'
enum : UCHAR { OP_HELLO = 1, OP_OFFER = 2, OP_READ = 3, OP_DATA = 4, OP_BOOT = 5, OP_MCPX = 6 };

// The UDP payload of every netboot message (little-endian), followed by the data of OP_HELLO / OP_DATA.
struct Message {
    ULONG Magic;
    UCHAR Op, Version;
    unsigned short Reserved;
    ULONG Session, Offset, Length; // OP_OFFER: Offset = CRC-32 of the image, Length = its size
};

static NIC_DESCRIPTOR *Rx() {
    return (NIC_DESCRIPTOR *)RING;
}

static NIC_DESCRIPTOR *Tx() {
    return (NIC_DESCRIPTOR *)RING + RX_COUNT;
}

static bool TimerRuns() {
    ULONG t = NicNow();
    for (int i = 0; i < 1000; i++) // about 1 ms of port cycles
        BlPost(0x30);
    return NicNow() != t;
}

// --- Frames.
static const UCHAR Mac[6] = {0x02, 0x58, 0x42, 0x4F, 0x58, 0x31}; // locally administered
static const UCHAR SourceIp[4] = {XK_NETBOOT_IP};                 // make NETBOOT_IP=
static ULONG TxNext;        // the next transmit descriptor (of 2)
static unsigned short IpId; // the IP identification of the next packet
static ULONG RxHead;        // the next receive descriptor to look at

static unsigned short IpChecksum(const UCHAR *p, ULONG n) {
    ULONG sum = 0;
    for (ULONG i = 0; i + 1 < n; i += 2)
        sum += (ULONG)p[i] << 8 | p[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

// One UDP broadcast from SourceIp:5141 to 255.255.255.255:5141.
static void Send(const Message &m, const void *data, ULONG length) {
    NIC_DESCRIPTOR &d = Tx()[TxNext & 1];
    for (ULONG t = NicNow(); (d.Flags & NIC_TX_VALID) && NicBefore(t, 20000);) {
        // Wait for the controller to finish with this descriptor, at most 20 ms.
    }
    if (d.Flags & NIC_TX_VALID)
        return;
    UCHAR *f = (UCHAR *)(TX_BUFFERS + (TxNext & 1) * BUFFER);
    memset(f, 0xFF, 6);
    memcpy(f + 6, Mac, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    UCHAR *h = f + 14;
    ULONG udp = 8 + sizeof(m) + length, ip = 20 + udp;
    memset(h, 0, 28);
    h[0] = 0x45;
    h[2] = (UCHAR)(ip >> 8);
    h[3] = (UCHAR)ip;
    h[4] = (UCHAR)(IpId >> 8);
    h[5] = (UCHAR)IpId;
    IpId++;
    h[8] = 64;
    h[9] = 17;
    memcpy(h + 12, SourceIp, 4);
    memset(h + 16, 0xFF, 4);
    unsigned short c = IpChecksum(h, 20);
    h[10] = (UCHAR)(c >> 8);
    h[11] = (UCHAR)c;
    h[20] = PORT >> 8;
    h[21] = PORT & 0xFF;
    h[22] = PORT >> 8;
    h[23] = PORT & 0xFF;
    h[24] = (UCHAR)(udp >> 8);
    h[25] = (UCHAR)udp; // checksum 0: none
    memcpy(h + 28, &m, sizeof(m));
    if (length)
        memcpy(h + 28 + sizeof(m), data, length);
    ULONG size = 14 + ip;
    while (size < 60)
        f[size++] = 0;
    NicTransmit(d, (ULONG)f, size);
    TxNext++;
}

// The next netboot message for Session (any op), or nullptr; Data points at what follows the header.
static const Message *Receive(ULONG Session, const UCHAR **Data) {
    for (ULONG n = 0; n < RX_COUNT; n++) {
        NIC_DESCRIPTOR &d = Rx()[RxHead];
        if (d.Flags & NIC_RX_AVAIL)
            return nullptr;
        // Copy the frame out before the descriptor goes back to the NIC.
        static UCHAR frame[BUFFER];
        ULONG length = d.Length & 0x3FFF;
        bool ok = (d.Flags & NIC_RX_VALID) && !(d.Flags & NIC_RX_ERRORS) && length <= BUFFER;
        if (ok)
            memcpy(frame, (const void *)d.Buffer, length);
        NicGiveBack(d);
        RxHead = (RxHead + 1) % RX_COUNT;
        if (!ok || length < 14 + 20 + 8 + sizeof(Message) || frame[12] != 0x08 || frame[13] != 0x00 ||
            frame[14 + 9] != 17)
            continue;
        ULONG ihl = (frame[14] & 0x0F) * 4;
        const UCHAR *u = frame + 14 + ihl;
        if (14 + ihl + 8 + sizeof(Message) > length || (u[2] << 8 | u[3]) != PORT)
            continue;
        auto m = (const Message *)(u + 8);
        if (m->Magic != MAGIC || m->Session != Session || m->Op == OP_HELLO || m->Op == OP_READ ||
            m->Op == OP_BOOT)
            continue; // not for us, or our own broadcast looped back
        *Data = u + 8 + sizeof(Message);
        return m;
    }
    return nullptr;
}

// --- NIC bring-up and shut-down (xk/nic.h): the rings at RING, then the card, waiting up to 4 s for the link.
static bool Start() {
    for (ULONG i = 0; i < RX_COUNT; i++)
        Rx()[i] = {RX_BUFFERS + i * BUFFER, (unsigned short)BUFFER, NIC_RX_AVAIL};
    Tx()[0] = Tx()[1] = {0, 0, 0};
    RxHead = TxNext = 0;
    return NicStart(Mac, RING, RX_COUNT, (ULONG)Tx(), 2, 4000000);
}

static ULONG Crc32(const UCHAR *p, ULONG n) {
    ULONG c = 0xFFFFFFFF;
    for (ULONG i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
    }
    return ~c;
}

// The downloaded bank, or nullptr to boot from flash.
static const UCHAR *Fetch() {
    if (!Start())
        return nullptr;
    const ULONG session = NicNow() ^ 0x4F584E42;
#if XK_MCPX_DUMP
    // The MCPX boot ROM copied by the entry code (start.S): sent three times, 100 ms apart, before the HELLOs
    // (tools/mcpx_dump/mcpx_dump.py receive). Offset 0, Length 512, the 512 bytes as data.
    if (*(volatile ULONG *)0x2200 == 0x5850434D) {
        for (int i = 0; i < 3; i++) {
            const Message dump = {MAGIC, OP_MCPX, 1, 0, session, 0, 512};
            Send(dump, (const void *)0x2000, 512);
            NicStall(100000);
        }
    }
#endif
    Message m = {MAGIC, OP_HELLO, 1, 0, session, 0, 0};
    const UCHAR *data;
    const Message *r = nullptr;
    // 1. Ask, every 250 ms for 2 s.
    for (int i = 0; i < 8 && !r; i++) {
        Send(m, Mac, sizeof(Mac));
        for (ULONG t = NicNow(); NicBefore(t, 250000) && !(r = Receive(session, &data));) {
            // Poll for the PC's answer, at most 250 ms.
        }
        if (r && r->Op != OP_OFFER)
            r = nullptr;
    }
    if (!r || r->Length < BANK_SIZE || r->Length > MAX_IMAGE || r->Length % CHUNK)
        return nullptr;
    const ULONG crc = r->Offset, size = r->Length;
    // 2. Read it chunk by chunk (each request repeated every 100 ms, 30 times at most).
    auto image = (UCHAR *)IMAGE;
    for (ULONG offset = 0; offset < size; offset += CHUNK) {
        m = {MAGIC, OP_READ, 1, 0, session, offset, CHUNK};
        bool got = false;
        for (int retry = 0; retry < 30 && !got; retry++) {
            Send(m, nullptr, 0);
            for (ULONG t = NicNow(); NicBefore(t, 100000) && !got;) {
                r = Receive(session, &data);
                if (r && r->Op == OP_DATA && r->Offset == offset && r->Length == CHUNK) {
                    memcpy(image + offset, data, CHUNK);
                    got = true;
                }
            }
        }
        if (!got)
            return nullptr;
    }
    // 3. Check it and say what happens next (Offset 0: booting the download, 1: bad image, booting flash).
    auto payload = (const XK_BOOT_PAYLOAD *)(image + 0x6000);
    bool good = Crc32(image, size) == crc && payload->Magic == PAYLOAD_MAGIC;
    m = {MAGIC, OP_BOOT, 1, 0, session, good ? 0u : 1u, 0};
    Send(m, nullptr, 0);
    for (ULONG t = NicNow(); (Tx()[(TxNext - 1) & 1].Flags & NIC_TX_VALID) && NicBefore(t, 20000);) {
        // Wait for the last packet to go out, at most 20 ms.
    }
    return good ? image : nullptr;
}

static const UCHAR *Boot() {
    if (!TimerRuns())
        return nullptr;
    const UCHAR *image = Fetch();
    NicStop();
    return image;
}

} // namespace netboot
