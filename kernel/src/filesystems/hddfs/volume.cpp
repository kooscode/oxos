// The volume layer both formats share: the lock, the block cache, FAT access, cluster allocation, file sizes, names
// and timestamps (spec/stock/04 §6, spec/stock/04b).
#include <xk/hddfs.h>
#include <xk/filesystems.h>
#include <xk/serial.h>
#include <xk/io.h>

static KEVENT HDDFSLock;
// Blocks mapped under the lock; unmapped when it is released (the oldest earlier if many are mapped).
constexpr ULONG HDDFS_MAX_HELD = 12;
static struct {
    PVOID Buffer;
    HDDFS_VOLUME *Volume;
    LONGLONG Block;
} HDDFSHeld[HDDFS_MAX_HELD];
static ULONG HDDFSHeldCount;

void HDDFSInitialize() {
    KeInitializeEvent(&HDDFSLock, SynchronizationEvent, TRUE);
    HDDFSHeldCount = 0;
}

void HDDFSAcquireVolume(HDDFS_VOLUME *) {
    KeWaitForSingleObject(&HDDFSLock, Executive, KernelMode, FALSE, nullptr);
}

void HDDFSReleaseVolume(HDDFS_VOLUME *) {
    while (HDDFSHeldCount)
        FscUnmapBuffer(HDDFSHeld[--HDDFSHeldCount].Buffer);
    KeSetEvent(&HDDFSLock, 0, FALSE);
}

NTSTATUS HDDFSDeviceIo(HDDFS_VOLUME *v, UCHAR Major, LONGLONG Offset, PVOID Buffer, ULONG Length) {
    LARGE_INTEGER o;
    o.QuadPart = Offset;
    return IoSynchronousFsdRequest(Major, v->TargetDevice, Buffer, Length, &o);
}

// --- Blocks (4 KB, through the file system cache; written through, never dirty). The lock is held.

NTSTATUS HDDFSMapBlock(HDDFS_VOLUME *v, LONGLONG Offset, PUCHAR *Data) {
    LONGLONG block = Offset & ~(LONGLONG)(HDDFS_BLOCK_SIZE - 1);
    for (ULONG i = HDDFSHeldCount; i--;) { // already held under this lock (FAT scans map the same block repeatedly)
        if (HDDFSHeld[i].Volume == v && HDDFSHeld[i].Block == block) {
            *Data = (PUCHAR)HDDFSHeld[i].Buffer + (ULONG)(Offset - block);
            return STATUS_SUCCESS;
        }
    }
    PVOID p;
    NTSTATUS status = FscMapBuffer(v->Cache, block, TRUE, &p);
    if (!NT_SUCCESS(status))
        return status;
    if (HDDFSHeldCount == HDDFS_MAX_HELD) {
        FscUnmapBuffer(HDDFSHeld[0].Buffer);
        RtlMoveMemory(HDDFSHeld, HDDFSHeld + 1, (HDDFS_MAX_HELD - 1) * sizeof(HDDFSHeld[0]));
        HDDFSHeldCount--;
    }
    HDDFSHeld[HDDFSHeldCount++] = {p, v, block};
    *Data = (PUCHAR)p + (ULONG)(Offset - block);
    return STATUS_SUCCESS;
}

// Write back the sectors [Start, Start+Length) of a mapped block.
NTSTATUS HDDFSWriteBlockRange(HDDFS_VOLUME *v, LONGLONG BlockOffset, ULONG Start, ULONG Length) {
    PUCHAR data;
    NTSTATUS status = HDDFSMapBlock(v, BlockOffset, &data);
    if (!NT_SUCCESS(status))
        return status;
    ULONG mask = v->SectorSize - 1, first = Start & ~mask, end = (Start + Length + mask) & ~mask;
    return HDDFSDeviceIo(v, IRP_MJ_WRITE, BlockOffset + first, data + first, end - first);
}

void HDDFSInvalidateRange(HDDFS_VOLUME *v, LONGLONG Offset, ULONG Length) {
    for (ULONG i = 0; i < HDDFSHeldCount; i++) { // held blocks of the range are not reused either
        if (HDDFSHeld[i].Volume == v && HDDFSHeld[i].Block + HDDFS_BLOCK_SIZE > Offset &&
            HDDFSHeld[i].Block < Offset + Length)
            HDDFSHeld[i].Volume = nullptr;
    }
    FscInvalidateByteRange(v->Cache, Offset, Length);
}

// The volume is going away: its blocks must not match a later volume allocated at the same address. An exFAT folder
// volume's key is the disk's, which outlives it (and its blocks stay valid: every write goes through).
void HDDFSForgetVolume(HDDFS_VOLUME *v) {
    if (v->Cache == &v->CacheExtension)
        FscInvalidateDevice(v->Cache);
}

// --- FAT

bool HDDFSIsValidCluster(HDDFS_VOLUME *v, ULONG Cluster) {
    return Cluster >= v->FirstClusterNumber && Cluster - v->FirstClusterNumber < v->NumberOfClusters;
}

LONGLONG HDDFSClusterOffset(HDDFS_VOLUME *v, ULONG Cluster) {
    return v->DataByteOffset + ((LONGLONG)(Cluster - v->FirstClusterNumber) << v->ClusterShift);
}

static void HDDFSFatLocation(HDDFS_VOLUME *v, ULONG Cluster, LONGLONG *Block, ULONG *Within) {
    ULONG byte = Cluster << v->FatEntryShift;
    *Block = v->FatByteOffset + (byte & ~(HDDFS_BLOCK_SIZE - 1));
    *Within = byte & (HDDFS_BLOCK_SIZE - 1);
}

NTSTATUS HDDFSGetFatEntry(HDDFS_VOLUME *v, ULONG Cluster, ULONG *Value) {
    LONGLONG block;
    ULONG within;
    HDDFSFatLocation(v, Cluster, &block, &within);
    PUCHAR data;
    NTSTATUS status = HDDFSMapBlock(v, block, &data);
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

NTSTATUS HDDFSSetFatEntry(HDDFS_VOLUME *v, ULONG Cluster, ULONG Value) {
    LONGLONG block;
    ULONG within;
    HDDFSFatLocation(v, Cluster, &block, &within);
    PUCHAR data;
    NTSTATUS status = HDDFSMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    if (v->FatEntryShift == 1)
        *(USHORT *)(data + within) = (USHORT)Value;
    else
        *(ULONG *)(data + within) = Value;
    return HDDFSWriteBlockRange(v, block, within, 1u << v->FatEntryShift);
}

static bool HDDFSIsLast(ULONG e) {
    return e >= 0xFFFFFFF8;
}

// Allocate Count clusters chained together; link them after PreviousLast (0 = new chain).
NTSTATUS HDDFSAllocateClusters(HDDFS_VOLUME *v, ULONG PreviousLast, ULONG Count, ULONG *First) {
    if (Count > v->Alloc->FreeClusters || (v->IsEXFAT && !EXFATMayAllocate(v, Count)))
        return (NTSTATUS)0xC000007F; // STATUS_DISK_FULL
    ULONG first = 0, prev = PreviousLast, c = v->Alloc->AllocationHint;
    for (ULONG n = 0; n < Count;) {
        if (!HDDFSIsValidCluster(v, c))
            c = v->FirstClusterNumber;
        ULONG e = 1;
        NTSTATUS status = STATUS_SUCCESS;
        bool free;
        if (v->IsEXFAT) {
            free = EXFATClusterFree(v, c, &status);
        } else {
            status = HDDFSGetFatEntry(v, c, &e);
            free = e == HDDFS_CLUSTER_FREE;
        }
        if (!NT_SUCCESS(status))
            return status;
        if (free) { // exFAT: the allocation bitmap says which clusters are free; the FAT links them
            if (v->IsEXFAT)
                status = EXFATMarkCluster(v, c, true);
            if (NT_SUCCESS(status))
                status = HDDFSSetFatEntry(v, c, HDDFS_CLUSTER_LAST);
            if (NT_SUCCESS(status) && prev)
                status = HDDFSSetFatEntry(v, prev, c);
            if (!NT_SUCCESS(status))
                return status;
            if (!first)
                first = c;
            prev = c;
            v->Alloc->FreeClusters--;
            v->UsedClusters++;
            n++;
        }
        c++;
    }
    v->Alloc->AllocationHint = c;
    *First = first;
    return STATUS_SUCCESS;
}

// Free a chain: at most Limit clusters (exFAT: the file's length decides where its chain ends).
NTSTATUS HDDFSFreeChain(HDDFS_VOLUME *v, ULONG First, ULONG Limit) {
    ULONG c = First;
    for (ULONG guard = 0; HDDFSIsValidCluster(v, c) && guard <= v->NumberOfClusters && guard < Limit; guard++) {
        ULONG next;
        NTSTATUS status = HDDFSGetFatEntry(v, c, &next);
        if (!NT_SUCCESS(status))
            return status;
        status = v->IsEXFAT ? EXFATMarkCluster(v, c, false) : HDDFSSetFatEntry(v, c, HDDFS_CLUSTER_FREE);
        if (!NT_SUCCESS(status))
            return status;
        v->Alloc->FreeClusters++;
        v->UsedClusters--;
        if (c < v->Alloc->AllocationHint)
            v->Alloc->AllocationHint = c;
        if (HDDFSIsLast(next))
            break;
        c = next;
    }
    return STATUS_SUCCESS;
}

NTSTATUS HDDFSChainLength(HDDFS_VOLUME *v, ULONG First, ULONG *Count, ULONG *Last) {
    ULONG n = 0, c = First, last = 0;
    while (HDDFSIsValidCluster(v, c)) {
        n++;
        last = c;
        if (n > v->NumberOfClusters)
            return (NTSTATUS)0xC0000032; // STATUS_DISK_CORRUPT_ERROR
        NTSTATUS status = HDDFSGetFatEntry(v, c, &c);
        if (!NT_SUCCESS(status))
            return status;
    }
    *Count = n;
    *Last = last;
    return STATUS_SUCCESS;
}

// exFAT: a file's or directory's allocation is its DataLength rounded up to clusters (EXFATAllocClusters); FAT entries
// past it mean nothing (another file may own those clusters now), so a chain is never followed beyond it. Only the
// root directory has no length: its chain ends at the end-of-chain mark, as on FATX.
static bool HDDFSChainIsBounded(HDDFS_VOLUME *v, const HDDFS_FCB *f) {
    return v->IsEXFAT && !(f->Flags & FCB_EXFAT_DISK_ROOT);
}

// The length of f's chain and its last cluster (Count = 0 for no allocation).
NTSTATUS HDDFSFileChain(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG *Count, ULONG *Last) {
    if (!f->FirstCluster) {
        *Count = *Last = 0;
        return STATUS_SUCCESS;
    }
    if (!HDDFSChainIsBounded(v, f))
        return HDDFSChainLength(v, f->FirstCluster, Count, Last);
    *Count = f->EXFATAllocClusters;
    if (!*Count) { // a first cluster without a length: owns nothing
        *Last = 0;
        return STATUS_SUCCESS;
    }
    NTSTATUS status = HDDFSClusterAtIndex(v, f, *Count - 1, Last);
    return status == STATUS_END_OF_FILE ? (NTSTATUS)0xC0000032 : status; // chain shorter than the length: corrupt
}

// Cluster number of the Index-th cluster of a file; STATUS_END_OF_FILE past the chain.
NTSTATUS HDDFSClusterAtIndex(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG Index, ULONG *Cluster) {
    if ((f->EXFATFlags & EXFAT_NOFATCHAIN) || HDDFSChainIsBounded(v, f)) {
        if (Index >= f->EXFATAllocClusters)
            return STATUS_END_OF_FILE;
    }
    if (f->EXFATFlags & EXFAT_NOFATCHAIN) { // consecutive clusters
        *Cluster = f->FirstCluster + Index;
        return STATUS_SUCCESS;
    }
    ULONG i = 0, c = f->FirstCluster;
    if (f->HintCluster && f->HintClusterIndex <= Index) {
        i = f->HintClusterIndex;
        c = f->HintCluster;
    }
    while (i < Index) {
        if (!HDDFSIsValidCluster(v, c))
            return STATUS_END_OF_FILE;
        NTSTATUS status = HDDFSGetFatEntry(v, c, &c);
        if (!NT_SUCCESS(status))
            return status;
        i++;
    }
    if (!HDDFSIsValidCluster(v, c))
        return STATUS_END_OF_FILE;
    f->HintClusterIndex = Index;
    f->HintCluster = c;
    *Cluster = c;
    return STATUS_SUCCESS;
}

// The cluster after Cluster in f's allocation (HDDFS_CLUSTER_LAST at the end).
NTSTATUS HDDFSNextCluster(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG Cluster, ULONG *Next) {
    if (f->EXFATFlags & EXFAT_NOFATCHAIN) {
        *Next = Cluster + 1 < f->FirstCluster + f->EXFATAllocClusters ? Cluster + 1 : HDDFS_CLUSTER_LAST;
        return STATUS_SUCCESS;
    }
    return HDDFSGetFatEntry(v, Cluster, Next);
}

// FileFsSizeInformation: total and free allocation units and sectors per unit.
void HDDFSVolumeSize(HDDFS_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit) {
    if (v->IsEXFAT)
        return EXFATVolumeSize(v, Total, Free, SectorsPerUnit);
    *Total = v->NumberOfClusters;
    *Free = v->Alloc->FreeClusters;
    *SectorsPerUnit = v->BytesPerCluster / v->SectorSize;
}

// --- Names and timestamps

bool HDDFSIsValidName(const OBJECT_STRING *Name) {
    if (Name->Length < 1 || Name->Length > FATX_MAX_NAME)
        return false;
    if (Name->Buffer[0] == '.' && (Name->Length == 1 || (Name->Length == 2 && Name->Buffer[1] == '.')))
        return false;
    for (ULONG i = 0; i < Name->Length; i++) {
        UCHAR c = (UCHAR)Name->Buffer[i];
        if (c < 0x20 || c > 0x7E)
            return false;
        for (const char *bad = "\"*/:<>?\\|"; *bad; bad++) {
            if (c == (UCHAR)*bad)
                return false;
        }
    }
    return true;
}

static CHAR HDDFSUpper(CHAR c) {
    return c >= 'a' && c <= 'z' ? (CHAR)(c - 0x20) : c;
}

bool HDDFSNameEqual(const CHAR *a, ULONG alen, const CHAR *b, ULONG blen) {
    if (alen != blen)
        return false;
    for (ULONG i = 0; i < alen; i++) {
        if (HDDFSUpper(a[i]) != HDDFSUpper(b[i]))
            return false;
    }
    return true;
}

bool HDDFSWildcardMatch(const CHAR *mask, ULONG mlen, const CHAR *name, ULONG nlen) {
    if (!mlen)
        return nlen == 0;
    if (*mask == '*') {
        for (ULONG skip = 0; skip <= nlen; skip++) {
            if (HDDFSWildcardMatch(mask + 1, mlen - 1, name + skip, nlen - skip))
                return true;
        }
        return false;
    }
    if (!nlen)
        return false;
    if (*mask != '?' && HDDFSUpper(*mask) != HDDFSUpper(*name))
        return false;
    return HDDFSWildcardMatch(mask + 1, mlen - 1, name + 1, nlen - 1);
}

// FAT-style stamp: date (high word: day, month, year-2000) and time (low word: seconds/2, minute, hour).
ULONG HDDFSTimeToTimestamp(const LARGE_INTEGER *Time) {
    TIME_FIELDS f;
    LARGE_INTEGER t = *Time;
    RtlTimeToTimeFields(&t, &f);
    if (f.Year < 2000)
        return (1 << 5 | 1) << 16; // 2000-01-01
    ULONG date = ((ULONG)(f.Year - 2000) << 9) | ((ULONG)f.Month << 5) | (ULONG)f.Day;
    ULONG time = ((ULONG)f.Hour << 11) | ((ULONG)f.Minute << 5) | ((ULONG)f.Second >> 1);
    return (date << 16) | time;
}

void HDDFSTimestampToTime(ULONG Stamp, PLARGE_INTEGER Time) {
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

ULONG HDDFSCurrentTimestamp() {
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    return HDDFSTimeToTimestamp(&now);
}

// --- File size

// Grow or shrink the cluster chain to cover NewSize bytes.
NTSTATUS HDDFSSetFileSize(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG NewSize) {
    if (f->EXFATFlags & EXFAT_NOFATCHAIN) { // exFAT: chain the clusters before the allocation changes
        NTSTATUS status = EXFATMakeChain(v, f);
        if (!NT_SUCCESS(status))
            return status;
    }
    ULONG need = (ULONG)(((ULONGLONG)NewSize + v->BytesPerCluster - 1) >> v->ClusterShift);
    ULONG have = 0, last = 0;
    NTSTATUS status = HDDFSFileChain(v, f, &have, &last);
    if (!NT_SUCCESS(status))
        return status;
    if (need > have) {
        ULONG first;
        status = HDDFSAllocateClusters(v, last, need - have, &first);
        if (!NT_SUCCESS(status))
            return status;
        if (!f->FirstCluster)
            f->FirstCluster = first;
    } else if (need < have) {
        f->HintCluster = 0;
        if (need == 0) {
            status = HDDFSFreeChain(v, f->FirstCluster, have);
            f->FirstCluster = 0;
        } else {
            ULONG keep;
            status = HDDFSClusterAtIndex(v, f, need - 1, &keep);
            ULONG next = 0;
            if (NT_SUCCESS(status))
                status = HDDFSGetFatEntry(v, keep, &next);
            if (NT_SUCCESS(status))
                status = HDDFSSetFatEntry(v, keep, HDDFS_CLUSTER_LAST);
            if (NT_SUCCESS(status))
                status = HDDFSFreeChain(v, next, have - need);
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    f->FileSize = NewSize;
    f->EXFATAllocClusters = need;
    f->Flags |= FCB_DIRENT_DIRTY;
    return HDDFSFlushFcb(v, f);
}
