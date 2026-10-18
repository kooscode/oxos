// FATX: directory entries, FCB tree, file size changes (spec/stock/04b).
#include "fatx.h"


// Map entry Index of a directory. With Extend, a directory that ends exactly there grows by one cluster
// (filled with end markers).
NTSTATUS FatxMapDirent(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_DIRENT **Entry, LONGLONG *BlockOffset,
                       bool Extend) {
    ULONG byte = Index * sizeof(FATX_DIRENT);
    ULONG clusterIndex = byte >> v->ClusterShift, within = byte & (v->BytesPerCluster - 1);
    ULONG cluster;
    NTSTATUS status = FatxClusterAtIndex(v, dir, clusterIndex, &cluster);
    if (status == STATUS_END_OF_FILE && Extend) {
        ULONG count, last;
        status = FatxChainLength(v, dir->FirstCluster, &count, &last);
        if (!NT_SUCCESS(status))
            return status;
        if (count != clusterIndex)
            return STATUS_END_OF_FILE;
        if (count * v->BytesPerCluster >= 4096 * sizeof(FATX_DIRENT))
            return (NTSTATUS)0xC000007F; // directories hold at most 4096 entries
        status = FatxAllocateClusters(v, last, 1, &cluster);
        if (!NT_SUCCESS(status))
            return status;
        LONGLONG base = FatxClusterOffset(v, cluster);
        for (ULONG off = 0; off < v->BytesPerCluster && NT_SUCCESS(status); off += FATX_BLOCK_SIZE) {
            PUCHAR data;
            status = FatxMapBlock(v, base + off, &data);
            if (NT_SUCCESS(status)) {
                RtlFillMemory(data, FATX_BLOCK_SIZE, FATX_DIRENT_NEVER_USED2);
                status = FatxWriteBlockRange(v, base + off, 0, FATX_BLOCK_SIZE);
            }
        }
    }
    if (!NT_SUCCESS(status))
        return status;
    LONGLONG block = FatxClusterOffset(v, cluster) + (within & ~(FATX_BLOCK_SIZE - 1));
    PUCHAR data;
    status = FatxMapBlock(v, block, &data);
    if (!NT_SUCCESS(status))
        return status;
    *Entry = (FATX_DIRENT *)(data + (within & (FATX_BLOCK_SIZE - 1)));
    *BlockOffset = block;
    return STATUS_SUCCESS;
}

NTSTATUS FatxWriteDirent(FATX_VOLUME *v, LONGLONG BlockOffset, FATX_DIRENT *Entry) {
    PUCHAR data;
    NTSTATUS status = FatxMapBlock(v, BlockOffset, &data);
    if (!NT_SUCCESS(status))
        return status;
    return FatxWriteBlockRange(v, BlockOffset, (ULONG)((PUCHAR)Entry - data), sizeof(FATX_DIRENT));
}

static bool FatxIsEnd(const FATX_DIRENT *e) {
    return e->FileNameLength == FATX_DIRENT_NEVER_USED || e->FileNameLength == FATX_DIRENT_NEVER_USED2;
}

static bool FatxIsLive(const FATX_DIRENT *e) {
    return !FatxIsEnd(e) && e->FileNameLength != FATX_DIRENT_DELETED && e->FileNameLength <= FATX_MAX_NAME;
}

NTSTATUS FatxLookup(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, FATX_DIRENT *Found, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FatxMapDirent(v, dir, i, &e, &block, false);
        if (status == STATUS_END_OF_FILE)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(status))
            return status;
        if (FatxIsEnd(e))
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (FatxIsLive(e) && FatxNameEqual(e->FileName, e->FileNameLength, Name->Buffer, Name->Length)) {
            *Found = *e;
            *Index = i;
            return STATUS_SUCCESS;
        }
    }
}

NTSTATUS FatxCreateDirent(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                          ULONG FirstCluster, ULONG *Index) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FatxMapDirent(v, dir, i, &e, &block, true);
        if (!NT_SUCCESS(status))
            return status;
        bool end = FatxIsEnd(e);
        if (!end && e->FileNameLength != FATX_DIRENT_DELETED)
            continue;
        ULONG now = FatxCurrentTimestamp();
        RtlZeroMemory(e, sizeof(*e));
        e->FileNameLength = (UCHAR)Name->Length;
        e->FileAttributes = Attributes;
        RtlCopyMemory(e->FileName, Name->Buffer, Name->Length);
        RtlFillMemory(e->FileName + Name->Length, FATX_MAX_NAME - Name->Length, 0xFF);
        e->FirstCluster = FirstCluster;
        e->CreationTime = e->LastWriteTime = e->LastAccessTime = now;
        status = FatxWriteDirent(v, block, e);
        if (!NT_SUCCESS(status))
            return status;
        if (end) {
            // The entry after the old end marker must still terminate the directory.
            FATX_DIRENT *next;
            LONGLONG nextBlock;
            status = FatxMapDirent(v, dir, i + 1, &next, &nextBlock, true);
            if (NT_SUCCESS(status) && !FatxIsEnd(next)) {
                RtlFillMemory(next, sizeof(*next), FATX_DIRENT_NEVER_USED2);
                status = FatxWriteDirent(v, nextBlock, next);
            }
            if (!NT_SUCCESS(status) && status != (NTSTATUS)0xC000007F)
                return status;
        }
        *Index = i;
        return STATUS_SUCCESS;
    }
}

// Write the FCB's size/cluster/attributes/times back into its directory entry.
NTSTATUS FatxFlushFcb(FATX_VOLUME *v, FATX_FCB *f) {
    if (f->Flags & (FCB_ROOT | FCB_VOLUME))
        return STATUS_SUCCESS;
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FatxMapDirent(v, f->Parent, f->DirentIndex, &e, &block, false);
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
    return FatxWriteDirent(v, block, e);
}

NTSTATUS FatxDeleteDirent(FATX_VOLUME *v, FATX_FCB *f) {
    FATX_DIRENT *e;
    LONGLONG block;
    NTSTATUS status = FatxMapDirent(v, f->Parent, f->DirentIndex, &e, &block, false);
    if (!NT_SUCCESS(status))
        return status;
    e->FileNameLength = FATX_DIRENT_DELETED;
    status = FatxWriteDirent(v, block, e);
    if (NT_SUCCESS(status) && f->FirstCluster)
        status = FatxFreeChain(v, f->FirstCluster);
    f->FirstCluster = 0;
    f->FileSize = 0;
    f->HintCluster = 0;
    return status;
}

NTSTATUS FatxIsDirectoryEmpty(FATX_VOLUME *v, FATX_FCB *dir) {
    for (ULONG i = 0;; i++) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS status = FatxMapDirent(v, dir, i, &e, &block, false);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && FatxIsEnd(e)))
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(status))
            return status;
        if (FatxIsLive(e))
            return (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    }
}

// --- FCB tree (only open files and their ancestors have FCBs)

FATX_FCB *FatxFindOpenFcb(FATX_FCB *dir, const CHAR *Name, ULONG Length) {
    for (PLIST_ENTRY l = dir->ChildList.Flink; l != &dir->ChildList; l = l->Flink) {
        auto f = CONTAINING_RECORD(l, FATX_FCB, SiblingLink);
        if (!(f->Flags & FCB_DELETE_PENDING) || f->CleanupCount)
            if (FatxNameEqual(f->Name, f->NameLength, Name, Length))
                return f;
    }
    return nullptr;
}

FATX_FCB *FatxCreateFcb(FATX_FCB *Parent, const FATX_DIRENT *e, ULONG Index) {
    auto f = (FATX_FCB *)ExAllocatePoolWithTag(sizeof(FATX_FCB), 'fxtF');
    if (!f)
        return nullptr;
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->ChildList);
    f->Parent = Parent;
    f->Flags = (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FCB_DIRECTORY : 0;
    f->Attributes = e->FileAttributes;
    f->NameLength = e->FileNameLength;
    RtlCopyMemory(f->Name, e->FileName, e->FileNameLength);
    f->FirstCluster = e->FirstCluster;
    f->FileSize = (f->Flags & FCB_DIRECTORY) ? 0 : e->FileSize;
    f->DirentIndex = Index;
    f->CreationTime = e->CreationTime;
    f->LastWriteTime = e->LastWriteTime;
    f->LastAccessTime = e->LastAccessTime;
    InsertTailList(&Parent->ChildList, &f->SiblingLink);
    Parent->ReferenceCount++;
    return f;
}

void FatxDereferenceFcb(FATX_FCB *f) {
    while (f && --f->ReferenceCount == 0 && !(f->Flags & (FCB_ROOT | FCB_VOLUME))) {
        FATX_FCB *parent = f->Parent;
        RemoveEntryList(&f->SiblingLink);
        ExFreePool(f);
        f = parent;
    }
}

// Grow or shrink the cluster chain to cover NewSize bytes.
NTSTATUS FatxSetFileSize(FATX_VOLUME *v, FATX_FCB *f, ULONG NewSize) {
    ULONG need = (ULONG)(((ULONGLONG)NewSize + v->BytesPerCluster - 1) >> v->ClusterShift);
    ULONG have = 0, last = 0;
    NTSTATUS status = f->FirstCluster ? FatxChainLength(v, f->FirstCluster, &have, &last) : STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
        return status;
    if (need > have) {
        ULONG first;
        status = FatxAllocateClusters(v, last, need - have, &first);
        if (!NT_SUCCESS(status))
            return status;
        if (!f->FirstCluster)
            f->FirstCluster = first;
    } else if (need < have) {
        f->HintCluster = 0;
        if (need == 0) {
            status = FatxFreeChain(v, f->FirstCluster);
            f->FirstCluster = 0;
        } else {
            ULONG keep;
            status = FatxClusterAtIndex(v, f, need - 1, &keep);
            ULONG next = 0;
            if (NT_SUCCESS(status))
                status = FatxGetFatEntry(v, keep, &next);
            if (NT_SUCCESS(status))
                status = FatxSetFatEntry(v, keep, FATX_CLUSTER_LAST);
            if (NT_SUCCESS(status))
                status = FatxFreeChain(v, next);
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    f->FileSize = NewSize;
    f->Flags |= FCB_DIRENT_DIRTY;
    return FatxFlushFcb(v, f);
}
