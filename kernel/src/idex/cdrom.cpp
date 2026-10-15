// IDE DVD drive (ATAPI, device 1): \Device\CdRom0, READ via bus-master DMA, packet IOCTLs (spec/04 §3).
#include <xk/arch.h>
#include <xk/idex.h>
#include <xk/rtl.h>

UCHAR IdexGetCompletionStatus(PUCHAR BusMasterStatus);

PDEVICE_OBJECT IdexCdRomDeviceObject;
static LONGLONG IdexCdRomLength;     // media size in bytes (READ CAPACITY), 0 = unknown
volatile UCHAR IdexCdRomLastSense[3]; // key, ASC, ASCQ of the last failed command

constexpr ULONG IOCTL_CDROM_READ_TOC = 0x24000, IOCTL_CDROM_GET_DRIVE_GEOMETRY = 0x2404C,
                IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014, IOCTL_DVD_READ_STRUCTURE = 0x335140;
constexpr NTSTATUS STATUS_NO_MEDIA_IN_DEVICE = (NTSTATUS)0xC0000013, STATUS_VERIFY_REQUIRED = (NTSTATUS)0x80000016;

// --- Polled ATAPI packet command (PIO data phase). Called with the channel owned (StartIo) or at init.

static NTSTATUS IdexCdRomMapSense() {
    UCHAR key = IdexCdRomLastSense[0] & 0x0F, asc = IdexCdRomLastSense[1];
    if (key == 2)
        return asc == 0x3A ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_DEVICE_NOT_READY;
    if (key == 6)
        return STATUS_VERIFY_REQUIRED;
    if (key == 5)
        return STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_IO_DEVICE_ERROR;
}

static bool IdexAtapiIssue(const UCHAR *cdb, bool dma, ULONG byteLimit) {
    outb(IDE_DRIVE, 0xB0);
    if (!IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, 0, 2000000))
        return false;
    outb(IDE_FEATURES, dma ? 1 : 0);
    outb(IDE_LBA1, (UCHAR)byteLimit);
    outb(IDE_LBA2, (UCHAR)(byteLimit >> 8));
    outb(IDE_COMMAND, 0xA0); // PACKET
    KeStallExecutionProcessor(1);
    if (!IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 2000000))
        return false;
    for (int i = 0; i < 6; i++)
        outw(IDE_DATA, (USHORT)(cdb[2 * i] | (cdb[2 * i + 1] << 8)));
    return true;
}

static NTSTATUS IdexAtapiPio(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred);

static void IdexAtapiRequestSense() {
    UCHAR cdb[12] = {0x03, 0, 0, 0, 18, 0};
    UCHAR sense[18] = {};
    ULONG got;
    IdexCdRomLastSense[0] = 0;
    IdexCdRomLastSense[1] = 0;
    IdexCdRomLastSense[2] = 0;
    if (IdexAtapiPio(cdb, sense, sizeof(sense), false, &got) == STATUS_SUCCESS && got >= 14) {
        IdexCdRomLastSense[0] = sense[2];
        IdexCdRomLastSense[1] = sense[12];
        IdexCdRomLastSense[2] = sense[13];
    }
}

static NTSTATUS IdexAtapiPio(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred) {
    *transferred = 0;
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    NTSTATUS status = STATUS_SUCCESS;
    if (!IdexAtapiIssue(cdb, false, length > 0xFFFE ? 0xFFFE : (length ? length : 2))) {
        status = STATUS_IO_DEVICE_ERROR;
    } else {
        for (;;) {
            if (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 10000000)) {
                status = STATUS_IO_TIMEOUT;
                break;
            }
            UCHAR st = inb(IDE_ALT_STATUS);
            if (!(st & IDE_STATUS_DRQ)) {
                if (st & IDE_STATUS_ERR)
                    status = STATUS_IO_DEVICE_ERROR;
                break;
            }
            ULONG count = inb(IDE_LBA1) | (inb(IDE_LBA2) << 8);
            for (ULONG i = 0; i < count; i += 2) {
                USHORT w = 0;
                if (toDevice) {
                    if (*transferred + 1 < length)
                        w = (USHORT)(data[*transferred] | (data[*transferred + 1] << 8));
                    outw(IDE_DATA, w);
                } else {
                    w = inw(IDE_DATA);
                    if (*transferred < length)
                        data[*transferred] = (UCHAR)w;
                    if (*transferred + 1 < length)
                        data[*transferred + 1] = (UCHAR)(w >> 8);
                }
                if (*transferred + 2 <= length)
                    *transferred += 2;
            }
        }
        inb(IDE_STATUS);
    }
    KfLowerIrql(old);
    if (status == STATUS_IO_DEVICE_ERROR && cdb[0] != 0x03) {
        IdexAtapiRequestSense();
        status = IdexCdRomMapSense();
    }
    return status;
}

static NTSTATUS IdexCdRomReadCapacity() {
    UCHAR cdb[12] = {0x25};
    UCHAR cap[8];
    ULONG got;
    NTSTATUS status = IdexAtapiPio(cdb, cap, sizeof(cap), false, &got);
    if (NT_SUCCESS(status)) {
        ULONG last = (cap[0] << 24) | (cap[1] << 16) | (cap[2] << 8) | cap[3];
        IdexCdRomLength = ((LONGLONG)last + 1) << 11;
    } else {
        IdexCdRomLength = 0;
    }
    return status;
}

// --- Dispatch

static NTSTATUS IdexCdRomComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static NTSTATUS NTAPI IdexCdRomCreateClose(PDEVICE_OBJECT, PIRP Irp) { return IdexCdRomComplete(Irp, 0, 0); }

static NTSTATUS NTAPI IdexCdRomRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG len = s->Parameters.Read.Length;
    LONGLONG offset = s->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR buffer = (PUCHAR)Irp->UserBuffer + s->Parameters.Read.BufferOffset;
    // The media size is learnt by READ CAPACITY when the request is started.
    if (((ULONG)buffer & 1) || (len & 0x7FF) || (offset & 0x7FF) || offset < 0 || Irp->SegmentArray)
        return IdexCdRomComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    if (!len)
        return IdexCdRomComplete(Irp, STATUS_SUCCESS, 0);
    Irp->IoStatus.Information = len;
    s->Parameters.Read.ByteOffset.QuadPart = offset >> 11;
    s->Parameters.Read.CacheBuffer = buffer;
    IoMarkIrpPending(Irp);
    IdexChannelStartPacket(DeviceObject, Irp);
    return STATUS_PENDING;
}

static NTSTATUS NTAPI IdexCdRomDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    switch (IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_READ_TOC:
    case IOCTL_SCSI_PASS_THROUGH_DIRECT:
    case IOCTL_DVD_READ_STRUCTURE:
        IoMarkIrpPending(Irp);
        IdexChannelStartPacket(DeviceObject, Irp);
        return STATUS_PENDING;
    default:
        return IdexCdRomComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static void IdexCdRomFinish(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IofCompleteRequest(Irp, 1);
    IdexChannelStartNextPacket();
}

// SCSI_PASS_THROUGH_DIRECT (Xbox layout): {Length, ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength,
// DataIn, DataTransferLength, TimeOutValue, DataBuffer, SenseInfoOffset, Cdb[16]}.
struct SCSI_PASS_THROUGH_DIRECT_ {
    USHORT Length;
    UCHAR ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength, DataIn;
    UCHAR Pad[3];
    ULONG DataTransferLength;
    ULONG TimeOutValue;
    PVOID DataBuffer;
    ULONG SenseInfoOffset;
    UCHAR Cdb[16];
};

static void IdexCdRomControl(PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR cdb[12] = {};
    ULONG got = 0;
    NTSTATUS status;
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_CDROM_CHECK_VERIFY: // TEST UNIT READY
        status = IdexAtapiPio(cdb, nullptr, 0, false, &got);
        if (NT_SUCCESS(status))
            IdexCdRomReadCapacity();
        IdexCdRomFinish(Irp, status, 0);
        return;
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
        if (outLen < sizeof(DISK_GEOMETRY)) {
            IdexCdRomFinish(Irp, STATUS_BUFFER_TOO_SMALL_IO, 0);
            return;
        }
        status = IdexCdRomReadCapacity();
        if (NT_SUCCESS(status)) {
            auto g = (PDISK_GEOMETRY)Irp->UserBuffer;
            g->Cylinders.QuadPart = IdexCdRomLength >> 11;
            g->MediaType = 11; // RemovableMedia
            g->TracksPerCylinder = 1;
            g->SectorsPerTrack = 1;
            g->BytesPerSector = 0x800;
        }
        IdexCdRomFinish(Irp, status, sizeof(DISK_GEOMETRY));
        return;
    case IOCTL_CDROM_READ_TOC:
        cdb[0] = 0x43;
        cdb[7] = (UCHAR)(outLen >> 8);
        cdb[8] = (UCHAR)outLen;
        status = IdexAtapiPio(cdb, (PUCHAR)Irp->UserBuffer, outLen, false, &got);
        IdexCdRomFinish(Irp, status, got);
        return;
    case IOCTL_DVD_READ_STRUCTURE: {
        // READ DVD STRUCTURE, format 0 (physical/layer descriptor): raw response into the output buffer.
        cdb[0] = 0xAD;
        cdb[8] = (UCHAR)(outLen >> 8);
        cdb[9] = (UCHAR)outLen;
        status = IdexAtapiPio(cdb, (PUCHAR)Irp->UserBuffer, outLen, false, &got);
        if (status == STATUS_INVALID_DEVICE_REQUEST)
            status = (NTSTATUS)0xC0000014; // STATUS_UNRECOGNIZED_MEDIA: not a DVD
        IdexCdRomFinish(Irp, status, got);
        return;
    }
    case IOCTL_SCSI_PASS_THROUGH_DIRECT: {
        auto p = (SCSI_PASS_THROUGH_DIRECT_ *)s->Parameters.DeviceIoControl.InputBuffer;
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(*p) || p->CdbLength > 12) {
            IdexCdRomFinish(Irp, STATUS_INVALID_PARAMETER, 0);
            return;
        }
        RtlCopyMemory(cdb, p->Cdb, p->CdbLength);
        status = IdexAtapiPio(cdb, (PUCHAR)p->DataBuffer, p->DataTransferLength, p->DataIn == 0 /*SCSI_IOCTL_DATA_OUT*/,
                              &got);
        p->ScsiStatus = NT_SUCCESS(status) ? 0 : 2; // CHECK CONDITION
        p->DataTransferLength = got;
        IdexCdRomFinish(Irp, status, got);
        return;
    }
    }
    IdexCdRomFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

// READ(10) of up to 128 KB through the bus master.
static void IdexCdRomStartTransfer(PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG chunk = s->Parameters.Read.Length < 0x20000 ? s->Parameters.Read.Length : 0x20000;
    ULONG lba = s->Parameters.Read.ByteOffset.LowPart, blocks = chunk >> 11;
    UCHAR cdb[12] = {0x28, 0, (UCHAR)(lba >> 24), (UCHAR)(lba >> 16), (UCHAR)(lba >> 8), (UCHAR)lba, 0,
                     (UCHAR)(blocks >> 8), (UCHAR)blocks};
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    if (!IdexProgramDma(s->Parameters.Read.CacheBuffer, chunk) || !IdexAtapiIssue(cdb, true, 0)) {
        KfLowerIrql(old);
        IdexCdRomFinish(Irp, STATUS_IO_DEVICE_ERROR, 0);
        return;
    }
    ch->ExpectingBusMasterInterrupt = TRUE;
    ch->Timeout = 10;
    outb(IDE_BM_COMMAND, 0x09);
    KfLowerIrql(old);
}

static void NTAPI IdexCdRomFinishIo() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    PIRP irp = ch->CurrentIrp;
    if (!irp)
        return;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    UCHAR bm, ata = IdexGetCompletionStatus(&bm);
    if ((ata & IDE_STATUS_ERR) || (bm & 2)) {
        IdexAtapiRequestSense();
        NTSTATUS status = IdexCdRomMapSense();
        if (status == STATUS_IO_DEVICE_ERROR && ch->IoRetries++ < ch->MaximumIoRetries) {
            IdexCdRomStartTransfer(irp);
            return;
        }
        IdexCdRomFinish(irp, status, 0);
        return;
    }
    ULONG chunk = s->Parameters.Read.Length < 0x20000 ? s->Parameters.Read.Length : 0x20000;
    s->Parameters.Read.Length -= chunk;
    if (s->Parameters.Read.Length) {
        s->Parameters.Read.CacheBuffer = (PUCHAR)s->Parameters.Read.CacheBuffer + chunk;
        s->Parameters.Read.ByteOffset.LowPart += chunk >> 11;
        ch->IoRetries = 0;
        IdexCdRomStartTransfer(irp);
        return;
    }
    IdexCdRomFinish(irp, STATUS_SUCCESS, irp->IoStatus.Information);
}

static void NTAPI IdexCdRomStartIo(PDEVICE_OBJECT, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s->MajorFunction != IRP_MJ_READ) {
        IdexCdRomControl(Irp);
        return;
    }
    // Bounds against the media size (learnt here, so a disc change is noticed).
    if (!IdexCdRomLength)
        IdexCdRomReadCapacity();
    LONGLONG end = ((LONGLONG)s->Parameters.Read.ByteOffset.LowPart << 11) + s->Parameters.Read.Length;
    if (!IdexCdRomLength) {
        IdexCdRomFinish(Irp, IdexCdRomMapSense(), 0);
        return;
    }
    if (end > IdexCdRomLength) {
        IdexCdRomFinish(Irp, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    IdexChannelObject.FinishIoRoutine = IdexCdRomFinishIo;
    IdexChannelObject.TimeoutExpiredRoutine = IdexCdRomFinishIo;
    IdexCdRomStartTransfer(Irp);
}

DRIVER_OBJECT IdexCdRomDriverObject = {
    IdexCdRomStartIo,
    nullptr,
    nullptr,
    {IdexCdRomCreateClose, IdexCdRomCreateClose, IdexCdRomRead, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, IdexCdRomDeviceControl, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest}};

void IdexCdRomInit() {
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    outb(IDE_DRIVE, 0xB0);
    ULONG tries = 0;
    while (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 50000))
        if (++tries == 600)
            IdexFatalError(10);
    // SET FEATURES transfer mode PIO 4 then UDMA 2 (polled).
    static const UCHAR modes[] = {0x0C, 0x42};
    for (UCHAR mode : modes) {
        outb(IDE_FEATURES, 0x03);
        outb(IDE_COUNT, mode);
        outb(IDE_COMMAND, 0xEF);
        KeStallExecutionProcessor(1);
        if (!IdexWaitForStatus(IDE_STATUS_BSY, 0, 5000000) || (inb(IDE_ALT_STATUS) & IDE_STATUS_ERR))
            IdexFatalError(12);
    }
    outb(IDE_COMMAND, 0xA1); // IDENTIFY PACKET DEVICE
    KeStallExecutionProcessor(1);
    if (!IdexWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 5000000))
        IdexFatalError(11);
    for (int i = 0; i < 256; i++)
        inw(IDE_DATA);
    inb(IDE_STATUS);
    KfLowerIrql(old);

    OBJECT_STRING name, link;
    RtlInitAnsiString(&name, "\\Device\\CdRom0");
    RtlInitAnsiString(&link, "\\??\\CdRom0:");
    PDEVICE_OBJECT dev;
    if (!NT_SUCCESS(IoCreateDevice(&IdexCdRomDriverObject, 0, &name, FILE_DEVICE_CD_ROM, FALSE, &dev)) ||
        !NT_SUCCESS(IoCreateSymbolicLink(&link, &name)))
        IdexFatalError(0x10E5F);
    dev->Flags |= 0x44;
    dev->AlignmentRequirement = 1;
    dev->SectorSize = 0x800;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    IdexCdRomDeviceObject = dev;
}

// Fatal boot errors (spec/05 §6): the reference reports the code through the SMC and halts.
volatile ULONG IdexFatalErrorCode;
[[noreturn]] void IdexFatalError(ULONG Code) {
    IdexFatalErrorCode = Code;
    KeBugCheckEx(0x00000F00 /* XK fatal error */, Code, 0, 0, 0);
}
