// Network boot log (OXOS addition, NETLOG=1 builds; spec/oxos-internal/03-debug-serial.md §7): everything the kernel
// prints to the debug serial port is also sent as UDP broadcasts (NETLOG_IP:5140 -> NETLOG_DEST:5140), so a
// console without a serial port can be followed from a PC on the same network (tools/netlog.py prints them).
// Addresses: make NETLOG_IP= (default 192.168.1.222) and NETLOG_DEST= (default the broadcast address).
// Transmit only, polled: it also works from the bug check and fatal error paths. The NIC set-up follows the
// nForce NIC sequence of XboxDev/cromwell's etherboot forcedeth driver (GPL-2.0); the link comes from PhyInitialize.
#include <xk/arch.h>
#include <xk/serial.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/rtl.h>
#include <xk/mm.h>

#if XK_NETLOG
XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2);
XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN Update);
XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);

constexpr ULONG NIC = 0xFEF00000; // BAR0, set by HalpInitPci; mapped (uncached) once the memory manager is up
enum : ULONG {
    NvIrqMask = 0x004, NvSetup6 = 0x008, NvPollingInterval = 0x00C, NvMisc1 = 0x080, NvTxControl = 0x084,
    NvTxStatus = 0x088, NvPacketFilter = 0x08C, NvOffload = 0x090, NvRxControl = 0x094, NvRxStatus = 0x098,
    NvRandomSeed = 0x09C, NvSetup1 = 0x0A0, NvSetup2 = 0x0A4, NvMacA = 0x0A8, NvMacB = 0x0AC, NvMcastA = 0x0B0,
    NvMcastB = 0x0B4, NvMcastMaskA = 0x0B8, NvMcastMaskB = 0x0BC, NvTxRing = 0x100, NvRxRing = 0x104,
    NvRingSizes = 0x108, NvTxUnknown = 0x10C, NvLinkSpeed = 0x110, NvSetup5 = 0x130, NvSetup3 = 0x134,
    NvTxRxControl = 0x144, NvMiiStatus = 0x180, NvSetup4 = 0x184, NvAdapterControl = 0x188, NvMiiSpeed = 0x18C,
    NvWakeUpFlags = 0x200, NvPowerState = 0x26C, NvIrqStatus = 0x000,
};
constexpr USHORT TX_VALID = 1u << 15, TX_FLAGS = (1u << 0) | (1u << 8) | (1u << 15); // last packet (x2), valid
constexpr ULONG TXRX_KICK = 1, TXRX_BIT1 = 2, TXRX_BIT2 = 4, TXRX_RESET = 0x10;

struct NV_DESC {
    ULONG Buffer;
    USHORT Length; // bytes - 1
    USHORT Flags;
};

static NV_DESC HalpNetTx[2] __attribute__((aligned(16)));
static NV_DESC HalpNetRx[1] __attribute__((aligned(16)));
static UCHAR HalpNetFrames[2][1536] __attribute__((aligned(16)));
static ULONG HalpNetNext, HalpNetId;
static volatile LONG HalpNetBusy;
volatile ULONG HalpNetState; // 0 not started, 1 buffering, 2 running, 3 no NIC/link (diagnostics)
volatile ULONG HalpNetSent, HalpNetDropped;
static char HalpNetPending[8192];
static ULONG HalpNetPendingLength;
// State 4: a title took the NIC (its network stack installs its own rings): the lines go to E:\oxoslog.txt instead
// (written by the heartbeat thread once a second, replayed over the network at the next boot: HalpNetLogReplay).
static char HalpNetFile[16384];
static ULONG HalpNetFileLength;


static volatile ULONG &Nv(ULONG offset) { return *(volatile ULONG *)(NIC + offset); }
static ULONG Phys(const void *p) { return (ULONG)p & 0x7FFFFFFF; } // the kernel image: 0x80000000 + physical

static bool NvWait(ULONG offset, ULONG mask, ULONG value, ULONG us) {
    for (; us; us -= 5) {
        if ((Nv(offset) & mask) == value)
            return true;
        KeStallExecutionProcessor(5);
        if (us < 5)
            break;
    }
    return (Nv(offset) & mask) == value;
}

static USHORT IpChecksum(const UCHAR *p, ULONG n) {
    ULONG sum = 0;
    for (ULONG i = 0; i + 1 < n; i += 2)
        sum += (ULONG)p[i] << 8 | p[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (USHORT)~sum;
}

static void HalpNetSend(const char *Text, ULONG Length) {
    NV_DESC &d = HalpNetTx[HalpNetNext & 1];
    for (ULONG us = 20000; (d.Flags & TX_VALID) && us; us -= 10) // the previous frame in this slot
        KeStallExecutionProcessor(10);
    if (d.Flags & TX_VALID) {
        HalpNetDropped = HalpNetDropped + 1;
        return;
    }
    UCHAR *f = HalpNetFrames[HalpNetNext & 1];
    static const UCHAR head[14] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x58, 0x42, 0x4F, 0x58, 0x31, 0x08, 0x00};
    RtlCopyMemory(f, head, 14);
    ULONG udp = 8 + Length, ip = 20 + udp;
    UCHAR *h = f + 14;
    RtlZeroMemory(h, 28);
    h[0] = 0x45;
    h[2] = (UCHAR)(ip >> 8), h[3] = (UCHAR)ip;
    h[4] = (UCHAR)(HalpNetId >> 8), h[5] = (UCHAR)HalpNetId;
    HalpNetId++;
    h[8] = 64, h[9] = 17;
    static const UCHAR source[4] = {XK_NETLOG_IP}, dest[4] = {XK_NETLOG_DEST}; // make NETLOG_IP= / NETLOG_DEST=
    RtlCopyMemory(h + 12, source, 4);
    RtlCopyMemory(h + 16, dest, 4);
    USHORT c = IpChecksum(h, 20);
    h[10] = (UCHAR)(c >> 8), h[11] = (UCHAR)c;
    h[20] = 5140 >> 8, h[21] = 5140 & 0xFF, h[22] = 5140 >> 8, h[23] = 5140 & 0xFF;
    h[24] = (UCHAR)(udp >> 8), h[25] = (UCHAR)udp; // checksum 0: none
    RtlCopyMemory(h + 28, Text, Length);
    ULONG size = 14 + ip;
    while (size < 60)
        f[size++] = 0;
    d.Buffer = Phys(f);
    d.Length = (USHORT)(size - 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d.Flags = TX_FLAGS;
    Nv(NvTxRxControl) = TXRX_KICK; // as cromwell (bit 2 would stop DMA)
    (void)Nv(NvTxRxControl);
#ifdef XK_NETLOG_SYNC_TX
    // Experiment: wait until the NIC has fetched the frame (its DMA is over) before going on.
    for (ULONG us = 2000; (d.Flags & TX_VALID) && us; us -= 2)
        KeStallExecutionProcessor(2);
#endif
    HalpNetNext++;
    HalpNetSent = HalpNetSent + 1;
}

static void HalpNetSendAll(const char *Text, ULONG Length) {
    while (Length && HalpNetState == 2) {
        ULONG n = Length > 1400 ? 1400 : Length;
        HalpNetSend(Text, n);
        Text += n, Length -= n;
    }
}

// After MmInitSystem (the NIC registers are mapped) and HalpInitPci (BAR0, bus master on).
extern "C" void HalpNetLogStart() {
    if (HalpNetState >= 2)
        return;
    HalpNetState = 3;
    ULONG ids;
    HalReadWritePCISpace(0, 4, 0, &ids, 4, FALSE);
    if (ids == 0xFFFFFFFF || !ids)
        return;
    // forcedeth_reset: 1) clear the previous set-up.
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
    KeStallExecutionProcessor(4);
    Nv(NvTxRxControl) = TXRX_BIT2;
    Nv(NvSetup6) = 0;
    // 2) rings: two transmit slots; one receive slot that is never handed to the NIC.
    HalpNetTx[0].Flags = HalpNetTx[1].Flags = 0;
    HalpNetRx[0] = {Phys(HalpNetFrames[0]), 0, 0};
    // 3) MAC address 02:58:42:4F:58:31 (locally administered).
    Nv(NvMacA) = 0x31584F42;
    Nv(NvMacB) = 0x5802;
    // 4) set-up.
    Nv(NvSetup3) = 0x200010;
    Nv(NvTxRxControl) = 0;
    (void)Nv(NvTxRxControl);
    Nv(NvTxRxControl) = TXRX_BIT1;
    NvWait(NvSetup5, 1u << 31, 1u << 31, 50000);
    Nv(NvSetup4) = 0;
    // 5) the link (PHY 1).
    Nv(NvMiiSpeed) = (1u << 8) | 5;
    PhyInitialize(FALSE, nullptr);
    ULONG link = PhyGetLinkState(FALSE);
    bool fast = link & 2, full = link & 8;
    ULONG speed = 0x10000 | (fast ? 100 : 10);
    // 6) set-up continued.
    Nv(NvMisc1) = 0x3B0F3C | (full ? 0 : 2);
    Nv(NvTxStatus) = Nv(NvTxStatus);
    Nv(NvPacketFilter) = 0x7F0008;
    Nv(NvOffload) = 0x5EE;
    Nv(NvRxStatus) = Nv(NvRxStatus);
    Nv(NvRandomSeed) = 0x7F00 | (75963081 & 0xFF);
    Nv(NvSetup1) = 0x16070F;
    Nv(NvSetup2) = 0x16;
    Nv(NvPollingInterval) = 970;
    Nv(NvSetup6) = 3;
    Nv(NvAdapterControl) = (1u << 24) | 0x4000; // PHY 1, PHY valid
    Nv(NvSetup4) = 8;
    Nv(NvWakeUpFlags) = 0x7770;
    // 7) start.
    Nv(NvRxRing) = Phys(HalpNetRx);
    Nv(NvTxRing) = Phys(HalpNetTx);
    Nv(NvRingSizes) = (0u << 16) | 1; // 1 receive, 2 transmit descriptors
    ULONG power = Nv(NvPowerState);
    if (!(power & 0x8000))
        Nv(NvPowerState) = power | 0x8000;
    KeStallExecutionProcessor(10);
    Nv(NvPowerState) = Nv(NvPowerState) | 0x100;
    Nv(NvAdapterControl) = 0x100000; // running
    Nv(NvIrqMask) = 0;
    Nv(NvIrqStatus) = 0x1FF;
    Nv(NvMiiStatus) = 0xF;
    Nv(NvIrqStatus) = 0x1FF;
    Nv(NvLinkSpeed) = speed;
    Nv(NvTxControl) = 1; // transmitter on
    (void)Nv(NvTxControl);
    HalpNetState = 2;
    char hello[96];
    int n = RtlSnprintf(hello, sizeof(hello), "netlog: link %s %s (PHY state %#lx)\n", fast ? "100" : "10",
                        full ? "full" : "half", link);
    HalpNetSendAll(hello, (ULONG)n);
    // No link, or xemu without a network backend: the first frame is never sent. Stop rather than make every later
    // line wait for a busy slot.
    for (ULONG us = 50000; (HalpNetTx[0].Flags & TX_VALID) && us; us -= 10)
        KeStallExecutionProcessor(10);
    if (HalpNetTx[0].Flags & TX_VALID) {
        HalpNetState = 3;
        HalpNetPendingLength = 0;
        return;
    }
    if (HalpNetPendingLength)
        HalpNetSendAll(HalpNetPending, HalpNetPendingLength);
    HalpNetPendingLength = 0;
}

// Heartbeat (NETLOG builds): once a second the tick count, the interrupts taken per IRQ line since the last beat and
// the IDE channel's state, so a stalled boot shows whether the system still runs (beats continue: a thread waits for
// I/O) or froze (beats stop; a jumping IRQ count before that means an interrupt storm).
extern volatile ULONG KiIrqCounts[16];
#if XK_TRACE_IMPORTS
extern "C" volatile ULONG (*XkTraceRing)[15]; // the tracer (src/xe/trace.cpp)
extern "C" volatile ULONG XkTraceCount;
#endif
#include <xk/idex.h>
void HalpNetLogWrite(const char *Text, ULONG Length);
#include <xk/io.h>
extern volatile ULONG KiDbgConfigured; // ke/dbgstep.cpp
static HANDLE HalpNetFileHandle;
static LONGLONG HalpNetFileOffset;
static char HalpNetFileCopy[sizeof(HalpNetFile)];

static NTSTATUS HalpNetOpenLog(HANDLE *h, ACCESS_MASK access, ULONG disposition) {
    OBJECT_STRING name;
    RtlInitAnsiString(&name, "\\Device\\Harddisk0\\Partition1\\oxoslog.txt");
    OBJECT_ATTRIBUTES a = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, access | SYNCHRONIZE, &a, &iosb, nullptr, 0, FILE_SHARE_READ, disposition,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

// Append the buffered lines to E:\oxoslog.txt and flush (heartbeat thread, PASSIVE_LEVEL).
[[maybe_unused]] static void HalpNetFileFlush() {
    ULONG flags = save_flags_cli();
    ULONG n = HalpNetFileLength;
    RtlCopyMemory(HalpNetFileCopy, HalpNetFile, n);
    HalpNetFileLength = 0;
    restore_flags(flags);
    if (!HalpNetFileHandle) {
        if (!NT_SUCCESS(HalpNetOpenLog(&HalpNetFileHandle, GENERIC_WRITE, FILE_OVERWRITE_IF)))
            return;
        static const char head[] = "netlog: the title took the NIC; continuing in E:\\oxoslog.txt\n";
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER at = {};
        NtWriteFile(HalpNetFileHandle, nullptr, nullptr, nullptr, &iosb, (PVOID)head, sizeof(head) - 1, &at);
        HalpNetFileOffset = sizeof(head) - 1;
    }
    if (n) {
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER at;
        at.QuadPart = HalpNetFileOffset;
        if (NT_SUCCESS(NtWriteFile(HalpNetFileHandle, nullptr, nullptr, nullptr, &iosb, HalpNetFileCopy, n, &at)))
            HalpNetFileOffset += n;
        NtFlushBuffersFile(HalpNetFileHandle, &iosb);
    }
}

// Before the title starts (title launcher, PASSIVE_LEVEL): send the previous run's E:\oxoslog.txt over the network,
// each line prefixed "prev: ", then empty the file.
extern "C" void HalpNetLogReplay() {
    // Not while breakpoint tracing (ke/dbgstep.cpp): reading the file after a frozen run has hung the title
    // launcher on a console, and a replay that stops half-way leaves the file for the next boot.
    if (HalpNetState != 2 || KiDbgConfigured)
        return;
    HANDLE h;
    if (!NT_SUCCESS(HalpNetOpenLog(&h, GENERIC_READ, FILE_OPEN)))
        return;
    static char chunk[1024], line[1200];
    ULONG used = 0;
    LONGLONG offset = 0;
    for (;;) {
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER at;
        at.QuadPart = offset;
        if (!NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, chunk, sizeof(chunk), &at)) ||
            !iosb.Information)
            break;
        offset += iosb.Information;
        for (ULONG i = 0; i < iosb.Information; i++) {
            if (!used) {
                RtlCopyMemory(line, "prev: ", 6);
                used = 6;
            }
            if (used < sizeof(line) - 2)
                line[used++] = chunk[i];
            if (chunk[i] == '\n') {
                HalpNetLogWrite(line, used);
                used = 0;
            }
        }
    }
    NtClose(h);
    if (offset && NT_SUCCESS(HalpNetOpenLog(&h, GENERIC_WRITE, FILE_OVERWRITE))) // empty it
        NtClose(h);
}

extern "C" void HalpSetBootLed(UCHAR Pattern);
extern "C" ULONG KiTextSum(); // ke/startup.cpp

static volatile ULONG HalpHeartbeatTick; // KeTickCount at the last beat (the watchdog's view of "threads still run")
extern "C" volatile ULONG KiPcRing[32];

// Watchdog (clock interrupt, NETLOG builds, once the title runs): no beat for 3 s means no thread at priority 30
// runs any more (an interrupt storm, or a title spinning at raised IRQL). Then take the NIC back from the title (it
// is not getting anywhere either) and send what the clock interrupt can see: the interrupt counts, the interrupted
// EIP of the last 32 ticks and the title's last kernel calls. Once per boot; the PC samples continue after it.
// A title's network stack asks for the NIC's interrupt (HalGetInterruptVector(4)): stop touching the NIC from here
// on (no frame in flight when the title resets it); the lines go to the disk file (state 4) as after a takeover.
void HalpNetLogReleaseNic() {
    if (HalpNetState != 2 || KiDbgConfigured) // breakpoint tracing: its lines need the network for as long as possible
        return;
    static const char note[] = "netlog: a title asked for the NIC's interrupt; releasing the NIC, continuing on disk\n";
    HalpNetLogWrite(note, sizeof(note) - 1);
    for (ULONG us = 20000; ((HalpNetTx[0].Flags | HalpNetTx[1].Flags) & TX_VALID) && us; us -= 10)
        KeStallExecutionProcessor(10); // let the last frames go out
    ULONG flags = save_flags_cli();
    HalpNetState = 4;
    restore_flags(flags);
}

// Take the NIC back from a title that owns it (state 4) for a last report: the watchdog and the fault tasks.
void HalpNetTakeBack() {
    HalpNetBusy = 0;
    if (HalpNetState == 4) {
        HalpNetState = 3;
        HalpNetLogStart();
    }
}

void HalpNetWatchdog() {
    static bool fired;
    if (fired || !HalpHeartbeatTick || KeTickCount - HalpHeartbeatTick < 3000)
        return;
    fired = true;
    HalpNetTakeBack();
    char line[200];
    int n = RtlSnprintf(line, sizeof(line), "*** watchdog: no heartbeat for %lu ms (tick %lu); interrupts so far:",
                        KeTickCount - HalpHeartbeatTick, KeTickCount);
    for (ULONG i = 0; i < 16 && n < 180; i++)
        if (KiIrqCounts[i])
            n += RtlSnprintf(line + n, sizeof(line) - n, " %lu:%lu", i, KiIrqCounts[i]);
    line[n++] = '\n';
    HalpNetLogWrite(line, (ULONG)n);
    for (ULONG k = 0; k < 32; k += 8) {
        ULONG t = KeTickCount + 1 + k; // oldest first
        n = RtlSnprintf(line, sizeof(line), "    pcs %#lx %#lx %#lx %#lx %#lx %#lx %#lx %#lx\n", KiPcRing[t % 32],
                        KiPcRing[(t + 1) % 32], KiPcRing[(t + 2) % 32], KiPcRing[(t + 3) % 32], KiPcRing[(t + 4) % 32],
                        KiPcRing[(t + 5) % 32], KiPcRing[(t + 6) % 32], KiPcRing[(t + 7) % 32]);
        HalpNetLogWrite(line, (ULONG)n);
    }
#if XK_TRACE_IMPORTS
    if (XkTraceRing) {
        ULONG count = XkTraceCount;
        for (ULONG k = count > 12 ? count - 12 : 0; k < count; k++) {
            volatile ULONG *e = XkTraceRing[k % 0x8000];
            n = RtlSnprintf(line, sizeof(line), "  call %lu: ordinal %lu -> %#lx, from %#lx thread %#lx args %#lx %#lx %#lx\n",
                            k, e[0], e[6], e[1], e[2], e[3], e[4], e[5]);
            HalpNetLogWrite(line, (ULONG)n);
        }
    }
#endif
}

#if XK_BOOT_SELFTEST
// Self-test builds on hardware: which boot self-test suites have finished (spec/oxos-internal/03 §7).
extern volatile ULONG KiTestThreadsDone, KiTestApcSuiteDone, KiTestMmDone, KiTestObDone, KiTestIoDone, KiTestHalDone,
    KiTestIdexDone, KiTestFatxDone, KiTestDvdDone, KiTestRtlDone, KiTestSehDone, KiTestXcDone, KiTestMiscDone;
static void HalpNetSelfTestProgress() {
    static const struct { const char *Name; volatile ULONG *Done; } suites[] = {
        {"threads", &KiTestThreadsDone}, {"apc", &KiTestApcSuiteDone}, {"mm", &KiTestMmDone}, {"ob", &KiTestObDone},
        {"io", &KiTestIoDone}, {"hal", &KiTestHalDone}, {"idex", &KiTestIdexDone}, {"fatx", &KiTestFatxDone},
        {"dvd", &KiTestDvdDone}, {"rtl", &KiTestRtlDone}, {"seh", &KiTestSehDone}, {"xc", &KiTestXcDone},
        {"misc", &KiTestMiscDone}};
    char line[200];
    int n = RtlSnprintf(line, sizeof(line), "selftest: done");
    bool all = true;
    for (auto &t : suites)
        if (*t.Done)
            n += RtlSnprintf(line + n, sizeof(line) - n, " %s", t.Name);
        else
            all = false;
    n += RtlSnprintf(line + n, sizeof(line) - n, all ? " (all)\n" : "; waiting:");
    if (!all) {
        for (auto &t : suites)
            if (!*t.Done)
                n += RtlSnprintf(line + n, sizeof(line) - n, " %s", t.Name);
        line[n++] = '\n';
    }
    HalpNetLogWrite(line, (ULONG)n);
}
#endif

static void NTAPI HalpNetHeartbeat(PVOID) {
    ULONG last[16] = {};
    for (ULONG beat = 1;; beat++) {
        HalpHeartbeatTick = KeTickCount;
        LARGE_INTEGER t;
        t.QuadPart = -10000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        char line[200];
        int n = RtlSnprintf(line, sizeof(line), "hb %lu: tick %lu irq", beat, KeTickCount);
        for (ULONG i = 0; i < 16 && n < 150; i++) {
            ULONG c = KiIrqCounts[i];
            if (c != last[i])
                n += RtlSnprintf(line + n, sizeof(line) - n, " %lu:+%lu", i, c - last[i]);
            last[i] = c;
        }
        // The alternate status register: reading 0x1F7 would clear a pending drive interrupt before the IDE ISR sees it.
        n += RtlSnprintf(line + n, sizeof(line) - n, "; ide busy %u timeout %u status %#x bm %#x\n",
                         IdexChannelObject.ExpectingBusMasterInterrupt, IdexChannelObject.Timeout, inb(0x3F6),
                         inb(0xFF62));
        HalpNetLogWrite(line, (ULONG)n);
        {
            n = RtlSnprintf(line, sizeof(line), "text %08lx\n", KiTextSum());
            HalpNetLogWrite(line, (ULONG)n);
        }
        // The GPU's command engines (a title waiting for its push buffer to be consumed shows why the FIFO is
        // stuck): PMC enable/interrupts, PFIFO interrupts, DMA pusher state, cache 1 GET/PUT and status, PGRAPH
        // status/interrupts, the memory size register.
        // Off by default: reading the registers of a GPU engine that is switched off (PMC 0x200) is not answered
        // properly, and with memory traffic going on it froze a console within a minute (2026-10-23, elimination
        // series stage 1). XK_HEARTBEAT_GPU turns it back on for debugging a title's GPU set-up.
#ifdef XK_HEARTBEAT_GPU
        {
            auto gpu = [](ULONG o) { return *(volatile ULONG *)(0xFD000000 + o); };
            char gl[320];
            int gn = RtlSnprintf(gl, sizeof(gl),
                            "gpu: pmc %08lx intr %08lx | pfifo intr %08lx caches %08lx push %08lx dma put %08lx get %08lx "
                            "state %08lx | c1 status %08lx | pgraph status %08lx intr %08lx | fb size %08lx\n",
                            gpu(0x200), gpu(0x100), gpu(0x2100), gpu(0x2500), gpu(0x3220), gpu(0x3240), gpu(0x3244),
                            gpu(0x3228), gpu(0x3214), gpu(0x400700), gpu(0x400100), gpu(0x10020C));
            HalpNetLogWrite(gl, (ULONG)gn);
            // The code at the latest sampled title EIP (the title is not ours to disassemble otherwise): 16 bytes
            // before and 32 after, when that page is mapped.
            ULONG pc = KiPcRing[KeTickCount % 32];
            if (pc >= 0x10010 && pc < 0x80000000 && MmIsAddressValid((PVOID)(pc - 16)) &&
                MmIsAddressValid((PVOID)(pc + 32))) {
                auto c = (const UCHAR *)(pc - 16);
                gn = RtlSnprintf(gl, sizeof(gl), "code at %08lx-16:", pc);
                for (ULONG i = 0; i < 48; i++)
                    gn += RtlSnprintf(gl + gn, sizeof(gl) - gn, " %02x", c[i]);
                gl[gn++] = '\n';
                HalpNetLogWrite(gl, (ULONG)gn);
            }
        }
#endif
#if XK_BOOT_SELFTEST
        HalpNetSelfTestProgress();
#endif
#if XK_TRACE_IMPORTS
        // The title's last kernel calls (trace builds): ordinal, return value (0xDEADDEAD = still inside), arguments.
        if (XkTraceRing) {
            ULONG count = XkTraceCount;
            for (ULONG k = count > 6 ? count - 6 : 0; k < count; k++) {
                volatile ULONG *e = XkTraceRing[k % 0x8000];
                n = RtlSnprintf(line, sizeof(line), "  call %lu: ordinal %lu -> %#lx, from %#lx args %#lx %#lx %#lx\n", k,
                                e[0], e[6], e[1], e[3], e[4], e[5]);
                HalpNetLogWrite(line, (ULONG)n);
            }
        }
#endif
        // No disk file any more: the heartbeat's writes to E: at top priority, next to a title's own disk I/O, are
        // suspected in hardware freezes, and the on-screen log (hal/splash.cpp) now shows what the file was for.
        // if (HalpNetState == 4)
        //     HalpNetFileFlush();
        // Liveness on the front LED: the beat alternates green with orange while the log goes over the network, with
        // red once it goes to the disk file (the title owns the NIC). A frozen colour means the system stopped.
#ifndef XK_HEARTBEAT_NO_LED
        HalpSetBootLed((beat & 1) ? 0x0F : (HalpNetState == 4 ? 0xF0 : 0xFF));
#endif
    }
}

extern "C" void HalpNetLogStartHeartbeat() {
    if (HalpNetState == 2)
        KiCreateSystemThread(HalpNetHeartbeat, nullptr, 0, 30);
}

void HalpScreenLog(const char *Text, ULONG Length);

void HalpNetLogWrite(const char *Text, ULONG Length) {
    if (__atomic_exchange_n(&HalpNetBusy, 1, __ATOMIC_SEQ_CST))
        return; // re-entered from an interrupt or another processor path: drop rather than corrupt a frame
    ULONG flags = save_flags_cli();
    if (HalpNetState == 2 && Nv(NvTxRing) != Phys(HalpNetTx))
        HalpNetState = 4;
    if (HalpNetState == 2) {
        XK_MARK("mark: netlog: sending");
        HalpNetSendAll(Text, Length);
        XK_MARK("mark: netlog: sent");
    } else if (HalpNetState == 4) {
        for (ULONG i = 0; i < Length && HalpNetFileLength < sizeof(HalpNetFile); i++)
            HalpNetFile[HalpNetFileLength++] = Text[i];
    } else if (HalpNetState <= 1) {
        HalpNetState = 1;
        for (ULONG i = 0; i < Length && HalpNetPendingLength < sizeof(HalpNetPending); i++)
            HalpNetPending[HalpNetPendingLength++] = Text[i];
    }
    HalpScreenLog(Text, Length); // the on-screen copy (hal/splash.cpp)
    XK_MARK("mark: netlog: done");
    restore_flags(flags);
    __atomic_store_n(&HalpNetBusy, 0, __ATOMIC_SEQ_CST);
}
#endif
