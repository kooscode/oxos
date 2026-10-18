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
constexpr NTSTATUS STATUS_NO_MEDIA_IN_DEVICE = (NTSTATUS)0xC0000013;

// --- Polled ATAPI packet command (PIO data phase). Called with the channel owned (StartIo) or at init.

// Sense mapping and retries (spec/04c §1, as the reference's): UNIT ATTENTION (the disc changed) and NOT READY with
// ASC 04 (becoming ready) are retried, the latter after 100 ms; a status is reported only once the retries are used.
enum IDEX_RETRY { IdexNoRetry, IdexRetryNow, IdexRetryLater };

static IDEX_RETRY IdexCdRomSenseRetry() {
    UCHAR key = IdexCdRomLastSense[0] & 0x0F, asc = IdexCdRomLastSense[1];
    if (key == 6)
        return IdexRetryNow;
    if (key == 2 && asc == 0x04)
        return IdexRetryLater;
    return IdexNoRetry;
}

static NTSTATUS IdexCdRomMapSense() {
    UCHAR key = IdexCdRomLastSense[0] & 0x0F, asc = IdexCdRomLastSense[1];
    if (key == 2)
        return asc == 0x3A ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_DEVICE_NOT_READY;
    if (key == 5)
        return STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_IO_DEVICE_ERROR; // UNIT ATTENTION too, once its retries are used up
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

static NTSTATUS IdexAtapiPioOnce(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred) {
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
    if (status == STATUS_IO_DEVICE_ERROR && cdb[0] != 0x03)
        IdexAtapiRequestSense();
    return status;
}

static NTSTATUS IdexAtapiPio(const UCHAR *cdb, PUCHAR data, ULONG length, bool toDevice, ULONG *transferred) {
    for (ULONG retries = 0;; retries++) {
        NTSTATUS status = IdexAtapiPioOnce(cdb, data, length, toDevice, transferred);
        if (status != STATUS_IO_DEVICE_ERROR || cdb[0] == 0x03)
            return status;
        IDEX_RETRY retry = IdexCdRomSenseRetry();
        if (retry == IdexNoRetry || retries == IdexChannelObject.MaximumIoRetries)
            return IdexCdRomMapSense();
        if (retry == IdexRetryLater)
            KeStallExecutionProcessor(100000);
    }
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

static NTSTATUS IdexCdRomAuthenticate();

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
    case 0x24080: // IOCTL_CDROM_AUTHENTICATE_DISK: synchronous, in the caller's thread
        return IdexCdRomComplete(Irp, IdexCdRomAuthenticate(), 0);
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

#if XK_DRIVE_LOG
// Diagnostics (DRIVE_LOG builds): the last 16 drive control requests: code, status, bytes, CDB, first 32 response bytes.
struct IDEX_CDROM_LOG {
    ULONG Code, Status, Got, Length;
    UCHAR Cdb[16], Data[32];
};
XK_PERSIST IDEX_CDROM_LOG IdexCdRomLog[16]; // kept across a quick reboot
XK_PERSIST volatile ULONG IdexCdRomLogCount;
static void IdexCdRomRecord(ULONG code, const UCHAR *cdb, const void *data, ULONG length, ULONG got, NTSTATUS status) {
    ULONG n = IdexCdRomLogCount;
    IdexCdRomLogCount = n + 1;
    IDEX_CDROM_LOG &l = IdexCdRomLog[n % 16];
    l.Code = code, l.Status = status, l.Got = got, l.Length = length;
    RtlCopyMemory(l.Cdb, cdb, 12);
    RtlZeroMemory(l.Data, 32);
    if (data && NT_SUCCESS(status))
        RtlCopyMemory(l.Data, data, got < 32 ? got : 32);
}

// Opens of \\Device\\CdRom0 (code 1: name, related flag, mounted volume; code 3: the mount's status).
void IdexCdRomRecordOpen(PDEVICE_OBJECT dev, POBJECT_STRING name, bool related, ULONG value) {
    if (dev != IdexCdRomDeviceObject)
        return;
    UCHAR text[44] = {};
    ULONG n = 0;
    if (name)
        for (; n < name->Length && n < sizeof(text); n++)
            text[n] = name->Buffer[n];
    ULONG k = IdexCdRomLogCount;
    IdexCdRomRecord(name ? 1 : 3, text, nullptr, 0, related, (NTSTATUS)value);
    RtlCopyMemory(IdexCdRomLog[k % 16].Data, text + 12, 32);
}

XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
XBAPI PVOID NTAPI AvGetSavedDataAddress();
XBAPI void NTAPI AvSetSavedDataAddress(PVOID Address);
XBAPI UCHAR HalBootSMCVideoMode;
extern "C" volatile UCHAR KiQuickRebootFlag;

// 5x7 glyphs (bit 4 = leftmost column) for the log screen.
static const char IdexLogChars[] = "0123456789ABCDEFGILORV";
static const UCHAR IdexLogGlyphs[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
};

static void IdexLogText(ULONG *fb, ULONG x, ULONG y, const char *text, ULONG color) {
    for (; *text; text++, x += 12)
        for (ULONG g = 0; IdexLogChars[g]; g++) {
            if (IdexLogChars[g] != *text)
                continue;
            for (ULONG r = 0; r < 14; r++)
                for (ULONG c = 0; c < 10; c++)
                    if (IdexLogGlyphs[g][r / 2] & (0x10 >> c / 2))
                        fb[(y + r) * 640 + x + c] = color;
        }
}

static char *IdexLogHex(char *p, ULONG v, ULONG digits) {
    for (ULONG i = digits; i--;)
        *p++ = "0123456789ABCDEF"[v >> i * 4 & 0xF];
    return p;
}

// DRIVE_LOG builds, before the dashboard starts: the last drive control requests on screen for 20 s, oldest first, so a
// failed disc boot can be read on hardware without a debugger. One row each: sequence, IOCTL, the 12 CDB bytes,
// status, bytes transferred. Response data is not shown.
void IdexCdRomShowLog() {
    ULONG count = IdexCdRomLogCount;
    if (!count || HalBootSMCVideoMode == 7)
        return;
    const ULONG pitch = 640 * 4, header = 0x1000;
    auto block = (ULONG *)MmAllocateContiguousMemoryEx(header + pitch * 480, 0, 0xFFFFFF, 0x1000,
                                                       PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!block)
        return;
    ULONG *fb = block + header / 4;
    RtlZeroMemory(block, header + pitch * 480);
    block[1] = MmGetPhysicalAddress(fb);
    char line[64];
    IdexLogText(fb, 40, 40, "DRIVE LOG", 0x40FF40);
    *IdexLogHex(line, count, 8) = 0;
    IdexLogText(fb, 160, 40, line, 0x40FF40);
    ULONG first = count > 16 ? count - 16 : 0;
    for (ULONG i = first, y = 70; i < count; i++, y += 22) {
        const IDEX_CDROM_LOG &l = IdexCdRomLog[i % 16];
        char *p = IdexLogHex(line, i & 0xFF, 2);
        *p++ = ' ';
        p = IdexLogHex(p, l.Code, 6);
        *p++ = ' ';
        for (ULONG b = 0; b < 12; b++)
            p = IdexLogHex(p, l.Cdb[b], 2);
        *p++ = ' ';
        p = IdexLogHex(p, l.Status, 8);
        *p++ = ' ';
        p = IdexLogHex(p, l.Got, 4);
        *p = 0;
        IdexLogText(fb, 40, y, line, NT_SUCCESS(l.Status) ? 0xE0E0E0 : 0xFF6060);
    }
    AvSetDisplayMode((PVOID)0xFD000000, 0, 0, 0x1E, pitch, block[1]);
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * 20000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
    // Leave it up as the persisted display (the dashboard's video set-up frees it), or go back to the one there was.
    auto saved = (ULONG *)AvGetSavedDataAddress();
    if (saved) {
        AvSetDisplayMode((PVOID)0xFD000000, 0, 0, 0x1E, pitch, saved[1]);
        MmFreeContiguousMemory(block);
    } else {
        MmPersistContiguousMemory(block, header + pitch * 480, TRUE);
        AvSetSavedDataAddress(block);
    }
}
#else
#define IdexCdRomRecord(...) ((void)0)
#endif

// --- Xbox drive security for titles (spec/04 §3a). Titles built with later XDKs check the disc themselves: MODE SENSE
// page 0x3E must report an authenticated Xbox disc, and the security sector's timestamp (+0x4AB, against the
// certificate's), SHA-1 (+0x4CF) and RSA signature (+0x4E3, under XePublicKeyData) must all hold.
// - XePublicKeyData is an open exponent-1 key (src/xe/loader.cpp), so every security sector handed to a title gets
//   the signature block of its own digest in place of Microsoft's signature, which this kernel cannot check.
// - A drive without the Xbox security commands (emulators: xemu rejects page 0x3E and answers the security read with
//   the ordinary physical-format block) gets them emulated: an authenticated page, and a security sector built for the
//   running title.
XBAPI void NTAPI XcSHAInit(PUCHAR);
XBAPI void NTAPI XcSHAUpdate(PUCHAR, const UCHAR *, ULONG);
XBAPI void NTAPI XcSHAFinal(PUCHAR, PUCHAR);
XBAPI BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress);
constexpr ULONG SECURITY_SECTOR_SIZE = 0x664;
static UCHAR IdexCdRomSecurityMode; // 0 = not probed yet, 1 = the drive implements page 0x3E, 2 = emulated

static bool IdexCdRomIsSecurityRead(const UCHAR *cdb) {
    return cdb[0] == 0xAD && cdb[2] == 0xFF && cdb[3] == 0x02 && cdb[4] == 0xFD && cdb[5] == 0xFF && cdb[6] == 0xFE;
}

static bool IdexCdRomEmulatingSecurity() {
    if (!IdexCdRomSecurityMode) {
        static const UCHAR sense[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
        UCHAR page[28];
        ULONG got;
        NTSTATUS status = IdexAtapiPio(sense, page, sizeof(page), false, &got);
        if (NT_SUCCESS(status))
            IdexCdRomSecurityMode = 1;
        else if (status == STATUS_INVALID_DEVICE_REQUEST) // ILLEGAL REQUEST: no such page
            IdexCdRomSecurityMode = 2;
    }
    return IdexCdRomSecurityMode == 2;
}

// The PKCS#1 block of the digest at +0x4CF, little-endian as XcVerifyPKCS1Signature decodes it with the exponent-1 key:
// digest reversed | SHA-1 DigestInfo reversed | 00 | FF.. | 01 | 00 (256 bytes).
static void IdexCdRomSignSecuritySector(PUCHAR sector) {
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

// An emulated security sector for the running title: its certificate timestamp at +0x4AB, the SHA-1 of the length
// (dword 0x4CB) and the 0x4CB bytes at +4 at +0x4CF (as titles compute it), no challenge table.
static void IdexCdRomBuildSecuritySector(PUCHAR sector) {
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

// SCSI pass-through for the security commands on an emulating drive; false = send the command to the drive.
static bool IdexCdRomEmulateSecurity(const UCHAR *cdb, PUCHAR data, ULONG length, NTSTATUS *status, ULONG *got) {
    bool sense = cdb[0] == 0x5A && (cdb[2] & 0x3F) == 0x3E;
    bool select = cdb[0] == 0x55 && data && length >= 9 && (data[8] & 0x3F) == 0x3E;
    bool read = IdexCdRomIsSecurityRead(cdb);
    if (!(sense || select || read) || !IdexCdRomEmulatingSecurity())
        return false;
    *status = STATUS_SUCCESS;
    *got = 0;
    if (sense) {
        // The drive still gets the command, as with a drive that has the page: its answer is dropped. After a disc
        // change the first command reports it (xemu: "no media" once, then UNIT ATTENTION), and the mount's
        // authentication is the command that takes it, as on the reference.
        UCHAR scratch[28];
        ULONG ignored;
        IdexAtapiPio(cdb, scratch, sizeof(scratch), false, &ignored);
        // Challenges done (10), Xbox disc (11), authenticated (12), disc id byte (13).
        static const UCHAR page[28] = {0, 0x1A, 0, 0, 0, 0, 0, 0, 0x3E, 0x12, 1, 1, 1, 1};
        *got = length < sizeof(page) ? length : sizeof(page);
        RtlCopyMemory(data, page, *got);
    } else if (read) {
        if (!data || length < SECURITY_SECTOR_SIZE) {
            *status = STATUS_INVALID_PARAMETER;
            return true;
        }
        IdexCdRomBuildSecuritySector(data);
        *got = SECURITY_SECTOR_SIZE;
    } // MODE SELECT (a challenge): accepted
    return true;
}

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
        IdexCdRomRecord(IOCTL_CDROM_CHECK_VERIFY, cdb, nullptr, 0, got, status);
        IdexCdRomFinish(Irp, status, 0);
        return;
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
        if (outLen < sizeof(DISK_GEOMETRY)) {
            IdexCdRomFinish(Irp, STATUS_BUFFER_TOO_SMALL_IO, 0);
            return;
        }
        cdb[0] = 0x25; // as IdexCdRomReadCapacity sends it (for the log)
        status = IdexCdRomReadCapacity();
        if (NT_SUCCESS(status)) {
            auto g = (PDISK_GEOMETRY)Irp->UserBuffer;
            g->Cylinders.QuadPart = IdexCdRomLength >> 11;
            g->MediaType = 11; // RemovableMedia
            g->TracksPerCylinder = 1;
            g->SectorsPerTrack = 1;
            g->BytesPerSector = 0x800;
        }
        IdexCdRomRecord(IOCTL_CDROM_GET_DRIVE_GEOMETRY, cdb, Irp->UserBuffer, outLen, sizeof(DISK_GEOMETRY), status);
        IdexCdRomFinish(Irp, status, sizeof(DISK_GEOMETRY));
        return;
    case IOCTL_CDROM_READ_TOC:
        cdb[0] = 0x43;
        cdb[7] = (UCHAR)(outLen >> 8);
        cdb[8] = (UCHAR)outLen;
        status = IdexAtapiPio(cdb, (PUCHAR)Irp->UserBuffer, outLen, false, &got);
        IdexCdRomRecord(IOCTL_CDROM_READ_TOC, cdb, Irp->UserBuffer, outLen, got, status);
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
        IdexCdRomRecord(IOCTL_DVD_READ_STRUCTURE, cdb, Irp->UserBuffer, outLen, got, status);
        IdexCdRomFinish(Irp, status, got);
        return;
    }
    case IOCTL_SCSI_PASS_THROUGH_DIRECT: {
        auto p = (SCSI_PASS_THROUGH_DIRECT_ *)s->Parameters.DeviceIoControl.InputBuffer;
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(*p)) {
            IdexCdRomFinish(Irp, STATUS_INVALID_PARAMETER, 0);
            return;
        }
        // Always the 12-byte ATAPI packet: CdbLength is ignored, as the reference does (callers commonly leave it 0).
        RtlCopyMemory(cdb, p->Cdb, 12);
        if (!IdexCdRomEmulateSecurity(cdb, (PUCHAR)p->DataBuffer, p->DataTransferLength, &status, &got))
            status = IdexAtapiPio(cdb, (PUCHAR)p->DataBuffer, p->DataTransferLength,
                                  p->DataIn == 0 /*SCSI_IOCTL_DATA_OUT*/, &got);
        if (NT_SUCCESS(status) && got >= 0x4E3 + 256 && IdexCdRomIsSecurityRead(cdb))
            IdexCdRomSignSecuritySector((PUCHAR)p->DataBuffer);
        p->ScsiStatus = NT_SUCCESS(status) ? 0 : 2; // CHECK CONDITION
        IdexCdRomRecord(IOCTL_SCSI_PASS_THROUGH_DIRECT, p->Cdb, p->DataBuffer, p->DataTransferLength | p->CdbLength << 24 | p->DataIn << 16, got, status);
        p->DataTransferLength = got;
        IdexCdRomFinish(Irp, status, got);
        return;
    }
    }
    IdexCdRomFinish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

// --- Xbox game disc authentication (spec/04 §3a). The drive's security mode page (0x3E) and the disc's security
// sector (READ DVD STRUCTURE 0xFF02FDFF, layer 0xFE) provide a challenge table; replaying it proves a pressed Xbox
// disc. Success marks the media as "title disc" (XBE media type 2) and pins the disc's title.

XBAPI void NTAPI XcRC4Key(PUCHAR, ULONG, const UCHAR *);
XBAPI void NTAPI XcRC4Crypt(PUCHAR, ULONG, PUCHAR);
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter();
extern UCHAR XeTitleDiscAuthenticated;
extern ULONG XeDiscTitleId;
constexpr NTSTATUS STATUS_TOO_MANY_SECRETS = (NTSTATUS)0xC0000156, STATUS_UNRECOGNIZED_MEDIA_ = (NTSTATUS)0xC0000014;

static NTSTATUS IdexCdRomPassThrough(const UCHAR *cdb, PVOID data, ULONG length, bool dataIn) {
    SCSI_PASS_THROUGH_DIRECT_ p = {};
    p.Length = sizeof(p);
    p.CdbLength = 12;
    p.DataIn = dataIn;
    p.DataTransferLength = length;
    p.DataBuffer = data;
    RtlCopyMemory(p.Cdb, cdb, 12);
    return IoSynchronousDeviceIoControlRequest(IOCTL_SCSI_PASS_THROUGH_DIRECT, IdexCdRomDeviceObject, &p, sizeof(p),
                                               nullptr, 0, nullptr, FALSE);
}

// MODE SENSE(10), the 28-byte security page; byte 11 must be 1.
static NTSTATUS IdexCdRomSecurityPage(PUCHAR page) {
    static const UCHAR cdb[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
    RtlZeroMemory(page, 28);
    NTSTATUS status = IdexCdRomPassThrough(cdb, page, 28, true);
    if (!NT_SUCCESS(status))
        return status == STATUS_TOO_MANY_SECRETS ? STATUS_UNRECOGNIZED_MEDIA_ : status;
    return page[11] == 1 ? STATUS_SUCCESS : STATUS_UNRECOGNIZED_MEDIA_;
}

// One challenge: MODE SELECT the page with the challenge, MODE SENSE it back, compare the response.
static NTSTATUS IdexCdRomChallenge(const UCHAR *sector, const UCHAR *entry, bool first, bool last) {
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
    NTSTATUS status = IdexCdRomPassThrough(select, page, 28, false);
    if (NT_SUCCESS(status)) {
        static const UCHAR sense[12] = {0x5A, 0, 0x3E, 0, 0, 0, 0, 0, 0x1C};
        status = IdexCdRomPassThrough(sense, page, 28, true);
    }
    if (NT_SUCCESS(status) && (!page[12] || RtlCompareMemory(page + 20, entry + 7, 4) != 4))
        status = STATUS_TOO_MANY_SECRETS;
    return status;
}

volatile ULONG IdexCdRomAuthStage, IdexCdRomAuthStatus; // diagnostics
volatile UCHAR IdexCdRomAuthPage[28];

static NTSTATUS IdexCdRomAuthenticate() {
    IdexCdRomAuthStage = 1;
    if (XeTitleDiscAuthenticated)
        return STATUS_SUCCESS;
    UCHAR page[28];
    NTSTATUS status = IdexCdRomSecurityPage(page);
    if (IdexCdRomSecurityMode == 2) // a drive without Xbox security (emulator): no disc to authenticate
        status = STATUS_UNRECOGNIZED_MEDIA_;
    for (int i = 0; i < 28; i++)
        IdexCdRomAuthPage[i] = page[i];
    IdexCdRomAuthStatus = status;
    if (!NT_SUCCESS(status))
        return status;
    IdexCdRomAuthStage = 2;
    auto sector = (PUCHAR)ExAllocatePoolWithTag(SECURITY_SECTOR_SIZE + 0x28, 'ScdI');
    if (!sector)
        return STATUS_INSUFFICIENT_RESOURCES;
    static const UCHAR read[12] = {0xAD, 0, 0xFF, 0x02, 0xFD, 0xFF, 0xFE, 0, 0x06, 0x64, 0, 0xC0};
    RtlZeroMemory(sector, SECURITY_SECTOR_SIZE);
    status = IdexCdRomPassThrough(read, sector, SECURITY_SECTOR_SIZE, true);
    IdexCdRomAuthStatus = status;
    if (NT_SUCCESS(status)) {
        IdexCdRomAuthStage = 3;
        status = STATUS_TOO_MANY_SECRETS;
        UCHAR sha[116], digest[20], rc4[258];
        XcSHAInit(sha);
        XcSHAUpdate(sha, sector + 4, 0x4CB);
        XcSHAFinal(sha, digest);
        // The digest must match; the reference also checks an RSA signature over it with Microsoft's public key,
        // which this open kernel does not carry (spec/04 §3a).
        if (page[13] == sector[4] && RtlCompareMemory(digest, sector + 0x4CF, 20) == 20) {
            XcSHAInit(sha);
            XcSHAUpdate(sha, sector + 0x4A3, 0x2C);
            XcSHAFinal(sha, digest);
            XcRC4Key(rc4, 7, digest);
            XcRC4Crypt(rc4, 0xFD, sector + 0x306);
            ULONG count = sector[0x305];
            IdexCdRomAuthStage = 4 | sector[0x304] << 8 | count << 16;
            if (sector[0x304] == 1 && count && count < 0x18) {
                ULONG start = KeQueryPerformanceCounter().LowPart % count, i = start, lastValid = 0xFFFFFFFF;
                bool first = true;
                status = STATUS_SUCCESS;
                do {
                    PUCHAR entry = sector + 0x306 + i * 11;
                    if (entry[0] == 1) {
                        status = IdexCdRomChallenge(sector, entry, first, false);
                        if (!NT_SUCCESS(status))
                            break;
                        first = false;
                        lastValid = i;
                    }
                    i = i + 1 == count ? 0 : i + 1;
                } while (i != start);
                if (NT_SUCCESS(status) && lastValid != 0xFFFFFFFF)
                    status = IdexCdRomChallenge(sector, sector + 0x306 + lastValid * 11, false, true);
                else if (NT_SUCCESS(status))
                    status = STATUS_TOO_MANY_SECRETS;
                IdexCdRomAuthStatus = status;
                if (NT_SUCCESS(status)) {
                    IdexCdRomAuthStage = 5;
                    XeTitleDiscAuthenticated = 1;
                    XeDiscTitleId = *(ULONG *)(sector + 0x4AB);
                }
            }
        }
    }
    ExFreePool(sector);
    return status;
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

// Reads in the drive log: code 2, the CDB holds the LBA and the remaining length.
static void IdexCdRomRecordRead(PIRP Irp, NTSTATUS Status) {
#if XK_DRIVE_LOG
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG v[3] = {s->Parameters.Read.ByteOffset.LowPart, s->Parameters.Read.Length, 0};
    IdexCdRomRecord(2, (const UCHAR *)v, nullptr, 0, 0, Status);
#endif
}

// A read retried 100 ms later (the drive was becoming ready).
static KTIMER IdexCdRomRetryTimer;
static KDPC IdexCdRomRetryDpc;
static void NTAPI IdexCdRomRetryRoutine(PKDPC, PVOID, PVOID, PVOID) {
    if (IdexChannelObject.CurrentIrp)
        IdexCdRomStartTransfer(IdexChannelObject.CurrentIrp);
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
        IDEX_RETRY retry = IdexCdRomSenseRetry();
        if ((status == STATUS_IO_DEVICE_ERROR || retry != IdexNoRetry) && ch->IoRetries++ < ch->MaximumIoRetries) {
            if (retry == IdexRetryLater) {
                LARGE_INTEGER due;
                due.QuadPart = -10000LL * 100;
                KeSetTimer(&IdexCdRomRetryTimer, due, &IdexCdRomRetryDpc);
            } else {
                IdexCdRomStartTransfer(irp);
            }
            return;
        }
        IdexCdRomRecordRead(irp, status);
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
        IdexCdRomRecordRead(Irp, IdexCdRomMapSense());
        IdexCdRomFinish(Irp, IdexCdRomMapSense(), 0);
        return;
    }
    IdexCdRomRecordRead(Irp, end > IdexCdRomLength ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS);
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
#if XK_DRIVE_LOG
    if (!KiQuickRebootFlag) { // the persisted log holds whatever RAM held at power-on
        RtlZeroMemory(IdexCdRomLog, sizeof(IdexCdRomLog));
        IdexCdRomLogCount = 0;
    }
#endif
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
    KeInitializeTimerEx(&IdexCdRomRetryTimer, NotificationTimer);
    KeInitializeDpc(&IdexCdRomRetryDpc, IdexCdRomRetryRoutine, nullptr);
}
