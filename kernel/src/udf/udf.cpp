// UDF 1.0x (ECMA-167, OSTA UDF 1.02/1.50), read-only, for DVD-video discs (spec/04d). The CD-ROM mount tries
// this file system before XDVDFS.
#include <xk/fsc.h>
#include <xk/idex.h>
#include <xk/rtl.h>

constexpr ULONG UDF_BLOCK = 0x800;
constexpr ULONG FILE_OPENED = 1;
constexpr ULONG UDF_MAX_EXTENTS = 32, UDF_MAX_DIRECTORY = 0x100000;
constexpr NTSTATUS STATUS_MEDIA_WRITE_PROTECTED = (NTSTATUS)0xC00000A2, STATUS_DISK_CORRUPT = (NTSTATUS)0xC0000032;
constexpr USHORT TAG_PVD = 1, TAG_AVDP = 2, TAG_PD = 5, TAG_LVD = 6, TAG_TERMINATOR = 8, TAG_FSD = 256,
                 TAG_FID = 257, TAG_FILE_ENTRY = 261;

struct UDF_EXTENT {
    ULONG Block; // partition-relative
    ULONG Length;
};

struct UDF_FCB {
    LIST_ENTRY Link;
    ULONG IcbBlock;   // the File Entry (partition-relative)
    ULONG Size;
    ULONG EntryOffset; // FileIndex: the FID's offset in its parent directory
    LARGE_INTEGER Time;
    BOOLEAN Directory, Volume, Embedded;
    UCHAR NameLength;
    USHORT EmbeddedOffset; // embedded data: offset in the File Entry block
    ULONG ExtentCount;
    UDF_EXTENT Extents[UDF_MAX_EXTENTS];
    CHAR Name[256];
};

struct UDF_CCB {
    ULONG NextOffset; // byte offset of the next FID for directory enumeration
    BOOLEAN HaveMask;
    UCHAR MaskLength;
    CHAR Mask[64];
};

struct UDF_VOLUME {
    PDEVICE_OBJECT TargetDevice;
    ULONG PartitionStart, PartitionLength; // in blocks
    ULONG RootIcb;
    LONGLONG MediaLength;
    KEVENT Lock;
    LIST_ENTRY Fcbs;
    PUCHAR Block; // one block of scratch
    FSCACHE_EXTENSION CacheExtension; // the volume's key in the file system cache
};

extern DRIVER_OBJECT UdfDriverObject;
static UDF_VOLUME *UdfVolume(PDEVICE_OBJECT d) { return (UDF_VOLUME *)d->DeviceExtension; }

static NTSTATUS UdfReadSector(PDEVICE_OBJECT target, ULONG sector, PVOID buffer) {
    LARGE_INTEGER o;
    o.QuadPart = (LONGLONG)sector * UDF_BLOCK;
    return IoSynchronousFsdRequest(IRP_MJ_READ, target, buffer, UDF_BLOCK, &o);
}

// Descriptor tag: version 2, the expected identifier and recorded location (no checksum or CRC check, like the
// reference).
static bool UdfTagOk(const UCHAR *d, USHORT id, ULONG location) {
    return *(USHORT *)(d + 2) == 2 && *(USHORT *)d == id && *(ULONG *)(d + 12) == location;
}

// ECMA-167 timestamp (12 bytes) → system time; the time zone is ignored.
static LARGE_INTEGER UdfTime(const UCHAR *t) {
    TIME_FIELDS f = {*(SHORT *)(t + 2), t[4], t[5], t[6], t[7], t[8], (SHORT)(t[9] * 10), 0};
    LARGE_INTEGER r;
    if (!RtlTimeFieldsToTime(&f, &r))
        r.QuadPart = 0;
    return r;
}

// --- File entries and file data

static NTSTATUS UdfLoadFileEntry(UDF_VOLUME *v, UDF_FCB *f) {
    if (f->IcbBlock >= v->PartitionLength)
        return STATUS_DISK_CORRUPT;
    PUCHAR b = v->Block;
    NTSTATUS status = UdfReadSector(v->TargetDevice, v->PartitionStart + f->IcbBlock, b);
    if (!NT_SUCCESS(status))
        return status;
    if (!UdfTagOk(b, TAG_FILE_ENTRY, f->IcbBlock))
        return STATUS_DISK_CORRUPT;
    UCHAR type = b[27];
    USHORT adType = *(USHORT *)(b + 34) & 7;
    ULONG eaLength = *(ULONG *)(b + 168), adLength = *(ULONG *)(b + 172);
    if (*(ULONG *)(b + 60) || 176 + eaLength + adLength > UDF_BLOCK) // files are limited to 4 GB here
        return STATUS_DISK_CORRUPT;
    f->Directory = type == 4;
    f->Size = *(ULONG *)(b + 56);
    f->Time = UdfTime(b + 84);
    f->ExtentCount = 0;
    f->Embedded = adType == 3;
    PUCHAR ad = b + 176 + eaLength;
    if (f->Embedded) {
        if (f->Size > adLength)
            return STATUS_DISK_CORRUPT;
        f->EmbeddedOffset = (USHORT)(ad - b);
        return STATUS_SUCCESS;
    }
    ULONG adSize = adType == 0 ? 8 : adType == 1 ? 16 : 0;
    if (!adSize)
        return STATUS_DISK_CORRUPT; // extended allocation descriptors are not used by UDF 1.0x
    for (ULONG i = 0; i + adSize <= adLength; i += adSize) {
        ULONG length = *(ULONG *)(ad + i), block = *(ULONG *)(ad + i + 4);
        if (!(length & 0x3FFFFFFF))
            break;
        if (length >> 30 == 3 || f->ExtentCount == UDF_MAX_EXTENTS)
            return STATUS_DISK_CORRUPT; // a continuation of the descriptor list, or too fragmented
        f->Extents[f->ExtentCount++] = {length >> 30 ? 0xFFFFFFFF : block, length & 0x3FFFFFFF}; // unrecorded = 0s
    }
    return STATUS_SUCCESS;
}

// File data. Cached reads (fileFlags without FO_NO_INTERMEDIATE_BUFFERING) go through the file system cache per
// extent; `toEnd` = the read runs to the end of the file.
static NTSTATUS UdfReadData(UDF_VOLUME *v, UDF_FCB *f, ULONG offset, PUCHAR out, ULONG length,
                            UCHAR fileFlags = FO_RANDOM_ACCESS, bool toEnd = false) {
    bool cached = !(fileFlags & FO_NO_INTERMEDIATE_BUFFERING);
    if (f->Embedded) {
        NTSTATUS status = UdfReadSector(v->TargetDevice, v->PartitionStart + f->IcbBlock, v->Block);
        if (NT_SUCCESS(status))
            RtlCopyMemory(out, v->Block + f->EmbeddedOffset + offset, length);
        return status;
    }
    ULONG base = 0;
    for (ULONG i = 0; i < f->ExtentCount && length; i++) {
        UDF_EXTENT *e = &f->Extents[i];
        if (offset >= base + e->Length) {
            base += e->Length;
            continue;
        }
        ULONG within = offset - base, run = e->Length - within < length ? e->Length - within : length;
        if (e->Block == 0xFFFFFFFF) {
            RtlZeroMemory(out, run);
        } else if (cached) {
            ULONG first = e->Block + within / UDF_BLOCK, last = e->Block + (within + run - 1) / UDF_BLOCK;
            if (first >= v->PartitionLength || last >= v->PartitionLength)
                return STATUS_DISK_CORRUPT;
            NTSTATUS status = FscCachedRead(&v->CacheExtension, fileFlags,
                                            (LONGLONG)(v->PartitionStart + e->Block) * UDF_BLOCK + within, run, out,
                                            toEnd && run == length);
            if (!NT_SUCCESS(status))
                return status;
        } else {
            ULONG pos = within;
            for (ULONG left = run; left;) {
                ULONG block = e->Block + pos / UDF_BLOCK, skip = pos % UDF_BLOCK;
                ULONG n = UDF_BLOCK - skip < left ? UDF_BLOCK - skip : left;
                if (block >= v->PartitionLength)
                    return STATUS_DISK_CORRUPT;
                LARGE_INTEGER o;
                o.QuadPart = (LONGLONG)(v->PartitionStart + block) * UDF_BLOCK;
                NTSTATUS status;
                if (!skip && n == UDF_BLOCK && !((ULONG)out & 3)) {
                    ULONG blocks = left / UDF_BLOCK; // whole blocks straight into the caller's buffer
                    n = blocks * UDF_BLOCK;
                    status = IoSynchronousFsdRequest(IRP_MJ_READ, v->TargetDevice, out, n, &o);
                } else {
                    status = IoSynchronousFsdRequest(IRP_MJ_READ, v->TargetDevice, v->Block, UDF_BLOCK, &o);
                    if (NT_SUCCESS(status))
                        RtlCopyMemory(out, v->Block + skip, n);
                }
                if (!NT_SUCCESS(status))
                    return status;
                out += n, pos += n, left -= n;
            }
            out -= run;
        }
        out += run, offset += run, length -= run, base += e->Length;
    }
    return length ? STATUS_DISK_CORRUPT : STATUS_SUCCESS;
}

// --- Directories: File Identifier Descriptors

struct UDF_ENTRY {
    ULONG Icb;
    UCHAR Characteristics; // 2 directory, 4 deleted, 8 parent
    UCHAR NameLength;
    CHAR Name[255];
};

// OSTA CS0: 8 → one byte per character, 16 → big-endian UCS-2 (characters above 0xFF become '_').
static void UdfName(const UCHAR *id, ULONG length, UDF_ENTRY *e) {
    e->NameLength = 0;
    if (!length)
        return;
    UCHAR comp = id[0];
    for (ULONG i = 1; i < length && e->NameLength < 255;) {
        if (comp == 16) {
            if (i + 1 >= length)
                break;
            e->Name[e->NameLength++] = id[i] ? '_' : (CHAR)id[i + 1];
            i += 2;
        } else {
            e->Name[e->NameLength++] = (CHAR)id[i++];
        }
    }
}

// Reads the FID at *offset of the directory data, advancing *offset past it.
static NTSTATUS UdfNextFid(UDF_VOLUME *v, UDF_FCB *dir, PUCHAR data, ULONG *offset, UDF_ENTRY *e) {
    for (;;) {
        ULONG o = *offset;
        if (o + 38 > dir->Size)
            return STATUS_NO_MORE_ENTRIES;
        PUCHAR fid = data + o;
        if (*(USHORT *)fid != TAG_FID || *(USHORT *)(fid + 2) != 2)
            return STATUS_DISK_CORRUPT;
        ULONG nameLength = fid[19], iuLength = *(USHORT *)(fid + 36);
        ULONG size = (38 + iuLength + nameLength + 3) & ~3u;
        if (o + 38 + iuLength + nameLength > dir->Size)
            return STATUS_DISK_CORRUPT;
        *offset = o + size;
        e->Characteristics = fid[18];
        e->Icb = *(ULONG *)(fid + 24);
        if (e->Characteristics & 0x0C) // deleted entries and the parent link are not listed
            continue;
        UdfName(fid + 38 + iuLength, nameLength, e);
        return STATUS_SUCCESS;
    }
}

static NTSTATUS UdfLoadDirectory(UDF_VOLUME *v, UDF_FCB *dir, PUCHAR *data) {
    *data = nullptr;
    if (!dir->Size)
        return STATUS_SUCCESS;
    if (dir->Size > UDF_MAX_DIRECTORY)
        return STATUS_DISK_CORRUPT;
    *data = (PUCHAR)ExAllocatePoolWithTag(dir->Size, 'dfdU');
    if (!*data)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = UdfReadData(v, dir, 0, *data, dir->Size);
    if (!NT_SUCCESS(status)) {
        ExFreePool(*data);
        *data = nullptr;
    }
    return status;
}

static CHAR UdfUpper(CHAR c) { return c >= 'a' && c <= 'z' ? (CHAR)(c - 0x20) : c; }

static bool UdfSameName(const CHAR *a, ULONG al, const CHAR *b, ULONG bl) {
    if (al != bl)
        return false;
    for (ULONG i = 0; i < al; i++)
        if (UdfUpper(a[i]) != UdfUpper(b[i]))
            return false;
    return true;
}

static NTSTATUS UdfLookup(UDF_VOLUME *v, UDF_FCB *dir, const CHAR *name, ULONG len, UDF_ENTRY *e, ULONG *where) {
    PUCHAR data;
    NTSTATUS status = UdfLoadDirectory(v, dir, &data);
    if (!NT_SUCCESS(status))
        return status;
    ULONG offset = 0;
    status = STATUS_OBJECT_NAME_NOT_FOUND;
    for (;;) {
        ULONG at = offset;
        NTSTATUS st = UdfNextFid(v, dir, data, &offset, e);
        if (st == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(st)) {
            status = st;
            break;
        }
        if (UdfSameName(name, len, e->Name, e->NameLength)) {
            *where = at;
            status = STATUS_SUCCESS;
            break;
        }
    }
    if (data)
        ExFreePool(data);
    return status;
}

// --- IRPs

static NTSTATUS UdfComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information = 0) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static UDF_FCB *UdfNewFcb(UDF_VOLUME *v) {
    auto f = (UDF_FCB *)ExAllocatePoolWithTag(sizeof(UDF_FCB), 'fcdU');
    if (f) {
        RtlZeroMemory(f, sizeof(*f));
        InsertTailList(&v->Fcbs, &f->Link);
    }
    return f;
}

static void UdfFreeFcb(UDF_FCB *f) {
    RemoveEntryList(&f->Link);
    ExFreePool(f);
}

static NTSTATUS UdfCreate(UDF_VOLUME *v, PIRP Irp, ULONG_PTR *info) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    ULONG options = s->Parameters.Create.Options & 0xFFFFFF, disposition = s->Parameters.Create.Options >> 24;
    ACCESS_MASK access = s->Parameters.Create.DesiredAccess;
    OBJECT_STRING rest = *s->Parameters.Create.RemainingName;
    auto related = fo->RelatedFileObject ? (UDF_FCB *)fo->RelatedFileObject->FsContext : nullptr;
    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | FILE_WRITE_ATTRIBUTES))
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    SHARE_ACCESS share;
    IoSetShareAccess(access, s->Parameters.Create.ShareAccess, fo, &share);
    UDF_FCB *f = UdfNewFcb(v);
    if (!f)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = STATUS_SUCCESS;
    if (!related && !rest.Length) { // the volume itself
        f->Volume = TRUE;
        fo->FsContext = f;
        *info = FILE_OPENED;
        return STATUS_SUCCESS;
    }
    if (related) {
        if (!related->Directory) {
            UdfFreeFcb(f);
            return STATUS_INVALID_PARAMETER;
        }
        *f = *related;
        InsertTailList(&v->Fcbs, &f->Link);
    } else if (rest.Buffer[0] != '\\') {
        UdfFreeFcb(f);
        return STATUS_OBJECT_NAME_INVALID;
    } else {
        f->IcbBlock = v->RootIcb;
        status = UdfLoadFileEntry(v, f);
    }
    if (rest.Length > 1 && rest.Buffer[rest.Length - 1] == '\\')
        rest.Length--;
    bool trailing = s->Parameters.Create.RemainingName->Length != rest.Length;
    while (NT_SUCCESS(status) && rest.Length) {
        if (rest.Buffer[0] == '\\')
            rest.Buffer++, rest.Length--;
        USHORT n = 0;
        while (n < rest.Length && rest.Buffer[n] != '\\')
            n++;
        if (!n) {
            if (rest.Length)
                status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        if (!f->Directory) {
            status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }
        UDF_ENTRY e;
        ULONG where;
        status = UdfLookup(v, f, rest.Buffer, n, &e, &where);
        rest.Buffer += n, rest.Length = (USHORT)(rest.Length - n);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            if (rest.Length)
                status = STATUS_OBJECT_PATH_NOT_FOUND;
            else if (disposition == FILE_OPEN_IF)
                status = STATUS_MEDIA_WRITE_PROTECTED;
            break;
        }
        if (!NT_SUCCESS(status))
            break;
        f->IcbBlock = e.Icb;
        f->EntryOffset = where;
        f->NameLength = e.NameLength;
        RtlCopyMemory(f->Name, e.Name, e.NameLength);
        status = UdfLoadFileEntry(v, f);
    }
    if (NT_SUCCESS(status) && f->Directory && (options & FILE_NON_DIRECTORY_FILE))
        status = (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
    if (NT_SUCCESS(status) && !f->Directory && ((options & FILE_DIRECTORY_FILE) || trailing))
        status = (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
    UDF_CCB *ccb = nullptr;
    if (NT_SUCCESS(status)) {
        ccb = (UDF_CCB *)ExAllocatePoolWithTag(sizeof(UDF_CCB), 'ccdU');
        if (!ccb)
            status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!NT_SUCCESS(status)) {
        UdfFreeFcb(f);
        return status;
    }
    RtlZeroMemory(ccb, sizeof(*ccb));
    fo->FsContext = f;
    fo->FsContext2 = ccb;
    *info = FILE_OPENED;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI UdfFsdCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    ULONG_PTR info = 0;
    NTSTATUS status = UdfCreate(v, Irp, &info);
    KeSetEvent(&v->Lock, 0, FALSE);
    return UdfComplete(Irp, status, NT_SUCCESS(status) ? info : 0);
}

static NTSTATUS NTAPI UdfFsdClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    if (auto f = (UDF_FCB *)fo->FsContext)
        UdfFreeFcb(f);
    if (fo->FsContext2)
        ExFreePool(fo->FsContext2);
    fo->FsContext = fo->FsContext2 = nullptr;
    KeSetEvent(&v->Lock, 0, FALSE);
    return UdfComplete(Irp, STATUS_SUCCESS);
}

static NTSTATUS NTAPI UdfFsdRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (UDF_FCB *)fo->FsContext;
    if (!f)
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    if (f->Volume) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        return IofCallDriver(v->TargetDevice, Irp);
    }
    if (f->Directory || Irp->SegmentArray)
        return UdfComplete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    LARGE_INTEGER offset = s->Parameters.Read.ByteOffset;
    ULONG length = s->Parameters.Read.Length;
    // As the reference (and XDVDFS): end of file first; a non-cached read's length need not be whole blocks (the
    // reference rounds the transfer up and reports the clipped length), only its offset must be block-aligned.
    if (offset.HighPart || offset.LowPart >= f->Size)
        return UdfComplete(Irp, length ? STATUS_END_OF_FILE : STATUS_SUCCESS);
    if ((fo->Flags & FO_NO_INTERMEDIATE_BUFFERING) && (offset.LowPart & (UDF_BLOCK - 1)))
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    bool toEnd = length >= ((f->Size + UDF_BLOCK - 1) & ~(UDF_BLOCK - 1)) - offset.LowPart; // as XDVDFS
    if (length > f->Size - offset.LowPart)
        length = f->Size - offset.LowPart;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = UdfReadData(v, f, offset.LowPart, (PUCHAR)Irp->UserBuffer, length, fo->Flags, toEnd);
    KeSetEvent(&v->Lock, 0, FALSE);
    if (NT_SUCCESS(status) && (fo->Flags & FO_SYNCHRONOUS_IO))
        fo->CurrentByteOffset.QuadPart = offset.QuadPart + length;
    return UdfComplete(Irp, status, NT_SUCCESS(status) ? length : 0);
}

static ULONG UdfAttributes(UDF_FCB *f) { return 0x01 | (f->Directory ? 0x10 : 0); } // read-only (+ directory)

static NTSTATUS NTAPI UdfFsdQueryInformation(PDEVICE_OBJECT, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (UDF_FCB *)fo->FsContext;
    auto out = (PUCHAR)Irp->UserBuffer;
    if (!f || f->Volume)
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    ULONG alloc = (f->Size + UDF_BLOCK - 1) & ~(UDF_BLOCK - 1);
    switch (s->Parameters.QueryFile.FileInformationClass) {
    case FileBasicInformation:
        for (int i = 0; i < 4; i++)
            ((PLARGE_INTEGER)out)[i] = f->Time;
        *(PULONG)(out + 32) = UdfAttributes(f);
        return UdfComplete(Irp, STATUS_SUCCESS, 40);
    case FileStandardInformation:
        RtlZeroMemory(out, 24);
        ((PULONG)out)[0] = alloc;
        ((PULONG)out)[2] = f->Size;
        ((PULONG)out)[4] = 1;
        out[21] = f->Directory;
        return UdfComplete(Irp, STATUS_SUCCESS, 24);
    case FileInternalInformation:
        ((PULONG)out)[0] = f->IcbBlock;
        ((PULONG)out)[1] = 0;
        return UdfComplete(Irp, STATUS_SUCCESS, 8);
    case FilePositionInformation:
        *(PLARGE_INTEGER)out = fo->CurrentByteOffset;
        return UdfComplete(Irp, STATUS_SUCCESS, 8);
    case FileNetworkOpenInformation:
        RtlZeroMemory(out, 56);
        for (int i = 0; i < 4; i++)
            ((PLARGE_INTEGER)out)[i] = f->Time;
        ((PULONG)out)[8] = alloc;
        ((PULONG)out)[10] = f->Size;
        ((PULONG)out)[12] = UdfAttributes(f);
        return UdfComplete(Irp, STATUS_SUCCESS, 56);
    default:
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    }
}

static NTSTATUS NTAPI UdfFsdSetInformation(PDEVICE_OBJECT, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s->Parameters.SetFile.FileInformationClass == FilePositionInformation) {
        s->FileObject->CurrentByteOffset = *(PLARGE_INTEGER)Irp->UserBuffer;
        return UdfComplete(Irp, STATUS_SUCCESS);
    }
    return UdfComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED);
}

static bool UdfMatch(const CHAR *m, ULONG ml, const CHAR *n, ULONG nl) {
    if (!ml)
        return !nl;
    if (*m == '*') {
        for (ULONG k = 0; k <= nl; k++)
            if (UdfMatch(m + 1, ml - 1, n + k, nl - k))
                return true;
        return false;
    }
    return nl && (*m == '?' || UdfUpper(*m) == UdfUpper(*n)) && UdfMatch(m + 1, ml - 1, n + 1, nl - 1);
}

static NTSTATUS NTAPI UdfFsdDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = s->FileObject;
    auto f = (UDF_FCB *)fo->FsContext;
    auto ccb = (UDF_CCB *)fo->FsContext2;
    if (s->MinorFunction != 1 || !f || !f->Directory || !ccb)
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    if (s->Parameters.QueryDirectory.FileInformationClass != FileDirectoryInformation)
        return UdfComplete(Irp, STATUS_INVALID_INFO_CLASS);
    bool first = !ccb->HaveMask;
    POBJECT_STRING mask = s->Parameters.QueryDirectory.FileName;
    if (first || (s->Flags & SL_RESTART_SCAN)) {
        ccb->NextOffset = 0;
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
    PUCHAR data;
    NTSTATUS status = UdfLoadDirectory(v, f, &data);
    ULONG used = 0;
    if (NT_SUCCESS(status)) {
        status = (NTSTATUS)0x80000006; // STATUS_NO_MORE_FILES
        for (;;) {
            UDF_ENTRY e;
            ULONG at = ccb->NextOffset, next = at;
            NTSTATUS st = data ? UdfNextFid(v, f, data, &next, &e) : STATUS_NO_MORE_ENTRIES;
            if (st == STATUS_NO_MORE_ENTRIES)
                break;
            if (!NT_SUCCESS(st)) {
                status = st;
                break;
            }
            if (!UdfMatch(ccb->Mask, ccb->MaskLength, e.Name, e.NameLength)) {
                ccb->NextOffset = next;
                continue;
            }
            if (len < 0x40) {
                status = STATUS_BUFFER_TOO_SMALL_IO;
                break;
            }
            UDF_FCB child = {};
            child.IcbBlock = e.Icb;
            st = UdfLoadFileEntry(v, &child);
            if (!NT_SUCCESS(st)) {
                status = st;
                break;
            }
            ccb->NextOffset = next;
            auto p = (PUCHAR)Irp->UserBuffer;
            RtlZeroMemory(p, 0x40);
            *(PULONG)(p + 4) = at;
            for (int i = 0; i < 4; i++)
                ((PLARGE_INTEGER)(p + 8))[i] = child.Time;
            *(PULONG)(p + 0x28) = child.Directory ? 0 : child.Size;
            *(PULONG)(p + 0x30) = child.Directory ? 0 : (child.Size + UDF_BLOCK - 1) & ~(UDF_BLOCK - 1);
            *(PULONG)(p + 0x38) = UdfAttributes(&child);
            *(PULONG)(p + 0x3C) = e.NameLength;
            ULONG room = len - 0x40, copy = e.NameLength < room ? e.NameLength : room;
            RtlCopyMemory(p + 0x40, e.Name, copy);
            used = 0x40 + copy;
            status = copy < e.NameLength ? (NTSTATUS)0x80000005 : STATUS_SUCCESS;
            break;
        }
        if (data)
            ExFreePool(data);
    }
    KeSetEvent(&v->Lock, 0, FALSE);
    if (status == (NTSTATUS)0x80000006 && first)
        status = (NTSTATUS)0xC000000F; // STATUS_NO_SUCH_FILE
    return UdfComplete(Irp, status, used);
}

static NTSTATUS NTAPI UdfFsdQueryVolume(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    auto out = (PULONG)Irp->UserBuffer;
    ULONG len = s->Parameters.QueryVolume.Length;
    switch (s->Parameters.QueryVolume.FsInformationClass) {
    case FileFsSizeInformation:
        if (len < 24)
            return UdfComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = v->PartitionLength, out[1] = 0;
        out[2] = out[3] = 0;
        out[4] = 1, out[5] = UDF_BLOCK;
        return UdfComplete(Irp, STATUS_SUCCESS, 24);
    case FileFsDeviceInformation:
        if (len < 8)
            return UdfComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = FILE_DEVICE_CD_ROM, out[1] = 1; // FILE_REMOVABLE_MEDIA
        return UdfComplete(Irp, STATUS_SUCCESS, 8);
    case FileFsAttributeInformation:
        if (len < 15)
            return UdfComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        out[0] = 0x80000, out[1] = 255, out[2] = 3; // FILE_READ_ONLY_VOLUME
        RtlCopyMemory(out + 3, "UDF", 3);
        return UdfComplete(Irp, STATUS_SUCCESS, 15);
    case FileFsVolumeInformation:
        if (len < 18)
            return UdfComplete(Irp, STATUS_INFO_LENGTH_MISMATCH);
        RtlZeroMemory(out, 18);
        return UdfComplete(Irp, STATUS_SUCCESS, 18);
    default:
        return UdfComplete(Irp, STATUS_INVALID_PARAMETER);
    }
}

static NTSTATUS NTAPI UdfFsdPassDown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IofCallDriver(UdfVolume(DeviceObject)->TargetDevice, Irp);
}

static NTSTATUS NTAPI UdfFsdSuccess(PDEVICE_OBJECT, PIRP Irp) { return UdfComplete(Irp, STATUS_SUCCESS); }

static NTSTATUS NTAPI UdfDismountVolume(PDEVICE_OBJECT DeviceObject) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (v->TargetDevice->MountedOrSelfDevice == DeviceObject)
        v->TargetDevice->MountedOrSelfDevice = nullptr;
    KfLowerIrql(old);
    IoDeleteDevice(DeviceObject);
    return STATUS_SUCCESS;
}

static void NTAPI UdfDeleteVolume(PDEVICE_OBJECT DeviceObject) {
    UDF_VOLUME *v = UdfVolume(DeviceObject);
    FscInvalidateDevice(&v->CacheExtension);
    ExFreePool(v->Block);
    ObfDereferenceObject(v->TargetDevice);
}

DRIVER_OBJECT UdfDriverObject = {
    nullptr,
    UdfDeleteVolume,
    UdfDismountVolume,
    {UdfFsdCreate, UdfFsdClose, UdfFsdRead, IoInvalidDeviceRequest, UdfFsdQueryInformation, UdfFsdSetInformation,
     UdfFsdSuccess, UdfFsdQueryVolume, UdfFsdDirectoryControl, IoFsControlDismountOnly, UdfFsdPassDown,
     IoInvalidDeviceRequest, IoInvalidDeviceRequest, UdfFsdSuccess}};

// --- Mount (spec/04d §1)

static const struct {
    char Id[6];
    UCHAR Code;
} UdfVrsIds[] = {{"BEA01", 1}, {"TEA01", 2}, {"CDROM", 3}, {"CD001", 4}, {"NSR01", 7},
                 {"NSR02", 8}, {"CDW01", 5}, {"CDW02", 6}, {"BOOT2", 9}};

// Volume recognition from sector 16: ISO 9660 descriptors may come first, then BEA01 ... NSR02.
static NTSTATUS UdfRecognize(PDEVICE_OBJECT target, ULONG sectors, PUCHAR b) {
    bool beginning = false;
    for (ULONG sector = 16; sector < sectors; sector++) {
        NTSTATUS status = UdfReadSector(target, sector, b);
        if (!NT_SUCCESS(status))
            return status;
        UCHAR code = 0;
        for (auto &id : UdfVrsIds)
            if (RtlCompareMemory(b + 1, id.Id, 5) == 5)
                code = id.Code;
        if (!code)
            return STATUS_UNRECOGNIZED_VOLUME;
        if (b[0] == 0) {
            if (code == 1) {
                if (beginning || b[6] != 1)
                    return STATUS_UNRECOGNIZED_VOLUME;
                beginning = true;
            } else if (code == 8) {
                return beginning && b[6] == 1 ? STATUS_SUCCESS : STATUS_UNRECOGNIZED_VOLUME;
            } else if (code == 2) {
                return STATUS_UNRECOGNIZED_VOLUME;
            }
        } else if ((b[0] > 2 && b[0] != 0xFF) || beginning || (code != 3 && code != 4)) {
            return STATUS_UNRECOGNIZED_VOLUME;
        }
    }
    return STATUS_UNRECOGNIZED_VOLUME;
}

static const UCHAR UdfCs0[] = "\0OSTA Compressed Unicode"; // charspec: type 0 + name, zero-padded to 64

static bool UdfIsCs0(const UCHAR *c) {
    for (ULONG i = 0; i < 64; i++)
        if (c[i] != (i < sizeof(UdfCs0) - 1 ? UdfCs0[i] : 0))
            return false;
    return true;
}

// The main volume descriptor sequence: the PVD, partition descriptor and LVD with the highest sequence numbers;
// they must agree on the partition. Returns the partition and the file set descriptor extent.
static NTSTATUS UdfReadVds(PDEVICE_OBJECT target, ULONG length, ULONG location, PUCHAR b, ULONG *partStart,
                           ULONG *partLength, ULONG fsd[4]) {
    if (!length || (length & (UDF_BLOCK - 1)))
        return STATUS_DISK_CORRUPT;
    bool pvdOk = false, pdOk = false, lvdOk = false;
    ULONG pvdSeq = 0, pdSeq = 0, lvdSeq = 0;
    USHORT pdNumber = 0, lvdNumber = 0;
    for (ULONG s = location, end = location + length / UDF_BLOCK; s < end; s++) {
        NTSTATUS status = UdfReadSector(target, s, b);
        if (!NT_SUCCESS(status))
            return status;
        USHORT id = *(USHORT *)b;
        if (!UdfTagOk(b, id, s))
            break;
        ULONG seq = *(ULONG *)(b + 16);
        if (id == TAG_PVD && seq >= pvdSeq) {
            pvdSeq = seq;
            pvdOk = *(USHORT *)(b + 56) < 2 && *(ULONG *)(b + 64) == 1 && *(ULONG *)(b + 68) == 1 &&
                    UdfIsCs0(b + 200) && UdfIsCs0(b + 264);
        } else if (id == TAG_PD && seq >= pdSeq) {
            pdSeq = seq;
            pdOk = RtlCompareMemory(b + 25, "+NSR02", 6) == 6;
            if (pdOk) {
                *partStart = *(ULONG *)(b + 188);
                *partLength = *(ULONG *)(b + 192);
                pdNumber = *(USHORT *)(b + 22);
            }
        } else if (id == TAG_LVD && seq >= lvdSeq) {
            lvdSeq = seq;
            ULONG mapTable = *(ULONG *)(b + 264);
            USHORT revision = *(USHORT *)(b + 240);
            lvdOk = ((mapTable + 0x1BB) & ~3u) <= UDF_BLOCK && *(ULONG *)(b + 212) == UDF_BLOCK &&
                    RtlCompareMemory(b + 217, "*OSTA UDF Compliant", 19) == 19 && revision >= 0x100 &&
                    revision <= 0x150 && UdfIsCs0(b + 20) && *(ULONG *)(b + 268) < 2 && mapTable > 5 &&
                    b[440] == 1 && b[441] == 6 && *(USHORT *)(b + 442) < 2;
            if (lvdOk) {
                RtlCopyMemory(fsd, b + 248, 16);
                lvdNumber = *(USHORT *)(b + 444);
            }
        } else if (id == TAG_TERMINATOR) {
            return pvdOk && pdOk && lvdOk && pdNumber == lvdNumber ? STATUS_SUCCESS : STATUS_UNRECOGNIZED_VOLUME;
        }
    }
    return STATUS_UNRECOGNIZED_VOLUME;
}

// File set descriptors (in the partition): the highest file set number gives the root directory ICB; the scan
// follows "next extent" links and must end at a terminating descriptor.
static NTSTATUS UdfFindRoot(PDEVICE_OBJECT target, ULONG partStart, ULONG partLength, const ULONG fsd[4],
                            PUCHAR b, ULONG *root) {
    ULONG length = fsd[0], block = fsd[1], best = 0;
    bool found = false;
    for (ULONG extents = 0; extents < 64; extents++) {
        if (!(length & 0x3FFFFFFF) || (length & 0xC00007FF))
            return STATUS_DISK_CORRUPT;
        ULONG end = block + ((length >> 11) & 0x7FFFF);
        bool next = false;
        while (block < end && !next) {
            if (block >= partLength)
                return STATUS_DISK_CORRUPT;
            NTSTATUS status = UdfReadSector(target, partStart + block, b);
            if (!NT_SUCCESS(status))
                return status;
            USHORT id = *(USHORT *)b;
            if (!UdfTagOk(b, id, block))
                return STATUS_DISK_CORRUPT;
            if (id != TAG_FSD)
                return id == TAG_TERMINATOR && found ? STATUS_SUCCESS : STATUS_DISK_CORRUPT;
            if (*(ULONG *)(b + 40) >= best) {
                best = *(ULONG *)(b + 40);
                *root = *(ULONG *)(b + 404); // root directory ICB (long_ad: length, block, partition)
                found = true;
                if (*(ULONG *)(b + 448) & 0x3FFFFFFF) { // next extent of the file set descriptor sequence
                    length = *(ULONG *)(b + 448), block = *(ULONG *)(b + 452);
                    next = true;
                    continue;
                }
            }
            block++;
        }
        if (!next)
            return STATUS_DISK_CORRUPT;
    }
    return STATUS_DISK_CORRUPT;
}

volatile ULONG UdfMountStage, UdfMountStatus; // diagnostics: how far the last mount attempt got

NTSTATUS UdfMountVolume(PDEVICE_OBJECT TargetDevice) {
    DISK_GEOMETRY g;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(0x2404C, TargetDevice, nullptr, 0, &g, sizeof(g), nullptr,
                                                          FALSE);
    if (!NT_SUCCESS(status))
        return status;
    auto b = (PUCHAR)ExAllocatePoolWithTag(UDF_BLOCK, 'bfdU');
    if (!b)
        return STATUS_INSUFFICIENT_RESOURCES;
    ULONG sectors = (ULONG)g.Cylinders.QuadPart, partStart = 0, partLength = 0, fsd[4] = {}, root = 0;
    if (NT_SUCCESS(status))
        UdfMountStage = 1;
    status = UdfRecognize(TargetDevice, sectors, b);
    if (NT_SUCCESS(status))
        status = sectors > 256 ? UdfReadSector(TargetDevice, 256, b) : STATUS_UNRECOGNIZED_VOLUME;
    if (NT_SUCCESS(status))
        UdfMountStage = 2;
    if (NT_SUCCESS(status) && !UdfTagOk(b, TAG_AVDP, 256))
        status = STATUS_DISK_CORRUPT;
    if (NT_SUCCESS(status))
        UdfMountStage = 3;
    if (NT_SUCCESS(status))
        status = UdfReadVds(TargetDevice, *(ULONG *)(b + 16), *(ULONG *)(b + 20), b, &partStart, &partLength, fsd);
    if (NT_SUCCESS(status))
        UdfMountStage = 4;
    if (NT_SUCCESS(status))
        status = UdfFindRoot(TargetDevice, partStart, partLength, fsd, b, &root);
    UdfMountStatus = status;
    PDEVICE_OBJECT dev = nullptr;
    if (NT_SUCCESS(status))
        status = IoCreateDevice(&UdfDriverObject, sizeof(UDF_VOLUME), nullptr, 8, FALSE, &dev);
    if (!NT_SUCCESS(status)) {
        ExFreePool(b);
        return status == STATUS_DISK_CORRUPT ? STATUS_UNRECOGNIZED_VOLUME : status;
    }
    dev->StackSize = (CHAR)(dev->StackSize + TargetDevice->StackSize);
    dev->SectorSize = UDF_BLOCK;
    UDF_VOLUME *v = UdfVolume(dev);
    v->TargetDevice = TargetDevice;
    v->PartitionStart = partStart;
    v->PartitionLength = partLength;
    v->RootIcb = root;
    v->MediaLength = g.Cylinders.QuadPart * UDF_BLOCK;
    v->CacheExtension.TargetDeviceObject = TargetDevice;
    v->CacheExtension.PartitionLength.QuadPart = v->MediaLength;
    v->CacheExtension.SectorSize = UDF_BLOCK;
    v->Block = b;
    KeInitializeEvent(&v->Lock, SynchronizationEvent, TRUE);
    InitializeListHead(&v->Fcbs);
    ObfReferenceObject(TargetDevice);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
