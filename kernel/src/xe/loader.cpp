// XBE loader and title launch (spec/05 §2-4).
#include <xk/io.h>
#include <xk/ps.h>
#include <xk/rtl.h>

XBAPI void NTAPI XcSHAInit(PUCHAR);
XBAPI void NTAPI XcSHAUpdate(PUCHAR, const UCHAR *, ULONG);
XBAPI void NTAPI XcSHAFinal(PUCHAR, PUCHAR);
XBAPI void NTAPI XcHMAC(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);
XBAPI NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits, PSIZE_T RegionSize,
                                             ULONG AllocationType, ULONG Protect);
XBAPI NTSTATUS NTAPI NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
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

// --- Exported state
XBAPI OBJECT_STRING XeImageFileName;
XBAPI PVOID LaunchDataPage;
XBAPI UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
XBAPI UCHAR XePublicKeyData[284]; // not populated (signature checks are not enforced, spec/05 §4.5)
OBJECT_STRING XeImageFileName;
PVOID LaunchDataPage;
UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
UCHAR XePublicKeyData[284];
ULONG XeConsoleGameRegion = 0x7FFFFFFF; // all regions until the EEPROM is read
volatile NTSTATUS XeLastLoadStatus;
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
    NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &size, MEM_COMMIT, PAGE_READWRITE);
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

NTSTATUS XeLoadImage(PCSZ Path, BOOLEAN Dashboard) {
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
            status = NtAllocateVirtualMemory(&base, 0, &size, MEM_COMMIT, PAGE_READWRITE);
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
    ULONG region = XbeField(0x118) ? *(ULONG *)(cert + 0xA0) : 0;
    if (NT_SUCCESS(status) && !(region & XeConsoleGameRegion))
        status = STATUS_REGION_MISMATCH;
    if (NT_SUCCESS(status) && !Dashboard) {
        ULONG media;
        status = XepMediaFlags(h, &media);
        if (NT_SUCCESS(status) && !(*(ULONG *)(cert + 0x9C) & media))
            status = STATUS_MEDIA_MISMATCH;
    }
    auto sec = (XBE_SECTION_HEADER *)sections;
    for (ULONG i = 0; NT_SUCCESS(status) && i < count; i++)
        if (sec[i].Flags & XBE_SECTION_PRELOAD)
            status = XepLoadSection(h, &sec[i]);
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
    return status;
}

// --- Launch (spec/05 §2-3, without the SMC tray handling and the LaunchDataPage paths)

volatile ULONG XeTitleLaunched; // 1 = title entered, 2 = dashboard entered
[[noreturn]] void IdexFatalError(ULONG Code);

static void NTAPI XepTitleThread(PVOID, PVOID) {
#if XK_BOOT_SELFTEST
    extern volatile ULONG KiTestDvdDone;
    while (!KiTestDvdDone) {
        LARGE_INTEGER t;
        t.QuadPart = -20 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
#endif
    ULONG launched = 1;
    NTSTATUS status = XeLoadImage("\\Device\\CdRom0\\default.xbe", FALSE);
    if (!NT_SUCCESS(status)) {
        launched = 2;
        status = XeLoadImage("\\Device\\Harddisk0\\Partition2\\xboxdash.xbe", TRUE);
    }
    if (!NT_SUCCESS(status))
        IdexFatalError(13);
    XeTitleLaunched = launched;
    ((void(__cdecl *)())XbeField(0x128))();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void XeStartTitleLauncher() {
    HANDLE h;
    if (NT_SUCCESS(PsCreateSystemThreadEx(&h, 0, 0x10000, 0, nullptr, nullptr, nullptr, FALSE, FALSE,
                                          XepTitleThread)))
        NtClose(h);
}
