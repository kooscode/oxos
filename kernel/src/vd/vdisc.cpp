// Virtual disc images: an Xbox disc image (XISO, a full disc dump or CCI) on the hard disk stands in for the DVD
// drive (spec/oxos-internal/16-virtual-discs.md). The device interface is the one Cerbios titles and tools use
// (spec/cerbios/07): the control device \Device\Virtual0\Image0 (links \Device\CdRom1, \??\CdRom1:), mount and unmount
// device controls, image slots 1 and 2 (\Device\Virtual0\Image1, Image2). OXOS design:
// - A mounted DVD image is the console's disc: \Device\CdRom0 serves it (VdServeCdRom, called by the drive's read
//   and device-control routines), so D: and every title's view of the drive need no renamed devices or links.
// - Mounts survive quick reboots (the slot table is in .persist): a launcher mounts an image, then launches
//   \Device\CdRom0\default.xbe through a quick reboot. A cold boot (or an eject, which resets the console) forgets them.
// - Image files are read through their file system with the ordinary file calls, in the caller's thread.
#include <xk/idex.h>
#include <xk/io.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>
#include <xk/rtl.h>
#include <xk/serial.h>

XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...);
XBAPI LONG FASTCALL InterlockedIncrement(volatile LONG *);
XBAPI LONG FASTCALL InterlockedDecrement(volatile LONG *);
void IdexCdRomEmulatedSecurityReply(const UCHAR *cdb, PUCHAR data, ULONG length, NTSTATUS *status, ULONG *got);
bool IdexCdRomIsSecurityCommand(const UCHAR *cdb, const UCHAR *data, ULONG length);
XBAPI NTSTATUS NTAPI IoDismountVolume(PDEVICE_OBJECT DeviceObject);
extern PDEVICE_OBJECT IdexCdRomDeviceObject;
extern ULONG XeDiscTitleId;
extern UCHAR XeTitleDiscAuthenticated;
extern DRIVER_OBJECT VdDriverObject;
ULONG VdReads, VdErrors;            // diagnostics (tests/boot_test.py): image reads, failed ones
volatile NTSTATUS VdLastMountStatus; // the last mount or unmount request's result

namespace {

constexpr ULONG VD_SLOTS = 2;      // images 1 and 2 (0 is the control device)
constexpr ULONG VD_NAME = 0x106;   // the longest path or link name kept (as Cerbios)
constexpr ULONG VD_SECTOR = 0x800;
constexpr ULONGLONG VD_GAME_PARTITION = 0x18300000; // full disc dumps: where the game partition starts
constexpr ULONG IOCTL_VD_PRESENT = 0x1EE7CD00, IOCTL_VD_MOUNT = 0x1EE7CD01, IOCTL_VD_UNMOUNT = 0x1EE7CD02,
                IOCTL_VD_PRESENT2 = 0x0CE52B00, IOCTL_VD_MOUNT2 = 0x0CE52B01, IOCTL_VD_UNMOUNT2 = 0x0CE52B02;
constexpr ULONG IOCTL_CDROM_READ_TOC = 0x24000, IOCTL_CDROM_GET_LAST_SESSION = 0x24038,
                IOCTL_CDROM_GET_DRIVE_GEOMETRY = 0x2404C, IOCTL_CDROM_AUTHENTICATE_DISK = 0x24080,
                IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014;
constexpr NTSTATUS STATUS_NO_MEDIA_IN_DEVICE_ = (NTSTATUS)0xC0000013, STATUS_UNRECOGNIZED_MEDIA_ = (NTSTATUS)0xC0000014;
constexpr ULONG CCI_MAGIC = 0x4D494343; // "CCIM"
constexpr ULONG CCI_WINDOW = 1024;      // index entries cached per part

// What a mount needs again after a quick reboot.
struct VD_SAVED {
    UCHAR InUse, Type, Parts, Pad;
    CHAR Path[2][VD_NAME + 1];
    CHAR Link[VD_NAME + 1];
};

struct VD_PART {
    HANDLE File;
    ULONGLONG Start, Length; // the part's range of the image (bytes)
    // CCI: the block index and a window of it.
    ULONGLONG IndexAt;
    ULONG FirstBlock, Blocks;
    ULONG WindowFirst; // first block whose entry is in Window; 0xFFFFFFFF = empty
    PULONG Window;     // CCI_WINDOW + 1 entries
};

struct VD_SLOT {
    PDEVICE_OBJECT Device;
    volatile BOOLEAN Mounted;
    UCHAR Type;
    UCHAR Parts;
    BOOLEAN LinkMade;
    ULONGLONG Skip;   // full disc dumps: the game partition's offset
    ULONGLONG Length; // the image as presented (bytes)
    VD_PART Part[2];
    volatile LONG Busy; // requests in progress (unmount waits for them)
    KEVENT Lock;        // CCI: one block buffer per slot
    PUCHAR Block;       // CCI: a stored block (up to two sectors)
};

XK_PERSIST VD_SAVED VdSaved[VD_SLOTS];
VD_SLOT VdSlot[VD_SLOTS];
PDEVICE_OBJECT VdControl;
volatile LONG VdDvd; // the slot (1, 2) that stands in for the drive, 0 = the drive itself
KEVENT VdMountLock;  // mount and unmount, one at a time

bool IsDvdType(UCHAR t) { return t == 'D' || t == 'd'; }

NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information = 0) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IofCompleteRequest(Irp, 0);
    return Status;
}

NTSTATUS ReadFile(HANDLE File, ULONGLONG Offset, PVOID Buffer, ULONG Length) {
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER o;
    o.QuadPart = (LONGLONG)Offset;
    NTSTATUS status = NtReadFile(File, nullptr, nullptr, nullptr, &iosb, Buffer, Length, &o);
    if (NT_SUCCESS(status) && iosb.Information != Length)
        status = STATUS_END_OF_FILE;
    return status;
}

// --- LZ4 blocks (CCI)

bool Lz4Decode(const UCHAR *src, ULONG srcLen, PUCHAR dst, ULONG dstLen) {
    ULONG ip = 0, op = 0;
    while (ip < srcLen) {
        UCHAR token = src[ip++];
        ULONG n = token >> 4;
        if (n == 15) {
            UCHAR b;
            do {
                if (ip >= srcLen)
                    return false;
                n += b = src[ip++];
            } while (b == 255);
        }
        if (n > srcLen - ip || n > dstLen - op)
            return false;
        RtlCopyMemory(dst + op, src + ip, n);
        ip += n, op += n;
        if (ip == srcLen) // the last sequence has literals only
            break;
        if (srcLen - ip < 2)
            return false;
        ULONG distance = src[ip] | (ULONG)src[ip + 1] << 8;
        ip += 2;
        n = (token & 15) + 4;
        if ((token & 15) == 15) {
            UCHAR b;
            do {
                if (ip >= srcLen)
                    return false;
                n += b = src[ip++];
            } while (b == 255);
        }
        if (!distance || distance > op || n > dstLen - op)
            return false;
        for (ULONG i = 0; i < n; i++, op++) // byte by byte: the copy may overlap its source
            dst[op] = dst[op - distance];
    }
    return op == dstLen;
}

// The stored offset of CCI block `Block` (and the next) of a part: index entries × 4, through the cached window.
NTSTATUS CciEntries(VD_PART *p, ULONG Block, ULONG *This, ULONG *Next) {
    if (p->WindowFirst == 0xFFFFFFFF || Block < p->WindowFirst || Block >= p->WindowFirst + CCI_WINDOW) {
        ULONG first = Block & ~(CCI_WINDOW - 1);
        ULONG count = p->Blocks + 1 - first < CCI_WINDOW + 1 ? p->Blocks + 1 - first : CCI_WINDOW + 1;
        p->WindowFirst = 0xFFFFFFFF;
        NTSTATUS status = ReadFile(p->File, p->IndexAt + (ULONGLONG)first * 4, p->Window, count * 4);
        if (!NT_SUCCESS(status))
            return status;
        p->WindowFirst = first;
    }
    *This = p->Window[Block - p->WindowFirst] << 2; // 32-bit: bits 30-31 (the writer's flags) drop out
    *Next = p->Window[Block - p->WindowFirst + 1] << 2;
    return STATUS_SUCCESS;
}

NTSTATUS CciReadBlock(VD_SLOT *s, VD_PART *p, ULONG Block, PUCHAR Out) {
    ULONG at, next;
    NTSTATUS status = CciEntries(p, Block, &at, &next);
    if (!NT_SUCCESS(status))
        return status;
    ULONG stored = next - at;
    if (next < at || !stored || stored > 2 * VD_SECTOR)
        return (NTSTATUS)0xC0000032; // STATUS_DISK_CORRUPT_ERROR
    if (stored == VD_SECTOR) // kept as it is
        return ReadFile(p->File, at, Out, VD_SECTOR);
    status = ReadFile(p->File, at, s->Block, stored);
    if (!NT_SUCCESS(status))
        return status;
    ULONG pad = s->Block[0]; // padding count, then the LZ4 block, then the padding
    if (pad + 1 > stored || !Lz4Decode(s->Block + 1, stored - 1 - pad, Out, VD_SECTOR))
        return (NTSTATUS)0xC0000032;
    return STATUS_SUCCESS;
}

// Image bytes [Offset, Offset + Length) of a slot: whole sectors (the drive's rules).
NTSTATUS ReadImage(VD_SLOT *s, ULONGLONG Offset, PUCHAR Buffer, ULONG Length) {
    if (s->Type == 'd') {
        KeWaitForSingleObject(&s->Lock, Executive, KernelMode, FALSE, nullptr);
        NTSTATUS status = STATUS_SUCCESS;
        for (; Length && NT_SUCCESS(status); Offset += VD_SECTOR, Buffer += VD_SECTOR, Length -= VD_SECTOR) {
            ULONG i = Offset >= s->Part[1].Start && s->Parts > 1 ? 1 : 0;
            VD_PART *p = &s->Part[i];
            status = CciReadBlock(s, p, (ULONG)((Offset - p->Start) / VD_SECTOR), Buffer);
        }
        KeSetEvent(&s->Lock, 0, FALSE);
        return status;
    }
    Offset += s->Skip;
    while (Length) {
        ULONG i = s->Parts > 1 && Offset >= s->Part[1].Start ? 1 : 0;
        VD_PART *p = &s->Part[i];
        ULONGLONG within = Offset - p->Start, left = p->Length - within;
        ULONG n = left < Length ? (ULONG)left : Length;
        if (!n)
            return STATUS_INVALID_PARAMETER;
        NTSTATUS status = ReadFile(p->File, within, Buffer, n);
        if (!NT_SUCCESS(status))
            return status;
        Offset += n, Buffer += n, Length -= n;
    }
    return STATUS_SUCCESS;
}

// --- Requests to an image (its own device, or \Device\CdRom0 while it stands in for the drive)

NTSTATUS ImageRead(VD_SLOT *s, PIRP Irp) {
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(Irp);
    ULONG length = st->Parameters.Read.Length;
    LONGLONG offset = st->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR buffer = (PUCHAR)Irp->UserBuffer + st->Parameters.Read.BufferOffset;
    if (st->MajorFunction != IRP_MJ_READ)
        return Complete(Irp, STATUS_INVALID_PARAMETER); // read-only media
    if ((length | (ULONG)offset) & (VD_SECTOR - 1) || offset < 0 || (ULONGLONG)offset + length > s->Length ||
        Irp->SegmentArray) {
        XK_SERIAL("vd: read %#llx+%#lx rejected (image %#llx bytes%s)\n", (ULONGLONG)offset, length, s->Length,
                  Irp->SegmentArray ? ", scatter list" : "");
        return Complete(Irp, STATUS_INVALID_PARAMETER);
    }
    VdReads++;
    NTSTATUS status = length ? ReadImage(s, (ULONGLONG)offset, buffer, length) : STATUS_SUCCESS;
    if (!NT_SUCCESS(status)) {
        VdErrors++;
        XK_SERIAL("vd: read %#llx+%#lx: %#lx\n", (ULONGLONG)offset, length, status);
    }
    return Complete(Irp, status, length);
}

NTSTATUS ImageControl(VD_SLOT *s, PIRP Irp) {
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = st->Parameters.DeviceIoControl.OutputBufferLength;
    auto out = (PUCHAR)Irp->UserBuffer;
    if (st->Parameters.DeviceIoControl.IoControlCode != IOCTL_SCSI_PASS_THROUGH_DIRECT) // logged with its command
        XK_SERIAL("vd: device control %#lx (in %lu, out %lu)\n", st->Parameters.DeviceIoControl.IoControlCode,
                  st->Parameters.DeviceIoControl.InputBufferLength, outLen);
    switch (st->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_CDROM_READ_TOC:
    case IOCTL_CDROM_GET_LAST_SESSION: {
        // One data track (as Cerbios answers for images).
        static const UCHAR toc[12] = {0, 0x12, 1, 1, 0, 0x14, 1, 0, 0, 0, 2, 0};
        if (outLen < sizeof(toc))
            return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
        RtlCopyMemory(out, toc, sizeof(toc));
        return Complete(Irp, STATUS_SUCCESS, sizeof(toc));
    }
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY: {
        if (outLen < sizeof(DISK_GEOMETRY))
            return Complete(Irp, STATUS_BUFFER_TOO_SMALL_IO);
        auto g = (PDISK_GEOMETRY)out;
        g->Cylinders.QuadPart = (LONGLONG)(s->Length / VD_SECTOR);
        g->MediaType = 11; // RemovableMedia
        g->TracksPerCylinder = 1;
        g->SectorsPerTrack = 1;
        g->BytesPerSector = VD_SECTOR;
        return Complete(Irp, STATUS_SUCCESS, sizeof(DISK_GEOMETRY));
    }
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_CDROM_AUTHENTICATE_DISK:
        return Complete(Irp, STATUS_SUCCESS);
    case IOCTL_SCSI_PASS_THROUGH_DIRECT: {
        // The Xbox security commands get the drive emulation's answers (titles that check the disc themselves);
        // TEST UNIT READY succeeds; anything else is not supported.
        struct PASS { // SCSI_PASS_THROUGH_DIRECT, Xbox layout (idex/cdrom.cpp)
            USHORT Length;
            UCHAR ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength, DataIn, Pad[3];
            ULONG DataTransferLength, TimeOutValue;
            PVOID DataBuffer;
            ULONG SenseInfoOffset;
            UCHAR Cdb[16];
        };
        auto p = (PASS *)st->Parameters.DeviceIoControl.InputBuffer;
        if (st->Parameters.DeviceIoControl.InputBufferLength < sizeof(PASS))
            return Complete(Irp, STATUS_INVALID_PARAMETER);
        NTSTATUS status = STATUS_SUCCESS;
        ULONG got = 0;
        const UCHAR *cdb = p->Cdb;
        auto data = (PUCHAR)p->DataBuffer;
        // The same set as the drive emulation's: MODE SENSE / SELECT of page 0x3E and READ DVD STRUCTURE of the
        // security sector (address 0xFF02FDFF, layer 0xFE). This used to test the layer in the wrong byte, so a
        // title's own security-sector read failed ("dirty disc": Prince of Persia).
        bool security = IdexCdRomIsSecurityCommand(cdb, data, p->DataTransferLength);
        if (security)
            IdexCdRomEmulatedSecurityReply(cdb, data, p->DataTransferLength, &status, &got);
        else if (cdb[0] != 0x00) // TEST UNIT READY
            status = STATUS_INVALID_DEVICE_REQUEST;
        XK_SERIAL("vd: pass-through %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (%lu bytes): %#lx, %lu back%s\n",
                  cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9], p->DataTransferLength,
                  status, got, security ? " (security, emulated)" : "");
        p->ScsiStatus = NT_SUCCESS(status) ? 0 : 2;
        p->DataTransferLength = got;
        return Complete(Irp, status, got);
    }
    default:
        XK_SERIAL("vd:   not supported\n");
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

// --- Mount and unmount

void Unmount(ULONG n) {
    VD_SLOT *s = &VdSlot[n - 1];
    VdSaved[n - 1].InUse = 0;
    if (!s->Mounted)
        return;
    s->Mounted = FALSE;
    if (VdDvd == (LONG)n) {
        ULONG other = 3 - n;
        VdDvd = VdSlot[other - 1].Mounted && IsDvdType(VdSlot[other - 1].Type) ? (LONG)other : 0;
        if (IdexCdRomDeviceObject)
            IoDismountVolume(IdexCdRomDeviceObject); // the next open mounts the drive (or the other image)
        XeTitleDiscAuthenticated = 0;
        XeDiscTitleId = 0;
    }
    if (s->Device && s->Device->MountedOrSelfDevice != s->Device)
        IoDismountVolume(s->Device);
    while (s->Busy) { // requests that started before the unmount
        LARGE_INTEGER t;
        t.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    if (s->LinkMade) {
        OBJECT_STRING link;
        RtlInitAnsiString(&link, VdSaved[n - 1].Link);
        IoDeleteSymbolicLink(&link);
        s->LinkMade = FALSE;
    }
    for (ULONG i = 0; i < s->Parts; i++) {
        NtClose(s->Part[i].File);
        if (s->Part[i].Window)
            ExFreePool(s->Part[i].Window);
    }
    if (s->Block)
        ExFreePool(s->Block);
    s->Block = nullptr;
    RtlZeroMemory(s->Part, sizeof(s->Part));
    s->Parts = 0;
    XK_SERIAL("vd: slot %lu unmounted\n", n);
}

void UnmountAll() {
    for (ULONG n = 1; n <= VD_SLOTS; n++)
        Unmount(n);
}

NTSTATUS OpenPart(const CHAR *Path, HANDLE *File, ULONGLONG *Size) {
    OBJECT_STRING name;
    RtlInitAnsiString(&name, Path);
    OBJECT_ATTRIBUTES oa = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtCreateFile(File, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, nullptr, 0, FILE_SHARE_READ, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        return status;
    struct {
        LARGE_INTEGER AllocationSize, EndOfFile;
        ULONG NumberOfLinks;
        BOOLEAN DeletePending, Directory;
    } info = {};
    status = NtQueryInformationFile(*File, &iosb, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        NtClose(*File);
        return status;
    }
    *Size = (ULONGLONG)info.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

// Bring slot n up from VdSaved[n - 1]: open the parts, read the format, create the device and the link.
NTSTATUS Attach(ULONG n) {
    VD_SAVED *saved = &VdSaved[n - 1];
    VD_SLOT *s = &VdSlot[n - 1];
    NTSTATUS status = STATUS_SUCCESS;
    s->Type = saved->Type;
    s->Skip = 0;
    s->Length = 0;
    s->Parts = 0;
    for (ULONG i = 0; i < saved->Parts && NT_SUCCESS(status); i++) {
        VD_PART *p = &s->Part[i];
        ULONGLONG size;
        status = OpenPart(saved->Path[i], &p->File, &size);
        if (!NT_SUCCESS(status))
            break;
        s->Parts++;
        p->Start = s->Length;
        if (s->Type == 'd') {
            // CCI header: magic, header size 0x20, image size, index offset, block size 0x800, version 1, alignment 2.
            UCHAR h[32];
            status = ReadFile(p->File, 0, h, sizeof(h));
            if (NT_SUCCESS(status) && (*(PULONG)h != CCI_MAGIC || *(PULONG)(h + 4) != 0x20 ||
                                       *(PULONG)(h + 0x18) != VD_SECTOR || h[0x1C] != 1 || h[0x1D] != 2))
                status = STATUS_UNRECOGNIZED_MEDIA_;
            if (!NT_SUCCESS(status))
                break;
            p->Length = *(ULONGLONG *)(h + 8) & ~(ULONGLONG)(VD_SECTOR - 1);
            p->IndexAt = *(ULONGLONG *)(h + 0x10);
            p->Blocks = (ULONG)(p->Length / VD_SECTOR);
            p->WindowFirst = 0xFFFFFFFF;
            p->Window = (PULONG)ExAllocatePoolWithTag((CCI_WINDOW + 1) * 4, 'iCdV');
            if (!p->Window) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
        } else {
            p->Length = size & ~(ULONGLONG)(VD_SECTOR - 1);
        }
        s->Length += p->Length;
    }
    if (NT_SUCCESS(status) && s->Type == 'd' && !(s->Block = (PUCHAR)ExAllocatePoolWithTag(2 * VD_SECTOR, 'bCdV')))
        status = STATUS_INSUFFICIENT_RESOURCES;
    if (NT_SUCCESS(status) && s->Type == 'D' && s->Part[0].Length > VD_GAME_PARTITION + 0x10800) {
        // A full disc dump: the game partition's volume descriptor (sector 32 of it) starts and ends with the magic.
        auto sector = (PUCHAR)ExAllocatePoolWithTag(VD_SECTOR, 'sCdV');
        if (sector) {
            if (NT_SUCCESS(ReadFile(s->Part[0].File, VD_GAME_PARTITION + 32 * VD_SECTOR, sector, VD_SECTOR)) &&
                RtlCompareMemory(sector, "MICROSOFT*XBOX*MEDIA", 20) == 20 &&
                RtlCompareMemory(sector + VD_SECTOR - 20, "MICROSOFT*XBOX*MEDIA", 20) == 20) {
                s->Skip = VD_GAME_PARTITION;
                s->Length -= VD_GAME_PARTITION;
            }
            ExFreePool(sector);
        }
    }
    if (NT_SUCCESS(status) && !s->Length)
        status = STATUS_UNRECOGNIZED_MEDIA_;
    if (NT_SUCCESS(status) && !s->Device) {
        CHAR name[32];
        RtlSnprintf(name, sizeof(name), "\\Device\\Virtual0\\Image%lu", n);
        OBJECT_STRING os;
        RtlInitAnsiString(&os, name);
        status = IoCreateDevice(&VdDriverObject, sizeof(ULONG), &os, FILE_DEVICE_CD_ROM, FALSE, &s->Device);
        if (NT_SUCCESS(status)) {
            *(PULONG)s->Device->DeviceExtension = n;
            s->Device->Flags |= DO_DIRECT_IO | 0x40;
            s->Device->AlignmentRequirement = 1;
            s->Device->SectorSize = VD_SECTOR;
            s->Device->Flags &= ~DO_DEVICE_INITIALIZING;
        } else {
            s->Device = nullptr;
        }
    }
    if (!NT_SUCCESS(status)) {
        for (ULONG i = 0; i < s->Parts; i++) {
            NtClose(s->Part[i].File);
            if (s->Part[i].Window)
                ExFreePool(s->Part[i].Window);
        }
        if (s->Block)
            ExFreePool(s->Block);
        s->Block = nullptr;
        RtlZeroMemory(s->Part, sizeof(s->Part));
        s->Parts = 0;
        saved->InUse = 0;
        XK_SERIAL("vd: slot %lu: %s failed %#lx\n", n, saved->Path[0], status);
        return status;
    }
    // The image's own link, unless it names the drive (which already reaches the image).
    s->LinkMade = FALSE;
    if (saved->Link[0]) {
        OBJECT_STRING link, target;
        RtlInitAnsiString(&link, saved->Link);
        if (RtlCompareMemory(saved->Link, "\\Device\\CdRom0", 15) != 15 &&
            RtlCompareMemory(saved->Link, "\\??\\CdRom0:", 12) != 12) {
            CHAR name[32];
            RtlSnprintf(name, sizeof(name), "\\Device\\Virtual0\\Image%lu", n);
            RtlInitAnsiString(&target, name);
            s->LinkMade = NT_SUCCESS(IoCreateSymbolicLink(&link, &target));
        }
    }
    s->Mounted = TRUE;
    if (IsDvdType(s->Type)) {
        VdDvd = (LONG)n;
        XeTitleDiscAuthenticated = 0;
        XeDiscTitleId = 0;
        if (IdexCdRomDeviceObject)
            IoDismountVolume(IdexCdRomDeviceObject); // the next open mounts the image
    }
    XK_SERIAL("vd: slot %lu: %c, %lu part(s), %#llx bytes%s: %s\n", n, s->Type, (ULONG)s->Parts, s->Length,
              s->Skip ? " (full dump)" : "", saved->Path[0]);
    return STATUS_SUCCESS;
}

// A counted ANSI string from the request, copied (bounded) into Out.
bool CopyName(const UCHAR *Field, CHAR *Out) {
    auto str = (const OBJECT_STRING *)Field;
    ULONG n = str->Length < VD_NAME ? str->Length : VD_NAME;
    Out[0] = 0;
    if (!n)
        return true;
    if (!str->Buffer || !MmIsAddressValid(str->Buffer) || !MmIsAddressValid(str->Buffer + n - 1))
        return false;
    RtlCopyMemory(Out, str->Buffer, n);
    Out[n] = 0;
    return true;
}

// The mount request (spec/cerbios/07 §2.1): {count, type, -, -, path[8] (counted strings at 4 + 8 i), link at 0x44}.
NTSTATUS MountRequest(ULONG Slot, const UCHAR *In, ULONG InLength) {
    if (!In || InLength < 0x4C || !In[0] || In[0] > 2)
        return STATUS_INVALID_DEVICE_REQUEST;
    UCHAR type = In[1];
    if (type == 0)
        type = 'D'; // "replace \Device\CdRom0": every DVD image does here
    if (!IsDvdType(type))
        return STATUS_INVALID_DEVICE_REQUEST; // hard-disk and memory-unit images: not supported (yet)
    if (Slot == 0) { // the control device: the first free slot
        for (Slot = 1; Slot <= VD_SLOTS && VdSlot[Slot - 1].Mounted; Slot++) {
        }
        if (Slot > VD_SLOTS)
            return STATUS_INVALID_DEVICE_REQUEST;
    } else if (VdSlot[Slot - 1].Mounted) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    VD_SAVED *saved = &VdSaved[Slot - 1];
    RtlZeroMemory(saved, sizeof(*saved));
    saved->Type = type;
    saved->Parts = In[0];
    for (ULONG i = 0; i < saved->Parts; i++)
        if (!CopyName(In + 4 + 8 * i, saved->Path[i]) || !saved->Path[i][0])
            return STATUS_INVALID_DEVICE_REQUEST;
    if (!CopyName(In + 0x44, saved->Link))
        return STATUS_INVALID_DEVICE_REQUEST;
    saved->InUse = 1;
    NTSTATUS status = Attach(Slot);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
}

// --- The driver (control device and image devices)

NTSTATUS NTAPI VdCreateClose(PDEVICE_OBJECT, PIRP Irp) { return Complete(Irp, STATUS_SUCCESS); }

NTSTATUS NTAPI VdRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    ULONG n = *(PULONG)DeviceObject->DeviceExtension;
    VD_SLOT *s = n ? &VdSlot[n - 1] : nullptr;
    if (!s)
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    InterlockedIncrement(&s->Busy);
    NTSTATUS status = s->Mounted ? ImageRead(s, Irp) : Complete(Irp, STATUS_NO_MEDIA_IN_DEVICE_);
    InterlockedDecrement(&s->Busy);
    return status;
}

NTSTATUS NTAPI VdDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    ULONG n = *(PULONG)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = st->Parameters.DeviceIoControl.IoControlCode;
    if (code == IOCTL_VD_PRESENT || code == IOCTL_VD_PRESENT2)
        return Complete(Irp, STATUS_SUCCESS);
    if (code == IOCTL_VD_MOUNT || code == IOCTL_VD_MOUNT2 || code == IOCTL_VD_UNMOUNT || code == IOCTL_VD_UNMOUNT2) {
        // As Cerbios: a mount first unmounts its target (the control device: every slot).
        KeWaitForSingleObject(&VdMountLock, Executive, KernelMode, FALSE, nullptr);
        if (n)
            Unmount(n);
        else
            UnmountAll();
        NTSTATUS status = STATUS_SUCCESS;
        if (code == IOCTL_VD_MOUNT || code == IOCTL_VD_MOUNT2)
            status = MountRequest(n, (const UCHAR *)st->Parameters.DeviceIoControl.InputBuffer,
                                  st->Parameters.DeviceIoControl.InputBufferLength);
        VdLastMountStatus = status;
        KeSetEvent(&VdMountLock, 0, FALSE);
        return Complete(Irp, status);
    }
    VD_SLOT *s = n ? &VdSlot[n - 1] : nullptr;
    if (!s)
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    InterlockedIncrement(&s->Busy);
    NTSTATUS status = s->Mounted ? ImageControl(s, Irp) : Complete(Irp, STATUS_NO_MEDIA_IN_DEVICE_);
    InterlockedDecrement(&s->Busy);
    return status;
}

} // namespace

DRIVER_OBJECT VdDriverObject = {
    nullptr,
    nullptr,
    nullptr,
    {VdCreateClose, VdCreateClose, VdRead, VdRead, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest, VdDeviceControl,
     IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest}};

// \Device\CdRom0's read and device-control routines (idex/cdrom.cpp): true when a mounted DVD image took the request.
bool VdServeCdRom(PIRP Irp, NTSTATUS *Status) {
    LONG n = VdDvd;
    if (!n)
        return false;
    VD_SLOT *s = &VdSlot[n - 1];
    InterlockedIncrement(&s->Busy);
    if (!s->Mounted) { // unmounted meanwhile: the drive again
        InterlockedDecrement(&s->Busy);
        return false;
    }
    UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    *Status = major == IRP_MJ_DEVICE_CONTROL ? ImageControl(s, Irp) : ImageRead(s, Irp);
    InterlockedDecrement(&s->Busy);
    return true;
}

// The loader's media check (xe/loader.cpp): a mounted image is an Xbox game disc.
bool VdDvdImageMounted() { return VdDvd != 0; }

// Phase 1, after the drive (idex/channel.cpp): the control device and its links.
void VdInitialize() {
    KeInitializeEvent(&VdMountLock, SynchronizationEvent, TRUE);
    for (auto &s : VdSlot)
        KeInitializeEvent(&s.Lock, SynchronizationEvent, TRUE);
    OBJECT_STRING dir, name, link;
    RtlInitAnsiString(&dir, "\\Device\\Virtual0");
    OBJECT_ATTRIBUTES oa = {nullptr, &dir, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT};
    HANDLE h;
    if (NT_SUCCESS(NtCreateDirectoryObject(&h, &oa)))
        NtClose(h);
    RtlInitAnsiString(&name, "\\Device\\Virtual0\\Image0");
    // Not a mountable type: opening \Device\CdRom1 reaches this driver directly.
    if (!NT_SUCCESS(IoCreateDevice(&VdDriverObject, sizeof(ULONG), &name, FILE_DEVICE_UNKNOWN, FALSE, &VdControl)))
        return;
    *(PULONG)VdControl->DeviceExtension = 0;
    VdControl->Flags &= ~DO_DEVICE_INITIALIZING;
    RtlInitAnsiString(&link, "\\Device\\CdRom1");
    IoCreateSymbolicLink(&link, &name);
    RtlInitAnsiString(&link, "\\??\\CdRom1:");
    IoCreateSymbolicLink(&link, &name);
}

// The title-launch thread, before the first title (xe/loader.cpp): mounts from before a quick reboot come back.
void VdRestoreMounts() {
    KeWaitForSingleObject(&VdMountLock, Executive, KernelMode, FALSE, nullptr);
    for (ULONG n = 1; n <= VD_SLOTS; n++)
        if (VdSaved[n - 1].InUse && !VdSlot[n - 1].Mounted)
            Attach(n);
    KeSetEvent(&VdMountLock, 0, FALSE);
}
