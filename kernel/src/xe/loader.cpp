// XBE loader and title launch (spec/05 §2-4).
#include <xk/io.h>
#include <xk/ps.h>
#include <xk/rtl.h>
#include <xk/serial.h>

XBAPI void NTAPI XcSHAInit(PUCHAR);
XBAPI void NTAPI XcSHAUpdate(PUCHAR, const UCHAR *, ULONG);
XBAPI void NTAPI XcSHAFinal(PUCHAR, PUCHAR);
XBAPI void NTAPI XcHMAC(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);
XBAPI NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                             ULONG AllocationType, ULONG Protect);
XBAPI NTSTATUS NTAPI NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS MiCommitImageMemory(PVOID *BaseAddress, PSIZE_T RegionSize); // committed as title image pages
XBAPI NTSTATUS NTAPI NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect,
                                            PULONG OldProtect);
extern UCHAR KiCertKey[16];

constexpr ULONG XBE_BASE = 0x10000, XBE_MAGIC = 0x48454258; // "XBEH"
constexpr ULONG XBE_SECTION_WRITABLE = 1, XBE_SECTION_PRELOAD = 2, XBE_SECTION_HEAD_RO = 0x10,
                XBE_SECTION_TAIL_RO = 0x20;
constexpr NTSTATUS STATUS_INVALID_IMAGE_FORMAT = (NTSTATUS)0xC000007B, STATUS_ORDINAL_NOT_FOUND = (NTSTATUS)0xC0000138,
                   STATUS_REGION_MISMATCH = (NTSTATUS)0xC0050001, STATUS_MEDIA_MISMATCH = (NTSTATUS)0xC0050002;

struct XBE_SECTION_HEADER {
    ULONG Flags, VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData, SectionName;
    ULONG SectionReferenceCount;
    USHORT *HeadSharedPageReferenceCount, *TailSharedPageReferenceCount;
    UCHAR SectionDigest[20];
};
static_assert(sizeof(XBE_SECTION_HEADER) == 0x38);

static ULONG XbeField(ULONG offset) { return *(ULONG *)(XBE_BASE + offset); }

BOOLEAN HalpSmcArmEvents(BOOLEAN Dashboard);
extern volatile ULONG HalpBootFlags;
extern "C" volatile UCHAR KiQuickRebootFlag;
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
#ifndef XK_RETAIL_MEDIA_POLICY
#define XK_RETAIL_MEDIA_POLICY 0
#endif
XK_PERSIST ULONG XeDiscTitleId; // the title pinned to the current game disc (0 = none)
XK_PERSIST UCHAR XeTitleDiscAuthenticated; // set by the DVD authentication (src/idex/cdrom.cpp)

// --- Exported state
XBAPI OBJECT_STRING XeImageFileName;
XBAPI XK_PERSIST PVOID LaunchDataPage;
XBAPI UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
XBAPI UCHAR XePublicKeyData[284];
OBJECT_STRING XeImageFileName;
PVOID LaunchDataPage;
UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
// No Microsoft key: an open BSAFE key with public exponent 1 (2048 bits, modulus unused), so a valid signature is the
// PKCS#1 block of the digest itself. The CD-ROM driver signs the disc security sectors titles read this way
// (spec/04 §3a); anything signed by Microsoft does not verify, as with no key at all (spec/05 §4.5).
UCHAR XePublicKeyData[284] = {'R', 'S', 'A', '1', 0x08, 0x01, 0, 0, 0, 0x08, 0, 0, 0xFF, 0, 0, 0, 1, 0, 0, 0};
XK_PERSIST ULONG XeConsoleGameRegion = 0x7FFFFFFF; // all regions until the EEPROM is read
volatile NTSTATUS XeLastLoadStatus, XeLoadHistory[4];
volatile ULONG XeLoadCount;
volatile ULONG XeLastMissingOrdinal, XeDigestMismatches;

static NTSTATUS XeReadFile(HANDLE h, PVOID buffer, ULONG length, ULONG offset, ULONG *got = nullptr) {
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER o;
    o.QuadPart = offset;
    NTSTATUS status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buffer, length, &o);
    if (got)
        *got = NT_SUCCESS(status) ? iosb.Information : 0;
    return status;
}

static NTSTATUS XeOpenImage(HANDLE *h) {
    OBJECT_ATTRIBUTES a = {nullptr, &XeImageFileName, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    return NtOpenFile(h, GENERIC_READ | SYNCHRONIZE, &a, &iosb, FILE_SHARE_READ,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

// --- Sections (refcounted; neighbouring sections share their head/tail pages)

static NTSTATUS XepLoadSection(HANDLE file, XBE_SECTION_HEADER *s) {
    PVOID base = (PVOID)s->VirtualAddress;
    SIZE_T size = s->VirtualSize;
    NTSTATUS status = MiCommitImageMemory(&base, &size);
    if (!NT_SUCCESS(status))
        return status;
    (*s->HeadSharedPageReferenceCount)++;
    (*s->TailSharedPageReferenceCount)++;
    if (s->SizeOfRawData)
        status = XeReadFile(file, (PVOID)s->VirtualAddress, s->SizeOfRawData, s->PointerToRawData);
    if (NT_SUCCESS(status)) {
        if (s->VirtualSize > s->SizeOfRawData)
            RtlZeroMemory((PUCHAR)s->VirtualAddress + s->SizeOfRawData, s->VirtualSize - s->SizeOfRawData);
        UCHAR ctx[116], digest[20];
        ULONG raw = s->SizeOfRawData;
        XcSHAInit(ctx);
        XcSHAUpdate(ctx, (PUCHAR)&raw, 4);
        XcSHAUpdate(ctx, (PUCHAR)s->VirtualAddress, raw);
        XcSHAFinal(ctx, digest);
        // Homebrew images carry no valid digests; like the header signature this is not enforced (policy, spec/05).
        for (int i = 0; i < 20; i++)
            if (digest[i] != s->SectionDigest[i]) {
                XeDigestMismatches = XeDigestMismatches + 1;
                break;
            }
    }
    if (!NT_SUCCESS(status)) {
        (*s->HeadSharedPageReferenceCount)--;
        (*s->TailSharedPageReferenceCount)--;
        return status;
    }
    s->SectionReferenceCount = 1;
    return STATUS_SUCCESS;
}

static void XepUnloadSection(XBE_SECTION_HEADER *s) {
    ULONG start = s->VirtualAddress, end = s->VirtualAddress + s->VirtualSize;
    if (--*s->HeadSharedPageReferenceCount)
        start = (start + 0x1000) & ~0xFFFu;
    if (--*s->TailSharedPageReferenceCount)
        end &= ~0xFFFu;
    if (start < end) {
        PVOID base = (PVOID)start;
        SIZE_T size = end - start;
        NtFreeVirtualMemory(&base, &size, MEM_DECOMMIT);
    }
}

XBAPI NTSTATUS NTAPI XeLoadSection(XBE_SECTION_HEADER *Section) {
    if (Section->SectionReferenceCount) {
        Section->SectionReferenceCount++;
        return STATUS_SUCCESS;
    }
    HANDLE h;
    NTSTATUS status = XeOpenImage(&h);
    if (NT_SUCCESS(status)) {
        status = XepLoadSection(h, Section);
        NtClose(h);
    }
    return status;
}

XBAPI NTSTATUS NTAPI XeUnloadSection(XBE_SECTION_HEADER *Section) {
    if (!Section->SectionReferenceCount)
        return STATUS_INVALID_PARAMETER;
    if (--Section->SectionReferenceCount == 0)
        XepUnloadSection(Section);
    return STATUS_SUCCESS;
}

#ifndef XK_TRACE_IMPORTS
#define XK_TRACE_IMPORTS 0
#endif
ULONG XkTraceWrap(USHORT ordinal, ULONG target);

// --- Kernel thunks: entries are ordinals (low 16 bits), resolved through our own export directory.
static NTSTATUS XepResolveKernelThunks(PULONG thunk) {
    auto image = (PUCHAR)0x80010000;
    PUCHAR nt = image + *(ULONG *)(image + 0x3C);
    ULONG exportRva = *(ULONG *)(nt + 0x78);
    if (!exportRva)
        return STATUS_ORDINAL_NOT_FOUND;
    PUCHAR exports = image + exportRva;
    ULONG ordinalBase = *(ULONG *)(exports + 0x10), count = *(ULONG *)(exports + 0x14);
    auto functions = (PULONG)(image + *(ULONG *)(exports + 0x1C));
    NTSTATUS status = STATUS_SUCCESS;
    for (; *thunk; thunk++) {
        ULONG index = (*thunk & 0xFFFF) - ordinalBase;
        if (index >= count || !functions[index]) {
            XeLastMissingOrdinal = *thunk & 0xFFFF;
            status = STATUS_ORDINAL_NOT_FOUND; // keep going so the list of missing ordinals is complete
            continue;
        }
        *thunk = (ULONG)image + functions[index];
#if XK_TRACE_IMPORTS
        *thunk = XkTraceWrap((USHORT)(index + ordinalBase), *thunk);
#endif
    }
    return status;
}

// Media of the launch device (spec/05 §4.10): 1 hard disk, CD 0xC, DVD variants otherwise.
static NTSTATUS XepMediaFlags(HANDLE file, ULONG *flags) {
    IO_STATUS_BLOCK iosb;
    ULONG dev[2];
    NTSTATUS status = NtQueryVolumeInformationFile(file, &iosb, dev, sizeof(dev), FileFsDeviceInformation);
    if (!NT_SUCCESS(status))
        return status;
    if (dev[0] != FILE_DEVICE_CD_ROM) {
        *flags = 1;
        return STATUS_SUCCESS;
    }
    if (XeTitleDiscAuthenticated) { // an authenticated Xbox game disc
        *flags = 2;
        return STATUS_SUCCESS;
    }
    ULONG in[6] = {}, out[6] = {};
    status = NtDeviceIoControlFile(file, nullptr, nullptr, nullptr, &iosb, 0x335140 /*IOCTL_DVD_READ_STRUCTURE*/,
                                   in, sizeof(in), out, sizeof(out));
    if (status == (NTSTATUS)0xC0000014) { // not a DVD
        *flags = 0xC;
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status))
        return status;
    UCHAR layer = ((PUCHAR)out)[6];
    *flags = (layer & 0x0F) == 1 ? ((layer & 0x60) ? 0x24 : 0x14) : ((layer & 0x60) ? 0x84 : 0x44);
    return STATUS_SUCCESS;
}

static void XepReleaseImage() {
    PVOID base = (PVOID)XBE_BASE;
    SIZE_T size = 0;
    NtFreeVirtualMemory(&base, &size, 0x8000 /*MEM_RELEASE*/);
}

static NTSTATUS XepLoadImage(PCSZ Path, BOOLEAN Dashboard) {
    if (XeImageFileName.Buffer)
        ExFreePool(XeImageFileName.Buffer);
    ULONG n = 0;
    while (Path[n])
        n++;
    XeImageFileName.Buffer = (PCHAR)ExAllocatePoolWithTag(n + 1, 'nFeX');
    if (!XeImageFileName.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(XeImageFileName.Buffer, Path, n + 1);
    XeImageFileName.Length = (USHORT)n;
    XeImageFileName.MaximumLength = (USHORT)(n + 1);

    HANDLE h;
    NTSTATUS status = XeOpenImage(&h);
    if (!NT_SUCCESS(status))
        return status;
    auto head = (PUCHAR)ExAllocatePoolWithTag(0x1000, 'iHeX');
    ULONG got = 0;
    if (!head) {
        NtClose(h);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = XeReadFile(h, head, 0x1000, 0, &got);
    ULONG sizeOfHeaders = *(ULONG *)(head + 0x108), sizeOfImage = *(ULONG *)(head + 0x10C);
    bool reserved = false;
    if (NT_SUCCESS(status) && (got < 0x178 || *(ULONG *)head != XBE_MAGIC || sizeOfHeaders < 0x179 ||
                               sizeOfImage < sizeOfHeaders || *(ULONG *)(head + 0x104) != XBE_BASE))
        status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(status)) {
        PVOID base = (PVOID)XBE_BASE;
        SIZE_T size = sizeOfImage;
        status = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
        reserved = NT_SUCCESS(status);
        size = sizeOfHeaders;
        if (reserved)
            status = MiCommitImageMemory(&base, &size);
    }
    if (NT_SUCCESS(status)) {
        RtlCopyMemory((PVOID)XBE_BASE, head, got < sizeOfHeaders ? got : sizeOfHeaders);
        if (sizeOfHeaders > got)
            status = XeReadFile(h, (PVOID)(XBE_BASE + got), sizeOfHeaders - got, got);
    }
    ExFreePool(head);
    ULONG cert = XbeField(0x118), sections = XbeField(0x120), count = XbeField(0x11C);
    if (NT_SUCCESS(status) && (cert < XBE_BASE || cert + 0x1D0 > XBE_BASE + sizeOfHeaders || sections < XBE_BASE ||
                               sections + count * 0x38 > XBE_BASE + sizeOfHeaders || XbeField(0x15C)))
        status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(status)) {
        // Entry point and thunk table are XOR-encoded: retail keys first, then the debug keys.
        ULONG entry = XbeField(0x128), thunk = XbeField(0x158);
        auto inside = [&](ULONG va) { return va >= XBE_BASE && va < XBE_BASE + sizeOfImage; };
        if (inside(entry ^ 0xA8FC57AB) && inside(thunk ^ 0x5B6D40B6))
            entry ^= 0xA8FC57AB, thunk ^= 0x5B6D40B6;
        else
            entry ^= 0x94859D4B, thunk ^= 0xEFB1F152;
        *(ULONG *)(XBE_BASE + 0x128) = entry;
        *(ULONG *)(XBE_BASE + 0x158) = thunk;
        if (!inside(entry) || !inside(thunk))
            status = STATUS_INVALID_IMAGE_FORMAT;
    }
    // Media/region policy: XK_RETAIL_MEDIA_POLICY=1 enforces both like a retail kernel; the default behaves like a
    // modded BIOS (which patches the stock kernel to allow any media and region), so discs that cannot be
    // authenticated (backups, XISOs in emulators) still start. Authenticated discs are pinned either way.
    ULONG region = XbeField(0x118) ? *(ULONG *)(cert + 0xA0) : 0;
    if (NT_SUCCESS(status) && !(region & XeConsoleGameRegion) && XK_RETAIL_MEDIA_POLICY)
        status = STATUS_REGION_MISMATCH;
    if (NT_SUCCESS(status) && !Dashboard) {
        ULONG media;
        status = XepMediaFlags(h, &media);
        if (NT_SUCCESS(status) && !(*(ULONG *)(cert + 0x9C) & media) && XK_RETAIL_MEDIA_POLICY)
            status = STATUS_MEDIA_MISMATCH;
        if (NT_SUCCESS(status) && media == 2) { // an authenticated game disc runs one title until it is changed
            ULONG titleId = *(ULONG *)(cert + 8);
            if (!XeDiscTitleId)
                XeDiscTitleId = titleId;
            else if (XeDiscTitleId != titleId)
                status = STATUS_MEDIA_MISMATCH;
        }
    }
    auto sec = (XBE_SECTION_HEADER *)sections;
    for (ULONG i = 0; NT_SUCCESS(status) && i < count; i++)
        if (sec[i].Flags & XBE_SECTION_PRELOAD)
            status = XepLoadSection(h, &sec[i]);
    // SMC events from now on; an eject handled during boot sends a disc title back (to the dashboard).
    if (NT_SUCCESS(status) && HalpSmcArmEvents(Dashboard) && !Dashboard)
        status = (NTSTATUS)0xC000003A;
    // SMC "reset on eject" (spec/05 §4.16): kept off (0x19 = 1, boot flag 8) for the dashboard, titles allowed on
    // non-secure media, boot flag 4 and manufacturing units; turned back on for everything else.
    if (NT_SUCCESS(status)) {
        bool keepOff = Dashboard || (*(ULONG *)(cert + 0x9C) & 0x80000000) || (HalpBootFlags & 4) ||
                       (XeConsoleGameRegion & 0x80000000);
        if (!keepOff && (!KiQuickRebootFlag || (HalpBootFlags & 8))) {
            HalpBootFlags &= ~8;
            while (!NT_SUCCESS(HalWriteSMBusValue(0x20, 0x19, FALSE, 0))) {
            }
        } else if (keepOff && !KiQuickRebootFlag && !(HalpBootFlags & 8)) {
            HalpBootFlags |= 8;
            while (!NT_SUCCESS(HalWriteSMBusValue(0x20, 0x19, FALSE, 1))) {
            }
        }
    }
    if (NT_SUCCESS(status))
        status = XepResolveKernelThunks((PULONG)XbeField(0x158));
    if (NT_SUCCESS(status)) {
        for (ULONG i = 0; i < count; i++) {
            if ((sec[i].Flags & (XBE_SECTION_PRELOAD | XBE_SECTION_WRITABLE)) != XBE_SECTION_PRELOAD)
                continue;
            ULONG start = sec[i].VirtualAddress, end = start + sec[i].VirtualSize;
            if (!(sec[i].Flags & XBE_SECTION_TAIL_RO))
                end &= ~0xFFFu;
            if (!(sec[i].Flags & XBE_SECTION_HEAD_RO))
                start = (start + 0x1000) & ~0xFFFu;
            if (start < end) {
                PVOID base = (PVOID)start;
                SIZE_T size = end - start;
                ULONG old;
                NtProtectVirtualMemory(&base, &size, PAGE_EXECUTE_READ, &old);
            }
        }
        // Per-title keys: HMAC(certificate key, title key).
        UCHAR mac[20];
        XcHMAC(KiCertKey, 16, (PUCHAR)cert + 0xB0, 16, nullptr, 0, mac);
        RtlCopyMemory(XboxLANKey, mac, 16);
        XcHMAC(KiCertKey, 16, (PUCHAR)cert + 0xC0, 16, nullptr, 0, mac);
        RtlCopyMemory(XboxSignatureKey, mac, 16);
        for (int i = 0; i < 16; i++) {
            XcHMAC(KiCertKey, 16, (PUCHAR)cert + 0xD0 + 16 * i, 16, nullptr, 0, mac);
            RtlCopyMemory(XboxAlternateSignatureKeys[i], mac, 16);
        }
        if (!(region & 0x80000000))
            RtlZeroMemory(XboxEEPROMKey, 16); // retail titles never see the EEPROM key
    }
    NtClose(h);
    if (!NT_SUCCESS(status) && reserved)
        XepReleaseImage();
    XeLastLoadStatus = status;
    XeLoadHistory[XeLoadCount & 3] = status; // diagnostics: the last four load results
    XeLoadCount = XeLoadCount + 1;
    return status;
}

NTSTATUS XeLoadImage(PCSZ Path, BOOLEAN Dashboard) {
    NTSTATUS status = XepLoadImage(Path, Dashboard);
    XK_SERIAL("load %s: %#lx\n", Path, status);
    return status;
}

// --- Launch (spec/05 §1-3, §5)

XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                               PULONG ResultLength);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue);
XBAPI NTSTATUS NTAPI IoDismountVolume(PDEVICE_OBJECT DeviceObject);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
XBAPI void NTAPI HalReturnToFirmware(ULONG Routine);
bool HalpCmosSignatureValid();
void IdexDiskCountPowerCycle();

volatile ULONG XeTitleLaunched; // 1 = disc title, 2 = dashboard, 3 = launched from the LaunchDataPage
volatile ULONG XeLaunchReason;  // the cold-boot pre-launch check result (§1)
[[noreturn]] void IdexFatalError(ULONG Code);
extern "C" volatile UCHAR KiQuickRebootFlag;

struct LAUNCH_DATA_PAGE {
    ULONG LaunchDataType;
    ULONG TitleId;
    CHAR LaunchPath[520];
    ULONG Flags;
    UCHAR Pad[492];
    ULONG DashboardReason; // +0x400
    ULONG Reserved;
    ULONG DashboardCode;   // +0x408
    UCHAR LaunchData[3060];
};
static_assert(sizeof(LAUNCH_DATA_PAGE) == 0x1000);

// §1: bit 0 the clock was never set (CMOS signature, see NtSetSystemTime) or is outside [2001, 2101);
// bits 1/2 the user settings are unreadable or lack a language (+0x08) / time zone (+0x30).
static ULONG XepCheckFirstBoot() {
    ULONG reason = 0;
    if (HalpCmosSignatureValid()) {
        LARGE_INTEGER now;
        KeQuerySystemTime(&now);
        if (now.QuadPart < 0x1C07385C89DC000LL /* 2001-01-01 */ || now.QuadPart >= 0x2309034F02AC000LL /* 2101-01-01 */)
            reason = 1;
    } else {
        reason = 1;
    }
    UCHAR settings[0x60];
    ULONG type, length;
    if (!NT_SUCCESS(ExQueryNonVolatileSetting(0xFF, &type, settings, sizeof(settings), &length)))
        return reason | 6;
    if (!*(ULONG *)(settings + 0x30))
        reason |= 4;
    if (!settings[0x08])
        reason |= 2;
    return reason;
}

#if XK_DRIVE_LOG
void IdexCdRomShowLog();
void HalpEndSplash();
#endif

static void XepLaunchDashboard() {
#if XK_DRIVE_LOG
    HalpEndSplash();
    IdexCdRomShowLog();
#endif
    if (NT_SUCCESS(XeLoadImage("\\Device\\Harddisk0\\Partition2\\xboxdash.xbe", TRUE)))
        return;
    if (!KiQuickRebootFlag) {
        auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
        ULONG code = 13;
        if (ldp && ldp->LaunchDataType == 1)
            code += ldp->DashboardReason;
        else if (XeTitleDiscAuthenticated)
            code = 20;
        IdexFatalError(code);
    }
    HalReturnToFirmware(4);
}

volatile ULONG XeDashboardLaunchData[3]; // diagnostics: {type, reason, code} as last written for the dashboard

static void XepLaunchDashboardWithReason(ULONG Reason, ULONG Code) {
    if (!LaunchDataPage)
        LaunchDataPage = MmAllocateContiguousMemory(0x1000);
    if (LaunchDataPage) {
        MmPersistContiguousMemory(LaunchDataPage, 0x1000, TRUE);
        auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
        RtlZeroMemory(ldp, 0x1000);
        ldp->LaunchDataType = 1;
        ldp->DashboardReason = Reason;
        ldp->DashboardCode = Code;
        XeDashboardLaunchData[0] = 1;
        XeDashboardLaunchData[1] = Reason;
        XeDashboardLaunchData[2] = Code;
    }
    XepLaunchDashboard();
}

static void XepFreeLaunchDataPage() {
    PVOID page = LaunchDataPage;
    LaunchDataPage = nullptr;
    MmFreeContiguousMemory(page);
}

static bool XepPlainDashboardError(NTSTATUS s) {
    return s == (NTSTATUS)0xC0000010 || s == (NTSTATUS)0xC0000013 || s == (NTSTATUS)0xC0000015 ||
           s == (NTSTATUS)0xC0000034 || s == (NTSTATUS)0xC000003A || s == (NTSTATUS)0xC00000A3 ||
           s == (NTSTATUS)0xC000014F;
}

// §2. Returns with XeTitleLaunched set once an image is loaded (or does not return).
static void XepLaunchTitle(ULONG Reason) {
    NTSTATUS status;
    auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
    if (!Reason && ldp && (ldp->Flags & 1)) // already tried once: do not loop
        XepFreeLaunchDataPage(), ldp = nullptr;
    if (!Reason && ldp) {
        // A. A title asked for another title.
        ldp->Flags |= 1;
        CHAR path[361];
        RtlCopyMemory(path, ldp->LaunchPath, 360);
        path[360] = 0;
        if (ldp->LaunchDataType == 0xFFFFFFFF)
            XepFreeLaunchDataPage();
        if (!path[0]) {
            XeTitleLaunched = 2;
            return XepLaunchDashboard();
        }
        // "\Device\...\dir;name.xbe": D: is the directory, the image is dir\name.xbe.
        OBJECT_STRING dir, link;
        RtlInitAnsiString(&link, "\\??\\D:");
        bool linked = false;
        for (ULONG i = 0; path[i]; i++)
            if (path[i] == ';') {
                path[i] = '\\';
                dir = {(USHORT)i, (USHORT)i, path};
                linked = NT_SUCCESS(IoCreateSymbolicLink(&link, &dir));
                break;
            }
        status = XeLoadImage(path, FALSE);
        if (NT_SUCCESS(status)) {
            XeTitleLaunched = 3;
            return;
        }
        if (linked)
            IoDeleteSymbolicLink(&link);
    } else {
        // B. The disc, else the dashboard.
        ULONG tray, before, after;
        HalReadSMCTrayState(&tray, &before);
        status = XeLoadImage("\\Device\\CdRom0\\default.xbe", FALSE);
        if (NT_SUCCESS(status)) {
            XeTitleLaunched = 1;
            return;
        }
        XeTitleLaunched = 2;
        if (Reason)
            return XepLaunchDashboardWithReason(3, Reason);
        HalReadSMCTrayState(&tray, &after);
        if (tray != 0x60)
            return XepLaunchDashboard();
        if (before != after)
            status = (NTSTATUS)0xC00000A3;
        if (XepPlainDashboardError(status))
            return XepLaunchDashboard();
    }
    XeTitleLaunched = 2;
    XepLaunchDashboardWithReason(1, status == STATUS_REGION_MISMATCH ? 3 : status == STATUS_MEDIA_MISMATCH ? 5 : 1);
}

// §5: keep the LaunchDataPage below the kernel, out of the title's way.
void HalpEndSplash();

static void XepPrepareToEnterTitle() {
    HalpEndSplash();
    if (!LaunchDataPage)
        return;
    ULONG kernelPhys = MmGetPhysicalAddress((PVOID)0x80010000);
    PVOID page = MmAllocateContiguousMemoryEx(0x1000, 0, kernelPhys - 1, 0, PAGE_READWRITE);
    if (!page)
        return;
    RtlCopyMemory(page, LaunchDataPage, 0x1000);
    MmPersistContiguousMemory(page, 0x1000, TRUE);
    PVOID old = LaunchDataPage;
    LaunchDataPage = page;
    MmFreeContiguousMemory(old);
}

// Boot flag 0x40 (the tray was opened during boot, spec/05 §3): wait while the tray is closing or settling, then
// the disc if one is in, otherwise the dashboard.
static void XepWaitForTray() {
    ULONG state;
    for (;;) {
        if (!NT_SUCCESS(HalReadSMBusValue(0x20, 0x03, FALSE, &state))) {
            state = 0x10;
            break;
        }
        state &= 0x70;
        if (state != 0x50 && state != 0)
            break;
        LARGE_INTEGER t;
        t.QuadPart = -250 * 10000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    if (state == 0x60) {
        extern PDEVICE_OBJECT IdexCdRomDeviceObject;
        IoDismountVolume(IdexCdRomDeviceObject);
        XepLaunchTitle(0);
    } else {
        XeTitleLaunched = 2;
        XepLaunchDashboard();
    }
}

// spec/03 §4: cold boot runs the pre-launch checks and honours the SMC boot flags; a quick reboot just launches.
static void XepChooseTitle() {
    if (KiQuickRebootFlag || (HalpBootFlags & 4) || (XeConsoleGameRegion & 0x80000000))
        return XepLaunchTitle(0);
    XeLaunchReason = XepCheckFirstBoot();
    if (XeLaunchReason)
        return XepLaunchTitle(XeLaunchReason);
    XeTitleLaunched = 2;
    if (HalpBootFlags & 0x80)
        return XepLaunchDashboardWithReason(5, 0);
    if (HalpBootFlags & 0x40)
        return XepWaitForTray();
    if (HalpBootFlags & 0x08)
        return XepLaunchDashboard();
    XepLaunchTitle(0);
}

static void NTAPI XepTitleThread(PVOID, PVOID) {
#if XK_BOOT_SELFTEST
    extern volatile ULONG KiTestDvdDone;
    while (!KiQuickRebootFlag && !KiTestDvdDone) {
        LARGE_INTEGER t;
        t.QuadPart = -20 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
#endif
    if (!KiQuickRebootFlag) {
        if (HalpBootFlags & 0x20) // the previous boot ended in a fatal error (scratch 2)
            IdexFatalError(21);
        IdexDiskCountPowerCycle();
    }
    XepChooseTitle();
    XepPrepareToEnterTitle();
    ((void(__cdecl *)())XbeField(0x128))();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void XeStartTitleLauncher() {
    HANDLE h;
    if (NT_SUCCESS(PsCreateSystemThreadEx(&h, 0, 0x3000, 0, nullptr, nullptr, nullptr, FALSE, FALSE,
                                          XepTitleThread)))
        NtClose(h);
}
