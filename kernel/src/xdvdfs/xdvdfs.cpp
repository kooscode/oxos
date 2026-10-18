// XDVDFS (Xbox game disc file system), read-only (spec/stock/04c).
#include <xk/fsc.h>
#include <xk/idex.h>
#include <xk/rtl.h>

constexpr ULONG XDVDFS_SECTOR = 0x800, XDVDFS_VOLUME_SECTOR = 32;
static const char XdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr ULONG FILE_OPENED = 1;
constexpr NTSTATUS STATUS_MEDIA_WRITE_PROTECTED = (NTSTATUS)0xC00000A2;

struct XDVDFS_FCB {
    LIST_ENTRY Link;
    LONG ReferenceCount;
    ULONG StartSector; // file data, or the directory table
    ULONG Size;
    UCHAR Attributes;
    UCHAR NameLength;
    CHAR Name[256];
    ULONG EntryOffset; // FileIndex: the entry's offset in its parent directory
    BOOLEAN Directory, Volume;
};

struct XDVDFS_CCB {
    ULONG NextIndex; // in-order position for directory enumeration
    BOOLEAN HaveMask;
    UCHAR MaskLength;
    CHAR Mask[64];
};

struct XDVDFS_VOLUME {
    PDEVICE_OBJECT TargetDevice;
    ULONG RootSector, RootSize;
    LARGE_INTEGER Time;
    LONGLONG MediaLength;
    KEVENT Lock;
    LIST_ENTRY Fcbs;
    PUCHAR Bounce; // one sector
    FSCACHE_EXTENSION CacheExtension; // the volume's key in the file system cache
};

extern DRIVER_OBJECT XdvdfsDriverObject;
static XDVDFS_VOLUME *XdvdfsVolume(PDEVICE_OBJECT d) { return (XDVDFS_VOLUME *)d->DeviceExtension; }

// Non-cached reads: straight to the device for whole sectors, partial sectors through the bounce sector.
static NTSTATUS XdvdfsReadDirect(XDVDFS_VOLUME *v, LONGLONG offset, PVOID buffer, ULONG length) {
    PUCHAR out = (PUCHAR)buffer;
    while (length) {
        ULONG within = (ULONG)(offset & (XDVDFS_SECTOR - 1));
        NTSTATUS status;
        if (!within && length >= XDVDFS_SECTOR && !((ULONG)out & 3)) {
            ULONG n = length & ~(XDVDFS_SECTOR - 1);
            LARGE_INTEGER o;
            o.QuadPart = offset;
            status = IoSynchronousFsdRequest(IRP_MJ_READ, v->TargetDevice, out, n, &o);
            if (!NT_SUCCESS(status))
                return status;
            offset += n, out += n, length -= n;
            continue;
        }
        LARGE_INTEGER o;
        o.QuadPart = offset - within;
        status = IoSynchronousFsdRequest(IRP_MJ_READ, v->TargetDevice, v->Bounce, XDVDFS_SECTOR, &o);
        if (!NT_SUCCESS(status))
            return status;
        ULONG n = XDVDFS_SECTOR - within < length ? XDVDFS_SECTOR - within : length;
        RtlCopyMemory(out, v->Bounce + within, n);
        offset += n, out += n, length -= n;
    }
    return STATUS_SUCCESS;
}

// Directory entries: through the file system cache, as the reference.
static NTSTATUS XdvdfsRead(XDVDFS_VOLUME *v, LONGLONG offset, PVOID buffer, ULONG length) {
    return FscCachedRead(&v->CacheExtension, FO_RANDOM_ACCESS, offset, length, buffer, false);
}

static CHAR XdvdfsUpper(CHAR c) { return c >= 'a' && c <= 'z' ? (CHAR)(c - 0x20) : c; }

// Tree order: case-insensitive byte compare, a prefix sorts first.
static int XdvdfsCompare(const CHAR *a, ULONG al, const CHAR *b, ULONG bl) {
    for (ULONG i = 0; i < al && i < bl; i++) {
        UCHAR x = (UCHAR)XdvdfsUpper(a[i]), y = (UCHAR)XdvdfsUpper(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return al == bl ? 0 : al < bl ? -1 : 1;
}

struct XDVDFS_ENTRY {
    USHORT Left, Right; // in dwords from the table start, 0 = none
    ULONG StartSector, Size;
    UCHAR Attributes, NameLength;
    CHAR Name[256];
};

static NTSTATUS XdvdfsReadEntry(XDVDFS_VOLUME *v, XDVDFS_FCB *dir, ULONG offset, XDVDFS_ENTRY *e) {
    if (offset + 14 > dir->Size)
        return (NTSTATUS)0xC0000032; // STATUS_DISK_CORRUPT_ERROR
    UCHAR raw[14 + 255];
    NTSTATUS status = XdvdfsRead(v, (LONGLONG)dir->StartSector * XDVDFS_SECTOR + offset, raw, 14);
    if (!NT_SUCCESS(status))
        return status;
    e->Left = *(USHORT *)raw;
    e->Right = *(USHORT *)(raw + 2);
    e->StartSector = *(ULONG *)(raw + 4);
    e->Size = *(ULONG *)(raw + 8);
    e->Attributes = raw[12];
    e->NameLength = raw[13];
    if (e->Left == 0xFFFF)
        return (NTSTATUS)0xC0000032;
    return XdvdfsRead(v, (LONGLONG)dir->StartSector * XDVDFS_SECTOR + offset + 14, e->Name, e->NameLength);
}

static NTSTATUS XdvdfsLookup(XDVDFS_VOLUME *v, XDVDFS_FCB *dir, const CHAR *name, ULONG len, XDVDFS_ENTRY *e,
                             ULONG *where) {
    if (!dir->Size)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    ULONG offset = 0;
    for (ULONG guard = 0; guard < 65536; guard++) {
        NTSTATUS status = XdvdfsReadEntry(v, dir, offset, e);
        if (!NT_SUCCESS(status))
            return status;
        int c = XdvdfsCompare(name, len, e->Name, e->NameLength);
        if (!c) {
            *where = offset;
            return STATUS_SUCCESS;
        }
        USHORT next = c < 0 ? e->Left : e->Right;
        if (!next)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        offset = next * 4u;
    }
    return (NTSTATUS)0xC0000032;
}

// The n-th entry of the directory in tree (sorted) order.
static NTSTATUS XdvdfsNth(XDVDFS_VOLUME *v, XDVDFS_FCB *dir, ULONG offset, ULONG *n, XDVDFS_ENTRY *e, ULONG *where,
                          ULONG depth) {
    if (depth > 64)
        return (NTSTATUS)0xC0000032;
    NTSTATUS status = XdvdfsReadEntry(v, dir, offset, e);
    if (!NT_SUCCESS(status))
        return status;
    USHORT left = e->Left, right = e->Right;
    if (left) {
        status = XdvdfsNth(v, dir, left * 4u, n, e, where, depth + 1);
        if (status != STATUS_NO_MORE_ENTRIES)
            return status;
        XdvdfsReadEntry(v, dir, offset, e);
    }
    if ((*n)-- == 0) {
        *where = offset;
        return STATUS_SUCCESS;
    }
    if (right)
        return XdvdfsNth(v, dir, right * 4u, n, e, where, depth + 1);
    return STATUS_NO_MORE_ENTRIES;
}

static NTSTATUS XdvdfsComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information = 0) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static XDVDFS_FCB *XdvdfsNewFcb(XDVDFS_VOLUME *v, const XDVDFS_ENTRY *e, ULONG where) {
    auto f = (XDVDFS_FCB *)ExAllocatePoolWithTag(sizeof(XDVDFS_FCB), 'fdvX');
    if (!f)
        return nullptr;
    RtlZeroMemory(f, sizeof(*f));
    f->ReferenceCount = 1;
    f->StartSector = e->StartSector;
    f->Size = e->Size;
    f->Attributes = e->Attributes;
    f->NameLength = e->NameLength;
    RtlCopyMemory(f->Name, e->Name, e->NameLength);
    f->EntryOffset = where;
    f->Directory = (e->Attributes & 0x10) != 0;
    InsertTailList(&v->Fcbs, &f->Link);
    return f;
}

static NTSTATUS XdvdfsCreate(XDVDFS_VOLUME *v, PIRP Irp, ULONG_PTR *info) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    ULONG options = s->Parameters.Create.Options & 0xFFFFFF, disposition = s->Parameters.Create.Options >> 24;
    ACCESS_MASK access = s->Parameters.Create.DesiredAccess;
    OBJECT_STRING rest = *s->Parameters.Create.RemainingName;
    XDVDFS_ENTRY e;
    ULONG where = 0;
    XDVDFS_FCB *dir = fo->RelatedFileObject ? (XDVDFS_FCB *)fo->RelatedFileObject->FsContext : nullptr;
    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | FILE_WRITE_ATTRIBUTES))
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    SHARE_ACCESS share; // read-only media: nothing conflicts, but the file object needs its access bits
    IoSetShareAccess(access, s->Parameters.Create.ShareAccess, fo, &share);
    if (!dir && !rest.Length) { // the volume itself
        e = {0, 0, 0, 0, 0, 0, {}};
        XDVDFS_FCB *f = XdvdfsNewFcb(v, &e, 0);
        if (!f)
            return STATUS_INSUFFICIENT_RESOURCES;
        f->Volume = TRUE;
        fo->FsContext = f;
        *info = FILE_OPENED;
        return STATUS_SUCCESS;
    }
    // Start at the root (or the related directory) and walk the path.
    e = {0, 0, v->RootSector, v->RootSize, 0x10, 0, {}};
    if (dir) {
        if (!dir->Directory)
            return STATUS_INVALID_PARAMETER;
        e.StartSector = dir->StartSector, e.Size = dir->Size, e.Attributes = dir->Attributes;
        e.NameLength = dir->NameLength;
        RtlCopyMemory(e.Name, dir->Name, dir->NameLength);
        where = dir->EntryOffset;
    } else if (rest.Buffer[0] != '\\') {
        return STATUS_OBJECT_NAME_INVALID;
    }
    if (rest.Length > 1 && rest.Buffer[rest.Length - 1] == '\\')
        rest.Length--;
    bool trailing = s->Parameters.Create.RemainingName->Length != rest.Length;
    while (rest.Length) {
        if (rest.Buffer[0] == '\\')
            rest.Buffer++, rest.Length--;
        USHORT n = 0;
        while (n < rest.Length && rest.Buffer[n] != '\\')
            n++;
        if (!n) {
            if (rest.Length)
                return STATUS_OBJECT_NAME_INVALID;
            break; // "\" = root
        }
        if (!(e.Attributes & 0x10))
            return STATUS_OBJECT_PATH_NOT_FOUND;
        XDVDFS_FCB parent = {};
        parent.StartSector = e.StartSector, parent.Size = e.Size;
        NTSTATUS status = XdvdfsLookup(v, &parent, rest.Buffer, n, &e, &where);
        rest.Buffer += n, rest.Length = (USHORT)(rest.Length - n);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            if (rest.Length)
                return STATUS_OBJECT_PATH_NOT_FOUND;
            return disposition == FILE_OPEN_IF ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (!NT_SUCCESS(status))
            return status;
    }
    bool isDir = e.Attributes & 0x10;
    if (isDir && (options & FILE_NON_DIRECTORY_FILE))
        return (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
    if (!isDir && ((options & FILE_DIRECTORY_FILE) || trailing))
        return (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
    XDVDFS_FCB *f = XdvdfsNewFcb(v, &e, where);
    if (!f)
        return STATUS_INSUFFICIENT_RESOURCES;
    auto ccb = (XDVDFS_CCB *)ExAllocatePoolWithTag(sizeof(XDVDFS_CCB), 'cdvX');
    if (!ccb) {
        RemoveEntryList(&f->Link);
        ExFreePool(f);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ccb, sizeof(*ccb));
    fo->FsContext = f;
    fo->FsContext2 = ccb;
    *info = FILE_OPENED;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI XdvdfsFsdCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    ULONG_PTR info = 0;
    NTSTATUS status = XdvdfsCreate(v, Irp, &info);
    KeSetEvent(&v->Lock, 0, FALSE);
    return XdvdfsComplete(Irp, status, NT_SUCCESS(status) ? info : 0);
}

static NTSTATUS NTAPI XdvdfsFsdClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    if (auto f = (XDVDFS_FCB *)fo->FsContext) {
        RemoveEntryList(&f->Link);
        ExFreePool(f);
    }
    if (fo->FsContext2)
        ExFreePool(fo->FsContext2);
    fo->FsContext = fo->FsContext2 = nullptr;
    KeSetEvent(&v->Lock, 0, FALSE);
    return XdvdfsComplete(Irp, STATUS_SUCCESS);
}

static NTSTATUS NTAPI XdvdfsFsdRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (XDVDFS_FCB *)fo->FsContext;
    if (!f)
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    if (f->Volume) { // raw sectors of the disc
        IoCopyCurrentIrpStackLocationToNext(Irp);
        return IofCallDriver(v->TargetDevice, Irp);
    }
    if (f->Directory)
        return XdvdfsComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    LARGE_INTEGER offset = s->Parameters.Read.ByteOffset;
    ULONG length = s->Parameters.Read.Length;
    // Non-cached reads start on a sector, but their length need not be whole sectors: the reference rounds the
    // transfer up and reports the length clipped to the file (Halo 2 reads its map files' tails this way).
    // End of file comes first: after such a tail the file position is the (unaligned) file size, and the next read
    // must report END_OF_FILE (Brute Force copies its .tgz files to the cache partition until it sees it).
    if (offset.HighPart || offset.LowPart >= f->Size)
        return XdvdfsComplete(Irp, length ? STATUS_END_OF_FILE : STATUS_SUCCESS);
    if ((fo->Flags & FO_NO_INTERMEDIATE_BUFFERING) && (offset.LowPart & (XDVDFS_SECTOR - 1)))
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    // The reference rounds a read that ends the file up to whole sectors only when the caller asked for at least
    // that much (0x8002b4e0): then the transfer may fill the buffer up to the sector's end.
    bool toEnd = length >= ((f->Size + XDVDFS_SECTOR - 1) & ~(XDVDFS_SECTOR - 1)) - offset.LowPart;
    if (length > f->Size - offset.LowPart)
        length = f->Size - offset.LowPart;
    LONGLONG disc = (LONGLONG)f->StartSector * XDVDFS_SECTOR + offset.LowPart;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    // Cached reads go through the file system cache (FscCachedRead decides what is copied from cache buffers).
    NTSTATUS status = (fo->Flags & FO_NO_INTERMEDIATE_BUFFERING)
                          ? XdvdfsReadDirect(v, disc, Irp->UserBuffer, length)
                          : FscCachedRead(&v->CacheExtension, fo->Flags, disc, length, Irp->UserBuffer, toEnd);
    KeSetEvent(&v->Lock, 0, FALSE);
    if (NT_SUCCESS(status) && (fo->Flags & FO_SYNCHRONOUS_IO))
        fo->CurrentByteOffset.QuadPart = offset.QuadPart + length;
    return XdvdfsComplete(Irp, status, NT_SUCCESS(status) ? length : 0);
}

static ULONG XdvdfsAttributes(UCHAR a) { return a ? a : 0x80; }

static NTSTATUS NTAPI XdvdfsFsdQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (XDVDFS_FCB *)fo->FsContext;
    auto out = (PUCHAR)Irp->UserBuffer;
    if (!f || f->Volume)
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    ULONG alloc = (f->Size + XDVDFS_SECTOR - 1) & ~(XDVDFS_SECTOR - 1);
    switch (s->Parameters.QueryFile.FileInformationClass) {
    case FileBasicInformation:
        for (int i = 0; i < 4; i++)
            ((PLARGE_INTEGER)out)[i] = v->Time;
        *(PULONG)(out + 32) = XdvdfsAttributes(f->Attributes);
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 40);
    case FileStandardInformation:
        RtlZeroMemory(out, 24);
        ((PULONG)out)[0] = alloc;
        ((PULONG)out)[2] = f->Size;
        ((PULONG)out)[4] = 1;
        out[21] = f->Directory;
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 24);
    case FileInternalInformation:
        ((PULONG)out)[0] = f->StartSector;
        ((PULONG)out)[1] = 0;
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 8);
    case FilePositionInformation:
        *(PLARGE_INTEGER)out = fo->CurrentByteOffset;
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 8);
    case FileNetworkOpenInformation:
        RtlZeroMemory(out, 56);
        for (int i = 0; i < 4; i++)
            ((PLARGE_INTEGER)out)[i] = v->Time;
        ((PULONG)out)[8] = alloc;
        ((PULONG)out)[10] = f->Size;
        ((PULONG)out)[12] = XdvdfsAttributes(f->Attributes);
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 56);
    default:
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    }
}

static NTSTATUS NTAPI XdvdfsFsdSetInformation(PDEVICE_OBJECT, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s->Parameters.SetFile.FileInformationClass == FilePositionInformation) {
        s->FileObject->CurrentByteOffset = *(PLARGE_INTEGER)Irp->UserBuffer;
        return XdvdfsComplete(Irp, STATUS_SUCCESS);
    }
    return XdvdfsComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED);
}

static bool XdvdfsMatch(const CHAR *m, ULONG ml, const CHAR *n, ULONG nl) {
    if (!ml)
        return !nl;
    if (*m == '*') {
        for (ULONG k = 0; k <= nl; k++)
            if (XdvdfsMatch(m + 1, ml - 1, n + k, nl - k))
                return true;
        return false;
    }
    return nl && (*m == '?' || XdvdfsUpper(*m) == XdvdfsUpper(*n)) && XdvdfsMatch(m + 1, ml - 1, n + 1, nl - 1);
}

static NTSTATUS NTAPI XdvdfsFsdDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (XDVDFS_FCB *)fo->FsContext;
    auto ccb = (XDVDFS_CCB *)fo->FsContext2;
    if (s->MinorFunction != 1 || !f || !f->Directory || !ccb)
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    if (s->Parameters.QueryDirectory.FileInformationClass != FileDirectoryInformation)
        return XdvdfsComplete(Irp, STATUS_INVALID_INFO_CLASS);
    bool first = !ccb->HaveMask;
    POBJECT_STRING mask = s->Parameters.QueryDirectory.FileName;
    if (first || (s->Flags & SL_RESTART_SCAN)) {
        ccb->NextIndex = 0;
        ccb->HaveMask = TRUE;
        ccb->MaskLength = 1;
        ccb->Mask[0] = '*';
        if (mask && mask->Length && mask->Length <= sizeof(ccb->Mask)) {
            ccb->MaskLength = (UCHAR)mask->Length;
            RtlCopyMemory(ccb->Mask, mask->Buffer, mask->Length);
        }
        first = true;
    }
    ULONG len = s->Parameters.QueryDirectory.Length;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = (NTSTATUS)0x80000006; // STATUS_NO_MORE_FILES
    ULONG used = 0;
    while (f->Size) {
        XDVDFS_ENTRY e;
        ULONG n = ccb->NextIndex, where;
        NTSTATUS st = XdvdfsNth(v, f, 0, &n, &e, &where, 0);
        if (st == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(st)) {
            status = st;
            break;
        }
        ccb->NextIndex++;
        if (!XdvdfsMatch(ccb->Mask, ccb->MaskLength, e.Name, e.NameLength))
            continue;
        if (len < 0x40) {
            status = STATUS_BUFFER_TOO_SMALL_IO;
            ccb->NextIndex--;
            break;
        }
        auto p = (PUCHAR)Irp->UserBuffer;
        RtlZeroMemory(p, 0x40);
        *(PULONG)(p + 4) = where;
        for (int i = 0; i < 4; i++)
            ((PLARGE_INTEGER)(p + 8))[i] = v->Time;
        bool dir = e.Attributes & 0x10;
        *(PULONG)(p + 0x28) = dir ? 0 : e.Size;
        *(PULONG)(p + 0x30) = dir ? 0 : (e.Size + XDVDFS_SECTOR - 1) & ~(XDVDFS_SECTOR - 1);
        *(PULONG)(p + 0x38) = XdvdfsAttributes(e.Attributes);
        *(PULONG)(p + 0x3C) = e.NameLength;
        ULONG room = len - 0x40, copy = e.NameLength < room ? e.NameLength : room;
        RtlCopyMemory(p + 0x40, e.Name, copy);
        used = 0x40 + copy;
        status = copy < e.NameLength ? (NTSTATUS)0x80000005 : STATUS_SUCCESS;
        break;
    }
    KeSetEvent(&v->Lock, 0, FALSE);
    if (status == (NTSTATUS)0x80000006 && first)
        status = (NTSTATUS)0xC000000F; // STATUS_NO_SUCH_FILE
    return XdvdfsComplete(Irp, status, used);
}

static NTSTATUS NTAPI XdvdfsFsdQueryVolume(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    auto out = (PULONG)Irp->UserBuffer;
    ULONG len = s->Parameters.QueryVolume.Length;
    switch (s->Parameters.QueryVolume.FsInformationClass) {
    case FileFsSizeInformation:
        if (len < 24)
            return XdvdfsComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = (ULONG)(v->MediaLength / XDVDFS_SECTOR), out[1] = 0;
        out[2] = out[3] = 0;
        out[4] = 1, out[5] = XDVDFS_SECTOR;
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 24);
    case FileFsDeviceInformation:
        if (len < 8)
            return XdvdfsComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = FILE_DEVICE_CD_ROM, out[1] = 1; // FILE_REMOVABLE_MEDIA
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 8);
    case FileFsAttributeInformation:
        if (len < 18)
            return XdvdfsComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = 0x80000, out[1] = 255, out[2] = 6; // FILE_READ_ONLY_VOLUME
        RtlCopyMemory(out + 3, "XDVDFS", 6);
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 18);
    case FileFsVolumeInformation:
        if (len < 18)
            return XdvdfsComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        RtlZeroMemory(out, 18);
        *(PLARGE_INTEGER)out = v->Time;
        return XdvdfsComplete(Irp, STATUS_SUCCESS, 18);
    default:
        return XdvdfsComplete(Irp, STATUS_INVALID_PARAMETER);
    }
}

static NTSTATUS NTAPI XdvdfsFsdPassDown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IofCallDriver(XdvdfsVolume(DeviceObject)->TargetDevice, Irp);
}

static NTSTATUS NTAPI XdvdfsFsdSuccess(PDEVICE_OBJECT, PIRP Irp) { return XdvdfsComplete(Irp, STATUS_SUCCESS); }

static NTSTATUS NTAPI XdvdfsDismountVolume(PDEVICE_OBJECT DeviceObject) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (v->TargetDevice->MountedOrSelfDevice == DeviceObject)
        v->TargetDevice->MountedOrSelfDevice = nullptr;
    KfLowerIrql(old);
    IoDeleteDevice(DeviceObject);
    return STATUS_SUCCESS;
}

static void NTAPI XdvdfsDeleteVolume(PDEVICE_OBJECT DeviceObject) {
    XDVDFS_VOLUME *v = XdvdfsVolume(DeviceObject);
    FscInvalidateDevice(&v->CacheExtension);
    ExFreePool(v->Bounce);
    ObfDereferenceObject(v->TargetDevice);
}

DRIVER_OBJECT XdvdfsDriverObject = {
    nullptr,
    XdvdfsDeleteVolume,
    XdvdfsDismountVolume,
    {XdvdfsFsdCreate, XdvdfsFsdClose, XdvdfsFsdRead, IoInvalidDeviceRequest, XdvdfsFsdQueryInformation,
     XdvdfsFsdSetInformation, XdvdfsFsdSuccess, XdvdfsFsdQueryVolume, XdvdfsFsdDirectoryControl,
     IoFsControlDismountOnly, XdvdfsFsdPassDown, IoInvalidDeviceRequest, IoInvalidDeviceRequest, XdvdfsFsdSuccess}};

NTSTATUS XdvdfsMountVolume(PDEVICE_OBJECT TargetDevice) {
    DISK_GEOMETRY g;
    // Authenticate an Xbox game disc first (the result only matters to the XBE loader's media check).
    IoSynchronousDeviceIoControlRequest(0x24080, TargetDevice, nullptr, 0, nullptr, 0, nullptr, FALSE);
    NTSTATUS status =
        IoSynchronousDeviceIoControlRequest(0x2404C, TargetDevice, nullptr, 0, &g, sizeof(g), nullptr, FALSE);
    if (!NT_SUCCESS(status))
        return status;
    auto sector = (PUCHAR)ExAllocatePoolWithTag(XDVDFS_SECTOR, 'bdvX');
    if (!sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    LARGE_INTEGER o;
    o.QuadPart = XDVDFS_VOLUME_SECTOR * XDVDFS_SECTOR;
    status = IoSynchronousFsdRequest(IRP_MJ_READ, TargetDevice, sector, XDVDFS_SECTOR, &o);
    bool ok = NT_SUCCESS(status);
    for (ULONG i = 0; ok && i < 20; i++)
        ok = sector[i] == (UCHAR)XdvdfsMagic[i] && sector[0x7EC + i] == (UCHAR)XdvdfsMagic[i];
    if (!ok) {
        ExFreePool(sector);
        return NT_SUCCESS(status) ? STATUS_UNRECOGNIZED_VOLUME : status;
    }
    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&XdvdfsDriverObject, sizeof(XDVDFS_VOLUME), nullptr, 8, FALSE, &dev);
    if (!NT_SUCCESS(status)) {
        ExFreePool(sector);
        return status;
    }
    dev->StackSize = (CHAR)(dev->StackSize + TargetDevice->StackSize);
    dev->SectorSize = XDVDFS_SECTOR;
    XDVDFS_VOLUME *v = XdvdfsVolume(dev);
    v->TargetDevice = TargetDevice;
    v->RootSector = *(ULONG *)(sector + 20);
    v->RootSize = *(ULONG *)(sector + 24);
    v->Time = *(PLARGE_INTEGER)(sector + 28);
    v->MediaLength = g.Cylinders.QuadPart * XDVDFS_SECTOR;
    v->CacheExtension.TargetDeviceObject = TargetDevice;
    v->CacheExtension.PartitionLength.QuadPart = v->MediaLength;
    v->CacheExtension.SectorSize = XDVDFS_SECTOR;
    v->Bounce = sector;
    KeInitializeEvent(&v->Lock, SynchronizationEvent, TRUE);
    InitializeListHead(&v->Fcbs);
    ObfReferenceObject(TargetDevice);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
