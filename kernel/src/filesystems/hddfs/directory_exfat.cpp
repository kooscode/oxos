// exFAT directories: entry sets (a file entry, a stream entry and name entries, 32 bytes each, with a checksum)
// (spec/oxos_internal/07_exfat_driver.md), and the walks over a folder's tree that count or empty it. An exFAT volume
// reaches its directory operations through EXFATFormat (at the end).
#include <xk/hddfs.h>
#include <xk/serial.h>

constexpr NTSTATUS STATUS_FILE_CORRUPT_ERROR_ = (NTSTATUS)0xC0000102;

// FATX timestamps count years from 2000, exFAT's from 1980; the other fields are the same.
static ULONG EXFATToFATXTime(ULONG t) {
    return (t >> 25) < 20 ? (1u << 5 | 1) << 16 : t - (20u << 25);
}

static ULONG EXFATFromFATXTime(ULONG t) {
    return (t >> 25) > 107 ? t | 0xFE000000 : t + (20u << 25);
}

static USHORT EXFATSetChecksum(const UCHAR *p, ULONG n) {
    USHORT sum = 0;
    for (ULONG i = 0; i < n; i++) {
        if (i != 2 && i != 3)
            sum = (USHORT)(((sum >> 1) | (sum << 15)) + p[i]);
    }
    return sum;
}

// --- Slots (32-byte directory entries)

NTSTATUS EXFATMapSlot(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Slot, PUCHAR *Entry, LONGLONG *Block) {
    ULONGLONG byte = (ULONGLONG)Slot * 32;
    ULONG cluster;
    NTSTATUS status = HDDFSClusterAtIndex(v, dir, (ULONG)(byte >> v->ClusterShift), &cluster);
    if (!NT_SUCCESS(status))
        return status; // STATUS_END_OF_FILE past the directory's clusters
    ULONG within = (ULONG)byte & (v->BytesPerCluster - 1);
    LONGLONG block = HDDFSClusterOffset(v, cluster) + (within & ~(HDDFS_BLOCK_SIZE - 1));
    PUCHAR data;
    status = HDDFSMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    *Entry = data + (within & (HDDFS_BLOCK_SIZE - 1));
    *Block = block;
    return STATUS_SUCCESS;
}

static NTSTATUS EXFATWriteSlot(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Slot, const UCHAR *Data) {
    PUCHAR e, base;
    LONGLONG block;
    NTSTATUS status = EXFATMapSlot(v, dir, Slot, &e, &block);
    if (NT_SUCCESS(status))
        status = HDDFSMapBlock(v, block, &base);
    if (!NT_SUCCESS(status))
        return status;
    RtlCopyMemory(e, Data, 32);
    return HDDFSWriteBlockRange(v, block, (ULONG)(e - base), 32);
}

// The entry set whose file entry is at Slot, copied out.
static NTSTATUS EXFATReadSet(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Slot, EXFAT_SET *Set) {
    PUCHAR e;
    LONGLONG block;
    NTSTATUS status = EXFATMapSlot(v, dir, Slot, &e, &block);
    if (!NT_SUCCESS(status))
        return status;
    Set->Count = 1u + e[1];
    if (e[0] != EXFAT_ENTRY_FILE || Set->Count < 3 || Set->Count > EXFAT_MAX_SET)
        return STATUS_FILE_CORRUPT_ERROR_;
    for (ULONG i = 0; i < Set->Count; i++) {
        status = EXFATMapSlot(v, dir, Slot + i, &e, &block);
        if (!NT_SUCCESS(status))
            return status == STATUS_END_OF_FILE ? STATUS_FILE_CORRUPT_ERROR_ : status;
        RtlCopyMemory(Set->e[i], e, 32);
    }
    if (Set->e[1][0] != EXFAT_ENTRY_STREAM ||
        EXFATSetChecksum(Set->e[0], Set->Count * 32) != *(USHORT *)(Set->e[0] + 2))
        return STATUS_FILE_CORRUPT_ERROR_;
    return STATUS_SUCCESS;
}

static NTSTATUS EXFATWriteSet(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Slot, EXFAT_SET *Set) {
    *(USHORT *)(Set->e[0] + 2) = EXFATSetChecksum(Set->e[0], Set->Count * 32);
    for (ULONG i = 0; i < Set->Count; i++) {
        NTSTATUS status = EXFATWriteSlot(v, dir, Slot + i, Set->e[i]);
        if (!NT_SUCCESS(status))
            return status;
    }
    return STATUS_SUCCESS;
}

// A set as titles see it: a FATX-valid name of at most 42 characters (all below 0x80, the FATX character set). Files
// of 4 GB or more (disc images) are visible but read-only (FileSizeHigh); directories must stay below 4 GB. Other
// sets stay on the disk untouched and are invisible.
static bool EXFATEntryFromSet(HDDFS_VOLUME *v, const EXFAT_SET *s, HDDFS_ENTRY *Out) {
    const UCHAR *f = s->e[0], *st = s->e[1];
    ULONG nameLength = st[3];
    if (nameLength < 1 || nameLength > FATX_MAX_NAME || 2 + (nameLength + 14) / 15 > s->Count)
        return false;
    USHORT attributes = *(const USHORT *)(f + 4);
    ULONGLONG length = *(const ULONGLONG *)(st + 24);
    if (length > 0xFFFFFFFFull && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
#if XK_DEBUG_SERIAL
    if (ULONGLONG valid = *(const ULONGLONG *)(st + 8); valid != length && length >= 0x4000000) {
        XK_SERIAL("exfat: file at cluster %#lx: valid data length %#llx of %#llx\n", *(const ULONG *)(st + 20), valid,
                  length);
    }
#endif
    RtlZeroMemory(Out, sizeof(*Out));
    for (ULONG i = 0; i < nameLength; i++) {
        const UCHAR *n = s->e[2 + i / 15];
        if (n[0] != EXFAT_ENTRY_NAME)
            return false;
        USHORT c = *(const USHORT *)(n + 2 + 2 * (i % 15));
        if (c >= 0x80)
            return false;
        Out->FileName[i] = (CHAR)c;
    }
    OBJECT_STRING name = {(USHORT)nameLength, (USHORT)nameLength, Out->FileName};
    if (!HDDFSIsValidName(&name))
        return false;
    bool isDir = attributes & FILE_ATTRIBUTE_DIRECTORY;
    Out->FileNameLength = (UCHAR)nameLength;
    Out->FileAttributes = (UCHAR)(attributes & 0x37);
    Out->FirstCluster = *(const ULONG *)(st + 20);
    Out->FileSize = isDir ? 0 : (ULONG)length;
    Out->FileSizeHigh = isDir ? 0 : (ULONG)(length >> 32);
    Out->CreationTime = EXFATToFATXTime(*(const ULONG *)(f + 8));
    Out->LastWriteTime = EXFATToFATXTime(*(const ULONG *)(f + 12));
    Out->LastAccessTime = EXFATToFATXTime(*(const ULONG *)(f + 16));
    Out->EXFATEntries = (UCHAR)s->Count;
    if (Out->FirstCluster) {
        Out->EXFATFlags = (st[1] & 2) ? EXFAT_NOFATCHAIN : 0;
        Out->EXFATAllocClusters = (ULONG)((length + v->BytesPerCluster - 1) >> v->ClusterShift);
    }
    Out->EXFATDirLength = isDir ? (ULONG)length : 0;
    return true;
}

// A complete entry set for a name (file, stream, name entries); the checksum is filled in when it is written.
static void EXFATBuildSet(EXFAT_SET *Set, const CHAR *Name, ULONG NameLength, UCHAR Attributes, ULONG Created,
                          ULONG Written, ULONG Accessed, ULONG First, ULONGLONG Length, bool NoFatChain) {
    ULONG names = (NameLength + 14) / 15;
    Set->Count = 2 + names;
    RtlZeroMemory(Set->e, Set->Count * 32);
    UCHAR *f = Set->e[0], *st = Set->e[1];
    f[0] = EXFAT_ENTRY_FILE;
    f[1] = (UCHAR)(Set->Count - 1);
    *(USHORT *)(f + 4) = Attributes;
    *(ULONG *)(f + 8) = EXFATFromFATXTime(Created);
    *(ULONG *)(f + 12) = EXFATFromFATXTime(Written);
    *(ULONG *)(f + 16) = EXFATFromFATXTime(Accessed);
    st[0] = EXFAT_ENTRY_STREAM;
    st[1] = 1 | (NoFatChain && First ? 2 : 0); // allocation possible, NoFatChain
    st[3] = (UCHAR)NameLength;
    USHORT hash = 0;
    for (ULONG i = 0; i < NameLength; i++) {
        USHORT c = EXFATDisk.Upcase[(UCHAR)Name[i]];
        hash = (USHORT)(((hash >> 1) | (hash << 15)) + (c & 0xFF));
        hash = (USHORT)(((hash >> 1) | (hash << 15)) + (c >> 8));
        UCHAR *n = Set->e[2 + i / 15];
        n[0] = EXFAT_ENTRY_NAME;
        *(USHORT *)(n + 2 + 2 * (i % 15)) = (UCHAR)Name[i];
    }
    for (ULONG k = 0; k < names; k++)
        Set->e[2 + k][0] = EXFAT_ENTRY_NAME;
    *(USHORT *)(st + 4) = hash;
    *(ULONGLONG *)(st + 8) = Length;  // ValidDataLength
    *(ULONG *)(st + 20) = First;
    *(ULONGLONG *)(st + 24) = Length; // DataLength
}

NTSTATUS EXFATInitDirectory(HDDFS_VOLUME *v, ULONG *First) {
    NTSTATUS status = HDDFSAllocateClusters(v, 0, 1, First);
    if (NT_SUCCESS(status)) {
        status = EXFATZeroCluster(v, *First);
        if (!NT_SUCCESS(status)) {
            HDDFSFreeChain(v, *First, 1);
            *First = 0;
        }
    }
    return status;
}

// The directory's entry set holds its length: write it after the directory grew.
static NTSTATUS EXFATGrowDirectory(HDDFS_VOLUME *v, HDDFS_FCB *dir) {
    ULONG cluster = 0;
    NTSTATUS status = STATUS_SUCCESS;
    if (dir->EXFATFlags & EXFAT_NOFATCHAIN) { // grow in place when the next cluster is free
        ULONG next = dir->FirstCluster + dir->EXFATAllocClusters;
        if (HDDFSIsValidCluster(v, next) && EXFATClusterFree(v, next, &status)) {
            status = EXFATMarkCluster(v, next, true);
            if (!NT_SUCCESS(status))
                return status;
            v->Alloc->FreeClusters--;
            v->UsedClusters++;
            cluster = next;
            dir->EXFATAllocClusters++;
        } else if (NT_SUCCESS(status)) {
            status = EXFATMakeChain(v, dir);
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    if (!cluster) {
        ULONG count, last;
        status = HDDFSFileChain(v, dir, &count, &last);
        if (NT_SUCCESS(status))
            status = HDDFSAllocateClusters(v, last, 1, &cluster);
        if (!NT_SUCCESS(status))
            return status;
        dir->EXFATAllocClusters = count + 1;
    }
    status = EXFATZeroCluster(v, cluster);
    if (!NT_SUCCESS(status))
        return status;
    dir->EXFATDirLength += v->BytesPerCluster;
    dir->HintCluster = 0;
    return dir == &v->EXFATDisk->Root ? STATUS_SUCCESS : EXFATFlushFcb(v, dir);
}

// Need consecutive free slots (unused or deleted), growing the directory when its end has too few.
static NTSTATUS EXFATFindFree(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Need, ULONG *Slot) {
    ULONG run = 0, start = 0;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = EXFATMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE) {
            status = EXFATGrowDirectory(v, dir);
            if (NT_SUCCESS(status))
                status = EXFATMapSlot(v, dir, slot, &e, &block);
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

NTSTATUS EXFATNextEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry) {
    for (ULONG slot = *Index;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = EXFATMapSlot(v, dir, slot, &e, &block);
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EXFAT_ENTRY_END)
            return STATUS_END_OF_FILE;
        if (e[0] != EXFAT_ENTRY_FILE)
            continue; // secondary, deleted or volume entries
        EXFAT_SET set;
        status = EXFATReadSet(v, dir, slot, &set);
        if (NT_SUCCESS(status) && EXFATEntryFromSet(v, &set, Entry)) {
            *Index = slot;
            return STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(status) && status != STATUS_FILE_CORRUPT_ERROR_)
            return status;
    }
}

NTSTATUS EXFATLookup(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        NTSTATUS status = EXFATNextEntry(v, dir, &i, Found);
        if (status == STATUS_END_OF_FILE)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(status))
            return status;
        if (HDDFSNameEqual(Found->FileName, Found->FileNameLength, Name->Buffer, Name->Length)) {
            *Index = i;
            return STATUS_SUCCESS;
        }
    }
}

NTSTATUS EXFATReadEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry) {
    EXFAT_SET set;
    NTSTATUS status = EXFATReadSet(v, dir, Index, &set);
    if (NT_SUCCESS(status) && !EXFATEntryFromSet(v, &set, Entry))
        status = STATUS_FILE_CORRUPT_ERROR_;
    return status;
}

NTSTATUS EXFATCreateDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                           ULONG FirstCluster, ULONG *Index) {
    EXFAT_SET set;
    ULONG now = HDDFSCurrentTimestamp();
    ULONGLONG length = (Attributes & FILE_ATTRIBUTE_DIRECTORY) && FirstCluster ? v->BytesPerCluster : 0;
    EXFATBuildSet(&set, Name->Buffer, Name->Length, Attributes, now, now, now, FirstCluster, length, false);
    NTSTATUS status = EXFATFindFree(v, dir, set.Count, Index);
    return NT_SUCCESS(status) ? EXFATWriteSet(v, dir, *Index, &set) : status;
}

// The directory whose entry set describes f (a folder partition's root is an entry of the volume's root).
static HDDFS_FCB *EXFATParentOf(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    return (f->Flags & FCB_ROOT) ? &v->EXFATDisk->Root : f->Parent;
}

NTSTATUS EXFATRemoveDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    HDDFS_FCB *parent = EXFATParentOf(v, f);
    EXFAT_SET set;
    NTSTATUS status = EXFATReadSet(v, parent, f->DirentIndex, &set);
    if (!NT_SUCCESS(status))
        return status;
    for (ULONG i = 0; NT_SUCCESS(status) && i < set.Count; i++) {
        set.e[i][0] &= 0x7F; // not in use
        status = EXFATWriteSlot(v, parent, f->DirentIndex + i, set.e[i]);
    }
    return status;
}

NTSTATUS EXFATFlushFcb(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    if ((f->Flags & FCB_VOLUME) || f == &v->EXFATDisk->Root)
        return STATUS_SUCCESS;
    HDDFS_FCB *parent = EXFATParentOf(v, f);
    bool isDir = f->Flags & FCB_DIRECTORY;
    EXFAT_SET set;
    EXFATBuildSet(&set, f->Name, f->NameLength, f->Attributes, f->CreationTime, f->LastWriteTime, f->LastAccessTime,
                  f->FirstCluster, isDir ? f->EXFATDirLength : HDDFSFileSize64(f), f->EXFATFlags & EXFAT_NOFATCHAIN);
    NTSTATUS status = STATUS_SUCCESS;
    if (set.Count != f->EXFATEntries) { // the name needs another number of entries: move the set
        status = EXFATRemoveDirent(v, f);
        if (NT_SUCCESS(status))
            status = EXFATFindFree(v, parent, set.Count, &f->DirentIndex);
    }
    if (NT_SUCCESS(status))
        status = EXFATWriteSet(v, parent, f->DirentIndex, &set);
    if (NT_SUCCESS(status)) {
        f->EXFATEntries = (UCHAR)set.Count;
        f->Flags &= ~FCB_DIRENT_DIRTY;
    }
    return status;
}

NTSTATUS EXFATIsDirectoryEmpty(HDDFS_VOLUME *v, HDDFS_FCB *dir) {
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = EXFATMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EXFAT_ENTRY_END))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EXFAT_ENTRY_FILE) // any file, also one titles cannot see
            return (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    }
}

// Delete a file's entry set and free its clusters; the FCB no longer has any.
NTSTATUS EXFATDeleteDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    NTSTATUS status = EXFATRemoveDirent(v, f);
    if (NT_SUCCESS(status))
        status = EXFATFreeFile(v, f);
    f->FirstCluster = 0;
    f->FileSize = f->FileSizeHigh = 0;
    f->HintCluster = 0;
    f->EXFATFlags = 0;
    f->EXFATAllocClusters = 0;
    return status;
}

// --- Folder trees: usage of the cache folders, and emptying one when a title formats it

bool EXFATIsCacheFolder(HDDFS_VOLUME *v) {
    return v->PartitionNumber >= 3 && v->PartitionNumber <= 5;
}

bool EXFATMayAllocate(HDDFS_VOLUME *v, ULONG Count) {
    return !EXFATIsCacheFolder(v) || ((ULONGLONG)v->UsedClusters + Count) << v->ClusterShift <= 0x2EE00000;
}

// A directory's or file's allocation as a temporary FCB (from its entry set).
static void EXFATFcbFromSet(HDDFS_VOLUME *v, const EXFAT_SET *s, HDDFS_FCB *f) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->ChildList);
    const UCHAR *st = s->e[1];
    ULONGLONG length = *(const ULONGLONG *)(st + 24);
    f->FirstCluster = *(const ULONG *)(st + 20);
    f->Flags = (*(const USHORT *)(s->e[0] + 4) & FILE_ATTRIBUTE_DIRECTORY) ? FCB_DIRECTORY : 0;
    if (f->FirstCluster) {
        f->EXFATFlags = (st[1] & 2) ? EXFAT_NOFATCHAIN : 0;
        f->EXFATAllocClusters = (ULONG)((length + v->BytesPerCluster - 1) >> v->ClusterShift);
    }
}

static NTSTATUS EXFATAllocationOf(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG *Clusters) {
    *Clusters = 0;
    if (!f->FirstCluster)
        return STATUS_SUCCESS;
    *Clusters = f->EXFATAllocClusters; // the length decides, also for a FAT chain (spec 07)
    return STATUS_SUCCESS;
}

// Count (Delete = false) or delete (true) everything below dir; Used counts the clusters seen.
NTSTATUS EXFATWalk(HDDFS_VOLUME *v, HDDFS_FCB *dir, bool Delete, ULONG Depth, ULONG *Used) {
    if (Depth > 32)
        return STATUS_FILE_CORRUPT_ERROR_;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        NTSTATUS status = EXFATMapSlot(v, dir, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EXFAT_ENTRY_END))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] != EXFAT_ENTRY_FILE)
            continue;
        EXFAT_SET set;
        status = EXFATReadSet(v, dir, slot, &set);
        if (status == STATUS_FILE_CORRUPT_ERROR_)
            continue;
        if (!NT_SUCCESS(status))
            return status;
        HDDFS_FCB child;
        EXFATFcbFromSet(v, &set, &child);
        if (child.Flags & FCB_DIRECTORY) {
            status = EXFATWalk(v, &child, Delete, Depth + 1, Used);
            if (!NT_SUCCESS(status))
                return status;
        }
        ULONG clusters;
        status = EXFATAllocationOf(v, &child, &clusters);
        if (!NT_SUCCESS(status))
            return status;
        *Used += clusters;
        if (Delete) {
            status = EXFATFreeFile(v, &child);
            for (ULONG i = 0; NT_SUCCESS(status) && i < set.Count; i++) {
                set.e[i][0] &= 0x7F;
                status = EXFATWriteSlot(v, dir, slot + i, set.e[i]);
            }
            if (!NT_SUCCESS(status))
                return status;
        }
        slot += set.Count - 1;
    }
}

NTSTATUS EXFATCountFolder(HDDFS_VOLUME *v) {
    ULONG used = 0, own;
    NTSTATUS status = EXFATWalk(v, &v->RootFcb, false, 0, &used);
    if (NT_SUCCESS(status))
        status = EXFATAllocationOf(v, &v->RootFcb, &own);
    if (NT_SUCCESS(status)) {
        v->UsedClusters = used + own;
        v->OwnClusters = own;
    }
    return status;
}

// The directory operations of an exFAT volume (HDDFS_VOLUME::Format).
const HDDFS_FORMAT EXFATFormat = {EXFATLookup, EXFATReadEntry, EXFATNextEntry, EXFATInitDirectory, EXFATRemoveDirent,
                                   EXFATCreateDirent, EXFATFlushFcb, EXFATDeleteDirent, EXFATIsDirectoryEmpty};
