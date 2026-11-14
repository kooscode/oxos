// IDE channel: interrupt, completion DPC, timeout timer, packet serialisation, bus-master DMA (spec/stock/04 §1, §5).
#include <xk/storage.h>
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/hardware.h>
#include <xk/memory.h>

IDE_CHANNEL_OBJECT IdexChannelObject; // exported (ordinal 357), outside the namespace

namespace xk::storage {

static volatile UCHAR LastAtaStatus, LastBusMasterStatus; // at the last interrupt (diagnostics)

// Poll the alternate status until (status & mask) == value, for at most timeoutUs microseconds.
bool WaitForIdeStatus(UCHAR mask, UCHAR value, ULONG timeoutUs) {
    for (ULONG i = 0; i < timeoutUs; i += 10) {
        if ((inb(IDE_ALT_STATUS) & mask) == value)
            return true;
        KeStallExecutionProcessor(10);
    }
    return (inb(IDE_ALT_STATUS) & mask) == value;
}

// --- Bus-master DMA

// Physical region descriptor table: 33 entries cover 128 KB starting at any page offset. 512-byte alignment keeps
// it inside one 64 KB region as the bus master requires.
struct IDE_PRD {
    ULONG Physical;
    ULONG Count; // bytes in bits 0-15 (0 = 64 KB), bit 31 = end of table
};
alignas(512) static IDE_PRD PrdTable[33];

volatile ULONG BadDmaPages; // diagnostics: transfers refused because a page was not valid RAM

// The pages of the chunk being transferred stay where they are until it is over: an unlocked page can be moved by a
// contiguous allocation (memory::RelocatePage), and the bus master would then write into its old frame, which by then
// belongs to someone else (a hardware transfer takes milliseconds; xemu finishes it at once).
static PVOID LockedDmaBuffer;
static ULONG LockedDmaLength;

// The pages of the last transfer back to the memory manager.
void UnlockDmaBuffer() {
    if (LockedDmaBuffer) {
        MmLockUnlockBufferPages(LockedDmaBuffer, LockedDmaLength, TRUE);
        LockedDmaBuffer = nullptr;
    }
}

// Describe Buffer to the bus master; merges physically contiguous pages, never crosses a 64 KB boundary.
static bool ProgramDmaPages(PVOID Buffer, ULONG Length) {
    ULONG va = (ULONG)Buffer, n = 0;
    while (Length) {
        // Real hardware hangs the bus on a transfer to an address that is not RAM (xemu ignores it): check each
        // page is mapped and inside the RAM (64 or 128 MB) before the engine sees it.
        if (!MmIsAddressValid((PVOID)va)) {
            BadDmaPages = BadDmaPages + 1;
            XK_SERIAL("*** ide: DMA buffer %p (%lu bytes left) is not mapped; transfer refused\n", (PVOID)va, Length);
            return false;
        }
        ULONG phys = MmGetPhysicalAddress((PVOID)va);
        if ((phys >> PAGE_SHIFT) > memory::HighestPhysicalPage || (phys & 1)) {
            BadDmaPages = BadDmaPages + 1;
            XK_SERIAL("*** ide: DMA buffer %p -> physical %08lx is not RAM; transfer refused\n", (PVOID)va, phys);
            return false;
        }
        ULONG piece = 0x1000 - (va & 0xFFF);
        if (piece > Length)
            piece = Length;
        if (n && PrdTable[n - 1].Physical + (PrdTable[n - 1].Count & 0x1FFFF) == phys &&
            ((phys ^ PrdTable[n - 1].Physical) & 0xFFFF0000) == 0 &&
            (PrdTable[n - 1].Count & 0x1FFFF) + piece <= 0x10000) {
            PrdTable[n - 1].Count += piece;
        } else {
            if (n == 33)
                return false;
            PrdTable[n].Physical = phys;
            PrdTable[n].Count = piece;
            n++;
        }
        va += piece;
        Length -= piece;
    }
    for (ULONG i = 0; i < n; i++)
        PrdTable[i].Count &= 0xFFFF; // 0x10000 encodes as 0
    PrdTable[n - 1].Count |= 0x80000000;
    outb(IDE_BM_COMMAND, 0);
    outb(IDE_BM_STATUS, inb(IDE_BM_STATUS) | 6); // clear interrupt and error
    outl(IDE_BM_PRDT, IdexChannelObject.PhysicalRegionDescriptorTablePhysical);
    return true;
}

// Lock Buffer's pages and describe them to the bus master; false (nothing left locked) when a page is not RAM.
bool ProgramIdeDma(PVOID Buffer, ULONG Length) {
    UnlockDmaBuffer();
    MmLockUnlockBufferPages(Buffer, Length, FALSE);
    LockedDmaBuffer = Buffer;
    LockedDmaLength = Length;
    if (ProgramDmaPages(Buffer, Length))
        return true;
    UnlockDmaBuffer();
    return false;
}

static BOOLEAN NTAPI IdeInterruptService(PKINTERRUPT, PVOID) {
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
    LastBusMasterStatus = bm;
    LastAtaStatus = inb(IDE_STATUS);
    ch->ExpectingBusMasterInterrupt = FALSE;
    KeInsertQueueDpc(&ch->FinishDpc, nullptr, nullptr);
    return TRUE;
}

static void NTAPI IdeFinishDpc(PKDPC, PVOID, PVOID, PVOID) {
    XK_MARK("mark: IDE finish DPC");
    UnlockDmaBuffer(); // the chunk is done (or abandoned)
    if (IdexChannelObject.FinishIoRoutine)
        IdexChannelObject.FinishIoRoutine();
}

// Once a second: a command that has not interrupted within Timeout seconds is abandoned.
static void NTAPI IdeTimerDpc(PKDPC, PVOID, PVOID, PVOID) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    bool expired = ch->ExpectingBusMasterInterrupt && --ch->Timeout == 0;
    if (expired) {
        ch->ExpectingBusMasterInterrupt = FALSE;
        outb(IDE_BM_COMMAND, 0);
        LastBusMasterStatus = 2; // reported as an error
        LastAtaStatus = IDE_STATUS_ERR;
    }
    KfLowerIrql(old);
    if (expired && ch->TimeoutExpiredRoutine)
        ch->TimeoutExpiredRoutine();
}

UCHAR IdeCompletionStatus(PUCHAR BusMasterStatus) {
    *BusMasterStatus = LastBusMasterStatus;
    return LastAtaStatus;
}

static void IdeStartIo(PIRP Irp) {
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
void NTAPI StartNextIdePacket() {
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
        IdeStartIo(CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry));
    } while (ch->StartPacketRequested);
}

// Both drives share one channel: requests from either device are serialised here.
void NTAPI StartIdePacket(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    XK_MARK("mark: IDE start packet");
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (!KeInsertDeviceQueue(&ch->DeviceQueue, &Irp->Tail.Overlay.DeviceQueueEntry)) {
        IdeStartIo(Irp);
        if (ch->StartPacketRequested)
            StartNextIdePacket();
    }
    KfLowerIrql(old);
}

void InitIdeChannel() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    ch->StartPacketRoutine = StartIdePacket;
    ch->StartNextPacketRoutine = StartNextIdePacket;
    KeInitializeDeviceQueue(&ch->DeviceQueue);
    KeInitializeDpc(&ch->TimerDpc, IdeTimerDpc, nullptr);
    KeInitializeTimerEx(&ch->Timer, SynchronizationTimer);
    LARGE_INTEGER due;
    due.QuadPart = -1000 * 10000LL;
    KeSetTimerEx(&ch->Timer, due, 1000, &ch->TimerDpc);
    KeInitializeDpc(&ch->FinishDpc, IdeFinishDpc, nullptr);
    outb(IDE_CONTROL, 0); // interrupts on
    ULONG vector = HalGetInterruptVector(14, &ch->InterruptIrql);
    KeInitializeInterrupt(&ch->InterruptObject, IdeInterruptService, nullptr, vector, ch->InterruptIrql, Latched,
                          FALSE);
    if (!KeConnectInterrupt(&ch->InterruptObject))
        KeBugCheckEx(0x2066B, (ULONG_PTR)(NTSTATUS)0xC0000001, 0, 0, 0);
    ch->PhysicalRegionDescriptorTablePhysical = MmGetPhysicalAddress(PrdTable);
    InitDisk();
    InitDvdDrive();
    InitVirtualDiscs();
}

// Software reset of both devices (SRST through the device control register), then wait for the master.
void ResetIdeChannel() {
    outb(IDE_CONTROL, 0);
    KeStallExecutionProcessor(50);
    outb(IDE_CONTROL, 4);
    KeStallExecutionProcessor(100);
    outb(IDE_CONTROL, 0);
    KeStallExecutionProcessor(50);
    WaitForIdeStatus(IDE_STATUS_BSY, 0, 25000000);
}

} // namespace xk::storage
