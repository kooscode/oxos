// FATX directories: 64-byte entries, ended by an end marker (spec/stock/04b). A FATX volume reaches its directory
// operations through FATXFormat (at the end).
#include <xk/hddfs.h>

// Map entry Index of a directory. With Extend, a directory that ends exactly there grows by one cluster
// (filled with end markers).
static NTSTATUS FATXMapDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, FATX_DIRENT **Entry,
                              LONGLONG *BlockOffset, bool Extend) {
    ULONG byte = Index * sizeof(FATX_DIRENT);
    ULONG clusterIndex = byte >> v->ClusterShift, within = byte & (v->BytesPerCluster - 1);
    ULONG cluster;
    NTSTATUS status = HDDFSClusterAtIndex(v, dir, clusterIndex, &cluster);
    if (status == STATUS_END_OF_FILE && Extend) {
        ULONG count, last;
        status = HDDFSChainLength(v, dir->FirstCluster, &count, &last);
        if (!NT_SUCCESS(status))
            return status;
        if (count != clusterIndex)
            return STATUS_END_OF_FILE;
        if (count * v->BytesPerCluster >= 4096 * sizeof(FATX_DIRENT))
            return (NTSTATUS)0xC000007F; // directories hold at most 4096 entries
        status = HDDFSAllocateClusters(v, last, 1, &cluster);
        if (!NT_SUCCESS(status))
            return status;
        LONGLONG base = HDDFSClusterOffset(v, cluster);
        for (ULONG off = 0; off < v->BytesPerCluster && NT_SUCCESS(status); off += HDDFS_BLOCK_SIZE) {
            PUCHAR data;
            status = HDDFSMapBlock(v, base + off, &data);
            if (NT_SUCCESS(status)) {
                RtlFillMemory(data, HDDFS_BLOCK_SIZE, FATX_DIRENT_NEVER_USED2);
                status = HDDFSWriteBlockRange(v, base + off, 0, HDDFS_BLOCK_SIZE);
            }
        }
    }
    if (!NT_SUCCESS(status))
        return status;
    LONGLONG block = HDDFSClusterOffset(v, cluster) + (within & ~(HDDFS_BLOCK_SIZE - 1));
    PUCHAR data;
    status = HDDFSMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    *Entry = (FATX_DIRENT *)(data + (within & (HDDFS_BLOCK_SIZE - 1)));
    *BlockOffset = block;
    return STATUS_SUCCESS;
}

static NTSTATUS FATXWriteDirent(HDDFS_VOLUME *v, LONGLONG BlockOffset, FATX_DIRENT *Entry) {
    PUCHAR data;
    NTSTATUS status = HDDFSMapBlock(v, BlockOffset, &data);
    if (!NT_SUCCESS(status))
        return status;
    return HDDFSWriteBlockRange(v, BlockOffset, (ULONG)((PUCHAR)Entry - data), sizeof(FATX_DIRENT));
}

static bool FATXIsEnd(const FATX_DIRENT *e) {
    return e->FileNameLength == FATX_DIRENT_NEVER_USED || e->FileNameLength == FATX_DIRENT_NEVER_USED2;
}

static bool FATXIsLive(const FATX_DIRENT *e) {
    return !FATXIsEnd(e) && e->FileNameLength != FATX_DIRENT_DELETED && e->FileNameLength <= FATX_MAX_NAME;
}

static void FATXEntryFromDirent(HDDFS_ENTRY *out, const FATX_DIRENT *e) {
    RtlZeroMemory(out, sizeof(*out));
    *(FATX_DIRENT *)out = *e;
}

NTSTATUS FATXLookup(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FATXMapDirent(v, dir, i, &e, &block, false);
        if (status == STATUS_END_OF_FILE)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(status))
            return status;
        if (FATXIsEnd(e))
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (FATXIsLive(e) && HDDFSNameEqual(e->FileName, e->FileNameLength, Name->Buffer, Name->Length)) {
            FATXEntryFromDirent(Found, e);
            *Index = i;
            return STATUS_SUCCESS;
        }
    }
}
NTSTATUS FATXReadEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry) {
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FATXMapDirent(v, dir, Index, &e, &block, false);
    if (NT_SUCCESS(status))
        FATXEntryFromDirent(Entry, e);
    return status;
}
NTSTATUS FATXNextEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry) {
    for (;; (*Index)++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FATXMapDirent(v, dir, *Index, &e, &block, false);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && FATXIsEnd(e)))
            return STATUS_END_OF_FILE;
        if (!NT_SUCCESS(status))
            return status;
        if (FATXIsLive(e)) {
            FATXEntryFromDirent(Entry, e);
            return STATUS_SUCCESS;
        }
    }
}
NTSTATUS FATXInitDirectory(HDDFS_VOLUME *v, ULONG *First) {
    NTSTATUS status = HDDFSAllocateClusters(v, 0, 1, First);
    LONGLONG base = NT_SUCCESS(status) ? HDDFSClusterOffset(v, *First) : 0;
    for (ULONG off = 0; NT_SUCCESS(status) && off < v->BytesPerCluster; off += HDDFS_BLOCK_SIZE) {
        PUCHAR data;
        status = HDDFSMapBlock(v, base + off, &data);
        if (NT_SUCCESS(status)) {
            RtlFillMemory(data, HDDFS_BLOCK_SIZE, FATX_DIRENT_NEVER_USED2);
            status = HDDFSWriteBlockRange(v, base + off, 0, HDDFS_BLOCK_SIZE);
        }
    }
    if (!NT_SUCCESS(status) && *First) {
        HDDFSFreeChain(v, *First);
        *First = 0;
    }
    return status;
}
NTSTATUS FATXRemoveDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FATXMapDirent(v, f->Parent, f->DirentIndex, &e, &block, false);
    if (NT_SUCCESS(status)) {
        e->FileNameLength = FATX_DIRENT_DELETED;
        status = FATXWriteDirent(v, block, e);
    }
    return status;
}
NTSTATUS FATXCreateDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                          ULONG FirstCluster, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FATXMapDirent(v, dir, i, &e, &block, true);
        if (!NT_SUCCESS(status))
            return status;
        bool end = FATXIsEnd(e);
        if (!end && e->FileNameLength != FATX_DIRENT_DELETED)
            continue;
        ULONG now = HDDFSCurrentTimestamp();
        RtlZeroMemory(e, sizeof(*e));
        e->FileNameLength = (UCHAR)Name->Length;
        e->FileAttributes = Attributes;
        RtlCopyMemory(e->FileName, Name->Buffer, Name->Length);
        RtlFillMemory(e->FileName + Name->Length, FATX_MAX_NAME - Name->Length, 0xFF);
        e->FirstCluster = FirstCluster;
        e->CreationTime = e->LastWriteTime = e->LastAccessTime = now;
        status = FATXWriteDirent(v, block, e);
        if (!NT_SUCCESS(status))
            return status;
        if (end) {
            // The entry after the old end marker must still terminate the directory.
            FATX_DIRENT *next;
            LONGLONG nextBlock;
            status = FATXMapDirent(v, dir, i + 1, &next, &nextBlock, true);
            if (NT_SUCCESS(status) && !FATXIsEnd(next)) {
                RtlFillMemory(next, sizeof(*next), FATX_DIRENT_NEVER_USED2);
                status = FATXWriteDirent(v, nextBlock, next);
            }
            if (!NT_SUCCESS(status) && status != (NTSTATUS)0xC000007F)
                return status;
        }
        *Index = i;
        return STATUS_SUCCESS;
    }
}
NTSTATUS FATXFlushFcb(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    if (f->Flags & (FCB_ROOT | FCB_VOLUME))
        return STATUS_SUCCESS;
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FATXMapDirent(v, f->Parent, f->DirentIndex, &e, &block, false);
    if (!NT_SUCCESS(status))
        return status;
    e->FileAttributes = f->Attributes;
    e->FirstCluster = f->FirstCluster;
    e->FileSize = (f->Flags & FCB_DIRECTORY) ? 0 : f->FileSize;
    e->CreationTime = f->CreationTime;
    e->LastWriteTime = f->LastWriteTime;
    e->LastAccessTime = f->LastAccessTime;
    e->FileNameLength = f->NameLength;
    RtlCopyMemory(e->FileName, f->Name, f->NameLength);
    f->Flags &= ~FCB_DIRENT_DIRTY;
    return FATXWriteDirent(v, block, e);
}
NTSTATUS FATXDeleteDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FATXMapDirent(v, f->Parent, f->DirentIndex, &e, &block, false);
    if (!NT_SUCCESS(status))
        return status;
    e->FileNameLength = FATX_DIRENT_DELETED;
    status = FATXWriteDirent(v, block, e);
    if (NT_SUCCESS(status) && f->FirstCluster)
        status = HDDFSFreeChain(v, f->FirstCluster);
    f->FirstCluster = 0;
    f->FileSize = 0;
    f->HintCluster = 0;
    return status;
}
NTSTATUS FATXIsDirectoryEmpty(HDDFS_VOLUME *v, HDDFS_FCB *dir) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FATXMapDirent(v, dir, i, &e, &block, false);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && FATXIsEnd(e)))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (FATXIsLive(e))
            return (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    }
}

// The directory operations of a FATX volume (HDDFS_VOLUME::Format).
const HDDFS_FORMAT FATXFormat = {FATXLookup, FATXReadEntry, FATXNextEntry, FATXInitDirectory, FATXRemoveDirent,
                                  FATXCreateDirent, FATXFlushFcb, FATXDeleteDirent, FATXIsDirectoryEmpty};
