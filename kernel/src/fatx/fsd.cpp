// FATX: IRP dispatch — create, cleanup/close, read/write, information, directory and volume queries (spec/stock/04b).
#include "fatx.h"

static NTSTATUS FatxComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information = 0) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static FATX_FCB *FatxFcbOf(PFILE_OBJECT f) { return (FATX_FCB *)f->FsContext; }

// Access allowed by the attributes (read-only files and directories refuse data writes and deletion).
static NTSTATUS FatxCheckAttributes(ACCESS_MASK access, ULONG options, UCHAR attributes) {
    if ((attributes & FILE_ATTRIBUTE_READONLY) &&
        ((access & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || (options & FILE_DELETE_ON_CLOSE)))
        return STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}

static void FatxSplitElement(OBJECT_STRING *rest, OBJECT_STRING *element) {
    if (rest->Length && rest->Buffer[0] == '\\') {
        rest->Buffer++;
        rest->Length--;
    }
    USHORT n = 0;
    while (n < rest->Length && rest->Buffer[n] != '\\')
        n++;
    *element = {n, n, rest->Buffer};
    rest->Buffer += n;
    rest->Length = (USHORT)(rest->Length - n);
}

// Opens hold one FCB reference (dropped at CLOSE) and one cleanup count (dropped at CLEANUP).
static NTSTATUS FatxOpenFcb(FATX_VOLUME *v, PIO_STACK_LOCATION s, FATX_FCB *f, PFILE_OBJECT fo) {
    ACCESS_MASK access = s->Parameters.Create.DesiredAccess;
    ULONG share = s->Parameters.Create.ShareAccess;
    NTSTATUS status = f->CleanupCount ? IoCheckShareAccess(access, share, fo, &f->ShareAccess, TRUE)
                                      : (IoSetShareAccess(access, share, fo, &f->ShareAccess), STATUS_SUCCESS);
    if (!NT_SUCCESS(status))
        return status;
    auto ccb = (FATX_CCB *)ExAllocatePoolWithTag(sizeof(FATX_CCB), 'cxtF');
    if (!ccb) {
        IoRemoveShareAccess(fo, &f->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ccb, sizeof(*ccb));
    f->ReferenceCount++;
    f->CleanupCount++;
    fo->FsContext = f;
    fo->FsContext2 = ccb;
    return STATUS_SUCCESS;
}

static NTSTATUS FatxCommonCreate(FATX_VOLUME *v, PIRP Irp, ULONG_PTR *Information) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    ULONG options = s->Parameters.Create.Options & 0xFFFFFF, disposition = s->Parameters.Create.Options >> 24;
    UCHAR attributes = (UCHAR)(s->Parameters.Create.FileAttributes & 0x37) & ~FILE_ATTRIBUTE_DIRECTORY;
    OBJECT_STRING rest = *s->Parameters.Create.RemainingName;
    bool openTarget = s->Flags & 4; // SL_OPEN_TARGET_DIRECTORY (rename)

    if (v->Dismounted)
        return (NTSTATUS)0xC000026E; // STATUS_VOLUME_DISMOUNTED
    if (options & 0x2000)
        return (NTSTATUS)0xC0000002; // FILE_OPEN_BY_FILE_ID: STATUS_NOT_IMPLEMENTED
    if (Irp->Overlay.AllocationSize.HighPart)
        return STATUS_INVALID_PARAMETER;

    FATX_FCB *dir;
    PFILE_OBJECT related = fo->RelatedFileObject;
    if (related) {
        dir = FatxFcbOf(related);
        if (!dir || !(dir->Flags & FCB_DIRECTORY))
            return STATUS_INVALID_PARAMETER;
        if (dir->Flags & FCB_DELETE_PENDING)
            return (NTSTATUS)0xC0000056; // STATUS_DELETE_PENDING
        if (rest.Length && rest.Buffer[0] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
    } else {
        if (!rest.Length) { // the volume itself
            if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF)
                return STATUS_ACCESS_DENIED;
            if (options & FILE_DIRECTORY_FILE)
                return (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
            if (options & FILE_DELETE_ON_CLOSE)
                return (NTSTATUS)0xC0000121; // STATUS_CANNOT_DELETE
            NTSTATUS status = FatxOpenFcb(v, s, &v->VolumeFcb, fo);
            *Information = FILE_OPENED;
            return status;
        }
        if (rest.Buffer[0] != '\\')
            return STATUS_OBJECT_NAME_INVALID;
        dir = &v->RootFcb;
    }
    if (rest.Length > FATX_MAX_PATH)
        return STATUS_OBJECT_NAME_INVALID;
    bool trailingSlash = rest.Length > 1 && rest.Buffer[rest.Length - 1] == '\\';
    if (trailingSlash)
        rest.Length--;

    // Opening the root directory ("\") or the related directory itself (empty name).
    if (!rest.Length || (rest.Length == 1 && rest.Buffer[0] == '\\' && !related)) {
        if (openTarget)
            return STATUS_INVALID_PARAMETER;
        if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF)
            return STATUS_OBJECT_NAME_COLLISION;
        if (options & FILE_NON_DIRECTORY_FILE)
            return (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
        *Information = FILE_OPENED;
        return FatxOpenFcb(v, s, dir, fo);
    }

    // Walk to the parent of the final element, creating FCBs for the directories on the way.
    FATX_FCB *parent = dir;
    parent->ReferenceCount++; // held while walking
    OBJECT_STRING element;
    NTSTATUS status = STATUS_SUCCESS;
    // A doubled separator ("dir\\name") is accepted once, as the reference does: its walk through already-open
    // directories stops at the first "\\" or at the first directory it must look up on disk, and the slower walk
    // that takes over drops one leading '\' of the rest; after that a doubled separator is invalid. Brute Force
    // opens its textures as "z:\splash_screen\\textures-splash_screen.tex".
    bool slow = false;
    for (;;) {
        FatxSplitElement(&rest, &element);
        if (!FatxIsValidName(&element)) {
            status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        if (rest.Length >= 2 && rest.Buffer[1] == '\\') {
            if (slow) {
                status = STATUS_OBJECT_NAME_INVALID;
                break;
            }
            rest.Buffer++;
            rest.Length--;
            slow = true;
        }
        if (!rest.Length)
            break;
        FATX_FCB *child = FatxFindOpenFcb(parent, element.Buffer, element.Length);
        if (!child) {
            slow = true;
            FATX_DIRENT e;
            ULONG index;
            status = FatxLookup(v, parent, &element, &e, &index);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND)
                status = STATUS_OBJECT_PATH_NOT_FOUND;
            if (NT_SUCCESS(status) && !(e.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                status = STATUS_OBJECT_PATH_NOT_FOUND;
            if (NT_SUCCESS(status) && !(child = FatxCreateFcb(parent, &e, index)))
                status = STATUS_INSUFFICIENT_RESOURCES;
            if (!NT_SUCCESS(status))
                break;
        } else if (!(child->Flags & FCB_DIRECTORY)) {
            status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }
        child->ReferenceCount++;
        FatxDereferenceFcb(parent);
        parent = child;
    }
    if (!NT_SUCCESS(status)) {
        FatxDereferenceFcb(parent);
        return status;
    }

    if (openTarget) {
        // Rename target: open the parent directory; Information says whether the final name exists.
        FATX_DIRENT e;
        ULONG index;
        NTSTATUS look = FatxLookup(v, parent, &element, &e, &index);
        status = FatxOpenFcb(v, s, parent, fo);
        *Information = NT_SUCCESS(look) ? FILE_OPENED /*FILE_EXISTS*/ : FILE_CREATED /*FILE_DOES_NOT_EXIST*/;
        FatxDereferenceFcb(parent);
        return status;
    }

    ACCESS_MASK access = s->Parameters.Create.DesiredAccess;
    FATX_FCB *f = FatxFindOpenFcb(parent, element.Buffer, element.Length);
    bool created = false;
    if (f && (f->Flags & FCB_DELETE_PENDING)) {
        FatxDereferenceFcb(parent);
        return (NTSTATUS)0xC0000056; // STATUS_DELETE_PENDING
    }
    if (!f) {
        FATX_DIRENT e;
        ULONG index;
        status = FatxLookup(v, parent, &element, &e, &index);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            if (parent->Flags & FCB_DELETE_PENDING)
                status = (NTSTATUS)0xC0000056;
            else if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
                status = STATUS_OBJECT_NAME_NOT_FOUND;
            else if (trailingSlash && (options & FILE_NON_DIRECTORY_FILE))
                status = STATUS_OBJECT_NAME_INVALID;
            else {
                bool isDir = options & FILE_DIRECTORY_FILE;
                UCHAR attrs = (UCHAR)(attributes | (isDir ? FILE_ATTRIBUTE_DIRECTORY : 0));
                status = FatxCheckAttributes(access, options, attrs);
                ULONG first = 0;
                if (NT_SUCCESS(status) && isDir) {
                    // A new directory gets one cluster of end markers.
                    status = FatxAllocateClusters(v, 0, 1, &first);
                    LONGLONG base = NT_SUCCESS(status) ? FatxClusterOffset(v, first) : 0;
                    for (ULONG off = 0; NT_SUCCESS(status) && off < v->BytesPerCluster; off += FATX_BLOCK_SIZE) {
                        PUCHAR data;
                        status = FatxMapBlock(v, base + off, &data);
                        if (NT_SUCCESS(status)) {
                            RtlFillMemory(data, FATX_BLOCK_SIZE, FATX_DIRENT_NEVER_USED2);
                            status = FatxWriteBlockRange(v, base + off, 0, FATX_BLOCK_SIZE);
                        }
                    }
                }
                if (NT_SUCCESS(status))
                    status = FatxCreateDirent(v, parent, &element, attrs, first, &index);
                if (!NT_SUCCESS(status) && first)
                    FatxFreeChain(v, first);
                if (NT_SUCCESS(status)) {
                    FATX_DIRENT *pe;
                    LONGLONG block;
                    status = FatxMapDirent(v, parent, index, &pe, &block, false);
                    if (NT_SUCCESS(status) && !(f = FatxCreateFcb(parent, pe, index)))
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    created = true;
                    if (NT_SUCCESS(status) && Irp->Overlay.AllocationSize.LowPart && !isDir) {
                        // Preallocate; the file size stays 0.
                        status = FatxSetFileSize(v, f, Irp->Overlay.AllocationSize.LowPart);
                        f->FileSize = 0;
                        FatxFlushFcb(v, f);
                    }
                }
            }
        } else if (NT_SUCCESS(status) && !(f = FatxCreateFcb(parent, &e, index))) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        if (!NT_SUCCESS(status)) {
            FatxDereferenceFcb(parent);
            return status;
        }
    }

    if (!created) {
        if (!(f->Flags & FCB_DIRECTORY)) {
            if (disposition == FILE_CREATE)
                status = STATUS_OBJECT_NAME_COLLISION;
            else if (trailingSlash || (options & FILE_DIRECTORY_FILE))
                status = (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
        } else if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF) {
            status = STATUS_OBJECT_NAME_COLLISION;
        } else if (options & FILE_NON_DIRECTORY_FILE) {
            status = (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
        }
        if (NT_SUCCESS(status))
            status = FatxCheckAttributes(access, options, f->Attributes);
    }
    ULONG info = created ? FILE_CREATED : FILE_OPENED;
    if (NT_SUCCESS(status))
        status = FatxOpenFcb(v, s, f, fo);
    if (NT_SUCCESS(status) && !created &&
        (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF)) {
        f->Attributes = (UCHAR)(attributes | FILE_ATTRIBUTE_ARCHIVE);
        f->LastWriteTime = FatxCurrentTimestamp();
        status = FatxSetFileSize(v, f, 0);
        if (NT_SUCCESS(status))
            info = disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        else {
            IoRemoveShareAccess(fo, &f->ShareAccess);
            f->CleanupCount--;
            f->ReferenceCount--;
            ExFreePool(fo->FsContext2);
            fo->FsContext = fo->FsContext2 = nullptr;
        }
    }
    if (NT_SUCCESS(status) && (options & FILE_DELETE_ON_CLOSE) && !(f->Flags & FCB_DIRECTORY))
        f->Flags |= FCB_DELETE_PENDING;
    if (!NT_SUCCESS(status) && f && f->ReferenceCount == 0) {
        f->ReferenceCount++; // drop an FCB nobody uses
        FatxDereferenceFcb(f);
    }
    FatxDereferenceFcb(parent);
    *Information = info;
    return status;
}

static NTSTATUS NTAPI FatxFsdCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    FatxAcquireVolume(v);
    ULONG_PTR info = 0;
    NTSTATUS status = FatxCommonCreate(v, Irp, &info);
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status, NT_SUCCESS(status) ? info : 0);
}

static NTSTATUS NTAPI FatxFsdCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    NTSTATUS status = STATUS_SUCCESS;
    if (f) {
        FatxAcquireVolume(v);
        IoRemoveShareAccess(fo, &f->ShareAccess);
        if (--f->CleanupCount == 0 && (f->Flags & FCB_DELETE_PENDING) && !(f->Flags & (FCB_ROOT | FCB_VOLUME)))
            status = FatxDeleteDirent(v, f);
        else if (f->Flags & FCB_DIRENT_DIRTY)
            status = FatxFlushFcb(v, f);
        FatxReleaseVolume(v);
    }
    return FatxComplete(Irp, status);
}

static NTSTATUS NTAPI FatxFsdClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    if (f) {
        FatxAcquireVolume(v);
        if (fo->FsContext2)
            ExFreePool(fo->FsContext2);
        fo->FsContext = fo->FsContext2 = nullptr;
        FatxDereferenceFcb(f);
        FatxReleaseVolume(v);
    }
    return FatxComplete(Irp, STATUS_SUCCESS);
}

// --- Read / write

static NTSTATUS FatxTransfer(FATX_VOLUME *v, FATX_FCB *f, bool write, ULONG offset, PUCHAR buffer, ULONG length) {
    while (length) {
        ULONG within = offset & (v->BytesPerCluster - 1), cluster;
        NTSTATUS status = FatxClusterAtIndex(v, f, offset >> v->ClusterShift, &cluster);
        if (!NT_SUCCESS(status))
            return status == STATUS_END_OF_FILE ? (NTSTATUS)0xC0000032 : status;
        // Extend the run over physically consecutive clusters.
        ULONG run = v->BytesPerCluster - within, c = cluster;
        while (run < length) {
            ULONG next;
            status = FatxGetFatEntry(v, c, &next);
            if (!NT_SUCCESS(status) || next != c + 1)
                break;
            c = next;
            run += v->BytesPerCluster;
        }
        if (run > length)
            run = length;
        LONGLONG disk = FatxClusterOffset(v, cluster) + within;
        ULONG mask = v->SectorSize - 1;
        if (!(disk & mask) && !(run & mask) && !((ULONG)buffer & 3)) {
            if (write)
                FatxInvalidateRange(v, disk, run);
            status = FatxDeviceIo(v, write ? IRP_MJ_WRITE : IRP_MJ_READ, disk, buffer, run);
        } else {
            // Partial sectors go through the block cache.
            run = run < FATX_BLOCK_SIZE - (ULONG)(disk & (FATX_BLOCK_SIZE - 1))
                      ? run
                      : FATX_BLOCK_SIZE - (ULONG)(disk & (FATX_BLOCK_SIZE - 1));
            LONGLONG block = disk & ~(LONGLONG)(FATX_BLOCK_SIZE - 1);
            ULONG start = (ULONG)(disk - block);
            PUCHAR data;
            status = FatxMapBlock(v, block, &data);
            if (NT_SUCCESS(status)) {
                if (write) {
                    RtlCopyMemory(data + start, buffer, run);
                    status = FatxWriteBlockRange(v, block, start, run);
                } else {
                    RtlCopyMemory(buffer, data + start, run);
                }
            }
        }
        if (!NT_SUCCESS(status))
            return status;
        offset += run;
        buffer += run;
        length -= run;
    }
    return STATUS_SUCCESS;
}

// Scatter/gather: one page-sized piece of the transfer per segment element (spec/stock/11 §5).
static NTSTATUS FatxTransferIrp(FATX_VOLUME *v, FATX_FCB *f, PIRP Irp, bool write, ULONG offset, PUCHAR buffer,
                                ULONG length) {
    if (!Irp->SegmentArray)
        return FatxTransfer(v, f, write, offset, buffer, length);
    for (ULONG i = 0; length; i++) {
        ULONG run = length < PAGE_SIZE ? length : PAGE_SIZE;
        NTSTATUS status = FatxTransfer(v, f, write, offset, (PUCHAR)Irp->SegmentArray[i].Buffer, run);
        if (!NT_SUCCESS(status))
            return status;
        offset += run;
        length -= run;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI FatxFsdReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    bool write = s->MajorFunction == IRP_MJ_WRITE;
    ULONG length = s->Parameters.Read.Length;
    LARGE_INTEGER offset = s->Parameters.Read.ByteOffset;
    PUCHAR buffer = (PUCHAR)Irp->UserBuffer;
    if (!f || (f->Flags & FCB_DIRECTORY))
        return FatxComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    if (f->Flags & FCB_VOLUME) {
        // Raw access to the partition through a volume handle: pass down unchanged.
        IoCopyCurrentIrpStackLocationToNext(Irp);
        return IofCallDriver(v->TargetDevice, Irp);
    }
    // Non-cached and scatter/gather transfers must be sector-aligned (offset and requested length). For reads the
    // reference checks this only after the end-of-file test: a read at the end of a file whose size is not whole
    // sectors reports STATUS_END_OF_FILE, not an invalid parameter (the stock dashboard reads files that way).
    bool unaligned = ((fo->Flags & FO_NO_INTERMEDIATE_BUFFERING) || Irp->SegmentArray) &&
                     ((offset.LowPart | length) & (v->SectorSize - 1));
    if (write && unaligned)
        return FatxComplete(Irp, STATUS_INVALID_PARAMETER);
    FatxAcquireVolume(v);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG done = 0;
    if (write) {
        if (offset.HighPart == -1 && offset.LowPart == 0xFFFFFFFF) // FILE_WRITE_TO_END_OF_FILE
            offset.QuadPart = f->FileSize;
        ULONGLONG end = (ULONGLONG)offset.QuadPart + length;
        if (offset.HighPart || end > 0xFFFFFFFFull)
            status = (NTSTATUS)0xC000007F; // beyond the 4 GB FATX limit: STATUS_DISK_FULL
        else if (length && end > f->FileSize)
            status = FatxSetFileSize(v, f, (ULONG)end);
        if (NT_SUCCESS(status) && length)
            status = FatxTransferIrp(v, f, Irp, true, offset.LowPart, buffer, length);
        if (NT_SUCCESS(status)) {
            done = length;
            f->LastWriteTime = FatxCurrentTimestamp();
            f->Attributes |= FILE_ATTRIBUTE_ARCHIVE;
            f->Flags |= FCB_DIRENT_DIRTY;
        }
    } else {
        if (offset.HighPart || offset.LowPart >= f->FileSize) {
            status = length ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        } else if (unaligned) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            done = f->FileSize - offset.LowPart < length ? f->FileSize - offset.LowPart : length;
            status = FatxTransferIrp(v, f, Irp, false, offset.LowPart, buffer, done);
            if (!NT_SUCCESS(status))
                done = 0;
        }
    }
    if (NT_SUCCESS(status) && (fo->Flags & FO_SYNCHRONOUS_IO))
        fo->CurrentByteOffset.QuadPart = offset.QuadPart + done;
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status, done);
}

// --- Information

static void FatxFillTimes(FATX_FCB *f, PLARGE_INTEGER times /*creation, access, write, change*/) {
    FatxTimestampToTime(f->CreationTime, &times[0]);
    FatxTimestampToTime(f->LastAccessTime, &times[1]);
    FatxTimestampToTime(f->LastWriteTime, &times[2]);
    times[3] = times[2];
}

static ULONG FatxAttributesOut(UCHAR a) { return a ? a : FILE_ATTRIBUTE_NORMAL; }

static ULONG FatxAllocationSize(FATX_VOLUME *v, FATX_FCB *f) {
    if (f->Flags & FCB_DIRECTORY)
        return 0;
    return (ULONG)(((ULONGLONG)f->FileSize + v->BytesPerCluster - 1) & ~(ULONGLONG)(v->BytesPerCluster - 1));
}

static ULONG FatxFullName(FATX_FCB *f, PCHAR out, ULONG max) {
    if (f->Flags & FCB_ROOT) {
        if (max)
            out[0] = '\\';
        return 1;
    }
    ULONG n = FatxFullName(f->Parent, out, max);
    if (f->Parent->Flags & FCB_ROOT)
        n = 0;
    if (n < max)
        out[n] = '\\';
    n++;
    for (ULONG i = 0; i < f->NameLength; i++, n++)
        if (n < max)
            out[n] = f->Name[i];
    return n;
}

static NTSTATUS NTAPI FatxFsdQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    ULONG len = s->Parameters.QueryFile.Length;
    auto out = (PUCHAR)Irp->UserBuffer;
    if (!f || (f->Flags & FCB_VOLUME))
        return FatxComplete(Irp, STATUS_INVALID_PARAMETER);
    FatxAcquireVolume(v);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;
    LARGE_INTEGER times[4];
    FatxFillTimes(f, times);
    switch (s->Parameters.QueryFile.FileInformationClass) {
    case FileBasicInformation:
        RtlCopyMemory(out, times, 32);
        *(PULONG)(out + 32) = FatxAttributesOut(f->Attributes);
        info = 40;
        break;
    case FileStandardInformation: {
        auto p = (PULONG)out;
        p[0] = FatxAllocationSize(v, f), p[1] = 0;
        p[2] = f->FileSize, p[3] = 0;
        p[4] = 1;
        out[20] = (f->Flags & FCB_DELETE_PENDING) != 0;
        out[21] = (f->Flags & FCB_DIRECTORY) != 0;
        info = 24;
        break;
    }
    case FileInternalInformation:
        ((PULONG)out)[0] = f->FirstCluster;
        ((PULONG)out)[1] = 0;
        info = 8;
        break;
    case FileEaInformation:
        *(PULONG)out = 0;
        info = 4;
        break;
    case FilePositionInformation:
        *(PLARGE_INTEGER)out = fo->CurrentByteOffset;
        info = 8;
        break;
    case FileNameInformation: {
        ULONG n = FatxFullName(f, (PCHAR)out + 4, len - 4);
        *(PULONG)out = n;
        info = 4 + (n < len - 4 ? n : len - 4);
        if (n > len - 4)
            status = (NTSTATUS)0x80000005; // STATUS_BUFFER_OVERFLOW
        break;
    }
    case FileNetworkOpenInformation: {
        RtlCopyMemory(out, times, 32);
        auto p = (PULONG)(out + 32);
        p[0] = FatxAllocationSize(v, f), p[1] = 0;
        p[2] = f->FileSize, p[3] = 0;
        p[4] = FatxAttributesOut(f->Attributes);
        p[5] = 0;
        info = 56;
        break;
    }
    case FileAllInformation: {
        // Basic, Standard, Internal, Ea, Access, Position, Mode, Alignment, Name.
        RtlZeroMemory(out, 0x68);
        RtlCopyMemory(out, times, 32);
        *(PULONG)(out + 32) = FatxAttributesOut(f->Attributes);
        *(PULONG)(out + 40) = FatxAllocationSize(v, f);
        *(PULONG)(out + 48) = f->FileSize;
        *(PULONG)(out + 56) = 1;
        out[60] = (f->Flags & FCB_DELETE_PENDING) != 0;
        out[61] = (f->Flags & FCB_DIRECTORY) != 0;
        *(PULONG)(out + 64) = f->FirstCluster;
        *(PLARGE_INTEGER)(out + 80) = fo->CurrentByteOffset;
        ULONG room = len - 0x64;
        ULONG n = FatxFullName(f, (PCHAR)out + 0x64, room);
        *(PULONG)(out + 0x60) = n;
        info = 0x64 + (n < room ? n : room);
        if (n > room)
            status = (NTSTATUS)0x80000005;
        break;
    }
    default:
        status = STATUS_INVALID_PARAMETER;
    }
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status, info);
}

static NTSTATUS FatxRename(FATX_VOLUME *v, FATX_FCB *f, PFILE_OBJECT target, PUCHAR in) {
    // FILE_RENAME_INFORMATION: {BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING FileName}
    BOOLEAN replace = in[0];
    auto name = (POBJECT_STRING)(in + 8);
    OBJECT_STRING rest = *name, element;
    // The I/O manager resolved the target directory when one is given (RootDirectory or an absolute name).
    FATX_FCB *dir = target ? FatxFcbOf(target) : f->Parent;
    while (rest.Length) // final element of the new name
        FatxSplitElement(&rest, &element);
    if (!dir || !FatxIsValidName(&element) || (f->Flags & FCB_ROOT))
        return STATUS_OBJECT_NAME_INVALID;
    for (FATX_FCB *p = dir; p; p = p->Parent)
        if (p == f)
            return STATUS_INVALID_PARAMETER; // into its own subtree
    if (f->Flags & FCB_DIRECTORY && !IsListEmpty(&f->ChildList))
        return STATUS_ACCESS_DENIED; // open children
    FATX_DIRENT existing;
    ULONG index;
    NTSTATUS status = FatxLookup(v, dir, &element, &existing, &index);
    bool same = dir == f->Parent && index == f->DirentIndex;
    if (NT_SUCCESS(status) && !same) {
        if (!replace || (existing.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            FatxFindOpenFcb(dir, element.Buffer, element.Length))
            return STATUS_OBJECT_NAME_COLLISION;
        FATX_FCB tmp = {};
        tmp.Parent = dir;
        tmp.DirentIndex = index;
        tmp.FirstCluster = existing.FirstCluster;
        status = FatxDeleteDirent(v, &tmp);
        if (!NT_SUCCESS(status))
            return status;
    } else if (status != STATUS_OBJECT_NAME_NOT_FOUND && !NT_SUCCESS(status)) {
        return status;
    }
    if (same || dir == f->Parent) {
        f->NameLength = (UCHAR)element.Length;
        RtlCopyMemory(f->Name, element.Buffer, element.Length);
        return FatxFlushFcb(v, f);
    }
    // Move: new entry in the target directory, then drop the old one (the clusters move with it).
    status = FatxCreateDirent(v, dir, &element, f->Attributes, f->FirstCluster, &index);
    if (!NT_SUCCESS(status))
        return status;
    FATX_DIRENT *old;
    LONGLONG block;
    status = FatxMapDirent(v, f->Parent, f->DirentIndex, &old, &block, false);
    if (NT_SUCCESS(status)) {
        old->FileNameLength = FATX_DIRENT_DELETED;
        status = FatxWriteDirent(v, block, old);
    }
    RemoveEntryList(&f->SiblingLink);
    FATX_FCB *oldParent = f->Parent;
    InsertTailList(&dir->ChildList, &f->SiblingLink);
    dir->ReferenceCount++;
    f->Parent = dir;
    f->DirentIndex = index;
    f->NameLength = (UCHAR)element.Length;
    RtlCopyMemory(f->Name, element.Buffer, element.Length);
    FatxDereferenceFcb(oldParent);
    if (NT_SUCCESS(status))
        status = FatxFlushFcb(v, f);
    return status;
}

static NTSTATUS NTAPI FatxFsdSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    auto in = (PUCHAR)Irp->UserBuffer;
    if (!f || (f->Flags & FCB_VOLUME))
        return FatxComplete(Irp, STATUS_INVALID_PARAMETER);
    FatxAcquireVolume(v);
    NTSTATUS status = STATUS_SUCCESS;
    switch (s->Parameters.SetFile.FileInformationClass) {
    case FileBasicInformation: {
        auto t = (PLARGE_INTEGER)in;
        if (t[0].QuadPart)
            f->CreationTime = FatxTimeToTimestamp(&t[0]);
        if (t[1].QuadPart)
            f->LastAccessTime = FatxTimeToTimestamp(&t[1]);
        if (t[2].QuadPart)
            f->LastWriteTime = FatxTimeToTimestamp(&t[2]);
        ULONG a = *(PULONG)(in + 32);
        if (a) {
            a &= 0x27; // READONLY HIDDEN SYSTEM ARCHIVE
            f->Attributes = (UCHAR)(a | (f->Flags & FCB_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : 0));
        }
        status = FatxFlushFcb(v, f);
        break;
    }
    case FileDispositionInformation:
        if (!*in) {
            f->Flags &= ~FCB_DELETE_PENDING;
        } else if (f->Flags & FCB_ROOT) {
            status = (NTSTATUS)0xC0000121; // STATUS_CANNOT_DELETE
        } else if (f->Attributes & FILE_ATTRIBUTE_READONLY) {
            status = (NTSTATUS)0xC0000121;
        } else if (f->Flags & FCB_DIRECTORY) {
            status = FatxIsDirectoryEmpty(v, f);
            if (NT_SUCCESS(status))
                f->Flags |= FCB_DELETE_PENDING;
        } else {
            f->Flags |= FCB_DELETE_PENDING;
        }
        break;
    case FilePositionInformation:
        fo->CurrentByteOffset = *(PLARGE_INTEGER)in;
        break;
    case FileEndOfFileInformation:
    case FileAllocationInformation: {
        LARGE_INTEGER size = *(PLARGE_INTEGER)in;
        if (f->Flags & FCB_DIRECTORY)
            status = STATUS_INVALID_PARAMETER;
        else if (size.HighPart)
            status = (NTSTATUS)0xC000007F;
        else if (s->Parameters.SetFile.FileInformationClass == FileEndOfFileInformation ||
                 size.LowPart < f->FileSize)
            status = FatxSetFileSize(v, f, size.LowPart);
        else { // allocation only grows the chain
            ULONG keep = f->FileSize;
            status = FatxSetFileSize(v, f, size.LowPart);
            f->FileSize = keep;
            FatxFlushFcb(v, f);
        }
        break;
    }
    case FileRenameInformation:
        status = FatxRename(v, f, s->Parameters.SetFile.FileObject, in);
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
    }
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status);
}

// --- Directory enumeration (IRP_MJ_DIRECTORY_CONTROL / IRP_MN_QUERY_DIRECTORY)

static NTSTATUS NTAPI FatxFsdDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    FATX_FCB *f = FatxFcbOf(fo);
    auto ccb = (FATX_CCB *)fo->FsContext2;
    if (s->MinorFunction != 1 || !f || !(f->Flags & FCB_DIRECTORY))
        return FatxComplete(Irp, STATUS_INVALID_PARAMETER);
    FILE_INFORMATION_CLASS cls = s->Parameters.QueryDirectory.FileInformationClass;
    ULONG fixed = cls == FileNamesInformation ? 12 : cls == FileDirectoryInformation ? 0x40 : 0;
    if (!fixed)
        return FatxComplete(Irp, STATUS_INVALID_INFO_CLASS);
    FatxAcquireVolume(v);
    bool first = !ccb->HaveMask;
    POBJECT_STRING mask = s->Parameters.QueryDirectory.FileName;
    if (first || (s->Flags & SL_RESTART_SCAN)) {
        ccb->NextIndex = 0;
        if ((mask && mask->Length) || first) {
            ccb->HaveMask = TRUE;
            ccb->MaskLength = 1;
            ccb->Mask[0] = '*';
            if (mask && mask->Length && mask->Length <= FATX_MAX_NAME + 2) {
                ccb->MaskLength = (UCHAR)mask->Length;
                RtlCopyMemory(ccb->Mask, mask->Buffer, mask->Length);
            }
        }
        first = true;
    }
    auto out = (PUCHAR)Irp->UserBuffer;
    ULONG len = s->Parameters.QueryDirectory.Length, used = 0, lastEntry = 0;
    bool single = s->Flags & SL_RETURN_SINGLE_ENTRY, any = false;
    NTSTATUS status = STATUS_SUCCESS;
    for (;;) {
        FATX_DIRENT *e;
        LONGLONG block;
        NTSTATUS st = FatxMapDirent(v, f, ccb->NextIndex, &e, &block, false);
        if (st == STATUS_END_OF_FILE || (NT_SUCCESS(st) && (e->FileNameLength == 0 || e->FileNameLength == 0xFF)))
            break;
        if (!NT_SUCCESS(st)) {
            status = st;
            break;
        }
        if (e->FileNameLength == FATX_DIRENT_DELETED || e->FileNameLength > FATX_MAX_NAME ||
            !FatxWildcardMatch(ccb->Mask, ccb->MaskLength, e->FileName, e->FileNameLength)) {
            ccb->NextIndex++;
            continue;
        }
        ULONG offset = (used + 7) & ~7u, need = fixed + e->FileNameLength;
        if (offset + fixed > len) {
            if (!any)
                status = STATUS_BUFFER_TOO_SMALL_IO;
            break;
        }
        if (any && offset + need > len)
            break;
        PUCHAR p = out + offset;
        RtlZeroMemory(p, fixed);
        ULONG nameLen = e->FileNameLength, room = len - offset - fixed;
        if (cls == FileDirectoryInformation) {
            *(PULONG)(p + 4) = ccb->NextIndex;
            FatxTimestampToTime(e->CreationTime, (PLARGE_INTEGER)(p + 8));
            FatxTimestampToTime(e->LastAccessTime, (PLARGE_INTEGER)(p + 0x10));
            FatxTimestampToTime(e->LastWriteTime, (PLARGE_INTEGER)(p + 0x18));
            *(PLARGE_INTEGER)(p + 0x20) = *(PLARGE_INTEGER)(p + 0x18);
            bool isDir = e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            *(PULONG)(p + 0x28) = isDir ? 0 : e->FileSize;
            *(PULONG)(p + 0x30) =
                isDir ? 0 : (e->FileSize + v->BytesPerCluster - 1) & ~(v->BytesPerCluster - 1);
            *(PULONG)(p + 0x38) = FatxAttributesOut(e->FileAttributes);
            *(PULONG)(p + 0x3C) = nameLen;
        } else {
            *(PULONG)(p + 4) = ccb->NextIndex;
            *(PULONG)(p + 8) = nameLen;
        }
        if (any)
            *(PULONG)(out + lastEntry) = offset - lastEntry;
        RtlCopyMemory(p + fixed, e->FileName, nameLen < room ? nameLen : room);
        lastEntry = offset;
        used = offset + fixed + (nameLen < room ? nameLen : room);
        any = true;
        ccb->NextIndex++;
        if (nameLen > room) {
            status = (NTSTATUS)0x80000005; // STATUS_BUFFER_OVERFLOW
            break;
        }
        if (single)
            break;
    }
    if (NT_SUCCESS(status) && !any)
        status = first ? (NTSTATUS)0xC000000F /*STATUS_NO_SUCH_FILE*/ : (NTSTATUS)0x80000006 /*NO_MORE_FILES*/;
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status, any ? used : 0);
}

// --- Volume information

static NTSTATUS NTAPI FatxFsdQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    auto out = (PUCHAR)Irp->UserBuffer;
    ULONG len = s->Parameters.QueryVolume.Length, info = 0;
    NTSTATUS status = STATUS_SUCCESS;
    FatxAcquireVolume(v);
    ULONG spc = v->BytesPerCluster / v->SectorSize;
    switch (s->Parameters.QueryVolume.FsInformationClass) {
    case FileFsVolumeInformation: // {CreationTime, SerialNumber, LabelLength, SupportsObjects, Label[]}
        if (len < 18) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        RtlZeroMemory(out, 18);
        *(PULONG)(out + 8) = v->SerialNumber;
        info = 18;
        break;
    case FileFsSizeInformation: // {Total, Available, SectorsPerAllocationUnit, BytesPerSector}
    case FileFsFullSizeInformation: {
        bool full = s->Parameters.QueryVolume.FsInformationClass == FileFsFullSizeInformation;
        ULONG need = full ? 32 : 24;
        if (len < need) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        auto p = (PULONG)out;
        p[0] = v->NumberOfClusters, p[1] = 0;
        p[2] = v->FreeClusters, p[3] = 0;
        if (full) {
            p[4] = v->FreeClusters, p[5] = 0;
            p[6] = spc, p[7] = v->SectorSize;
        } else {
            p[4] = spc, p[5] = v->SectorSize;
        }
        info = need;
        break;
    }
    case FileFsDeviceInformation: // {DeviceType, Characteristics}
        if (len < 8) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        ((PULONG)out)[0] = FILE_DEVICE_DISK;
        ((PULONG)out)[1] = 0;
        info = 8;
        break;
    case FileFsAttributeInformation: // {Attributes, MaxComponentNameLength, NameLength, Name[]}
        if (len < 12) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        ((PULONG)out)[0] = 2; // FILE_CASE_PRESERVED_NAMES
        ((PULONG)out)[1] = FATX_MAX_NAME;
        ((PULONG)out)[2] = 4;
        info = 12;
        if (len >= 16) {
            RtlCopyMemory(out + 12, "FATX", 4);
            info = 16;
        } else {
            status = (NTSTATUS)0x80000005;
        }
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
    }
    FatxReleaseVolume(v);
    return FatxComplete(Irp, status, info);
}

static NTSTATUS NTAPI FatxFsdFlushOrDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    // Flushing: the cache is write-through, so only the drive cache is left: pass down. Device controls on a
    // volume handle go to the partition.
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IofCallDriver(FatxVolume(DeviceObject)->TargetDevice, Irp);
}

static NTSTATUS NTAPI FatxDismountVolume(PDEVICE_OBJECT DeviceObject);

// FSCTL_DISMOUNT_VOLUME (used by the utility-drive format: XAPI rewrites the FATX metadata through the volume handle,
// then dismounts so the next open mounts the new file system). The reference also accepts 0x9411C / 0x98120.
static NTSTATUS NTAPI FatxFsdFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.FsControlCode == FSCTL_DISMOUNT_VOLUME)
        return FatxComplete(Irp, FatxDismountVolume(DeviceObject));
    return FatxComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
}

// Dismount: new opens remount; the volume device goes away with its last file object.
static NTSTATUS NTAPI FatxDismountVolume(PDEVICE_OBJECT DeviceObject) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    FatxAcquireVolume(v);
    v->Dismounted = TRUE;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (v->TargetDevice->MountedOrSelfDevice == DeviceObject)
        v->TargetDevice->MountedOrSelfDevice = nullptr;
    KfLowerIrql(old);
    FatxReleaseVolume(v);
    IoDeleteDevice(DeviceObject);
    return STATUS_SUCCESS;
}

static void NTAPI FatxDeleteVolume(PDEVICE_OBJECT DeviceObject) {
    FATX_VOLUME *v = FatxVolume(DeviceObject);
    FatxForgetVolume(v);
    ObfDereferenceObject(v->TargetDevice);
}

DRIVER_OBJECT FatxDriverObject = {
    nullptr,
    FatxDeleteVolume,
    FatxDismountVolume,
    {FatxFsdCreate, FatxFsdClose, FatxFsdReadWrite, FatxFsdReadWrite, FatxFsdQueryInformation, FatxFsdSetInformation,
     FatxFsdFlushOrDeviceControl, FatxFsdQueryVolumeInformation, FatxFsdDirectoryControl, FatxFsdFileSystemControl,
     FatxFsdFlushOrDeviceControl, IoInvalidDeviceRequest, IoInvalidDeviceRequest, FatxFsdCleanup}};
