// IDE DVD drive (ATAPI, device 1): \Device\CdRom0, READ via bus-master DMA, packet IOCTLs (spec/stock/04 §3).
#include <storage/storage.h>
#include <user_interface/user_interface.h>
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <runtime/runtime.h>
#include <io/io.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <crypto/crypto.h>
#include <titles/titles.h>
#include <core/core.h>

namespace xk::hardware::ide {

PDEVICE_OBJECT DvdDeviceObject;
static LONGLONG DvdLength;      // media size in bytes (READ CAPACITY), 0 = unknown
volatile UCHAR DvdLastSense[3]; // key, ASC, ASCQ of the last failed command

constexpr ULONG IOCTL_CDROM_READ_TOC = 0x24000, IOCTL_CDROM_GET_DRIVE_GEOMETRY = 0x2404C, IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014,
                IOCTL_DVD_READ_STRUCTURE = 0x335140;
constexpr NTSTATUS STATUS_NO_MEDIA_IN_DEVICE = (NTSTATUS)0xC0000013;

// --- Polled ATAPI packet command (PIO data phase). Called with the channel owned (StartIo) or at init.

// Sense mapping and retries (spec/stock/04c §1, as the reference's): UNIT ATTENTION (the disc changed) and NOT READY with ASC 04 (becoming ready) are retried,
// the latter after 100 ms; a status is reported only once the retries are used.
enum IDEX_RETRY { NoRetry, RetryNow, RetryLater };

static IDEX_RETRY DvdSenseRetry() {
    UCHAR key = DvdLastSense[0] & 0x0F, asc = DvdLastSense[1];
    if (key == 6)
        return RetryNow;
    if (key == 2 && asc == 0x04)
        return RetryLater;
    return NoRetry;
}

static NTSTATUS DvdMapSense() {
    UCHAR key = DvdLastSense[0] & 0x0F, asc = DvdLastSense[1];
    if (key == 2)
        return asc == 0x3A ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_DEVICE_NOT_READY;
    if (key == 5)
        return STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_IO_DEVICE_ERROR; // UNIT ATTENTION too, once its retries are used up
}

static bool AtapiIssue(const UCHAR *cdb, bool dma, ULONG byteLimit) {
    outb(IDE_DRIVE, 0xB0);
    if (!WaitForIdeStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, 0, 2000000))
        return false;
    outb(IDE_FEATURES, dma ? 1 : 0);
    outb(IDE_LBA1, (UCHAR)byteLimit);
    outb(IDE_LBA2, (UCHAR)(byteLimit >> 8));
    outb(IDE_COMMAND, 0xA0); // PACKET
    KeStallExecutionProcessor(1);
    if (!WaitForIdeStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 2000000))
        return false;
    for (int i = 0; i < 6; i++)
        outw(IDE_DATA, (USHORT)(cdb[2 * i] | (cdb[2 * i + 1] << 8)));
    return true;
}

static NTSTATUS AtapiPio(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred);

static void AtapiRequestSense() {
    UCHAR cdb[12] = {0x03, 0, 0, 0, 18, 0};
    UCHAR sense[18] = {};
    ULONG got;
    DvdLastSense[0] = 0;
    DvdLastSense[1] = 0;
    DvdLastSense[2] = 0;
    if (AtapiPio(cdb, sense, sizeof(sense), false, &got) == STATUS_SUCCESS && got >= 14) {
        DvdLastSense[0] = sense[2];
        DvdLastSense[1] = sense[12];
        DvdLastSense[2] = sense[13];
    }
}

static NTSTATUS AtapiPioOnce(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred) {
    *transferred = 0;
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    NTSTATUS status = STATUS_SUCCESS;
    if (!AtapiIssue(cdb, false, length > 0xFFFE ? 0xFFFE : (length ? length : 2))) {
        status = STATUS_IO_DEVICE_ERROR;
    } else {
        // Bounded: a drive that keeps DRQ up (or reports a byte count of 0) must not hold the CPU here forever.
        for (ULONG phase = 0;; phase++) {
            if (phase == 64 || *transferred > length + 0x10000) {
                XK_SERIAL("*** atapi: command %#x still requesting data after %lu phases, %lu bytes\n", cdb[0], phase,
                          *transferred);
                status = STATUS_IO_DEVICE_ERROR;
                break;
            }
            if (!WaitForIdeStatus(IDE_STATUS_BSY, 0, 10000000)) {
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
            if (!count) // DRQ without a byte count: not a data phase we can serve
                count = 2;
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
    if (status == STATUS_IO_DEVICE_ERROR && cdb[0] != 0x03)
        AtapiRequestSense();
    return status;
}

static NTSTATUS AtapiPio(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred) {
    for (ULONG retries = 0;; retries++) {
        NTSTATUS status = AtapiPioOnce(cdb, data, length, toDevice, transferred);
        if (status != STATUS_IO_DEVICE_ERROR || cdb[0] == 0x03)
            return status;
        IDEX_RETRY retry = DvdSenseRetry();
        if (retry == NoRetry || retries == IdexChannelObject.MaximumIoRetries)
            return DvdMapSense();
        if (retry == RetryLater)
            KeStallExecutionProcessor(100000);
    }
}

static NTSTATUS DvdReadCapacity() {
    UCHAR cdb[12] = {0x25};
    UCHAR cap[8];
    ULONG got;
    NTSTATUS status = AtapiPio(cdb, cap, sizeof(cap), false, &got);
    if (NT_SUCCESS(status)) {
        ULONG last = (cap[0] << 24) | (cap[1] << 16) | (cap[2] << 8) | cap[3];
        DvdLength = ((LONGLONG)last + 1) << 11;
    } else {
        DvdLength = 0;
    }
    return status;
}

// --- Dispatch

volatile ULONG DvdAbsentReason; // 0 = a DVD drive, else why none was found (DvdStart)

static NTSTATUS DvdComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static NTSTATUS NTAPI DvdCreateClose(PDEVICE_OBJECT, PIRP Irp) {
    return DvdComplete(Irp, 0, 0);
}

static NTSTATUS NTAPI DvdRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    NTSTATUS served;
    if (storage::ServeVirtualDvd(Irp, &served))
        return served;
    if (DvdAbsentReason) // no drive: an empty tray
        return DvdComplete(Irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG len = s->Parameters.Read.Length;
    LONGLONG offset = s->Parameters.Read.ByteOffset.QuadPart;
    PUCHAR buffer = (PUCHAR)Irp->UserBuffer + s->Parameters.Read.BufferOffset;
    // The media size is learnt by READ CAPACITY when the request is started.
    if (((ULONG)buffer & 1) || (len & 0x7FF) || (offset & 0x7FF) || offset < 0 || Irp->SegmentArray)
        return DvdComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    if (!len)
        return DvdComplete(Irp, STATUS_SUCCESS, 0);
    Irp->IoStatus.Information = len;
    s->Parameters.Read.ByteOffset.QuadPart = offset >> 11;
    s->Parameters.Read.CacheBuffer = buffer;
    IoMarkIrpPending(Irp);
    StartIdePacket(DeviceObject, Irp);
    return STATUS_PENDING;
}

static NTSTATUS DvdAuthenticate();

static NTSTATUS NTAPI DvdDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    NTSTATUS served;
    if (storage::ServeVirtualDvd(Irp, &served))
        return served;
    if (DvdAbsentReason) // no drive: an empty tray
        return DvdComplete(Irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
    switch (IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_READ_TOC:
    case IOCTL_SCSI_PASS_THROUGH_DIRECT:
    case IOCTL_DVD_READ_STRUCTURE:
        IoMarkIrpPending(Irp);
        StartIdePacket(DeviceObject, Irp);
        return STATUS_PENDING;
    case 0x24080: // IOCTL_CDROM_AUTHENTICATE_DISK: synchronous, in the caller's thread
        return DvdComplete(Irp, DvdAuthenticate(), 0);
    default:
        return DvdComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static void DvdFinish(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IofCompleteRequest(Irp, 1);
    StartNextIdePacket();
}

// SCSI_PASS_THROUGH_DIRECT (Xbox layout): {Length, ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength, DataIn, DataTransferLength, TimeOutValue,
// DataBuffer, SenseInfoOffset, Cdb[16]}.
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

// --- Xbox drive security for titles (spec/stock/04 §3a). Titles built with later XDKs check the disc themselves:
// MODE SENSE page 0x3E must report an authenticated Xbox disc, and the security sector's timestamp (+0x4AB, against the certificate's), SHA-1 (+0x4CF) and RSA
// signature (+0x4E3, under XePublicKeyData) must all hold.
// - XePublicKeyData is an open exponent-1 key (src/titles/title_loader.cpp), so every security sector handed to a title gets the signature block of its own
//   digest in place of Microsoft's signature, which this kernel cannot check.
// - A drive without the Xbox security commands (emulators: xemu rejects page 0x3E and answers the security read with the ordinary physical-format block) gets
//   them emulated: an authenticated page, and a security sector built for the running title.
constexpr ULONG SECURITY_SECTOR_SIZE = 0x664;
static UCHAR DvdSecurityMode; // 0 = not probed yet, 1 = the drive implements page 0x3E, 2 = emulated

static bool DvdIsSecurityRead(const UCHAR *cdb) {
    return cdb[0] == 0xAD && cdb[2] == 0xFF && cdb[3] == 0x02 && cdb[4] == 0xFD && cdb[5] == 0xFF && cdb[6] == 0xFE;
}

static bool DvdEmulatingSecurity() {
    if (!DvdSecurityMode) {
        static const UCHAR sense[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
        UCHAR page[28];
        ULONG got;
        NTSTATUS status = AtapiPio(sense, page, sizeof(page), false, &got);
        if (NT_SUCCESS(status))
            DvdSecurityMode = 1;
        else if (status == STATUS_INVALID_DEVICE_REQUEST) // ILLEGAL REQUEST: no such page
            DvdSecurityMode = 2;
    }
    return DvdSecurityMode == 2;
}

// The PKCS#1 block of the digest at +0x4CF, little-endian as XcVerifyPKCS1Signature decodes it with the exponent-1 key: digest reversed | SHA-1 DigestInfo
// reversed | 00 | FF.. | 01 | 00 (256 bytes).
static void DvdSignSecuritySector(PUCHAR sector) {
    static const UCHAR sha1Prefix[] = {0x14, 0x04, 0x00, 0x05, 0x1A, 0x02, 0x03, 0x0E,
                                       0x2B, 0x05, 0x06, 0x09, 0x30, 0x21, 0x30};
    PUCHAR sig = sector + 0x4E3;
    for (ULONG i = 0; i < 20; i++)
        sig[i] = sector[0x4CF + 19 - i];
    RtlCopyMemory(sig + 20, sha1Prefix, sizeof(sha1Prefix));
    sig[35] = 0;
    RtlFillMemory(sig + 36, 254 - 36, 0xFF);
    sig[254] = 1;
    sig[255] = 0;
}

// An emulated security sector for the running title: its certificate timestamp at +0x4AB, the SHA-1 of the length (dword 0x4CB) and the 0x4CB bytes at +4 at
// +0x4CF (as titles compute it), no challenge table.
static void DvdBuildSecuritySector(PUCHAR sector) {
    RtlZeroMemory(sector, SECURITY_SECTOR_SIZE);
    sector[4] = 1; // the disc's id byte, as in the emulated mode page (byte 13)
    auto header = (PUCHAR)0x10000; // the running XBE's header; not mapped while no title is loaded
    if (MmIsAddressValid(header + 0x118) && *(ULONG *)header == 0x48454258 /*XBEH*/) {
        auto cert = *(PUCHAR *)(header + 0x118);
        if (MmIsAddressValid(cert) && MmIsAddressValid(cert + 7))
            RtlCopyMemory(sector + 0x4AB, cert + 4, 4);
    }
    UCHAR sha[116];
    ULONG length = 0x4CB;
    XcSHAInit(sha);
    XcSHAUpdate(sha, (PUCHAR)&length, 4);
    XcSHAUpdate(sha, sector + 4, 0x4CB);
    XcSHAFinal(sha, sector + 0x4CF);
}

// The Xbox security commands (spec/stock/04 §3a); disc images (storage/virtual_disc.cpp) answer the same set.
bool IsDvdSecurityCommand(const UCHAR *cdb, const UCHAR *data, ULONG length) {
    return (cdb[0] == 0x5A && (cdb[2] & 0x3F) == 0x3E) ||                       // MODE SENSE, page 0x3E
           (cdb[0] == 0x55 && data && length >= 9 && (data[8] & 0x3F) == 0x3E) || // MODE SELECT (a challenge)
           DvdIsSecurityRead(cdb);
}

// The emulated answer to a security command (IsDvdSecurityCommand), without the drive. Disc images use it too (storage/virtual_disc.cpp): the security sector
// comes back signed for the open key, as from an emulating drive.
void EmulatedDvdSecurityReply(const UCHAR *cdb, PUCHAR data, ULONG length, NTSTATUS *status, ULONG *got) {
    *status = STATUS_SUCCESS;
    *got = 0;
    if (cdb[0] == 0x5A) {
        // Challenges done (10), Xbox disc (11), authenticated (12), disc id byte (13).
        static const UCHAR page[28] = {0, 0x1A, 0, 0, 0, 0, 0, 0, 0x3E, 0x12, 1, 1, 1, 1};
        *got = length < sizeof(page) ? length : sizeof(page);
        if (data)
            RtlCopyMemory(data, page, *got);
    } else if (DvdIsSecurityRead(cdb)) {
        if (!data || length < SECURITY_SECTOR_SIZE) {
            *status = STATUS_INVALID_PARAMETER;
            return;
        }
        DvdBuildSecuritySector(data);
        DvdSignSecuritySector(data);
        *got = SECURITY_SECTOR_SIZE;
    } // MODE SELECT (a challenge): accepted
}

// SCSI pass-through for the security commands on an emulating drive; false = send the command to the drive.
static bool DvdEmulateSecurity(const UCHAR *cdb, PUCHAR data, ULONG length, NTSTATUS *status, ULONG *got) {
    if (!IsDvdSecurityCommand(cdb, data, length) || !DvdEmulatingSecurity())
        return false;
    if (cdb[0] == 0x5A) {
        // The drive still gets the command, as with a drive that has the page: its answer is dropped. After a disc change the first command reports it (xemu:
        // "no media" once, then UNIT ATTENTION), and the mount's authentication is the command that takes it, as on the reference.
        UCHAR scratch[28];
        ULONG ignored;
        AtapiPio(cdb, scratch, sizeof(scratch), false, &ignored);
    }
    EmulatedDvdSecurityReply(cdb, data, length, status, got);
    return true;
}

static void DvdControl(PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR cdb[12] = {};
    ULONG got = 0;
    NTSTATUS status;
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_CDROM_CHECK_VERIFY: // TEST UNIT READY
        status = AtapiPio(cdb, nullptr, 0, false, &got);
        if (NT_SUCCESS(status))
            DvdReadCapacity();
        DvdFinish(Irp, status, 0);
        return;
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
        if (outLen < sizeof(DISK_GEOMETRY)) {
            DvdFinish(Irp, STATUS_BUFFER_TOO_SMALL_IO, 0);
            return;
        }
        cdb[0] = 0x25; // as DvdReadCapacity sends it (for the log)
        status = DvdReadCapacity();
        if (NT_SUCCESS(status)) {
            auto g = (PDISK_GEOMETRY)Irp->UserBuffer;
            g->Cylinders.QuadPart = DvdLength >> 11;
            g->MediaType = 11; // RemovableMedia
            g->TracksPerCylinder = 1;
            g->SectorsPerTrack = 1;
            g->BytesPerSector = 0x800;
        }
        DvdFinish(Irp, status, sizeof(DISK_GEOMETRY));
        return;
    case IOCTL_CDROM_READ_TOC:
        cdb[0] = 0x43;
        cdb[7] = (UCHAR)(outLen >> 8);
        cdb[8] = (UCHAR)outLen;
        status = AtapiPio(cdb, (PUCHAR)Irp->UserBuffer, outLen, false, &got);
        DvdFinish(Irp, status, got);
        return;
    case IOCTL_DVD_READ_STRUCTURE: {
        // READ DVD STRUCTURE, format 0 (physical/layer descriptor): raw response into the output buffer.
        cdb[0] = 0xAD;
        cdb[8] = (UCHAR)(outLen >> 8);
        cdb[9] = (UCHAR)outLen;
        status = AtapiPio(cdb, (PUCHAR)Irp->UserBuffer, outLen, false, &got);
        if (status == STATUS_INVALID_DEVICE_REQUEST)
            status = (NTSTATUS)0xC0000014; // STATUS_UNRECOGNIZED_MEDIA: not a DVD
        DvdFinish(Irp, status, got);
        return;
    }
    case IOCTL_SCSI_PASS_THROUGH_DIRECT: {
        auto p = (SCSI_PASS_THROUGH_DIRECT_ *)s->Parameters.DeviceIoControl.InputBuffer;
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(*p)) {
            DvdFinish(Irp, STATUS_INVALID_PARAMETER, 0);
            return;
        }
        // Always the 12-byte ATAPI packet: CdbLength is ignored, as the reference does (callers commonly leave it 0).
        RtlCopyMemory(cdb, p->Cdb, 12);
        if (!DvdEmulateSecurity(cdb, (PUCHAR)p->DataBuffer, p->DataTransferLength, &status, &got)) {
            status = AtapiPio(cdb, (PUCHAR)p->DataBuffer, p->DataTransferLength,
                              p->DataIn == 0 /*SCSI_IOCTL_DATA_OUT*/, &got);
        }
        if (NT_SUCCESS(status) && got >= 0x4E3 + 256 && DvdIsSecurityRead(cdb))
            DvdSignSecuritySector((PUCHAR)p->DataBuffer);
        p->ScsiStatus = NT_SUCCESS(status) ? 0 : 2; // CHECK CONDITION
        p->DataTransferLength = got;
        DvdFinish(Irp, status, got);
        return;
    }
    }
    DvdFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

// --- Xbox game disc authentication (spec/stock/04 §3a). The drive's security mode page (0x3E) and the disc's security
// sector (READ DVD STRUCTURE 0xFF02FDFF, layer 0xFE) provide a challenge table; replaying it proves a pressed Xbox disc. Success marks the media as "title
// disc" (XBE media type 2) and pins the disc's title.

constexpr NTSTATUS STATUS_TOO_MANY_SECRETS = (NTSTATUS)0xC0000156, STATUS_UNRECOGNIZED_MEDIA_ = (NTSTATUS)0xC0000014;
volatile ULONG DvdAuthStage, DvdAuthStatus; // diagnostics: how far the last authentication got
volatile UCHAR DvdAuthPage[28];             // diagnostics: the drive's security page it read

static NTSTATUS DvdPassThrough(const UCHAR *cdb, PVOID data, ULONG length, bool dataIn) {
    SCSI_PASS_THROUGH_DIRECT_ p = {};
    p.Length = sizeof(p);
    p.CdbLength = 12;
    p.DataIn = dataIn;
    p.DataTransferLength = length;
    p.DataBuffer = data;
    RtlCopyMemory(p.Cdb, cdb, 12);
    return IoSynchronousDeviceIoControlRequest(IOCTL_SCSI_PASS_THROUGH_DIRECT, DvdDeviceObject, &p, sizeof(p), nullptr, 0, nullptr, FALSE);
}

// MODE SENSE(10), the 28-byte security page; byte 11 must be 1.
static NTSTATUS DvdSecurityPage(PUCHAR page) {
    static const UCHAR cdb[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
    RtlZeroMemory(page, 28);
    NTSTATUS status = DvdPassThrough(cdb, page, 28, true);
    if (!NT_SUCCESS(status))
        return status == STATUS_TOO_MANY_SECRETS ? STATUS_UNRECOGNIZED_MEDIA_ : status;
    return page[11] == 1 ? STATUS_SUCCESS : STATUS_UNRECOGNIZED_MEDIA_;
}

// One challenge: MODE SELECT the page with the challenge, MODE SENSE it back, compare the response.
static NTSTATUS DvdChallenge(const UCHAR *sector, const UCHAR *entry, bool first, bool last) {
    UCHAR page[28] = {};
    page[1] = 0x1A;
    page[8] = 0x3E;
    page[9] = 0x12;
    page[10] = last;
    page[11] = 1;
    page[12] = !first;
    page[13] = sector[4];
    page[14] = 1;
    page[15] = entry[1];
    RtlCopyMemory(page + 16, entry + 2, 4);
    static const UCHAR select[12] = {0x55, 0, 0, 0, 0, 0, 0, 0, 0x1C};
    NTSTATUS status = DvdPassThrough(select, page, 28, false);
    if (NT_SUCCESS(status)) {
        static const UCHAR sense[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
        status = DvdPassThrough(sense, page, 28, true);
    }
    if (NT_SUCCESS(status) && (!page[12] || RtlCompareMemory(page + 20, entry + 7, 4) != 4))
        status = STATUS_TOO_MANY_SECRETS;
    return status;
}

static NTSTATUS DvdAuthenticate() {
    DvdAuthStage = 1;
    if (titles::TitleDiscAuthenticated)
        return STATUS_SUCCESS;
    UCHAR page[28];
    NTSTATUS status = DvdSecurityPage(page);
    if (DvdSecurityMode == 2) // a drive without Xbox security (emulator): no disc to authenticate
        status = STATUS_UNRECOGNIZED_MEDIA_;
    for (int i = 0; i < 28; i++)
        DvdAuthPage[i] = page[i];
    DvdAuthStatus = status;
    if (!NT_SUCCESS(status))
        return status;
    DvdAuthStage = 2;
    auto sector = (PUCHAR)ExAllocatePoolWithTag(SECURITY_SECTOR_SIZE + 0x28, 'ScdI');
    if (!sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    static const UCHAR read[12] = {0xAD, 0, 0xFF, 0x02, 0xFD, 0xFF, 0xFE, 0, 0x06, 0x64, 0, 0xC0};
    RtlZeroMemory(sector, SECURITY_SECTOR_SIZE);
    status = DvdPassThrough(read, sector, SECURITY_SECTOR_SIZE, true);
    DvdAuthStatus = status;
    if (NT_SUCCESS(status)) {
        DvdAuthStage = 3;
        status = STATUS_TOO_MANY_SECRETS;
        UCHAR sha[116], digest[20], rc4[258];
        XcSHAInit(sha);
        XcSHAUpdate(sha, sector + 4, 0x4CB);
        XcSHAFinal(sha, digest);
        // The digest must match; the reference also checks an RSA signature over it with Microsoft's public key, which this open kernel does not carry
        // (spec/stock/04 §3a).
        if (page[13] == sector[4] && RtlCompareMemory(digest, sector + 0x4CF, 20) == 20) {
            XcSHAInit(sha);
            XcSHAUpdate(sha, sector + 0x4A3, 0x2C);
            XcSHAFinal(sha, digest);
            XcRC4Key(rc4, 7, digest);
            XcRC4Crypt(rc4, 0xFD, sector + 0x306);
            ULONG count = sector[0x305];
            DvdAuthStage = 4 | sector[0x304] << 8 | count << 16;
            if (sector[0x304] == 1 && count && count < 0x18) {
                ULONG start = KeQueryPerformanceCounter().LowPart % count, i = start, lastValid = 0xFFFFFFFF;
                bool first = true;
                status = STATUS_SUCCESS;
                do {
                    PUCHAR entry = sector + 0x306 + i * 11;
                    if (entry[0] == 1) {
                        status = DvdChallenge(sector, entry, first, false);
                        if (!NT_SUCCESS(status))
                            break;
                        first = false;
                        lastValid = i;
                    }
                    i = i + 1 == count ? 0 : i + 1;
                } while (i != start);
                if (NT_SUCCESS(status) && lastValid != 0xFFFFFFFF)
                    status = DvdChallenge(sector, sector + 0x306 + lastValid * 11, false, true);
                else if (NT_SUCCESS(status))
                    status = STATUS_TOO_MANY_SECRETS;
                DvdAuthStatus = status;
                if (NT_SUCCESS(status)) {
                    DvdAuthStage = 5;
                    titles::TitleDiscAuthenticated = 1;
                    titles::DiscTitleId = *(ULONG *)(sector + 0x4AB);
                }
            }
        }
    }
    ExFreePool(sector);
    return status;
}

// READ(10) of up to 128 KB through the bus master.
static void DvdStartTransfer(PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG chunk = s->Parameters.Read.Length < 0x20000 ? s->Parameters.Read.Length : 0x20000;
    ULONG lba = s->Parameters.Read.ByteOffset.LowPart, blocks = chunk >> 11;
    UCHAR cdb[12] = {0x28, 0, (UCHAR)(lba >> 24), (UCHAR)(lba >> 16), (UCHAR)(lba >> 8), (UCHAR)lba, 0, (UCHAR)(blocks >> 8), (UCHAR)blocks};
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    KIRQL old = KfRaiseIrql(ch->InterruptIrql);
    if (!ProgramIdeDma(s->Parameters.Read.CacheBuffer, chunk) || !AtapiIssue(cdb, true, 0)) {
        KfLowerIrql(old);
        DvdFinish(Irp, STATUS_IO_DEVICE_ERROR, 0);
        return;
    }
    ch->ExpectingBusMasterInterrupt = TRUE;
    ch->Timeout = 10;
    outb(IDE_BM_COMMAND, 0x09);
    KfLowerIrql(old);
}

// A read retried 100 ms later (the drive was becoming ready).
static KTIMER DvdRetryTimer;
static KDPC DvdRetryDpc;
static void NTAPI DvdRetryRoutine(PKDPC, PVOID, PVOID, PVOID) {
    if (IdexChannelObject.CurrentIrp)
        DvdStartTransfer(IdexChannelObject.CurrentIrp);
}

static void NTAPI DvdFinishIo() {
    PIDE_CHANNEL_OBJECT ch = &IdexChannelObject;
    PIRP irp = ch->CurrentIrp;
    if (!irp)
        return;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    UCHAR bm, ata = IdeCompletionStatus(&bm);
    if ((ata & IDE_STATUS_ERR) || (bm & 2)) {
        AtapiRequestSense();
        NTSTATUS status = DvdMapSense();
        IDEX_RETRY retry = DvdSenseRetry();
        if ((status == STATUS_IO_DEVICE_ERROR || retry != NoRetry) && ch->IoRetries++ < ch->MaximumIoRetries) {
            if (retry == RetryLater) {
                LARGE_INTEGER due;
                due.QuadPart = -10000LL * 100;
                KeSetTimer(&DvdRetryTimer, due, &DvdRetryDpc);
            } else {
                DvdStartTransfer(irp);
            }
            return;
        }
        DvdFinish(irp, status, 0);
        return;
    }
    ULONG chunk = s->Parameters.Read.Length < 0x20000 ? s->Parameters.Read.Length : 0x20000;
    s->Parameters.Read.Length -= chunk;
    if (s->Parameters.Read.Length) {
        s->Parameters.Read.CacheBuffer = (PUCHAR)s->Parameters.Read.CacheBuffer + chunk;
        s->Parameters.Read.ByteOffset.LowPart += chunk >> 11;
        ch->IoRetries = 0;
        DvdStartTransfer(irp);
        return;
    }
    DvdFinish(irp, STATUS_SUCCESS, irp->IoStatus.Information);
}

static void NTAPI DvdStartIo(PDEVICE_OBJECT, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s->MajorFunction != IRP_MJ_READ) {
        DvdControl(Irp);
        return;
    }
    // Bounds against the media size (learnt here, so a disc change is noticed).
    if (!DvdLength)
        DvdReadCapacity();
    LONGLONG end = ((LONGLONG)s->Parameters.Read.ByteOffset.LowPart << 11) + s->Parameters.Read.Length;
    if (!DvdLength) {
        DvdFinish(Irp, DvdMapSense(), 0);
        return;
    }
    if (end > DvdLength) {
        DvdFinish(Irp, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    IdexChannelObject.FinishIoRoutine = DvdFinishIo;
    IdexChannelObject.TimeoutExpiredRoutine = DvdFinishIo;
    DvdStartTransfer(Irp);
}

DRIVER_OBJECT DvdDriverObject = {
    DvdStartIo,
    nullptr,
    nullptr,
    {DvdCreateClose, DvdCreateClose, DvdRead, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, IoInvalidDeviceRequest, IoInvalidDeviceRequest, DvdDeviceControl, IoInvalidDeviceRequest, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest}};

// Bring the drive up (device 1): 0 = ready, else the step that found no drive (1 select, 2 busy, 3 transfer mode, 4 IDENTIFY PACKET). Called at the channel's
// IRQL. One attempt at bringing device 1 up (SET FEATURES PIO 4, UDMA 2; IDENTIFY PACKET DEVICE): 0, or the failing step. Wait for (status & mask) == value, as
// WaitForIdeStatus, but give up early when nothing answers: a command written with device 1 selected and no device 1 on the cable leaves the status register at
// 0 (device 0 answers the read for it; seen with SSDs), while a device that is there shows BSY within microseconds and then DRQ, DRDY or ERR. 1 = the status
// was reached, 0 = timed out, -1 = the status stayed 0 for 100 ms (no device 1).
static int DvdWaitForStatus(UCHAR mask, UCHAR value, ULONG timeoutUs) {
    ULONG quiet = 0;
    for (ULONG i = 0; i < timeoutUs; i += 10) {
        UCHAR s = inb(IDE_ALT_STATUS);
        if ((s & mask) == value)
            return 1;
        quiet = s ? 0 : quiet + 10;
        if (quiet >= 100000)
            return -1;
        KeStallExecutionProcessor(10);
    }
    return (inb(IDE_ALT_STATUS) & mask) == value ? 1 : 0;
}

static ULONG DvdIdentify() {
    static const UCHAR modes[] = {0x0C, 0x42};
    for (UCHAR mode : modes) {
        outb(IDE_FEATURES, 0x03);
        outb(IDE_COUNT, mode);
        outb(IDE_COMMAND, 0xEF);
        KeStallExecutionProcessor(1);
        if (!WaitForIdeStatus(IDE_STATUS_BSY, 0, 5000000) || (inb(IDE_ALT_STATUS) & IDE_STATUS_ERR))
            return 3;
    }
    outb(IDE_COMMAND, 0xA1); // IDENTIFY PACKET DEVICE
    KeStallExecutionProcessor(1);
    if (DvdWaitForStatus(IDE_STATUS_BSY | IDE_STATUS_DRQ, IDE_STATUS_DRQ, 5000000) != 1)
        return 4;
    for (int i = 0; i < 256; i++)
        inw(IDE_DATA);
    // A drive that offers more than one block (DRQ still up) holds the channel, and the disk on the same cable with it: read until it lets go (bounded).
    for (int i = 0; i < 65536 && (inb(IDE_ALT_STATUS) & IDE_STATUS_DRQ); i++)
        inw(IDE_DATA);
    inb(IDE_STATUS);
    return 0;
}

// ATAPI DEVICE RESET (0x08) of device 1, then wait (bounded) until it is no longer busy.
static void DvdReset() {
    outb(IDE_COMMAND, 0x08);
    KeStallExecutionProcessor(10);
    WaitForIdeStatus(IDE_STATUS_BSY, 0, 5000000);
}

// Bring the drive up (device 1): 0 = ready, else the step that found no drive (1 select, 2 busy, 3 transfer mode, 4 IDENTIFY PACKET). Called at the channel's
// IRQL. A drive that answers but is not ready yet (a DVD drive still starting up when the boot is fast) is reset and tried again, for about 30 s in all (the
// reference's busy wait). Waits for device 1 to be ready for its first command: 0, or why there is no drive (1 nothing on the bus, 2 still busy after 30 s).
// Runs at the caller's IRQL (OXOS: the reference waits raised), so the boot splash and jingle keep running while a drive starts up; it only reads the alternate
// status. Its limits follow the clock, as other threads take the processor meanwhile.
static ULONG DvdWaitReady() {
    outb(IDE_DRIVE, 0xB0);
    KeStallExecutionProcessor(1);
    UCHAR st = inb(IDE_ALT_STATUS);
    if (st == 0xFF || st == 0x7F) // nothing drives the bus for device 1 (floating, or only the host's pull-down on DD7)
        return 1;
    const ULONGLONG start = KeQueryInterruptTime();
    auto elapsedMs = [&] { return (ULONG)((KeQueryInterruptTime() - start) / 10000); };
    while (inb(IDE_ALT_STATUS) & IDE_STATUS_BSY) {
        if (elapsedMs() >= 30000)
            return 2;
        KeStallExecutionProcessor(10);
    }
    // A drive still starting up (a fast boot: the DVD drive spins up for seconds after power-on) reads back a status without DRDY, or a floating pattern (0x3F
    // / 0x7F / 0xFF): wait up to 10 s for it to be ready before the first command, instead of failing the transfer-mode command and taking the no-drive path.
    for (const ULONG ready = elapsedMs(); elapsedMs() - ready < 10000;) {
        UCHAR s = inb(IDE_ALT_STATUS);
        if (s == 0) // nothing answers (device 0 reads back 0 for an absent device 1)
            break;
        if (s != 0x3F && s != 0x7F && s != 0xFF && !(s & IDE_STATUS_BSY) && (s & 0x40))
            break;
        KeStallExecutionProcessor(10000);
    }
    return 0;
}

// The drive's IDENTIFY, tried three times (at the IDE interrupt's IRQL): 0, or the step that failed.
static ULONG DvdStart() {
    outb(IDE_DRIVE, 0xB0);
    KeStallExecutionProcessor(1);
    ULONG step = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        step = DvdIdentify();
        if (!step)
            return 0;
        UCHAR status = inb(IDE_ALT_STATUS);
        XK_SERIAL("dvd: attempt %d failed at step %lu, status %#x\n", attempt + 1, step, status);
        if (!status) // nothing answers (device 0 reads back for an absent device 1): no drive, no retries
            break;
        DvdReset();
    }
    return step;
}

// A console without a DVD drive (OXOS deviation: the reference stops with fatal error 10, 11 or 12) still gets \Device\CdRom0, because every title's start-up
// links D: to it, but the device acts as an empty drive: reads and drive commands fail with STATUS_NO_MEDIA_IN_DEVICE without touching the bus, so the
// dashboard starts.
void InitDvdDrive() {
    ULONG reason = DvdWaitReady();
    KIRQL old = KfRaiseIrql(IdexChannelObject.InterruptIrql);
    DvdAbsentReason = reason ? reason : DvdStart();
    if (DvdAbsentReason) {
        // The disk again. A busy device ignores the device-select register, so device 1 was reset (and waited for) above; wait for the disk to be idle too
        // before the next request (bounded).
        outb(IDE_DRIVE, 0xA0);
        KeStallExecutionProcessor(1);
        WaitForIdeStatus(IDE_STATUS_BSY, 0, 5000000);
        inb(IDE_STATUS);
    }
    XK_SERIAL("dvd: lowering IRQL %u -> %u; IRQ 14 taken %lu\n", KeGetCurrentIrql(), old, core::IrqCounts[14]);
    KfLowerIrql(old);
    XK_SERIAL("dvd: %s (step %lu)\n", DvdAbsentReason ? "no drive" : "drive found", DvdAbsentReason);

    OBJECT_STRING name, link;
    RtlInitAnsiString(&name, "\\Device\\CdRom0");
    RtlInitAnsiString(&link, "\\??\\CdRom0:");
    PDEVICE_OBJECT dev;
    objects::TraceSymbolicLink = 1;
    NTSTATUS created = IoCreateDevice(&DvdDriverObject, 0, &name, FILE_DEVICE_CD_ROM, FALSE, &dev);
    objects::TraceSymbolicLink = 0;
    XK_SERIAL("dvd: IoCreateDevice %#lx; IRQ 14 taken %lu\n", created, core::IrqCounts[14]);
    objects::TraceSymbolicLink = 1;
    NTSTATUS linked = NT_SUCCESS(created) ? IoCreateSymbolicLink(&link, &name) : created;
    objects::TraceSymbolicLink = 0;
    if (!NT_SUCCESS(created) || !NT_SUCCESS(linked))
        user_interface::ShowFatalError(0x10E5F);
    XK_SERIAL("dvd: link made\n");
    dev->Flags |= 0x44;
    dev->AlignmentRequirement = 1;
    dev->SectorSize = 0x800;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    DvdDeviceObject = dev;
    KeInitializeTimerEx(&DvdRetryTimer, NotificationTimer);
    KeInitializeDpc(&DvdRetryDpc, DvdRetryRoutine, nullptr);
    XK_SERIAL("dvd: timer and DPC set up\n");
    // Leave device 0 (the disk) selected and both devices out of a data phase.
    outb(IDE_DRIVE, 0xA0);
    KeStallExecutionProcessor(1);
    XK_SERIAL("dvd: device ready, selected %#x, status %#x\n", inb(IDE_DRIVE), inb(IDE_ALT_STATUS));
}

} // namespace xk::hardware::ide
