// IDE channel: interrupt, completion DPC, timeout timer, packet serialisation, bus-master DMA (spec/04 §1, §5).
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
bool IdexProgramDma(PVOID Buffer, ULONG Length) {
    ULONG va = (ULONG)Buffer, n = 0;
    while (Length) {
        ULONG phys = MmGetPhysicalAddress((PVOID)va);
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
}
