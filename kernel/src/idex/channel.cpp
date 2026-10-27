// IDE channel: interrupt, completion DPC, timeout timer, packet serialisation, bus-master DMA (spec/stock/04 §1, §5).
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/idex.h>

IDE_CHANNEL_OBJECT IdexChannelObject;

// Physical region descriptor table: 33 entries cover 128 KB starting at any page offset. 512-byte alignment keeps
// it inside one 64 KB region as the bus master requires.
struct IDE_PRD {
    ULONG Physical;
    ULONG Count; // bytes in bits 0-15 (0 = 64 KB), bit 31 = end of table
};
alignas(512) static IDE_PRD IdexPrdTable[33];

static volatile UCHAR IdexLastAtaStatus, IdexLastBusMasterStatus;

bool IdexWaitForStatus(UCHAR mask, UCHAR value, ULONG timeoutUs) {
    for (ULONG i = 0; i < timeoutUs; i += 10) {
        if ((inb(IDE_ALT_STATUS) & mask) == value)
            return true;
        KeStallExecutionProcessor(10);
    }
    return (inb(IDE_ALT_STATUS) & mask) == value;
}

// Describe Buffer to the bus master; merges physically contiguous pages, never crosses a 64 KB boundary.
volatile ULONG IdexBadDmaPages; // diagnostics: transfers refused because a page was not valid RAM

// The pages of the chunk being transferred stay where they are until it is over: an unlocked page can be moved by a
// contiguous allocation (MiRelocatePage), and the bus master would then write into its old frame, which by then
// belongs to someone else (a hardware transfer takes milliseconds; xemu finishes it at once).
static PVOID IdexLockedBuffer;
static ULONG IdexLockedLength;

void IdexUnlockDmaBuffer() {
    if (IdexLockedBuffer) {
        MmLockUnlockBufferPages(IdexLockedBuffer, IdexLockedLength, TRUE);
        IdexLockedBuffer = nullptr;
    }
}

static bool IdexProgramDmaPages(PVOID Buffer, ULONG Length);

bool IdexProgramDma(PVOID Buffer, ULONG Length) {
    IdexUnlockDmaBuffer();
    MmLockUnlockBufferPages(Buffer, Length, FALSE);
    IdexLockedBuffer = Buffer;
    IdexLockedLength = Length;
    if (IdexProgramDmaPages(Buffer, Length))
        return true;
    IdexUnlockDmaBuffer();
    return false;
}

static bool IdexProgramDmaPages(PVOID Buffer, ULONG Length) {
    ULONG va = (ULONG)Buffer, n = 0;
    while (Length) {
        // Real hardware hangs the bus on a transfer to an address that is not RAM (xemu ignores it): check each
        // page is mapped and below 64 MB before the engine sees it.
        if (!MmIsAddressValid((PVOID)va)) {
            IdexBadDmaPages = IdexBadDmaPages + 1;
            XK_SERIAL("*** ide: DMA buffer %p (%lu bytes left) is not mapped; transfer refused\n", (PVOID)va, Length);
            return false;
        }
        ULONG phys = MmGetPhysicalAddress((PVOID)va);
        if (phys >= 0x04000000 || (phys & 1)) {
            IdexBadDmaPages = IdexBadDmaPages + 1;
            XK_SERIAL("*** ide: DMA buffer %p -> physical %08lx is not RAM; transfer refused\n", (PVOID)va, phys);
            return false;
        }
        ULONG piece = 0x1000 - (va & 0xFFF);
        if (piece > Length)
            piece = Length;
        if (n && IdexPrdTable[n - 1].Physical + (IdexPrdTable[n - 1].Count & 0x1FFFF) == phys &&
            ((phys ^ IdexPrdTable[n - 1].Physical) & 0xFFFF0000) == 0 &&
            (IdexPrdTable[n - 1].Count & 0x1FFFF) + piece <= 0x10000) {
            IdexPrdTable[n - 1].Count += piece;
        } else {
            if (n == 33)
                return false;
            IdexPrdTable[n].Physical = phys;
            IdexPrdTable[n].Count = piece;
            n++;
        }
        va += piece;
        Length -= piece;
    }
    for (ULONG i = 0; i < n; i++)
        IdexPrdTable[i].Count &= 0xFFFF; // 0x10000 encodes as 0
    IdexPrdTable[n - 1].Count |= 0x80000000;
    outb(IDE_BM_COMMAND, 0);
    outb(IDE_BM_STATUS, inb(IDE_BM_STATUS) | 6); // clear interrupt and error
    outl(IDE_BM_PRDT, IdexChannelObject.PhysicalRegionDescriptorTablePhysical);
    return true;
}

static BOOLEAN NTAPI IdexInterruptService(PKINTERRUPT, PVOID) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    XK_MARK("mark: IDE interrupt");
    if (!ch->ExpectingBusMasterInterrupt) {
        inb(IDE_STATUS); // acknowledge anything unexpected (PIO commands are polled)
        return FALSE;
    }
    UCHAR bm = inb(IDE_BM_STATUS);
    if (!(bm & 4))
        return FALSE;
    outb(IDE_BM_COMMAND, 0);
    outb(IDE_BM_STATUS, bm | 6);
    IdexLastBusMasterStatus = bm;
    IdexLastAtaStatus = inb(IDE_STATUS);
    ch->ExpectingBusMasterInterrupt = FALSE;
    KeInsertQueueDpc(&ch->FinishDpc, nullptr, nullptr);
    return TRUE;
}

static void NTAPI IdexFinishDpc(PKDPC, PVOID, PVOID, PVOID) {
    XK_MARK("mark: IDE finish DPC");
    IdexUnlockDmaBuffer(); // the chunk is done (or abandoned)
    if (IdexChannelObject.FinishIoRoutine)
        IdexChannelObject.FinishIoRoutine();
}

// Once a second: a command that has not interrupted within Timeout seconds is abandoned.
static void NTAPI IdexTimerDpc(PKDPC, PVOID, PVOID, PVOID) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    bool expired = ch->ExpectingBusMasterInterrupt && --ch->Timeout == 0;
    if (expired) {
        ch->ExpectingBusMasterInterrupt = FALSE;
        outb(IDE_BM_COMMAND, 0);
        IdexLastBusMasterStatus = 2; // reported as an error
        IdexLastAtaStatus = IDE_STATUS_ERR;
    }
    KfLowerIrql(old);
    if (expired && ch->TimeoutExpiredRoutine)
        ch->TimeoutExpiredRoutine();
}

UCHAR IdexGetCompletionStatus(PUCHAR BusMasterStatus) {
    *BusMasterStatus = IdexLastBusMasterStatus;
    return IdexLastAtaStatus;
}

static void IdexStartIo(PIRP Irp) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    PDEVICE_OBJECT dev = IoGetCurrentIrpStackLocation(Irp)->DeviceObject;
    ch->CurrentIrp = Irp;
    ch->IoRetries = 0;
    ch->MaximumIoRetries = 4;
    ch->StartPacketBusy = TRUE;
    dev->DriverObject->DriverStartIo(dev, Irp);
    ch->StartPacketBusy = FALSE;
}

// Called at DISPATCH_LEVEL after an IRP finished.
void NTAPI IdexChannelStartNextPacket() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    if (ch->StartPacketBusy) {
        ch->StartPacketRequested = TRUE;
        return;
    }
    do {
        ch->StartPacketRequested = FALSE;
        PKDEVICE_QUEUE_ENTRY e = KeRemoveDeviceQueue(&ch->DeviceQueue);
        if (!e) {
            ch->CurrentIrp = nullptr;
            return;
        }
        IdexStartIo(CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry));
    } while (ch->StartPacketRequested);
}

// Both drives share one channel: requests from either device are serialised here.
void NTAPI IdexChannelStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    XK_MARK("mark: IDE start packet");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (!KeInsertDeviceQueue(&ch->DeviceQueue, &Irp->Tail.Overlay.DeviceQueueEntry)) {
        IdexStartIo(Irp);
        if (ch->StartPacketRequested)
            IdexChannelStartNextPacket();
    }
    KfLowerIrql(old);
}

void IdexChannelInit() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    ch->StartPacketRoutine = IdexChannelStartPacket;
    ch->StartNextPacketRoutine = IdexChannelStartNextPacket;
    KeInitializeDeviceQueue(&ch->DeviceQueue);
    KeInitializeDpc(&ch->TimerDpc, IdexTimerDpc, nullptr);
    KeInitializeTimerEx(&ch->Timer, SynchronizationTimer);
    LARGE_INTEGER due;
    due.QuadPart = -1000 * 10000LL;
    KeSetTimerEx(&ch->Timer, due, 1000, &ch->TimerDpc);
    KeInitializeDpc(&ch->FinishDpc, IdexFinishDpc, nullptr);
    outb(IDE_CONTROL, 0); // interrupts on
    ULONG vector = HalGetInterruptVector(14, &ch->InterruptIrql);
    KeInitializeInterrupt(&ch->InterruptObject, IdexInterruptService, nullptr, vector, ch->InterruptIrql, Latched,
                          FALSE);
    if (!KeConnectInterrupt(&ch->InterruptObject))
        KeBugCheckEx(0x2066B, (ULONG_PTR)(NTSTATUS)0xC0000001, 0, 0, 0);
    ch->PhysicalRegionDescriptorTablePhysical = MmGetPhysicalAddress(IdexPrdTable);
    IdexDiskInit();
    IdexCdRomInit();
    void VdInitialize(); // vd/vdisc.cpp: disc images (the control device)
    VdInitialize();
}

// Software reset of both devices (SRST through the device control register), then wait for the master.
void IdexChannelReset() {
    outb(IDE_CONTROL, 0);
    KeStallExecutionProcessor(50);
    outb(IDE_CONTROL, 4);
    KeStallExecutionProcessor(100);
    outb(IDE_CONTROL, 0);
    KeStallExecutionProcessor(50);
    IdexWaitForStatus(IDE_STATUS_BSY, 0, 25000000);
}
