// Open files: the tree of FCBs (only open files and their ancestors have one), shared by both formats.
#include <xk/hddfs.h>

HDDFS_FCB *HDDFSFindOpenFcb(HDDFS_FCB *dir, const CHAR *Name, ULONG Length) {
    for (PLIST_ENTRY l = dir->ChildList.Flink; l != &dir->ChildList; l = l->Flink) {
        auto f = CONTAINING_RECORD(l, HDDFS_FCB, SiblingLink);
        if (!(f->Flags & FCB_DELETE_PENDING) || f->CleanupCount) {
            if (HDDFSNameEqual(f->Name, f->NameLength, Name, Length))
                return f;
        }
    }
    return nullptr;
}

HDDFS_FCB *HDDFSCreateFcb(HDDFS_FCB *Parent, const HDDFS_ENTRY *e, ULONG Index) {
    auto f = (HDDFS_FCB *)ExAllocatePoolWithTag(sizeof(HDDFS_FCB), 'fxtF');
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
    f->FileSizeHigh = (f->Flags & FCB_DIRECTORY) ? 0 : e->FileSizeHigh;
    f->DirentIndex = Index;
    f->CreationTime = e->CreationTime;
    f->LastWriteTime = e->LastWriteTime;
    f->LastAccessTime = e->LastAccessTime;
    f->EXFATFlags = e->EXFATFlags;
    f->EXFATEntries = e->EXFATEntries;
    f->EXFATAllocClusters = e->EXFATAllocClusters;
    f->EXFATDirLength = e->EXFATDirLength;
    InsertTailList(&Parent->ChildList, &f->SiblingLink);
    Parent->ReferenceCount++;
    return f;
}

void HDDFSDereferenceFcb(HDDFS_FCB *f) {
    while (f && --f->ReferenceCount == 0 && !(f->Flags & (FCB_ROOT | FCB_VOLUME))) {
        HDDFS_FCB *parent = f->Parent;
        RemoveEntryList(&f->SiblingLink);
        ExFreePool(f);
        f = parent;
    }
}
