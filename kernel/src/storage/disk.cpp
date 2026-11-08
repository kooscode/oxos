// IDE hard disk: identification, partition devices, DMA transfers, IOCTLs (spec/stock/04 §2, §4, §5), 48-bit
// addressing for disks over 128 GiB (spec/oxos_internal/04_lba48.md).
#include <xk/storage.h>
#include <xk/serial.h>
#include <xk/arch.h>
#include <xk/hardware.h>
#include <xk/runtime.h>
#include <xk/crypto.h>
#include <xk/titles.h>
#include <xk/disk.h>

ULONGLONG IdexDiskLengthInSectors;
BOOLEAN IdexDiskLba48, IdexDiskSecurityUnlocked;
ULONG HalDiskCachePartitionCount = 3; // X, Y, Z
static char IdexDiskModel[40], IdexDiskSerial[20];
STRING HalDiskModelNumber = {0, sizeof(IdexDiskModel), IdexDiskModel};
STRING HalDiskSerialNumber = {0, sizeof(IdexDiskSerial), IdexDiskSerial};
volatile USHORT IdexDiskSecurityWord; // IDENTIFY word 128, for diagnostics

static PDEVICE_OBJECT IdexDiskPartitionTable[IDE_MAX_PARTITIONS];

struct IDE_DISK_DEVICE {
    IDE_DISK_EXTENSION Extension;
    UCHAR Kind;
};

// On an exFAT disk (disk_layout.cpp), Partition1-6 are folders of the exFAT volume (the file system mounts them through
// IdexDiskFolderOf), and Partition0's retail reserved area (bytes 0-0x7FFFF: refurb record, cache database) lives at
// sectors 1024-2047, before the volume.
constexpr LONGLONG IDE_RESERVED_LENGTH = 0x80000, IDE_RESERVED_AT = 1024 * 512;
static const char *const IdexDiskFolders[] = {nullptr, "E", "C", "X", "Y", "Z", "F"}; // folder of Partition1-6

constexpr ULONG IDE_MAX_TRANSFER = 0x20000;

static UCHAR IdexKind(PDEVICE_OBJECT dev) {
    return ((IDE_DISK_DEVICE *)dev->DeviceExtension)->Kind;
}

static NTSTATUS NTAPI IdexDiskCreateClose(PDEVICE_OBJECT, PIRP Irp) {
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IofCompleteRequest(Irp, 0);
    return STATUS_SUCCESS;
}

static PIDE_DISK_EXTENSION IdexExtension(PDEVICE_OBJECT dev) {
    return (PIDE_DISK_EXTENSION)dev->DeviceExtension;
}

// READ/WRITE: sector-aligned, inside the partition. The stack location is rewritten for the transfer:
// Read.ByteOffset.QuadPart = absolute LBA, Read.CacheBuffer = current buffer address.
static NTSTATUS NTAPI IdexDiskReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    PPARTITION_INFORMATION part = &IdexExtension(DeviceObject)->PartitionInformation;
    ULONG len = s->Parameters.Read.Length;
    LONGLONG offset = s->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR buffer = (PUCHAR)Irp->UserBuffer + s->Parameters.Read.BufferOffset;
    UCHAR kind = IdexKind(DeviceObject);
    bool remap = IdexDiskEXFATLayout && kind == IDE_KIND_PARTITION; // Partition0 on an exFAT disk: reserved area only
    // Writes refused on a foreign disk, and after a format or a partition delete until the next boot (the old
    // partitions' file systems would write into the new layout); the whole-disk device writes the new layout.
    bool foreignWrite = (IdexDiskForeignLayout || IdexDiskReformatted) && s->MajorFunction == IRP_MJ_WRITE &&
                        kind != IDE_KIND_PHYSICAL;
    if (kind == IDE_KIND_FOLDER || (remap && offset + len > IDE_RESERVED_LENGTH) || foreignWrite) {
        Irp->IoStatus.Status = kind == IDE_KIND_FOLDER ? STATUS_INVALID_DEVICE_REQUEST
                               : foreignWrite          ? (NTSTATUS)0xC00000A2 // STATUS_MEDIA_WRITE_PROTECTED
                                                       : STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        NTSTATUS status = Irp->IoStatus.Status;
        IofCompleteRequest(Irp, 0);
        return status;
    }
    if (remap)
        offset += IDE_RESERVED_AT;
    if (((ULONG)buffer & 1) || (len & 0x1FF) || (offset & 0x1FF) || offset < 0 ||
        offset + len > (remap ? (LONGLONG)IdexDiskLengthInSectors << 9 : part->PartitionLength.QuadPart) ||
        Irp->SegmentArray) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IofCompleteRequest(Irp, 0);
        return STATUS_INVALID_PARAMETER;
    }
    Irp->IoStatus.Information = len;
    if (!len) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IofCompleteRequest(Irp, 0);
        return STATUS_SUCCESS;
    }
    s->Parameters.Read.ByteOffset.QuadPart = (part->StartingOffset.QuadPart + offset) >> 9;
    s->Parameters.Read.CacheBuffer = buffer;
    IoMarkIrpPending(Irp);
    IdexChannelStartPacket(DeviceObject, Irp);
    return STATUS_PENDING;
}

static void IdexDiskCompleteIrp(PIRP Irp, NTSTATUS Status) {
    XK_MARK(NT_SUCCESS(Status) ? "mark: disk IRP done" : "mark: disk IRP failed");
    Irp->IoStatus.Status = Status;
    if (!NT_SUCCESS(Status))
        Irp->IoStatus.Information = 0;
    IofCompleteRequest(Irp, 1 /*IO_DISK_INCREMENT*/);
    IdexChannelStartNextPacket();
}

// Program the next chunk of the current IRP (DISPATCH_LEVEL).
static void IdexDiskStartTransfer(PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG chunk = s->Parameters.Read.Length < IDE_MAX_TRANSFER ? s->Parameters.Read.Length : IDE_MAX_TRANSFER;
    ULONGLONG lba = s->Parameters.Read.ByteOffset.QuadPart;
    ULONG count = chunk >> 9;
    bool read = s->MajorFunction == IRP_MJ_READ;
    // The 28-bit commands address sectors up to 0x0FFFFFFF; a chunk that reaches past it uses the EXT forms.
    bool ext = IdexDiskLba48 && lba + count > 0x0FFFFFFF;
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    if (!IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRDY, IDE_STATUS_DRDY, 1000000) ||
        !IdexProgramDma(s->Parameters.Read.CacheBuffer, chunk)) {
        KfLowerIrql(old);
        IdexDiskCompleteIrp(Irp, STATUS_IO_DEVICE_ERROR);
        return;
    }
    if (ext) { // each register takes the high byte first, then the low byte
        outb(IDE_DRIVE, 0xE0);
        outb(IDE_COUNT, (UCHAR)(count >> 8)); // 256 sectors = 0x0100 (0 would mean 65536)
        outb(IDE_LBA0, (UCHAR)(lba >> 24));
        outb(IDE_LBA1, (UCHAR)(lba >> 32));
        outb(IDE_LBA2, (UCHAR)(lba >> 40));
        outb(IDE_COUNT, (UCHAR)count);
        outb(IDE_LBA0, (UCHAR)lba);
        outb(IDE_LBA1, (UCHAR)(lba >> 8));
        outb(IDE_LBA2, (UCHAR)(lba >> 16));
        outb(IDE_COMMAND, read ? 0x25 : 0x35); // READ DMA EXT / WRITE DMA EXT
    } else {
        outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(IDE_LBA0, (UCHAR)lba);
        outb(IDE_LBA1, (UCHAR)(lba >> 8));
        outb(IDE_LBA2, (UCHAR)(lba >> 16));
        outb(IDE_COUNT, (UCHAR)count); // 0 = 256 sectors
        outb(IDE_COMMAND, read ? 0xC8 : 0xCA); // READ DMA / WRITE DMA
    }
    ch->ExpectingBusMasterInterrupt = TRUE;
    ch->Timeout = 10;
    outb(IDE_BM_COMMAND, read ? 0x09 : 0x01);
    XK_MARK("mark: disk DMA started");
    KfLowerIrql(old);
}

static void NTAPI IdexDiskFinishIo() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    PIRP irp = ch->CurrentIrp;
    if (!irp)
        return;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    UCHAR bm, ata = IdexGetCompletionStatus(&bm);
    if ((ata & (IDE_STATUS_ERR | IDE_STATUS_DF)) || (bm & 2)) {
        if (ch->IoRetries++ < ch->MaximumIoRetries) {
            IdexDiskStartTransfer(irp);
            return;
        }
        IdexDiskCompleteIrp(irp, STATUS_IO_DEVICE_ERROR);
        return;
    }
    ULONG chunk = s->Parameters.Read.Length < IDE_MAX_TRANSFER ? s->Parameters.Read.Length : IDE_MAX_TRANSFER;
    s->Parameters.Read.Length -= chunk;
    if (s->Parameters.Read.Length) {
        s->Parameters.Read.CacheBuffer = (PUCHAR)s->Parameters.Read.CacheBuffer + chunk;
        s->Parameters.Read.ByteOffset.QuadPart += chunk >> 9;
        ch->IoRetries = 0;
        IdexDiskStartTransfer(irp);
        return;
    }
    IdexDiskCompleteIrp(irp, STATUS_SUCCESS);
}

static void NTAPI IdexDiskTimeout() {
    IdexDiskFinishIo();
}

static void IdexDiskPassThrough(PIRP Irp);

static void NTAPI IdexDiskStartIo(PDEVICE_OBJECT, PIRP Irp) {
    if (IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_DEVICE_CONTROL)
        return IdexDiskPassThrough(Irp);
    IdexChannelObject.FinishIoRoutine = IdexDiskFinishIo;
    IdexChannelObject.TimeoutExpiredRoutine = IdexDiskTimeout;
    IdexDiskStartTransfer(Irp);
}

static NTSTATUS IdexCompleteWith(PIRP Irp, NTSTATUS Status, ULONG Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static NTSTATUS NTAPI IdexDiskDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY: {
        if (outLen < sizeof(DISK_GEOMETRY))
            return IdexCompleteWith(Irp, STATUS_BUFFER_TOO_SMALL_IO, 0);
        auto g = (PDISK_GEOMETRY)Irp->UserBuffer;
        g->Cylinders.QuadPart = IdexDiskLengthInSectors;
        g->MediaType = 12; // FixedMedia
        g->TracksPerCylinder = 1;
        g->SectorsPerTrack = 1;
        g->BytesPerSector = 512;
        return IdexCompleteWith(Irp, STATUS_SUCCESS, sizeof(DISK_GEOMETRY));
    }
    case IOCTL_DISK_GET_PARTITION_INFO:
        if (outLen < sizeof(PARTITION_INFORMATION))
            return IdexCompleteWith(Irp, STATUS_BUFFER_TOO_SMALL_IO, 0);
        *(PPARTITION_INFORMATION)Irp->UserBuffer = IdexExtension(DeviceObject)->PartitionInformation;
        return IdexCompleteWith(Irp, STATUS_SUCCESS, sizeof(PARTITION_INFORMATION));
    case IOCTL_DISK_VERIFY: {
        struct VERIFY_INFORMATION {
            LARGE_INTEGER StartingOffset;
            ULONG Length;
        };
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(VERIFY_INFORMATION))
            return IdexCompleteWith(Irp, STATUS_INFO_LENGTH_MISMATCH, 0);
        auto v = (VERIFY_INFORMATION *)s->Parameters.DeviceIoControl.InputBuffer;
        PPARTITION_INFORMATION p = &IdexExtension(DeviceObject)->PartitionInformation;
        bool inside = v->StartingOffset.QuadPart >= 0 &&
                      v->StartingOffset.QuadPart + v->Length <= p->PartitionLength.QuadPart;
        return IdexCompleteWith(Irp, inside ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER, 0);
    }
    case IOCTL_IDE_PASS_THROUGH: {
        // The buffer is used in place: input and output must be the same ATA_PASS_THROUGH (16 bytes or more).
        if (Irp->UserBuffer != s->Parameters.DeviceIoControl.InputBuffer || outLen < sizeof(ATA_PASS_THROUGH) ||
            s->Parameters.DeviceIoControl.InputBufferLength < sizeof(ATA_PASS_THROUGH))
            return IdexCompleteWith(Irp, STATUS_INVALID_PARAMETER, 0);
        IoMarkIrpPending(Irp);
        IdexChannelObject.StartPacketRoutine(DeviceObject, Irp);
        return STATUS_PENDING;
    }
    default:
        return IdexCompleteWith(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

// IDE pass-through (spec/stock/04 §5): the task file is written as given (the drive select is ours), PIO data follows
// in the direction HostSendsData says, and the returned registers are copied back. The command itself may fail
// (status ERR): the IRP still succeeds. Ours polls at the channel IRQL, up to 10 s like the reference timeout.
static NTSTATUS IdexDiskExecutePassThrough(ATA_PASS_THROUGH *p) {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    NTSTATUS status = STATUS_SUCCESS;
    outb(IDE_DRIVE, 0xA0);
    if (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 1000000)) {
        IdexChannelReset();
        status = STATUS_REQUEST_ABORTED;
    } else {
        outb(IDE_FEATURES, p->IdeReg.bFeaturesReg);
        outb(IDE_COUNT, p->IdeReg.bSectorCountReg);
        outb(IDE_LBA0, p->IdeReg.bSectorNumberReg);
        outb(IDE_LBA1, p->IdeReg.bCylLowReg);
        outb(IDE_LBA2, p->IdeReg.bCylHighReg);
        outb(IDE_COMMAND, p->IdeReg.bCommandReg);
        KeStallExecutionProcessor(1);
        auto data = (PUCHAR)p->DataBuffer;
        ULONG size = p->DataBufferSize;
        if (size && p->IdeReg.bHostSendsData) {
            if (IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 10000000)) {
                for (ULONG i = 0; i + 1 < size; i += 2)
                    outw(IDE_DATA, (USHORT)(data[i] | data[i + 1] << 8));
                if (size & 1)
                    outb(IDE_DATA, data[size - 1]);
            }
        } else if (size && IdexWaitForStatus(IDE_STATUS_BSY, 0, 10000000) && (inb(IDE_ALT_STATUS) & IDE_STATUS_DRQ)) {
            for (ULONG i = 0; i + 1 < size; i += 2) {
                USHORT w = inw(IDE_DATA);
                data[i] = (UCHAR)w, data[i + 1] = (UCHAR)(w >> 8);
            }
            if (size & 1)
                data[size - 1] = inb(IDE_DATA);
        }
        IdexWaitForStatus(IDE_STATUS_BSY, 0, 10000000);
        p->IdeReg.bCommandReg = inb(IDE_STATUS); // the returned status
        p->IdeReg.bFeaturesReg = inb(IDE_ERROR);
        p->IdeReg.bSectorCountReg = inb(IDE_COUNT);
        p->IdeReg.bSectorNumberReg = inb(IDE_LBA0);
        p->IdeReg.bCylLowReg = inb(IDE_LBA1);
        p->IdeReg.bCylHighReg = inb(IDE_LBA2);
    }
    KfLowerIrql(old);
    return status;
}

static void IdexDiskPassThrough(PIRP Irp) {
    Irp->IoStatus.Information = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength;
    Irp->IoStatus.Status = IdexDiskExecutePassThrough((ATA_PASS_THROUGH *)Irp->UserBuffer);
    IofCompleteRequest(Irp, 1);
    IdexChannelStartNextPacket();
}

static NTSTATUS NTAPI IdexDiskFlush(PDEVICE_OBJECT, PIRP Irp) {
    // The drive's write cache is flushed with FLUSH CACHE; the command is short, so it is polled.
    KIRQL old = KeRaiseIrqlToDpcLevel();
    NTSTATUS status = STATUS_SUCCESS;
    if (!IdexChannelObject.CurrentIrp) {
        KIRQL o2 = KfRaiseIrql(IdexChannelObject.InterruptIrql);
        outb(IDE_DRIVE, 0xE0);
        outb(IDE_COMMAND, 0xE7);
        if (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 2000000) || (inb(IDE_ALT_STATUS) & IDE_STATUS_ERR))
            status = STATUS_IO_DEVICE_ERROR;
        KfLowerIrql(o2);
    }
    KfLowerIrql(old);
    return IdexCompleteWith(Irp, status, 0);
}

DRIVER_OBJECT IdexDiskDriverObject = {
    IdexDiskStartIo,
    nullptr,
    nullptr,
    {IdexDiskCreateClose, IdexDiskCreateClose, IdexDiskReadWrite, IdexDiskReadWrite, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, IdexDiskFlush, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IdexDiskDeviceControl, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest}};

// --- Partition devices

NTSTATUS IdexDiskCreateDevice(ULONG Number, LONGLONG Start, LONGLONG Length, PDEVICE_OBJECT *Out, UCHAR Kind) {
    PDEVICE_OBJECT dev;
    NTSTATUS status = IoCreateDevice(&IdexDiskDriverObject, sizeof(IDE_DISK_DEVICE), nullptr, FILE_DEVICE_DISK,
                                     FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->Flags |= 0x44;
    dev->AlignmentRequirement = 1;
    dev->SectorSize = 512;
    PIDE_DISK_EXTENSION ext = IdexExtension(dev);
    ext->DeviceObject = dev;
    ext->PartitionInformation.StartingOffset.QuadPart = Start;
    ext->PartitionInformation.PartitionLength.QuadPart = Length;
    ext->PartitionInformation.HiddenSectors = (ULONG)(Start >> 9);
    ext->PartitionInformation.PartitionNumber = Number;
    ((IDE_DISK_DEVICE *)ext)->Kind = Kind;
    if (Number == 0)
        dev->Flags |= DO_RAW_MOUNT_ONLY; // the whole disk never carries a file system
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    *Out = dev;
    return STATUS_SUCCESS;
}

// The folder partitions of an exFAT disk report the retail places and sizes (F: after E:, the whole volume), so
// partition queries look as on a retail disk (spec §6 of the storage spec).
static void IdexDiskFolderGeometry(ULONG Number, LONGLONG *Start, LONGLONG *Length) {
    static const ULONG starts[] = {0, 0x55F400, 0x465400, 0x400, 0x177400, 0x2EE400, 0xEE8AB0};
    *Start = (LONGLONG)starts[Number] << 9;
    switch (Number) {
    case 1: *Length = 0x1312D6000LL; break;
    case 2: *Length = 0x1F400000; break;
    case 3: case 4: case 5: *Length = 0x2EE00000; break;
    default: *Length = IdexExtension(IdexDiskEXFATVolume)->PartitionInformation.PartitionLength.QuadPart;
    }
}

// Partition N of an exFAT disk is a folder of the volume: its name, or nullptr for any other device.
PCSZ IdexDiskFolderOf(PDEVICE_OBJECT Device) {
    if (Device->DriverObject != &IdexDiskDriverObject || IdexKind(Device) != IDE_KIND_FOLDER)
        return nullptr;
    return IdexDiskFolders[IdexExtension(Device)->PartitionInformation.PartitionNumber];
}

static NTSTATUS IdexDiskCreatePartitionDevice(ULONG Number) {
    PDEVICE_OBJECT dev;
    if (IdexDiskEXFATLayout) {
        if (Number < 1 || Number > 6)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        LONGLONG start, length;
        IdexDiskFolderGeometry(Number, &start, &length);
        NTSTATUS status = IdexDiskCreateDevice(Number, start, length, &dev, IDE_KIND_FOLDER);
        if (!NT_SUCCESS(status))
            return status;
        PDEVICE_OBJECT expected = nullptr;
        if (!__atomic_compare_exchange_n(&IdexDiskPartitionTable[Number], &expected, dev, false, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST))
            IoDeleteDevice(dev);
        return STATUS_SUCCESS;
    }
    if (Number < 1 || Number > 14 || !IdexDiskPartitions[Number].Length)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    const IDE_PARTITION &p = IdexDiskPartitions[Number];
    NTSTATUS status = IdexDiskCreateDevice(Number, (LONGLONG)(p.Start << 9), (LONGLONG)(p.Length << 9), &dev);
    if (!NT_SUCCESS(status))
        return status;
    PDEVICE_OBJECT expected = nullptr;
    if (__atomic_compare_exchange_n(&IdexDiskPartitionTable[Number], &expected, dev, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        return STATUS_SUCCESS;
    IoDeleteDevice(dev); // somebody else created it first
    return STATUS_SUCCESS;
}

// \Device\Harddisk0 parses "PartitionN[\rest]" and hands the rest to the partition device.
static NTSTATUS NTAPI IdexDiskParseDirectory(PVOID ParseObject, POBJECT_TYPE ObjectType, ULONG Attributes,
                                             POBJECT_STRING CompleteName, POBJECT_STRING RemainingName,
                                             PVOID Context, PVOID *Object) {
    *Object = nullptr;
    if (!RemainingName->Length)
        return STATUS_ACCESS_DENIED;
    const char *p = RemainingName->Buffer, *end = p + RemainingName->Length;
    if (*p != '\\')
        return STATUS_OBJECT_NAME_INVALID;
    const char *element = ++p;
    while (p < end && *p != '\\')
        p++;
    OBJECT_STRING rest = {(USHORT)(end - p), (USHORT)(end - p), (PCHAR)p};
    static const char prefix[] = "partition";
    ULONG len = (ULONG)(p - element), number = 0;
    bool ok = len > 9;
    for (ULONG i = 0; ok && i < 9; i++)
        ok = (element[i] | 0x20) == prefix[i];
    for (ULONG i = 9; ok && i < len; i++) {
        ok = element[i] >= '0' && element[i] <= '9';
        number = number * 10 + (element[i] - '0');
    }
    if (!ok || number >= IDE_MAX_PARTITIONS)
        return rest.Length ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
    IdexDiskDetectLayout();
    while (!IdexDiskPartitionTable[number]) {
        NTSTATUS status = IdexDiskCreatePartitionDevice(number);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND)
            return rest.Length ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(status))
            return status;
    }
    return IopParseDevice(IdexDiskPartitionTable[number], ObjectType, Attributes, CompleteName, &rest, Context,
                          Object);
}

static OBJECT_TYPE IdexDiskDirectoryObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr,
                                                  IdexDiskParseDirectory, nullptr, 'xedI'};

// --- Initialisation (PIO commands, polled)

volatile UCHAR IdexDiskLastStatus; // diagnostics: the disk's status register when its start-up wait ended

static bool IdexDiskCommand(UCHAR command, UCHAR features, UCHAR count) {
    outb(IDE_DRIVE, 0xE0);
    if (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 1000000))
        return false;
    outb(IDE_FEATURES, features);
    outb(IDE_COUNT, count);
    outb(IDE_COMMAND, command);
    KeStallExecutionProcessor(1);
    return IdexWaitForStatus(IDE_STATUS_BSY, 0, 5000000) && !(inb(IDE_ALT_STATUS) & IDE_STATUS_ERR);
}

static void IdexCopyIdentifyString(const USHORT *words, ULONG count, char *out, STRING *s) {
    for (ULONG i = 0; i < count; i++) {
        out[2 * i] = (char)(words[i] >> 8);
        out[2 * i + 1] = (char)words[i];
    }
    ULONG n = count * 2;
    while (n && out[n - 1] == ' ')
        n--;
    s->Length = (USHORT)n;
}

// The drive password: HMAC-SHA1(XboxHDKey, model, serial) over the trimmed IDENTIFY strings, zero-padded to 32.
void IdexDiskComputePassword(PUCHAR Password) {
    XcHMAC(XboxHDKey, 16, (PUCHAR)HalDiskModelNumber.Buffer, HalDiskModelNumber.Length,
           (PUCHAR)HalDiskSerialNumber.Buffer, HalDiskSerialNumber.Length, Password);
}

static NTSTATUS IdexDiskExecutePassThrough(ATA_PASS_THROUGH *p);

// SECURITY UNLOCK (0xF2, user password) with the pass-through code (the reference sends the IOCTL to Partition0;
// ours runs on the boot thread, which cannot own IRPs), then IDENTIFY again and
// INITIALIZE DEVICE PARAMETERS (16 heads, 63 sectors). Failures: fatal error 6 (unlock, except on manufacturing
// units) or 8 (the follow-up commands).
static void IdexDiskUnlock(const UCHAR *Password) {
    UCHAR block[512]; // on the boot stack: a static buffer would cost a resident image page (spec/stock/09 §7)
    RtlZeroMemory(block, sizeof(block));
    RtlCopyMemory(block + 2, Password, 32);
    ATA_PASS_THROUGH p = {};
    p.IdeReg.bCommandReg = 0xF2;
    p.IdeReg.bHostSendsData = TRUE;
    p.DataBufferSize = sizeof(block);
    p.DataBuffer = block;
    NTSTATUS status = IdexDiskExecutePassThrough(&p);
    if (NT_SUCCESS(status) && !(p.IdeReg.bCommandReg & IDE_STATUS_ERR))
        IdexDiskSecurityUnlocked = TRUE;
    else if (!(XeConsoleGameRegion & 0x80000000))
        IdexFatalError(6);
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    bool ok = IdexDiskCommand(0xEC, 0, 0) || (inb(IDE_ALT_STATUS) & IDE_STATUS_DRQ);
    if (ok) {
        for (ULONG i = 0; i < 256; i++)
            inw(IDE_DATA);
        outb(IDE_DRIVE, 0xAF);
        ok = IdexWaitForStatus(IDE_STATUS_BSY, 0, 1000000);
        if (ok) {
            outb(IDE_COUNT, 0x3F);
            outb(IDE_COMMAND, 0x91);
            KeStallExecutionProcessor(1);
            ok = IdexWaitForStatus(IDE_STATUS_BSY, 0, 5000000) && !(inb(IDE_ALT_STATUS) & IDE_STATUS_ERR);
        }
    }
    KfLowerIrql(old);
    if (!ok)
        IdexFatalError(8);
}

// End of disk init (cold boot, retail units): count the power cycle in the refurb record (on the disk, so it runs
// on a system thread: the title launcher calls it).
void IdexDiskCountPowerCycle() {
    if (XeConsoleGameRegion & 0x80000000)
        return;
    // Only on a disk known to be ours: exFAT (the record goes to the reserved-area copy), or FATX with a FATX volume
    // on C: or E:. A disk taken for FATX by mistake (its layout could not be read) keeps sector 3 untouched.
    IdexDiskDetectLayout();
    if (IdexDiskForeignLayout)
        return;
    if (!IdexDiskEXFATLayout) {
        auto sector = (PUCHAR)ExAllocatePoolWithTag(512, 'cpLI');
        bool foundFATX = false;
        for (ULONG n = 1; sector && n <= 2 && !foundFATX; n++) {
            foundFATX = IdexDiskPartitions[n].Length &&
                        NT_SUCCESS(IdexDiskReadPhysical(IdexDiskPartitions[n].Start, sector, 512)) &&
                        RtlCompareMemory(sector, "FATX", 4) == 4;
        }
        if (sector)
            ExFreePool(sector);
        if (!foundFATX) {
            XK_SERIAL("disk: no FATX volume on C: or E:; power cycle not counted\n");
            return;
        }
    }
    ULONG info[4];
    if (NT_SUCCESS(ExReadWriteRefurbInfo(info, sizeof(info), FALSE))) {
        info[1]++;
        ExReadWriteRefurbInfo(info, sizeof(info), TRUE);
    }
}

void IdexDiskInit() {
    outb(IDE_DRIVE, 0xA0);
    // Wait for the disk: not busy and ready, as the reference does, for up to 30 s. OXOS deviations:
    // - a disk that is no longer busy but has not set DRDY after 1 s is used anyway (IDENTIFY below decides), as
    //   XboxDev/cromwell does; some drives and SATA bridges only set DRDY once they have seen a command. Only a disk
    //   still busy after 30 s is fatal error 7;
    // - the wait runs at the caller's IRQL (the reference raises first), so the boot splash and jingle keep running
    //   while a hard disk spins up; it only reads the alternate status, and the interrupt handler only acknowledges
    //   an interrupt nobody expects. Its limits follow the clock, as other threads take the processor meanwhile.
    const ULONGLONG start = KeQueryInterruptTime();
    for (;;) {
        UCHAR st = inb(IDE_ALT_STATUS);
        if ((st & (IDE_STATUS_BSY | IDE_STATUS_DRDY)) == IDE_STATUS_DRDY)
            break;
        ULONG ms = (ULONG)((KeQueryInterruptTime() - start) / 10000);
        if (!(st & IDE_STATUS_BSY) && ms >= 1000)
            break;
        if (ms >= 30000) {
            IdexDiskLastStatus = st;
            XK_SERIAL("disk: not ready after 30 s, status %#x\n", st);
            IdexFatalError(7);
        }
        KeStallExecutionProcessor(10);
    }
    XK_SERIAL("disk: ready after %lu ms\n", (ULONG)((KeQueryInterruptTime() - start) / 10000));
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    IdexDiskLastStatus = inb(IDE_ALT_STATUS);
    if (!IdexDiskCommand(0xEF, 0x03, 0x0C) || !IdexDiskCommand(0xEF, 0x03, 0x42)) // PIO 4, UDMA 2
        IdexFatalError((IdexDiskLastStatus = inb(IDE_ALT_STATUS), 9));
    outb(IDE_DRIVE, 0xE0);
    outb(IDE_COMMAND, 0xEC); // IDENTIFY DEVICE
    KeStallExecutionProcessor(1);
    if (!IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 5000000))
        IdexFatalError((IdexDiskLastStatus = inb(IDE_ALT_STATUS), 8));
    USHORT id[256];
    for (auto &w : id)
        w = inw(IDE_DATA);
    inb(IDE_STATUS);
    KfLowerIrql(old);

    IdexCopyIdentifyString(&id[27], 20, IdexDiskModel, &HalDiskModelNumber);
    IdexCopyIdentifyString(&id[10], 10, IdexDiskSerial, &HalDiskSerialNumber);
    // Words 83 and 86 bit 10: the 48-bit feature set, supported and enabled; its size is words 100-103.
    IdexDiskLba48 = (id[83] & 0xC400) == 0x4400 && (id[86] & 0x0400);
    IdexDiskLengthInSectors = id[60] | ((ULONG)id[61] << 16);
    if (IdexDiskLba48) {
        ULONGLONG n = id[100] | ((ULONGLONG)id[101] << 16) | ((ULONGLONG)id[102] << 32) | ((ULONGLONG)id[103] << 48);
        if (n > IdexDiskLengthInSectors)
            IdexDiskLengthInSectors = n;
    }
    if (IdexDiskLengthInSectors < 0xEE8AB0)
        IdexFatalError((IdexDiskLastStatus = inb(IDE_ALT_STATUS), 9));
    IdexDiskSecurityWord = id[128];
    XK_SERIAL("disk: %.40s, %lu sectors%s, security %#x, status %#x\n", IdexDiskModel, (ULONG)IdexDiskLengthInSectors,
              IdexDiskLba48 ? " (LBA48)" : "", IdexDiskSecurityWord, IdexDiskLastStatus);

    OBJECT_STRING name;
    RtlInitAnsiString(&name, "\\Device\\Harddisk0");
    OBJECT_ATTRIBUTES attrs = {nullptr, &name, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE};
    PVOID dir;
    HANDLE h;
    if (NT_SUCCESS(ObCreateObject(&IdexDiskDirectoryObjectType, &attrs, sizeof(ULONG), &dir)) &&
        NT_SUCCESS(ObInsertObject(dir, &attrs, 0, &h)))
        NtClose(h);
    IdexDiskCreateDevice(0, 0, (LONGLONG)IdexDiskLengthInSectors << 9, &IdexDiskPartitionTable[0]);
    IdexDiskLayoutInit();

    // ATA security (IDENTIFY word 128: bit 1 enabled, bit 2 locked).
    if (IdexDiskSecurityWord & 2) {
        if (!(IdexDiskSecurityWord & 4)) {
            IdexDiskSecurityUnlocked = TRUE;
        } else {
            UCHAR password[32] = {};
            IdexDiskComputePassword(password);
            IdexDiskUnlock(password);
        }
    }
}
