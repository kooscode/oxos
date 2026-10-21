// exFAT: the folder partitions of an exFAT disk (spec/oxos-internal/07-exfat-driver.md, 05-exfat-storage-spec.md).
// Each folder partition mounts as its own volume whose root is that folder; they share one EXFAT_DISK (geometry,
// up-case table, free count, block cache key). The FATX dispatcher (fsd.cpp) serves them through the directory and
// cluster layers, which branch here for exFAT.
#include "fatx.h"

struct EXFAT_DISK {
    BOOLEAN Mounted;
    UCHAR ClusterShift;
    ULONG BytesPerCluster, ClusterCount, FatByteOffset, Serial;
    LONGLONG HeapByteOffset, BitmapByteOffset;
    FATX_ALLOC Alloc;
    USHORT Upcase[256];
    FATX_FCB Root; // the volume's root directory (it has no entry set)
};
static EXFAT_DISK ExfatDisk;

constexpr UCHAR EX_END = 0x00, EX_BITMAP = 0x81, EX_UPCASE = 0x82, EX_FILE = 0x85, EX_STREAM = 0xC0,
                EX_NAME = 0xC1;
constexpr ULONG EX_MAX_SET = 19; // file + stream + 17 name entries (255 characters)
constexpr NTSTATUS STATUS_FILE_CORRUPT_ERROR_ = (NTSTATUS)0xC0000102;

struct EX_SET {
    UCHAR e[EX_MAX_SET][32];
    ULONG Count;
};

// FATX timestamps count years from 2000, exFAT's from 1980; the other fields are the same.
static ULONG ExToFatxTime(ULONG t) { return (t >> 25) < 20 ? (1u << 5 | 1) << 16 : t - (20u << 25); }
static ULONG ExFromFatxTime(ULONG t) { return (t >> 25) > 107 ? t | 0xFE000000 : t + (20u << 25); }

static USHORT ExSetChecksum(const UCHAR *p, ULONG n) {
    USHORT sum = 0;
    for (ULONG i = 0; i < n; i++)
        if (i != 2 && i != 3)
            sum = (USHORT)(((sum >> 1) | (sum << 15)) + p[i]);
    return sum;
}

// --- Slots (32-byte directory entries)

static NTSTATUS ExMapSlot(FATX_VOLUME *v, FATX_FCB *dir, ULONG Slot, PUCHAR *Entry, LONGLONG *Block) {
    ULONGLONG byte = (ULONGLONG)Slot * 32;
    ULONG cluster;
    NTSTATUS status = FatxClusterAtIndex(v, dir, (ULONG)(byte >> v->ClusterShift), &cluster);
    if (!NT_SUCCESS(status))
        return status; // STATUS_END_OF_FILE past the directory's clusters
    ULONG within = (ULONG)byte & (v->BytesPerCluster - 1);
    LONGLONG block = FatxClusterOffset(v, cluster) + (within & ~(FATX_BLOCK_SIZE - 1));
    PUCHAR data;
    status = FatxMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    *Entry = data + (within & (FATX_BLOCK_SIZE - 1));
    *Block = block;
    return STATUS_SUCCESS;
}

static NTSTATUS ExWriteSlot(FATX_VOLUME *v, FATX_FCB *dir, ULONG Slot, const UCHAR *Data) {
    PUCHAR e, base;
    LONGLONG block;
    NTSTATUS status = ExMapSlot(v, dir, Slot, &e, &block);
    if (NT_SUCCESS(status))
        status = FatxMapBlock(v, block, &base);
    if (!NT_SUCCESS(status))
        return status;
    RtlCopyMemory(e, Data, 32);
    return FatxWriteBlockRange(v, block, (ULONG)(e - base), 32);
}

// The entry set whose file entry is at Slot, copied out.
static NTSTATUS ExReadSet(FATX_VOLUME *v, FATX_FCB *dir, ULONG Slot, EX_SET *Set) {
    PUCHAR e;
    LONGLONG block;
    NTSTATUS status = ExMapSlot(v, dir, Slot, &e, &block);
    if (!NT_SUCCESS(status))
        return status;
    Set->Count = 1u + e[1];
    if (e[0] != EX_FILE || Set->Count < 3 || Set->Count > EX_MAX_SET)
        return STATUS_FILE_CORRUPT_ERROR_;
    for (ULONG i = 0; i < Set->Count; i++) {
        status = ExMapSlot(v, dir, Slot + i, &e, &block);
        if (!NT_SUCCESS(status))
            return status == STATUS_END_OF_FILE ? STATUS_FILE_CORRUPT_ERROR_ : status;
        RtlCopyMemory(Set->e[i], e, 32);
    }
    if (Set->e[1][0] != EX_STREAM ||
        ExSetChecksum(Set->e[0], Set->Count * 32) != *(USHORT *)(Set->e[0] + 2))
        return STATUS_FILE_CORRUPT_ERROR_;
    return STATUS_SUCCESS;
}

static NTSTATUS ExWriteSet(FATX_VOLUME *v, FATX_FCB *dir, ULONG Slot, EX_SET *Set) {
    *(USHORT *)(Set->e[0] + 2) = ExSetChecksum(Set->e[0], Set->Count * 32);
    for (ULONG i = 0; i < Set->Count; i++) {
        NTSTATUS status = ExWriteSlot(v, dir, Slot + i, Set->e[i]);
        if (!NT_SUCCESS(status))
            return status;
    }
    return STATUS_SUCCESS;
}

// A set as titles see it: a FATX-valid name of at most 42 characters (all below 0x80, the FATX character set) and a
// size below 4 GB. Other files stay on the disk untouched and are invisible.
static bool ExEntryFromSet(FATX_VOLUME *v, const EX_SET *s, FATX_ENTRY *Out) {
    const UCHAR *f = s->e[0], *st = s->e[1];
    ULONG nameLength = st[3];
    if (nameLength < 1 || nameLength > FATX_MAX_NAME || 2 + (nameLength + 14) / 15 > s->Count)
        return false;
    USHORT attributes = *(const USHORT *)(f + 4);
    ULONGLONG length = *(const ULONGLONG *)(st + 24);
    if (length > 0xFFFFFFFFull)
        return false;
    RtlZeroMemory(Out, sizeof(*Out));
    for (ULONG i = 0; i < nameLength; i++) {
        const UCHAR *n = s->e[2 + i / 15];
        if (n[0] != EX_NAME)
            return false;
        USHORT c = *(const USHORT *)(n + 2 + 2 * (i % 15));
        if (c >= 0x80)
            return false;
        Out->FileName[i] = (CHAR)c;
    }
    OBJECT_STRING name = {(USHORT)nameLength, (USHORT)nameLength, Out->FileName};
    if (!FatxIsValidName(&name))
        return false;
    bool isDir = attributes & FILE_ATTRIBUTE_DIRECTORY;
    Out->FileNameLength = (UCHAR)nameLength;
    Out->FileAttributes = (UCHAR)(attributes & 0x37);
    Out->FirstCluster = *(const ULONG *)(st + 20);
    Out->FileSize = isDir ? 0 : (ULONG)length;
    Out->CreationTime = ExToFatxTime(*(const ULONG *)(f + 8));
    Out->LastWriteTime = ExToFatxTime(*(const ULONG *)(f + 12));
    Out->LastAccessTime = ExToFatxTime(*(const ULONG *)(f + 16));
    Out->ExEntries = (UCHAR)s->Count;
    if (Out->FirstCluster) {
        Out->ExFlags = (st[1] & 2) ? EXFAT_NOFATCHAIN : 0;
        Out->ExAllocClusters = (ULONG)((length + v->BytesPerCluster - 1) >> v->ClusterShift);
    }
    Out->ExDirLength = isDir ? (ULONG)length : 0;
    return true;
}

// A complete entry set for a name (file, stream, name entries); the checksum is filled in when it is written.
static void ExBuildSet(EX_SET *Set, const CHAR *Name, ULONG NameLength, UCHAR Attributes, ULONG Created,
                       ULONG Written, ULONG Accessed, ULONG First, ULONGLONG Length, bool NoFatChain) {
    ULONG names = (NameLength + 14) / 15;
    Set->Count = 2 + names;
    RtlZeroMemory(Set->e, Set->Count * 32);
    UCHAR *f = Set->e[0], *st = Set->e[1];
    f[0] = EX_FILE;
    f[1] = (UCHAR)(Set->Count - 1);
    *(USHORT *)(f + 4) = Attributes;
    *(ULONG *)(f + 8) = ExFromFatxTime(Created);
    *(ULONG *)(f + 12) = ExFromFatxTime(Written);
    *(ULONG *)(f + 16) = ExFromFatxTime(Accessed);
    st[0] = EX_STREAM;
    st[1] = 1 | (NoFatChain && First ? 2 : 0); // allocation possible, NoFatChain
    st[3] = (UCHAR)NameLength;
    USHORT hash = 0;
    for (ULONG i = 0; i < NameLength; i++) {
        USHORT c = ExfatDisk.Upcase[(UCHAR)Name[i]];
        hash = (USHORT)(((hash >> 1) | (hash << 15)) + (c & 0xFF));
        hash = (USHORT)(((hash >> 1) | (hash << 15)) + (c >> 8));
        UCHAR *n = Set->e[2 + i / 15];
        n[0] = EX_NAME;
        *(USHORT *)(n + 2 + 2 * (i % 15)) = (UCHAR)Name[i];
    }
    for (ULONG k = 0; k < names; k++)
        Set->e[2 + k][0] = EX_NAME;
    *(USHORT *)(st + 4) = hash;
    *(ULONGLONG *)(st + 8) = Length;  // ValidDataLength
    *(ULONG *)(st + 20) = First;
    *(ULONGLONG *)(st + 24) = Length; // DataLength
}

// --- Allocation bitmap

static NTSTATUS ExBitmapByte(FATX_VOLUME *v, ULONG Cluster, LONGLONG *Block, PUCHAR *Byte) {
    LONGLONG at = v->Ex->BitmapByteOffset + ((Cluster - 2) >> 3);
    *Block = at & ~(LONGLONG)(FATX_BLOCK_SIZE - 1);
    PUCHAR data;
    NTSTATUS status = FatxMapBlock(v, *Block, &data);
    if (NT_SUCCESS(status))
        *Byte = data + (ULONG)(at - *Block);
    return status;
}

bool ExfatClusterFree(FATX_VOLUME *v, ULONG Cluster, NTSTATUS *Status) {
    LONGLONG block;
    PUCHAR byte;
    *Status = ExBitmapByte(v, Cluster, &block, &byte);
    return NT_SUCCESS(*Status) && !(*byte & (1 << ((Cluster - 2) & 7)));
}

NTSTATUS ExfatMarkCluster(FATX_VOLUME *v, ULONG Cluster, bool Used) {
    LONGLONG block;
    PUCHAR byte, base;
    NTSTATUS status = ExBitmapByte(v, Cluster, &block, &byte);
    if (NT_SUCCESS(status))
        status = FatxMapBlock(v, block, &base);
    if (!NT_SUCCESS(status))
        return status;
    UCHAR bit = (UCHAR)(1 << ((Cluster - 2) & 7));
    *byte = Used ? *byte | bit : *byte & ~bit;
    return FatxWriteBlockRange(v, block, (ULONG)(byte - base), 1);
}

static NTSTATUS ExZeroCluster(FATX_VOLUME *v, ULONG Cluster) {
    LONGLONG base = FatxClusterOffset(v, Cluster);
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG off = 0; NT_SUCCESS(status) && off < v->BytesPerCluster; off += FATX_BLOCK_SIZE) {
        PUCHAR data;
        status = FatxMapBlock(v, base + off, &data);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(data, FATX_BLOCK_SIZE);
            status = FatxWriteBlockRange(v, base + off, 0, FATX_BLOCK_SIZE);
        }
    }
    return status;
}

// A NoFatChain allocation gets its FAT entries, so it can grow or shrink out of place.
NTSTATUS ExfatMakeChain(FATX_VOLUME *v, FATX_FCB *f) {
    if (!(f->ExFlags & EXFAT_NOFATCHAIN))
        return STATUS_SUCCESS;
    for (ULONG i = 0; i < f->ExAllocClusters; i++) {
        ULONG c = f->FirstCluster + i;
        NTSTATUS status = FatxSetFatEntry(v, c, i + 1 < f->ExAllocClusters ? c + 1 : FATX_CLUSTER_LAST);
        if (!NT_SUCCESS(status))
            return status;
    }
    f->ExFlags &= ~EXFAT_NOFATCHAIN;
    f->HintCluster = 0;
    f->Flags |= FCB_DIRENT_DIRTY;
    return STATUS_SUCCESS;
}

NTSTATUS ExfatFreeFile(FATX_VOLUME *v, FATX_FCB *f) {
    if (!f->FirstCluster)
        return STATUS_SUCCESS;
    if (!(f->ExFlags & EXFAT_NOFATCHAIN))
        return FatxFreeChain(v, f->FirstCluster);
    for (ULONG i = 0; i < f->ExAllocClusters; i++) {
        NTSTATUS status = ExfatMarkCluster(v, f->FirstCluster + i, false);
        if (!NT_SUCCESS(status))
            return status;
        v->Alloc->FreeClusters++;
        v->UsedClusters--;
    }
    if (f->FirstCluster < v->Alloc->AllocationHint)
        v->Alloc->AllocationHint = f->FirstCluster;
    return STATUS_SUCCESS;
}

NTSTATUS ExfatInitDirectory(FATX_VOLUME *v, ULONG *First) {
    NTSTATUS status = FatxAllocateClusters(v, 0, 1, First);
    if (NT_SUCCESS(status)) {
        status = ExZeroCluster(v, *First);
        if (!NT_SUCCESS(status)) {
            FatxFreeChain(v, *First);
            *First = 0;
        }
    }
    return status;
}

// The directory's entry set holds its length: write it after the directory grew.
static NTSTATUS ExGrowDirectory(FATX_VOLUME *v, FATX_FCB *dir) {
    ULONG cluster = 0;
    NTSTATUS status = STATUS_SUCCESS;
    if (dir->ExFlags & EXFAT_NOFATCHAIN) { // grow in place when the next cluster is free
        ULONG next = dir->FirstCluster + dir->ExAllocClusters;
        if (FatxIsValidCluster(v, next) && ExfatClusterFree(v, next, &status)) {
            status = ExfatMarkCluster(v, next, true);
            if (!NT_SUCCESS(status))
                return status;
            v->Alloc->FreeClusters--;
            v->UsedClusters++;
            cluster = next;
            dir->ExAllocClusters++;
        } else if (NT_SUCCESS(status)) {
            status = ExfatMakeChain(v, dir);
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    if (!cluster) {
        ULONG count, last;
        status = FatxChainLength(v, dir->FirstCluster, &count, &last);
        if (NT_SUCCESS(status))
            status = FatxAllocateClusters(v, last, 1, &cluster);
        if (!NT_SUCCESS(status))
            return status;
        dir->ExAllocClusters = count + 1;
    }
    status = ExZeroCluster(v, cluster);
    if (!NT_SUCCESS(status))
        return status;
    dir->ExDirLength += v->BytesPerCluster;
    dir->HintCluster = 0;
    return dir == &v->Ex->Root ? STATUS_SUCCESS : ExfatFlushFcb(v, dir);
}

// Need consecutive free slots (unused or deleted), growing the directory when its end has too few.
static NTSTATUS ExFindFree(FATX_VOLUME *v, FATX_FCB *dir, ULONG Need, ULONG *Slot) {
    ULONG run = 0, start = 0;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = ExMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE) {
            status = ExGrowDirectory(v, dir);
            if (NT_SUCCESS(status))
                status = ExMapSlot(v, dir, slot, &e, &block);
        }
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] & 0x80) {
            run = 0;
            continue;
        }
        if (!run)
            start = slot;
        if (++run == Need) {
            *Slot = start;
            return STATUS_SUCCESS;
        }
    }
}

// --- Directory layer

NTSTATUS ExfatNextEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG *Index, FATX_ENTRY *Entry) {
    for (ULONG slot = *Index;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = ExMapSlot(v, dir, slot, &e, &block);
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EX_END)
            return STATUS_END_OF_FILE;
        if (e[0] != EX_FILE)
            continue; // secondary, deleted or volume entries
        EX_SET set;
        status = ExReadSet(v, dir, slot, &set);
        if (NT_SUCCESS(status) && ExEntryFromSet(v, &set, Entry)) {
            *Index = slot;
            return STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(status) && status != STATUS_FILE_CORRUPT_ERROR_)
            return status;
    }
}

NTSTATUS ExfatLookup(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, FATX_ENTRY *Found, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        NTSTATUS status = ExfatNextEntry(v, dir, &i, Found);
        if (status == STATUS_END_OF_FILE)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(status))
            return status;
        if (FatxNameEqual(Found->FileName, Found->FileNameLength, Name->Buffer, Name->Length)) {
            *Index = i;
            return STATUS_SUCCESS;
        }
    }
}

NTSTATUS ExfatReadEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_ENTRY *Entry) {
    EX_SET set;
    NTSTATUS status = ExReadSet(v, dir, Index, &set);
    if (NT_SUCCESS(status) && !ExEntryFromSet(v, &set, Entry))
        status = STATUS_FILE_CORRUPT_ERROR_;
    return status;
}

NTSTATUS ExfatCreateDirent(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                           ULONG FirstCluster, ULONG *Index) {
    EX_SET set;
    ULONG now = FatxCurrentTimestamp();
    ULONGLONG length = (Attributes & FILE_ATTRIBUTE_DIRECTORY) && FirstCluster ? v->BytesPerCluster : 0;
    ExBuildSet(&set, Name->Buffer, Name->Length, Attributes, now, now, now, FirstCluster, length, false);
    NTSTATUS status = ExFindFree(v, dir, set.Count, Index);
    return NT_SUCCESS(status) ? ExWriteSet(v, dir, *Index, &set) : status;
}

// The directory whose entry set describes f (a folder partition's root is an entry of the volume's root).
static FATX_FCB *ExParentOf(FATX_VOLUME *v, FATX_FCB *f) { return (f->Flags & FCB_ROOT) ? &v->Ex->Root : f->Parent; }

NTSTATUS ExfatRemoveDirent(FATX_VOLUME *v, FATX_FCB *f) {
    FATX_FCB *parent = ExParentOf(v, f);
    EX_SET set;
    NTSTATUS status = ExReadSet(v, parent, f->DirentIndex, &set);
    if (!NT_SUCCESS(status))
        return status;
    for (ULONG i = 0; NT_SUCCESS(status) && i < set.Count; i++) {
        set.e[i][0] &= 0x7F; // not in use
        status = ExWriteSlot(v, parent, f->DirentIndex + i, set.e[i]);
    }
    return status;
}

NTSTATUS ExfatFlushFcb(FATX_VOLUME *v, FATX_FCB *f) {
    if ((f->Flags & FCB_VOLUME) || f == &v->Ex->Root)
        return STATUS_SUCCESS;
    FATX_FCB *parent = ExParentOf(v, f);
    bool isDir = f->Flags & FCB_DIRECTORY;
    EX_SET set;
    ExBuildSet(&set, f->Name, f->NameLength, f->Attributes, f->CreationTime, f->LastWriteTime, f->LastAccessTime,
               f->FirstCluster, isDir ? f->ExDirLength : f->FileSize, f->ExFlags & EXFAT_NOFATCHAIN);
    NTSTATUS status = STATUS_SUCCESS;
    if (set.Count != f->ExEntries) { // the name needs another number of entries: move the set
        status = ExfatRemoveDirent(v, f);
        if (NT_SUCCESS(status))
            status = ExFindFree(v, parent, set.Count, &f->DirentIndex);
    }
    if (NT_SUCCESS(status))
        status = ExWriteSet(v, parent, f->DirentIndex, &set);
    if (NT_SUCCESS(status)) {
        f->ExEntries = (UCHAR)set.Count;
        f->Flags &= ~FCB_DIRENT_DIRTY;
    }
    return status;
}

NTSTATUS ExfatIsDirectoryEmpty(FATX_VOLUME *v, FATX_FCB *dir) {
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = ExMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EX_END))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EX_FILE) // any file, also one titles cannot see
            return (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    }
}

// --- Folder trees: usage of the cache folders, and emptying one when a title formats it

static bool ExIsCacheFolder(FATX_VOLUME *v) { return v->PartitionNumber >= 3 && v->PartitionNumber <= 5; }

bool ExfatMayAllocate(FATX_VOLUME *v, ULONG Count) {
    return !ExIsCacheFolder(v) || ((ULONGLONG)v->UsedClusters + Count) << v->ClusterShift <= 0x2EE00000;
}

// A directory's or file's allocation as a temporary FCB (from its entry set).
static void ExFcbFromSet(FATX_VOLUME *v, const EX_SET *s, FATX_FCB *f) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->ChildList);
    const UCHAR *st = s->e[1];
    ULONGLONG length = *(const ULONGLONG *)(st + 24);
    f->FirstCluster = *(const ULONG *)(st + 20);
    f->Flags = (*(const USHORT *)(s->e[0] + 4) & FILE_ATTRIBUTE_DIRECTORY) ? FCB_DIRECTORY : 0;
    if (f->FirstCluster) {
        f->ExFlags = (st[1] & 2) ? EXFAT_NOFATCHAIN : 0;
        f->ExAllocClusters = (ULONG)((length + v->BytesPerCluster - 1) >> v->ClusterShift);
    }
}

static NTSTATUS ExAllocationOf(FATX_VOLUME *v, FATX_FCB *f, ULONG *Clusters) {
    *Clusters = 0;
    if (!f->FirstCluster)
        return STATUS_SUCCESS;
    if (f->ExFlags & EXFAT_NOFATCHAIN) {
        *Clusters = f->ExAllocClusters;
        return STATUS_SUCCESS;
    }
    ULONG last;
    return FatxChainLength(v, f->FirstCluster, Clusters, &last);
}

// Count (Delete = false) or delete (true) everything below dir; Used counts the clusters seen.
static NTSTATUS ExWalk(FATX_VOLUME *v, FATX_FCB *dir, bool Delete, ULONG Depth, ULONG *Used) {
    if (Depth > 32)
        return STATUS_FILE_CORRUPT_ERROR_;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = ExMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EX_END))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] != EX_FILE)
            continue;
        EX_SET set;
        status = ExReadSet(v, dir, slot, &set);
        if (status == STATUS_FILE_CORRUPT_ERROR_)
            continue;
        if (!NT_SUCCESS(status))
            return status;
        FATX_FCB child;
        ExFcbFromSet(v, &set, &child);
        if (child.Flags & FCB_DIRECTORY) {
            status = ExWalk(v, &child, Delete, Depth + 1, Used);
            if (!NT_SUCCESS(status))
                return status;
        }
        ULONG clusters;
        status = ExAllocationOf(v, &child, &clusters);
        if (!NT_SUCCESS(status))
            return status;
        *Used += clusters;
        if (Delete) {
            status = ExfatFreeFile(v, &child);
            for (ULONG i = 0; NT_SUCCESS(status) && i < set.Count; i++) {
                set.e[i][0] &= 0x7F;
                status = ExWriteSlot(v, dir, slot + i, set.e[i]);
            }
            if (!NT_SUCCESS(status))
                return status;
        }
        slot += set.Count - 1;
    }
}

static NTSTATUS ExCountFolder(FATX_VOLUME *v) {
    ULONG used = 0, own;
    NTSTATUS status = ExWalk(v, &v->RootFcb, false, 0, &used);
    if (NT_SUCCESS(status))
        status = ExAllocationOf(v, &v->RootFcb, &own);
    if (NT_SUCCESS(status))
        v->UsedClusters = used + own;
    return status;
}

// --- Volume handle and size (spec §5-6 of the storage spec)

// A title formats its cache partition (X/Y/Z) through the volume handle: a FATX header written at offset 0, then the
// allocation table and the root directory, then a dismount. A folder partition has no sectors of its own, so the
// header write empties the folder and the metadata writes are dropped; reads show a freshly formatted FATX volume.
// E:, C: and F: refuse to be formatted this way.
static ULONG ExfatFormatSerial[7]; // the serial of the last header written to X/Y/Z (shown by later mounts)

static void ExFormattedLayout(FATX_VOLUME *v, ULONG *EntrySize, ULONG *RootAt) {
    ULONG entries = (ULONG)(v->ReportedLength >> 14) + 1;
    *EntrySize = entries < 0xFFF0 ? 2 : 4;
    *RootAt = 0x1000 + ((entries * *EntrySize + 0xFFF) & ~0xFFFu);
}

NTSTATUS ExfatVolumeTransfer(FATX_VOLUME *v, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG length = s->Parameters.Read.Length;
    LONGLONG offset = s->Parameters.Read.ByteOffset.QuadPart;
    auto buffer = (PUCHAR)Irp->UserBuffer;
    ULONG entrySize, rootAt;
    ExFormattedLayout(v, &entrySize, &rootAt);
    LONGLONG metaEnd = rootAt + 0x4000;
    NTSTATUS status = STATUS_SUCCESS;
    FatxAcquireVolume(v);
    if (offset < 0 || offset + length > v->ReportedLength) {
        status = STATUS_INVALID_PARAMETER;
    } else if (s->MajorFunction == IRP_MJ_READ) {
        for (ULONG i = 0; i < length; i++) {
            LONGLONG at = offset + i;
            UCHAR b = 0;
            if (at < 16) {
                ULONG header[4] = {FATX_SIGNATURE, ExfatFormatSerial[v->PartitionNumber] ? ExfatFormatSerial[v->PartitionNumber]
                                                                                          : v->SerialNumber, 32, 1};
                b = ((PUCHAR)header)[at];
            } else if (at >= 0x1000 && at < 0x1000 + 2 * entrySize) {
                b = (at - 0x1000) == 0 ? 0xF8 : 0xFF; // entry 0 (media), entry 1 (the root's end of chain)
            } else if (at >= rootAt && at < metaEnd) {
                b = 0xFF; // empty root directory
            }
            buffer[i] = b;
        }
    } else if (offset == 0 && length >= 16 && *(PULONG)buffer == FATX_SIGNATURE) {
        if (!ExIsCacheFolder(v) || !IsListEmpty(&v->RootFcb.ChildList)) {
            status = STATUS_ACCESS_DENIED;
        } else {
            ULONG used = 0;
            status = ExWalk(v, &v->RootFcb, true, 0, &used);
            if (NT_SUCCESS(status))
                status = ExCountFolder(v);
            if (NT_SUCCESS(status))
                ExfatFormatSerial[v->PartitionNumber] = v->SerialNumber = ((PULONG)buffer)[1];
        }
    } else if (offset + length > metaEnd || !ExIsCacheFolder(v)) {
        status = STATUS_ACCESS_DENIED; // only the format's own metadata writes are expected
    }
    FatxReleaseVolume(v);
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = NT_SUCCESS(status) ? length : 0;
    IofCompleteRequest(Irp, 0);
    return status;
}

// Reported in 16 KB units (the FATX cluster size titles expect): E: and C: the volume capped at 2 TB, X/Y/Z their
// retail 750 MB less what the folder holds, F: everything.
void ExfatVolumeSize(FATX_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit) {
    constexpr ULONGLONG unit = 0x4000, cap = 2ull << 40;
    ULONGLONG total = (ULONGLONG)v->Ex->ClusterCount << v->ClusterShift;
    ULONGLONG free = (ULONGLONG)v->Alloc->FreeClusters << v->ClusterShift;
    if (ExIsCacheFolder(v)) {
        total = 0x2EE00000;
        ULONGLONG used = (ULONGLONG)v->UsedClusters << v->ClusterShift;
        if (free > total - (used < total ? used : total))
            free = total - (used < total ? used : total);
    } else if (v->PartitionNumber <= 2 && total > cap) {
        total = cap;
    }
    if (free > total)
        free = total;
    *Total = total / unit > 0xFFFFFFFF ? 0xFFFFFFFF : (ULONG)(total / unit);
    *Free = free / unit > 0xFFFFFFFF ? 0xFFFFFFFF : (ULONG)(free / unit);
    *SectorsPerUnit = unit / v->SectorSize;
}

// --- Mount

static void ExInitFcb(FATX_FCB *f, ULONG Flags) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->SiblingLink);
    InitializeListHead(&f->ChildList);
    f->Flags = Flags;
    f->ReferenceCount = 1; // permanent
}

static ULONG ExBootChecksum(FATX_VOLUME *v, NTSTATUS *Status) {
    ULONG sum = 0;
    for (ULONG i = 0; i < 11 * 512; i++) {
        PUCHAR data;
        *Status = FatxMapBlock(v, i & ~(FATX_BLOCK_SIZE - 1), &data);
        if (!NT_SUCCESS(*Status))
            return 0;
        if (i != 106 && i != 107 && i != 112)
            sum = ((sum >> 1) | (sum << 31)) + data[i & (FATX_BLOCK_SIZE - 1)];
    }
    return sum;
}

// Read the boot sector, the root's bitmap and up-case entries, and count the free clusters (the lock is held).
static NTSTATUS ExMountDisk(FATX_VOLUME *v) {
    EXFAT_DISK *x = v->Ex;
    PUCHAR b;
    NTSTATUS status = FatxMapBlock(v, 0, &b);
    if (!NT_SUCCESS(status))
        return status;
    UCHAR sectorShift = b[108], clusterShift = (UCHAR)(b[108] + b[109]);
    if (RtlCompareMemory(b + 3, "EXFAT   ", 8) != 8 || sectorShift != 9 || clusterShift < 12 || clusterShift > 25 ||
        *(USHORT *)(b + 510) != 0xAA55)
        return STATUS_UNRECOGNIZED_VOLUME;
    ULONG fatOffset = *(ULONG *)(b + 80), fatLength = *(ULONG *)(b + 84), heap = *(ULONG *)(b + 88);
    ULONG count = *(ULONG *)(b + 92), root = *(ULONG *)(b + 96), serial = *(ULONG *)(b + 100);
    USHORT flags = *(USHORT *)(b + 106);
    UCHAR fats = b[110];
    if (!count || (fats != 1 && fats != 2) ||
        ((ULONGLONG)heap << 9) + ((ULONGLONG)count << clusterShift) > (ULONGLONG)v->PartitionLength)
        return STATUS_UNRECOGNIZED_VOLUME;
    PUCHAR check;
    status = FatxMapBlock(v, 11 * 512 & ~(FATX_BLOCK_SIZE - 1), &check);
    ULONG want = NT_SUCCESS(status) ? *(ULONG *)(check + (11 * 512 & (FATX_BLOCK_SIZE - 1))) : 0;
    if (!NT_SUCCESS(status) || ExBootChecksum(v, &status) != want || !NT_SUCCESS(status))
        return STATUS_UNRECOGNIZED_VOLUME;
    x->ClusterShift = clusterShift;
    x->BytesPerCluster = 1u << clusterShift;
    x->ClusterCount = count;
    x->FatByteOffset = (fatOffset + (fats == 2 && (flags & 1) ? fatLength : 0)) << 9;
    x->HeapByteOffset = (LONGLONG)heap << 9;
    x->Serial = serial;
    v->ClusterShift = clusterShift;
    v->BytesPerCluster = x->BytesPerCluster;
    v->NumberOfClusters = count;
    v->FatByteOffset = x->FatByteOffset;
    v->FatEntryShift = 2;
    v->DataByteOffset = x->HeapByteOffset;
    for (ULONG i = 0; i < 256; i++)
        x->Upcase[i] = (USHORT)i;
    ExInitFcb(&x->Root, FCB_DIRECTORY | FCB_ROOT);
    x->Root.Attributes = FILE_ATTRIBUTE_DIRECTORY;
    x->Root.FirstCluster = root;
    if (!FatxIsValidCluster(v, root))
        return STATUS_UNRECOGNIZED_VOLUME;

    // The root's allocation bitmap (its clusters must be consecutive) and up-case table.
    ULONG bitmapFirst = 0, upcaseFirst = 0;
    ULONGLONG bitmapLength = 0, upcaseLength = 0;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        status = ExMapSlot(v, &x->Root, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EX_END))
            break;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EX_BITMAP && !bitmapFirst) {
            bitmapFirst = *(ULONG *)(e + 20);
            bitmapLength = *(ULONGLONG *)(e + 24);
        } else if (e[0] == EX_UPCASE) {
            upcaseFirst = *(ULONG *)(e + 20);
            upcaseLength = *(ULONGLONG *)(e + 24);
        }
    }
    if (!FatxIsValidCluster(v, bitmapFirst) || bitmapLength < (count + 7) / 8)
        return STATUS_UNRECOGNIZED_VOLUME;
    ULONG bitmapClusters = (ULONG)((bitmapLength + x->BytesPerCluster - 1) >> clusterShift);
    for (ULONG i = 0, c = bitmapFirst; i + 1 < bitmapClusters; i++, c++) {
        ULONG next;
        status = FatxGetFatEntry(v, c, &next);
        if (!NT_SUCCESS(status) || next != c + 1)
            return STATUS_UNRECOGNIZED_VOLUME;
    }
    x->BitmapByteOffset = FatxClusterOffset(v, bitmapFirst);
    if (FatxIsValidCluster(v, upcaseFirst)) {
        // The mapping of characters 0-255 (the compressed form: 0xFFFF n = n characters map to themselves).
        FATX_FCB table;
        ExInitFcb(&table, 0);
        table.FirstCluster = upcaseFirst;
        ULONG ch = 0;
        for (ULONG at = 0; at + 1 < upcaseLength && ch < 256; at += 2) {
            ULONG cluster;
            status = FatxClusterAtIndex(v, &table, at >> clusterShift, &cluster);
            if (!NT_SUCCESS(status))
                break;
            LONGLONG pos = FatxClusterOffset(v, cluster) + (at & (x->BytesPerCluster - 1));
            PUCHAR data;
            status = FatxMapBlock(v, pos & ~(LONGLONG)(FATX_BLOCK_SIZE - 1), &data);
            if (!NT_SUCCESS(status))
                break;
            USHORT value = *(USHORT *)(data + (pos & (FATX_BLOCK_SIZE - 1)));
            if (value == 0xFFFF && at + 3 < upcaseLength) {
                at += 2;
                pos += 2;
                status = FatxMapBlock(v, pos & ~(LONGLONG)(FATX_BLOCK_SIZE - 1), &data);
                if (!NT_SUCCESS(status))
                    break;
                ch += *(USHORT *)(data + (pos & (FATX_BLOCK_SIZE - 1)));
            } else {
                x->Upcase[ch++] = value;
            }
        }
        status = STATUS_SUCCESS;
    }

    // Free clusters, and the first free one.
    x->Alloc.FreeClusters = 0;
    x->Alloc.AllocationHint = 0;
    for (ULONG done = 0; done < count;) {
        LONGLONG at = x->BitmapByteOffset + (done >> 3);
        PUCHAR data;
        status = FatxMapBlock(v, at, &data);
        if (!NT_SUCCESS(status))
            return status;
        // A dword (32 clusters) at a time: the bitmap of a large volume is megabytes long.
        for (ULONG i = 0; i < FATX_BLOCK_SIZE / 4 && done < count; i++) {
            ULONG word = ((PULONG)data)[i], n = count - done < 32 ? count - done : 32;
            if (n < 32)
                word |= ~0u << n; // past the last cluster: not free
            if (word != ~0u) {
                if (!x->Alloc.FreeClusters)
                    x->Alloc.AllocationHint = done + 2 + __builtin_ctz(~word);
                x->Alloc.FreeClusters += 32 - __builtin_popcount(word);
            }
            done += n;
        }
    }
    x->Mounted = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS ExfatMountVolume(PDEVICE_OBJECT Target, PCSZ Folder) {
    PDEVICE_OBJECT disk = IdexDiskExfatVolume;
    PARTITION_INFORMATION volume, target;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, disk, nullptr, 0, &volume,
                                                          sizeof(volume), nullptr, FALSE);
    if (NT_SUCCESS(status))
        status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, Target, nullptr, 0, &target,
                                                     sizeof(target), nullptr, FALSE);
    if (!NT_SUCCESS(status))
        return status;
    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&FatxDriverObject, sizeof(FATX_VOLUME), nullptr, 8 /*FILE_DEVICE_DISK_FILE_SYSTEM*/,
                            FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->StackSize = (CHAR)(dev->StackSize + (disk->StackSize > Target->StackSize ? disk->StackSize : Target->StackSize));
    if (dev->AlignmentRequirement < disk->AlignmentRequirement)
        dev->AlignmentRequirement = disk->AlignmentRequirement;
    dev->Flags |= disk->Flags & 0x40;
    dev->SectorSize = 512;
    FATX_VOLUME *v = FatxVolume(dev);
    v->TargetDevice = disk;
    v->MountTarget = Target;
    v->Exfat = TRUE;
    v->FirstClusterNumber = 2;
    v->Ex = &ExfatDisk;
    v->Alloc = &ExfatDisk.Alloc;
    v->PartitionNumber = target.PartitionNumber;
    v->PartitionLength = volume.PartitionLength.QuadPart;
    v->ReportedLength = target.PartitionLength.QuadPart;
    v->SectorSize = 512;
    v->CacheExtension = {disk, volume.PartitionLength, 512};
    ExInitFcb(&v->RootFcb, FCB_DIRECTORY | FCB_ROOT);
    ExInitFcb(&v->VolumeFcb, FCB_VOLUME);
    v->RootFcb.Attributes = FILE_ATTRIBUTE_DIRECTORY;

    FatxAcquireVolume(v);
    status = ExfatDisk.Mounted ? STATUS_SUCCESS : ExMountDisk(v);
    if (NT_SUCCESS(status)) {
        v->ClusterShift = ExfatDisk.ClusterShift;
        v->BytesPerCluster = ExfatDisk.BytesPerCluster;
        v->NumberOfClusters = ExfatDisk.ClusterCount;
        v->FatByteOffset = ExfatDisk.FatByteOffset;
        v->FatEntryShift = 2;
        v->DataByteOffset = ExfatDisk.HeapByteOffset;
        v->SerialNumber = ExfatDisk.Serial;
        // The folder: found in the volume's root, or created there.
        ULONG length = 0;
        while (Folder[length])
            length++;
        OBJECT_STRING name = {(USHORT)length, (USHORT)length, (PCHAR)Folder};
        FATX_ENTRY e;
        ULONG index;
        status = ExfatLookup(v, &ExfatDisk.Root, &name, &e, &index);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            ULONG first = 0;
            status = ExfatInitDirectory(v, &first);
            if (NT_SUCCESS(status))
                status = ExfatCreateDirent(v, &ExfatDisk.Root, &name, FILE_ATTRIBUTE_DIRECTORY, first, &index);
            if (NT_SUCCESS(status))
                status = ExfatReadEntry(v, &ExfatDisk.Root, index, &e);
        }
        if (NT_SUCCESS(status) && !(e.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            status = STATUS_UNRECOGNIZED_VOLUME;
        if (NT_SUCCESS(status)) {
            FATX_FCB *r = &v->RootFcb;
            r->FirstCluster = e.FirstCluster;
            r->ExFlags = e.ExFlags;
            r->ExEntries = e.ExEntries;
            r->ExAllocClusters = e.ExAllocClusters;
            r->ExDirLength = e.ExDirLength;
            r->DirentIndex = index;
            r->NameLength = e.FileNameLength;
            RtlCopyMemory(r->Name, e.FileName, e.FileNameLength);
            r->CreationTime = e.CreationTime;
            r->LastWriteTime = e.LastWriteTime;
            r->LastAccessTime = e.LastAccessTime;
            if (ExfatFormatSerial[v->PartitionNumber])
                v->SerialNumber = ExfatFormatSerial[v->PartitionNumber];
            if (ExIsCacheFolder(v))
                status = ExCountFolder(v);
        }
    }
    FatxReleaseVolume(v);
    if (!NT_SUCCESS(status)) {
        FatxForgetVolume(v);
        IoDeleteDevice(dev);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    ObfReferenceObject(Target);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    Target->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
