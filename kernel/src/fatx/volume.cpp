// FATX: block cache, FAT access, cluster allocation, mount, names and timestamps (spec/04 §6, spec/04b).
#include "fatx.h"

static KEVENT FatxLock;
// Blocks mapped under the lock; unmapped when it is released (the oldest earlier if many are mapped).
constexpr ULONG FATX_MAX_HELD = 12;
static struct {
    PVOID Buffer;
    FATX_VOLUME *Volume;
    LONGLONG Block;
} FatxHeld[FATX_MAX_HELD];
static ULONG FatxHeldCount;

void FatxInitialize() {
    KeInitializeEvent(&FatxLock, SynchronizationEvent, TRUE);
    FatxHeldCount = 0;
}

void FatxAcquireVolume(FATX_VOLUME *) { KeWaitForSingleObject(&FatxLock, Executive, KernelMode, FALSE, nullptr); }
void FatxReleaseVolume(FATX_VOLUME *) {
    while (FatxHeldCount)
        FscUnmapBuffer(FatxHeld[--FatxHeldCount].Buffer);
    KeSetEvent(&FatxLock, 0, FALSE);
}

NTSTATUS FatxDeviceIo(FATX_VOLUME *v, UCHAR Major, LONGLONG Offset, PVOID Buffer, ULONG Length) {
    LARGE_INTEGER o;
    o.QuadPart = Offset;
    return IoSynchronousFsdRequest(Major, v->TargetDevice, Buffer, Length, &o);
}

// --- Blocks (4 KB, through the file system cache; written through, never dirty). The lock is held.

NTSTATUS FatxMapBlock(FATX_VOLUME *v, LONGLONG Offset, PUCHAR *Data) {
    LONGLONG block = Offset & ~(LONGLONG)(FATX_BLOCK_SIZE - 1);
    for (ULONG i = FatxHeldCount; i--;) // already held under this lock (FAT scans map the same block repeatedly)
        if (FatxHeld[i].Volume == v && FatxHeld[i].Block == block) {
            *Data = (PUCHAR)FatxHeld[i].Buffer + (ULONG)(Offset - block);
            return STATUS_SUCCESS;
        }
    PVOID p;
    NTSTATUS status = FscMapBuffer(&v->CacheExtension, block, TRUE, &p);
    if (!NT_SUCCESS(status))
        return status;
    if (FatxHeldCount == FATX_MAX_HELD) {
        FscUnmapBuffer(FatxHeld[0].Buffer);
        RtlMoveMemory(FatxHeld, FatxHeld + 1, (FATX_MAX_HELD - 1) * sizeof(FatxHeld[0]));
        FatxHeldCount--;
    }
    FatxHeld[FatxHeldCount++] = {p, v, block};
    *Data = (PUCHAR)p + (ULONG)(Offset - block);
    return STATUS_SUCCESS;
}

// Write back the sectors [Start, Start+Length) of a mapped block.
NTSTATUS FatxWriteBlockRange(FATX_VOLUME *v, LONGLONG BlockOffset, ULONG Start, ULONG Length) {
    PUCHAR data;
    NTSTATUS status = FatxMapBlock(v, BlockOffset, &data);
    if (!NT_SUCCESS(status))
        return status;
    ULONG mask = v->SectorSize - 1, first = Start & ~mask, end = (Start + Length + mask) & ~mask;
    return FatxDeviceIo(v, IRP_MJ_WRITE, BlockOffset + first, data + first, end - first);
}

void FatxInvalidateRange(FATX_VOLUME *v, LONGLONG Offset, ULONG Length) {
    for (ULONG i = 0; i < FatxHeldCount; i++) // held blocks of the range are not reused either
        if (FatxHeld[i].Volume == v && FatxHeld[i].Block + FATX_BLOCK_SIZE > Offset && FatxHeld[i].Block < Offset + Length)
            FatxHeld[i].Volume = nullptr;
    FscInvalidateByteRange(&v->CacheExtension, Offset, Length);
}

// The volume is going away: its blocks must not match a later volume allocated at the same address.
void FatxForgetVolume(FATX_VOLUME *v) { FscInvalidateDevice(&v->CacheExtension); }

// --- FAT

bool FatxIsValidCluster(FATX_VOLUME *v, ULONG Cluster) { return Cluster >= 1 && Cluster <= v->NumberOfClusters; }

LONGLONG FatxClusterOffset(FATX_VOLUME *v, ULONG Cluster) {
    return v->DataByteOffset + ((LONGLONG)(Cluster - 1) << v->ClusterShift);
}

static void FatxFatLocation(FATX_VOLUME *v, ULONG Cluster, LONGLONG *Block, ULONG *Within) {
    ULONG byte = Cluster << v->FatEntryShift;
    *Block = v->FatByteOffset + (byte & ~(FATX_BLOCK_SIZE - 1));
    *Within = byte & (FATX_BLOCK_SIZE - 1);
}

NTSTATUS FatxGetFatEntry(FATX_VOLUME *v, ULONG Cluster, ULONG *Value) {
    LONGLONG block;
    ULONG within;
    FatxFatLocation(v, Cluster, &block, &within);
    PUCHAR data;
    NTSTATUS status = FatxMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    if (v->FatEntryShift == 1) {
        ULONG e = *(USHORT *)(data + within);
        *Value = e >= 0xFFF0 ? e | 0xFFFF0000 : e; // special values keep their FATX32 meaning
    } else {
        *Value = *(ULONG *)(data + within);
    }
    return STATUS_SUCCESS;
}

NTSTATUS FatxSetFatEntry(FATX_VOLUME *v, ULONG Cluster, ULONG Value) {
    LONGLONG block;
    ULONG within;
    FatxFatLocation(v, Cluster, &block, &within);
    PUCHAR data;
    NTSTATUS status = FatxMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    if (v->FatEntryShift == 1)
        *(USHORT *)(data + within) = (USHORT)Value;
    else
        *(ULONG *)(data + within) = Value;
    return FatxWriteBlockRange(v, block, within, 1u << v->FatEntryShift);
}

static bool FatxIsLast(ULONG e) { return e >= 0xFFFFFFF8; }

// Allocate Count clusters chained together; link them after PreviousLast (0 = new chain).
NTSTATUS FatxAllocateClusters(FATX_VOLUME *v, ULONG PreviousLast, ULONG Count, ULONG *First) {
    if (Count > v->FreeClusters)
        return (NTSTATUS)0xC000007F; // STATUS_DISK_FULL
    ULONG first = 0, prev = PreviousLast, c = v->AllocationHint;
    for (ULONG n = 0; n < Count;) {
        if (!FatxIsValidCluster(v, c))
            c = 1;
        ULONG e;
        NTSTATUS status = FatxGetFatEntry(v, c, &e);
        if (!NT_SUCCESS(status))
            return status;
        if (e == FATX_CLUSTER_FREE) {
            status = FatxSetFatEntry(v, c, FATX_CLUSTER_LAST);
            if (NT_SUCCESS(status) && prev)
                status = FatxSetFatEntry(v, prev, c);
            if (!NT_SUCCESS(status))
                return status;
            if (!first)
                first = c;
            prev = c;
            v->FreeClusters--;
            n++;
        }
        c++;
    }
    v->AllocationHint = c;
    *First = first;
    return STATUS_SUCCESS;
}

NTSTATUS FatxFreeChain(FATX_VOLUME *v, ULONG First) {
    ULONG c = First;
    for (ULONG guard = 0; FatxIsValidCluster(v, c) && guard <= v->NumberOfClusters; guard++) {
        ULONG next;
        NTSTATUS status = FatxGetFatEntry(v, c, &next);
        if (!NT_SUCCESS(status))
            return status;
        status = FatxSetFatEntry(v, c, FATX_CLUSTER_FREE);
        if (!NT_SUCCESS(status))
            return status;
        v->FreeClusters++;
        if (c < v->AllocationHint)
            v->AllocationHint = c;
        if (FatxIsLast(next))
            break;
        c = next;
    }
    return STATUS_SUCCESS;
}

NTSTATUS FatxChainLength(FATX_VOLUME *v, ULONG First, ULONG *Count, ULONG *Last) {
    ULONG n = 0, c = First, last = 0;
    while (FatxIsValidCluster(v, c)) {
        n++;
        last = c;
        if (n > v->NumberOfClusters)
            return (NTSTATUS)0xC0000032; // STATUS_DISK_CORRUPT_ERROR
        NTSTATUS status = FatxGetFatEntry(v, c, &c);
        if (!NT_SUCCESS(status))
            return status;
    }
    *Count = n;
    *Last = last;
    return STATUS_SUCCESS;
}

// Cluster number of the Index-th cluster of a file; STATUS_END_OF_FILE past the chain.
NTSTATUS FatxClusterAtIndex(FATX_VOLUME *v, FATX_FCB *f, ULONG Index, ULONG *Cluster) {
    ULONG i = 0, c = f->FirstCluster;
    if (f->HintCluster && f->HintClusterIndex <= Index) {
        i = f->HintClusterIndex;
        c = f->HintCluster;
    }
    while (i < Index) {
        if (!FatxIsValidCluster(v, c))
            return STATUS_END_OF_FILE;
        NTSTATUS status = FatxGetFatEntry(v, c, &c);
        if (!NT_SUCCESS(status))
            return status;
        i++;
    }
    if (!FatxIsValidCluster(v, c))
        return STATUS_END_OF_FILE;
    f->HintClusterIndex = Index;
    f->HintCluster = c;
    *Cluster = c;
    return STATUS_SUCCESS;
}

// --- Names and timestamps

bool FatxIsValidName(const OBJECT_STRING *Name) {
    if (Name->Length < 1 || Name->Length > FATX_MAX_NAME)
        return false;
    if (Name->Buffer[0] == '.' && (Name->Length == 1 || (Name->Length == 2 && Name->Buffer[1] == '.')))
        return false;
    for (ULONG i = 0; i < Name->Length; i++) {
        UCHAR c = (UCHAR)Name->Buffer[i];
        if (c < 0x20 || c > 0x7E)
            return false;
        for (const char *bad = "\"*/:<>?\\|"; *bad; bad++)
            if (c == (UCHAR)*bad)
                return false;
    }
    return true;
}

static CHAR FatxUpper(CHAR c) { return c >= 'a' && c <= 'z' ? (CHAR)(c - 0x20) : c; }

bool FatxNameEqual(const CHAR *a, ULONG alen, const CHAR *b, ULONG blen) {
    if (alen != blen)
        return false;
    for (ULONG i = 0; i < alen; i++)
        if (FatxUpper(a[i]) != FatxUpper(b[i]))
            return false;
    return true;
}

bool FatxWildcardMatch(const CHAR *mask, ULONG mlen, const CHAR *name, ULONG nlen) {
    if (!mlen)
        return nlen == 0;
    if (*mask == '*') {
        for (ULONG skip = 0; skip <= nlen; skip++)
            if (FatxWildcardMatch(mask + 1, mlen - 1, name + skip, nlen - skip))
                return true;
        return false;
    }
    if (!nlen)
        return false;
    if (*mask != '?' && FatxUpper(*mask) != FatxUpper(*name))
        return false;
    return FatxWildcardMatch(mask + 1, mlen - 1, name + 1, nlen - 1);
}

// FAT-style stamp: date (high word: day, month, year-2000) and time (low word: seconds/2, minute, hour).
ULONG FatxTimeToTimestamp(const LARGE_INTEGER *Time) {
    TIME_FIELDS f;
    LARGE_INTEGER t = *Time;
    RtlTimeToTimeFields(&t, &f);
    if (f.Year < 2000)
        return (1 << 5 | 1) << 16; // 2000-01-01
    ULONG date = ((ULONG)(f.Year - 2000) << 9) | ((ULONG)f.Month << 5) | (ULONG)f.Day;
    ULONG time = ((ULONG)f.Hour << 11) | ((ULONG)f.Minute << 5) | ((ULONG)f.Second >> 1);
    return (date << 16) | time;
}

void FatxTimestampToTime(ULONG Stamp, PLARGE_INTEGER Time) {
    TIME_FIELDS f = {};
    f.Year = (SHORT)(2000 + (Stamp >> 25));
    f.Month = (SHORT)((Stamp >> 21) & 15);
    f.Day = (SHORT)((Stamp >> 16) & 31);
    f.Hour = (SHORT)((Stamp >> 11) & 31);
    f.Minute = (SHORT)((Stamp >> 5) & 63);
    f.Second = (SHORT)((Stamp & 31) * 2);
    if (!RtlTimeFieldsToTime(&f, Time))
        Time->QuadPart = 0;
}

ULONG FatxCurrentTimestamp() {
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    return FatxTimeToTimestamp(&now);
}

// --- Mount (spec/04 §6)

static void FatxInitFcb(FATX_FCB *f, ULONG Flags) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->SiblingLink);
    InitializeListHead(&f->ChildList);
    f->Flags = Flags;
    f->ReferenceCount = 1; // permanent
}

NTSTATUS FatxMountVolume(PDEVICE_OBJECT TargetDevice) {
    DISK_GEOMETRY geometry;
    PARTITION_INFORMATION partition;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, TargetDevice, nullptr, 0,
                                                          &geometry, sizeof(geometry), nullptr, FALSE);
    if (NT_SUCCESS(status))
        status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, TargetDevice, nullptr, 0,
                                                     &partition, sizeof(partition), nullptr, FALSE);
    if (!NT_SUCCESS(status))
        return status;

    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&FatxDriverObject, sizeof(FATX_VOLUME), nullptr, 8 /*FILE_DEVICE_DISK_FILE_SYSTEM*/,
                            FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->StackSize = (CHAR)(dev->StackSize + TargetDevice->StackSize);
    if (dev->AlignmentRequirement < TargetDevice->AlignmentRequirement)
        dev->AlignmentRequirement = TargetDevice->AlignmentRequirement;
    dev->Flags |= TargetDevice->Flags & 0x40;
    dev->SectorSize = geometry.BytesPerSector;
    FATX_VOLUME *v = FatxVolume(dev);
    v->TargetDevice = TargetDevice;
    v->PartitionLength = partition.PartitionLength.QuadPart;
    v->SectorSize = geometry.BytesPerSector;
    v->CacheExtension = {TargetDevice, partition.PartitionLength, geometry.BytesPerSector};
    FatxInitFcb(&v->RootFcb, FCB_DIRECTORY | FCB_ROOT);
    FatxInitFcb(&v->VolumeFcb, FCB_VOLUME);
    v->RootFcb.Attributes = FILE_ATTRIBUTE_DIRECTORY;

    // Reserved area / FAT alignment: max(4 KB, sector size rounded up to 4 KB).
    ULONG align = (v->SectorSize + 0xFFF) & ~0xFFFu;
    ULONG reserved = align > 0x1000 ? align : 0x1000;
    status = STATUS_UNRECOGNIZED_VOLUME;
    PUCHAR boot;
    FatxAcquireVolume(v); // the block cache is shared
    if (v->PartitionLength > reserved && NT_SUCCESS(FatxMapBlock(v, 0, &boot)) &&
        *(ULONG *)boot == FATX_SIGNATURE) {
        ULONG spc = *(ULONG *)(boot + 8), root = *(ULONG *)(boot + 0xC);
        v->SerialNumber = *(ULONG *)(boot + 4);
        bool spcOk = spc && spc <= 128 && !(spc & (spc - 1));
        ULONG sectorShift = 0;
        while ((1u << sectorShift) < v->SectorSize)
            sectorShift++;
        v->BytesPerCluster = spcOk ? spc << sectorShift : 0;
        while (spcOk && (1u << v->ClusterShift) < v->BytesPerCluster)
            v->ClusterShift++;
        if (spcOk && v->BytesPerCluster >= align) {
            ULONG entries = (ULONG)(v->PartitionLength >> v->ClusterShift) + 1;
            v->FatEntryShift = entries < 0xFFF0 ? 1 : 2;
            ULONG fatSize = ((entries << v->FatEntryShift) + align - 1) & ~(align - 1);
            if ((LONGLONG)fatSize < v->PartitionLength - reserved) {
                v->FatByteOffset = reserved;
                v->DataByteOffset = reserved + fatSize;
                v->NumberOfClusters = (ULONG)((v->PartitionLength - reserved - fatSize) >> v->ClusterShift);
                v->RootFcb.FirstCluster = root;
                if (FatxIsValidCluster(v, root))
                    status = STATUS_SUCCESS;
            }
        }
    }
    // Count free clusters (FileFsSizeInformation) and find the first free one.
    for (ULONG c = 1; NT_SUCCESS(status) && c <= v->NumberOfClusters; c++) {
        ULONG e;
        status = FatxGetFatEntry(v, c, &e);
        if (NT_SUCCESS(status) && e == FATX_CLUSTER_FREE) {
            if (!v->FreeClusters)
                v->AllocationHint = c;
            v->FreeClusters++;
        }
    }
    FatxReleaseVolume(v);
    if (!NT_SUCCESS(status)) {
        FatxForgetVolume(v);
        IoDeleteDevice(dev);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    ObfReferenceObject(TargetDevice);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}

NTSTATUS XdvdfsMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS UdfMountVolume(PDEVICE_OBJECT TargetDevice);

// CD-ROM: XDVDFS first, then UDF (DVD-video) when XDVDFS does not recognise the media (spec/04c §2, spec/04d).
static NTSTATUS IopMountCdRom(PDEVICE_OBJECT TargetDevice) {
    NTSTATUS status = XdvdfsMountVolume(TargetDevice);
    return status == STATUS_UNRECOGNIZED_VOLUME ? UdfMountVolume(TargetDevice) : status;
}
NTSTATUS RawMountVolume(PDEVICE_OBJECT TargetDevice);

void FsInitSystem() {
    IopFileSystems.Disk = FatxMountVolume;
    IopFileSystems.CdRom = IopMountCdRom;
    IopFileSystems.Raw = RawMountVolume;
}
