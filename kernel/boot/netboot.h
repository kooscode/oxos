// Network boot in the 2BL (NETBOOT=1 images; spec/oxos-internal/09-netboot.md; PC side: tools/netboot/netboot.py).
// Included by bl.cpp after its PCI / port helpers. Before the kernel in flash is unpacked, the 2BL brings up the NIC,
// asks the network for a boot image (UDP broadcasts to port 5141) and, if a PC answers, downloads a 256 KiB open image
// into RAM and boots the kernel and key block from it instead. No answer, no link, or a bad download: the flash image
// boots as usual.
//
// Runs with paging off (physical addresses), interrupts off, polled. The NIC sequence follows the kernel's network
// log (kernel/src/hal/netlog.cpp: XboxDev/cromwell's etherboot forcedeth order); the receive ring follows nxdk's
// nvnetdrv (MIT): a descriptor handed to the NIC has flags bit 15 (available) set; the NIC clears it and sets bit 0
// (valid) and the length; the driver gives it back and writes TxRxControl bit 1 (get).

namespace netboot {

constexpr ULONG NIC = 0xFEF00000;                     // BAR0, as the kernel's HalpInitPci assigns it
constexpr ULONG RING = 0x00300000;                    // RX_COUNT receive descriptors, then 2 transmit descriptors
constexpr ULONG RX_BUFFERS = 0x00301000, TX_BUFFERS = 0x00320000;
constexpr ULONG IMAGE = 0x00800000;                   // the downloaded bank
constexpr ULONG RX_COUNT = 32, BUFFER = 2048, BANK_SIZE = 0x40000, CHUNK = 1024;
// A download is one bank or more (a kernel too large for one bank, a netbooted title's files after it), at most
// 4 MiB: 0x800000-0xBFFFFF, free RAM while the 2BL runs (the kernel keeps a title's pages: xe/nettitle.cpp).
constexpr ULONG MAX_IMAGE = 0x400000;
constexpr unsigned short PORT = 5141;
constexpr ULONG MAGIC = 0x424E584F;                   // 'OXNB'
enum : UCHAR { OP_HELLO = 1, OP_OFFER = 2, OP_READ = 3, OP_DATA = 4, OP_BOOT = 5, OP_MCPX = 6 };

enum : ULONG {
    NvIrqStatus = 0x000, NvIrqMask = 0x004, NvSetup6 = 0x008, NvPollingInterval = 0x00C, NvMisc1 = 0x080,
    NvTxControl = 0x084, NvTxStatus = 0x088, NvPacketFilter = 0x08C, NvOffload = 0x090, NvRxControl = 0x094,
    NvRxStatus = 0x098, NvRandomSeed = 0x09C, NvSetup1 = 0x0A0, NvSetup2 = 0x0A4, NvMacA = 0x0A8, NvMacB = 0x0AC,
    NvMcastA = 0x0B0, NvMcastB = 0x0B4, NvMcastMaskA = 0x0B8, NvMcastMaskB = 0x0BC, NvTxRing = 0x100,
    NvRxRing = 0x104, NvRingSizes = 0x108, NvTxUnknown = 0x10C, NvLinkSpeed = 0x110, NvSetup5 = 0x130,
    NvSetup3 = 0x134, NvTxRxControl = 0x144, NvMiiStatus = 0x180, NvSetup4 = 0x184, NvAdapterControl = 0x188,
    NvMiiSpeed = 0x18C, NvMiiControl = 0x190, NvMiiData = 0x194, NvWakeUpFlags = 0x200, NvPowerState = 0x26C,
};
constexpr unsigned short TX_VALID = 1u << 15, TX_FLAGS = (1u << 0) | (1u << 8) | (1u << 15);
constexpr unsigned short RX_AVAIL = 1u << 15, RX_VALID = 1u << 0, RX_ERRORS = 0x1F80; // errors 1-4, CRC, overflow
constexpr ULONG TXRX_KICK = 1, TXRX_GET = 2, TXRX_BIT2 = 4, TXRX_RESET = 0x10;

struct Desc {
    ULONG Buffer;
    unsigned short Length;
    unsigned short Flags;
};

// The UDP payload of every netboot message (little-endian), followed by the data of OP_HELLO / OP_DATA.
struct Message {
    ULONG Magic;
    UCHAR Op, Version;
    unsigned short Reserved;
    ULONG Session, Offset, Length; // OP_OFFER: Offset = CRC-32 of the image, Length = its size
};

static volatile ULONG &Nv(ULONG offset) { return *(volatile ULONG *)(NIC + offset); }
static Desc *Rx() { return (Desc *)RING; }
static Desc *Tx() { return (Desc *)RING + RX_COUNT; }

// --- Time: the MCPX ACPI PM timer (I/O 0x8008, 3.375 MHz), as the kernel's KeStallExecutionProcessor.
static ULONG Now() { return BlIn32(0x8008); }
static ULONG Ticks(ULONG us) { return us * 27 / 8; }
static bool Before(ULONG start, ULONG us) { return Now() - start < Ticks(us); }
static void Stall(ULONG us) {
    for (ULONG t = Now(); Before(t, us);)
        ;
}
static bool TimerRuns() {
    ULONG t = Now();
    for (int i = 0; i < 1000; i++) // about 1 ms of port cycles
        BlPost(0x30);
    return Now() != t;
}

// --- PHY 1 over MII (as kernel/src/hal/phy.cpp).
static bool MiiWait() {
    for (ULONG t = Now(); Before(t, 10000);)
        if (!(Nv(NvMiiControl) & 0x8000))
            return true;
    return false;
}
static ULONG MiiRead(ULONG reg) {
    MiiWait();
    Nv(NvMiiControl) = reg | 0x20;
    MiiWait();
    return Nv(NvMiiData);
}

// --- Frames.
static ULONG TxNext;
static unsigned short IpId;

static unsigned short IpChecksum(const UCHAR *p, ULONG n) {
    ULONG sum = 0;
    for (ULONG i = 0; i + 1 < n; i += 2)
        sum += (ULONG)p[i] << 8 | p[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

static const UCHAR Mac[6] = {0x02, 0x58, 0x42, 0x4F, 0x58, 0x31}; // as the kernel's network log
static const UCHAR SourceIp[4] = {XK_NETLOG_IP};                  // make NETLOG_IP=

// One UDP broadcast from SourceIp:5141 to 255.255.255.255:5141.
static void Send(const Message &m, const void *data, ULONG length) {
    Desc &d = Tx()[TxNext & 1];
    for (ULONG t = Now(); (d.Flags & TX_VALID) && Before(t, 20000);)
        ;
    if (d.Flags & TX_VALID)
        return;
    UCHAR *f = (UCHAR *)(TX_BUFFERS + (TxNext & 1) * BUFFER);
    memset(f, 0xFF, 6);
    memcpy(f + 6, Mac, 6);
    f[12] = 0x08, f[13] = 0x00;
    UCHAR *h = f + 14;
    ULONG udp = 8 + sizeof(m) + length, ip = 20 + udp;
    memset(h, 0, 28);
    h[0] = 0x45;
    h[2] = (UCHAR)(ip >> 8), h[3] = (UCHAR)ip;
    h[4] = (UCHAR)(IpId >> 8), h[5] = (UCHAR)IpId;
    IpId++;
    h[8] = 64, h[9] = 17;
    memcpy(h + 12, SourceIp, 4);
    memset(h + 16, 0xFF, 4);
    unsigned short c = IpChecksum(h, 20);
    h[10] = (UCHAR)(c >> 8), h[11] = (UCHAR)c;
    h[20] = PORT >> 8, h[21] = PORT & 0xFF, h[22] = PORT >> 8, h[23] = PORT & 0xFF;
    h[24] = (UCHAR)(udp >> 8), h[25] = (UCHAR)udp; // checksum 0: none
    memcpy(h + 28, &m, sizeof(m));
    if (length)
        memcpy(h + 28 + sizeof(m), data, length);
    ULONG size = 14 + ip;
    while (size < 60)
        f[size++] = 0;
    d.Buffer = (ULONG)f;
    d.Length = (unsigned short)(size - 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d.Flags = TX_FLAGS;
    Nv(NvTxRxControl) = TXRX_KICK;
    (void)Nv(NvTxRxControl);
    TxNext++;
}

static ULONG RxHead;

// The next netboot message for Session (any op), or nullptr; Data points at what follows the header.
static const Message *Receive(ULONG Session, const UCHAR **Data) {
    for (ULONG n = 0; n < RX_COUNT; n++) {
        Desc &d = Rx()[RxHead];
        if (d.Flags & RX_AVAIL)
            return nullptr;
        // Copy the frame out before the descriptor goes back to the NIC.
        static UCHAR frame[BUFFER];
        ULONG length = d.Length & 0x3FFF;
        bool ok = (d.Flags & RX_VALID) && !(d.Flags & RX_ERRORS) && length <= BUFFER;
        if (ok)
            memcpy(frame, (const void *)d.Buffer, length);
        d.Length = BUFFER;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        d.Flags = RX_AVAIL;
        Nv(NvTxRxControl) = TXRX_GET;
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

// --- NIC bring-up and shut-down.
static bool Start() {
    BlPciWrite(4, 0x10, NIC);
    BlPciWrite(4, 0x14, 0xE001);
    BlPciWrite(4, 0x04, BlPciRead(4, 0x04) | 7); // I/O, memory, bus master
    ULONG ids = BlPciRead(4, 0x00);
    if (ids == 0xFFFFFFFF || !ids)
        return false;
    Nv(NvMcastA) = 1;
    Nv(NvMcastB) = 0;
    Nv(NvMcastMaskA) = 0;
    Nv(NvMcastMaskB) = 0;
    Nv(NvPacketFilter) = 0;
    Nv(NvAdapterControl) = 0;
    Nv(NvLinkSpeed) = 0;
    Nv(NvTxUnknown) = 0;
    Nv(NvTxRxControl) = TXRX_BIT2 | TXRX_RESET;
    (void)Nv(NvTxRxControl);
    Stall(4);
    Nv(NvTxRxControl) = TXRX_BIT2;
    Nv(NvSetup6) = 0;
    for (ULONG i = 0; i < RX_COUNT; i++)
        Rx()[i] = {RX_BUFFERS + i * BUFFER, (unsigned short)BUFFER, RX_AVAIL};
    Tx()[0] = Tx()[1] = {0, 0, 0};
    RxHead = TxNext = 0;
    Nv(NvMacA) = (ULONG)Mac[0] | Mac[1] << 8 | Mac[2] << 16 | (ULONG)Mac[3] << 24; // byte order as nxdk
    Nv(NvMacB) = (ULONG)Mac[4] | Mac[5] << 8;
    Nv(NvSetup3) = 0x200010;
    Nv(NvTxRxControl) = 0;
    (void)Nv(NvTxRxControl);
    Nv(NvTxRxControl) = TXRX_GET;
    for (ULONG t = Now(); !(Nv(NvSetup5) & 1u << 31) && Before(t, 50000);)
        ;
    Nv(NvSetup4) = 0;
    // The link: auto-negotiation finishes about 2-3 s after power-on.
    Nv(NvMiiSpeed) = (1u << 8) | 5;
    ULONG status = 0;
    for (ULONG t = Now(); Before(t, 4000000);) {
        status = MiiRead(1);
        if ((status & 0x24) == 0x24) // negotiated, link up
            break;
        Stall(10000);
    }
    if ((status & 0x24) != 0x24)
        return false;
    ULONG common = MiiRead(4) & MiiRead(5);
    bool fast = common & 0x180, full = fast ? (common & 0x100) : (common & 0x40);
    Nv(NvMisc1) = 0x3B0F3C | (full ? 0 : 2);
    Nv(NvTxStatus) = Nv(NvTxStatus);
    Nv(NvPacketFilter) = 0x7F0020; // always + our address (broadcasts included), as nxdk
    Nv(NvOffload) = 0x5EE;
    Nv(NvRxStatus) = Nv(NvRxStatus);
    Nv(NvRandomSeed) = 0x7F00 | (Now() & 0xFF);
    Nv(NvSetup1) = 0x16070F;
    Nv(NvSetup2) = 0x16;
    Nv(NvPollingInterval) = 970;
    Nv(NvSetup6) = 3;
    Nv(NvAdapterControl) = (1u << 24) | 0x4000;
    Nv(NvSetup4) = 8;
    Nv(NvWakeUpFlags) = 0x7770;
    Nv(NvRxRing) = RING;
    Nv(NvTxRing) = (ULONG)Tx();
    Nv(NvRingSizes) = ((RX_COUNT - 1) << 16) | 1;
    ULONG power = Nv(NvPowerState);
    if (!(power & 0x8000))
        Nv(NvPowerState) = power | 0x8000;
    Stall(10);
    Nv(NvPowerState) = Nv(NvPowerState) | 0x100;
    Nv(NvAdapterControl) = 0x100000;
    Nv(NvIrqMask) = 0;
    Nv(NvIrqStatus) = 0x1FF;
    Nv(NvMiiStatus) = 0xF;
    Nv(NvIrqStatus) = 0x1FF;
    Nv(NvLinkSpeed) = 0x10000 | (fast ? 100 : 1000); // forcedeth: 100 = 100 Mbit/s, 1000 = 10 Mbit/s
    Nv(NvTxControl) = 1;
    Nv(NvRxControl) = 1;
    Nv(NvTxRxControl) = TXRX_KICK | TXRX_GET;
    (void)Nv(NvTxControl);
    return true;
}

// Leave the NIC quiet: no DMA into RAM the kernel is about to reuse. The kernel's own drivers set it up again.
static void Stop() {
    Nv(NvRxControl) = 0;
    Nv(NvTxControl) = 0;
    Stall(100);
    Nv(NvTxRxControl) = TXRX_BIT2 | TXRX_RESET;
    (void)Nv(NvTxRxControl);
    Stall(10);
    Nv(NvTxRxControl) = TXRX_BIT2;
    Nv(NvRxRing) = 0;
    Nv(NvTxRing) = 0;
    Nv(NvAdapterControl) = 0;
    Nv(NvIrqMask) = 0;
    Nv(NvIrqStatus) = 0x1FF;
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
    const ULONG session = Now() ^ 0x4F584E42;
#if XK_MCPX_DUMP
    // The MCPX boot ROM copied by the entry code (start.S): sent three times, 100 ms apart, before the HELLOs
    // (tools/mcpx-dump/mcpx_dump.py receive). Offset 0, Length 512, the 512 bytes as data.
    if (*(volatile ULONG *)0x2200 == 0x5850434D)
        for (int i = 0; i < 3; i++) {
            const Message dump = {MAGIC, OP_MCPX, 1, 0, session, 0, 512};
            Send(dump, (const void *)0x2000, 512);
            Stall(100000);
        }
#endif
    Message m = {MAGIC, OP_HELLO, 1, 0, session, 0, 0};
    const UCHAR *data;
    const Message *r = nullptr;
    // 1. Ask, every 250 ms for 2 s.
    for (int i = 0; i < 8 && !r; i++) {
        Send(m, Mac, sizeof(Mac));
        for (ULONG t = Now(); Before(t, 250000) && !(r = Receive(session, &data));)
            ;
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
            for (ULONG t = Now(); Before(t, 100000) && !got;) {
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
    for (ULONG t = Now(); (Tx()[(TxNext - 1) & 1].Flags & TX_VALID) && Before(t, 20000);)
        ;
    return good ? image : nullptr;
}

static const UCHAR *Boot() {
    if (!TimerRuns())
        return nullptr;
    const UCHAR *image = Fetch();
    Stop();
    return image;
}

} // namespace netboot
