// XBE loader and title launch (spec/stock/05 §2-4).
#include <titles/titles.h>
#include <toasts/toasts.h>
#include <diagnostics/diagnostics.h>
#include <user_interface/user_interface.h>
#include <io/io.h>
#include <extensions/extensions.h>
#include <kernel_modules/kernel_modules.h>
#include <ini_file/launch_redirect_ini.h>
#include <ini_file/title_patcher_ini.h>
#include <core/core.h>
#include <runtime/runtime.h>
#include <titles/title_start_call.h>
#include <hardware/chipset/chipset.h>
#include <storage/storage.h>
#include <memory/memory.h>
#include <crypto/crypto.h>
#include <boot_splash/boot_splash.h>
#include <filesystems/filesystems.h>
#include <gamepad/gamepad.h>
#include <webserver/webserver.h>
#if XK_BOOT_SELFTEST
#include <kernel_self_tests.h> // tests/kernel_self_tests (SELFTEST=1 builds only)
#endif

// The exported launch state (titles see these names): outside the namespace.
OBJECT_STRING XeImageFileName;
XK_PERSIST PVOID LaunchDataPage;

namespace xk::titles {

constexpr ULONG XBE_BASE = 0x10000, XBE_MAGIC = 0x48454258; // "XBEH"
// The XOR keys of the header's entry point (+0x128) and kernel thunk table address (+0x158): retail and debug XBEs.
constexpr ULONG XBE_ENTRY_KEY_RETAIL = 0xA8FC57AB, XBE_THUNK_KEY_RETAIL = 0x5B6D40B6;
constexpr ULONG XBE_ENTRY_KEY_DEBUG = 0x94859D4B, XBE_THUNK_KEY_DEBUG = 0xEFB1F152;
constexpr ULONG XBE_SECTION_WRITABLE = 1, XBE_SECTION_PRELOAD = 2, XBE_SECTION_HEAD_RO = 0x10, XBE_SECTION_TAIL_RO = 0x20;
constexpr ULONG XBE_INIT_LIMIT_64_MB = 4; // header +0x124 (init flags): the title keeps to 64 MB on a 128 MB console
constexpr NTSTATUS STATUS_INVALID_IMAGE_FORMAT = (NTSTATUS)0xC000007B, STATUS_ORDINAL_NOT_FOUND = (NTSTATUS)0xC0000138,
                   STATUS_REGION_MISMATCH = (NTSTATUS)0xC0050001, STATUS_MEDIA_MISMATCH = (NTSTATUS)0xC0050002;
constexpr NTSTATUS STATUS_REPARSE = 0x00000104; // LoadImage: an extension starts another XBE in its place


static ULONG XbeField(ULONG offset) {
    return *(ULONG *)(XBE_BASE + offset);
}

#ifndef XK_RETAIL_MEDIA_POLICY
#define XK_RETAIL_MEDIA_POLICY 0
#endif
XK_PERSIST ULONG DiscTitleId;            // the title pinned to the current game disc (0 = none)
XK_PERSIST UCHAR TitleDiscAuthenticated; // set by the DVD authentication (src/hardware/ide/dvd_drive.cpp)

// --- Exported state
XBAPI UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
XBAPI UCHAR XePublicKeyData[284];
UCHAR XboxSignatureKey[16], XboxLANKey[16], XboxAlternateSignatureKeys[16][16];
// No Microsoft key: an open BSAFE key with public exponent 1 (2048 bits, modulus unused), so a valid signature is the PKCS#1 block of the digest itself. The
// CD-ROM driver signs the disc security sectors titles read this way (spec/stock/04 §3a); anything signed by Microsoft does not verify, as with no key at all
// (spec/stock/05 §4.5).
UCHAR XePublicKeyData[284] = {'R', 'S', 'A', '1', 0x08, 0x01, 0, 0, 0, 0x08, 0, 0, 0xFF, 0, 0, 0, 1, 0, 0, 0};

XK_PERSIST ULONG ConsoleGameRegion = 0x7FFFFFFF; // all regions until the EEPROM is read

// Load diagnostics: the last load's status and the last four, an import missing from the kernel, digest mismatches.
volatile NTSTATUS LastLoadStatus, LoadHistory[4];
volatile ULONG LoadCount;
volatile ULONG LastMissingOrdinal, DigestMismatches;

static NTSTATUS ReadWholeFile(HANDLE h, PVOID buffer, ULONG length, ULONG offset, ULONG *got = nullptr) {
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER o;
    o.QuadPart = offset;
    NTSTATUS status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buffer, length, &o);
    if (got)
        *got = NT_SUCCESS(status) ? iosb.Information : 0;
    return status;
}

static NTSTATUS OpenImage(HANDLE *h) {
    OBJECT_ATTRIBUTES a = {nullptr, &XeImageFileName, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    return NtOpenFile(h, GENERIC_READ | SYNCHRONIZE, &a, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

// --- Kernel files in C:\OXOS (oxos_settings.ini, oxos_title_patches.ini): C: on a FATX disk, /C/OXOS on an
// exFAT disk

// A whole file of at most MaxSize bytes, in pool memory the caller frees, with its length in *Got. Null when it does not open (*Status says why:
// STATUS_OBJECT_NAME_NOT_FOUND when it does not exist), is empty or larger, or cannot be read.
char *ReadOXOSFile(PCSZ Path, ULONG MaxSize, ULONG Tag, ULONG *Got, NTSTATUS *Status) {
    *Got = 0;
    HANDLE h;
    NTSTATUS status = io::OpenFile(Path, GENERIC_READ, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE, &h);
    *Status = status;
    if (!NT_SUCCESS(status))
        return nullptr;
    IO_STATUS_BLOCK iosb;
    ULONG info[6] = {}; // FILE_STANDARD_INFORMATION: EndOfFile at +8
    status = NtQueryInformationFile(h, &iosb, info, sizeof(info), FileStandardInformation);
    ULONG size = info[2];
    if (NT_SUCCESS(status) && (info[3] || size > MaxSize))
        status = STATUS_INSUFFICIENT_RESOURCES;
    auto text = NT_SUCCESS(status) && size ? (char *)ExAllocatePoolWithTag(size, Tag) : nullptr;
    if (text)
        status = ReadWholeFile(h, text, size, 0, Got);
    NtClose(h);
    *Status = status;
    if (text && !NT_SUCCESS(status)) {
        ExFreePool(text);
        return nullptr;
    }
    return text;
}

// --- Title patches from C:\OXOS\oxos_title_patches.ini (spec/oxos_internal/25 §3): read once per title load, written into
// each section whenever it is read from the image (at load for preloaded sections, later through XeLoadSection).

static PUCHAR PatchRecords; // this title's records (the title patcher module), or null
static ULONG PatchFileSize;

// This title's records, built by the title patcher module (kernel/modules/title_patcher.cpp); without the kernel modules, none.
static void LoadPatches(ULONG TitleId, ULONG Version, const XBE_SECTION_HEADER *Sections, ULONG Count) {
    if (PatchRecords) {
        ExFreePool(PatchRecords);
        PatchRecords = nullptr;
        PatchFileSize = 0;
    }
    if (kernel_modules::Modules)
        kernel_modules::Modules->BuildTitlePatches(TitleId, Version, Sections, Count, &PatchRecords, &PatchFileSize);
}

// A patch is written only where its current bytes are: so never into another build of the title, nor into a launcher that carries the title's identity
// (spec/oxos_internal/25 §3.2). The patches checked against a section's `match =` bytes go first: another patch of the section may change those bytes (a
// section's first patch can sit at its match address).
static void ApplyPatches(const XBE_SECTION_HEADER *s) {
    for (ULONG pass = 0; pass < 2; pass++) {
        for (ULONG at = 0; at < PatchFileSize;) {
            auto r = (INI_PATCH_RECORD *)(PatchRecords + at);
            at += oxos::ini_file::PatchRecordSize(r);
            if (r->Kind != INI_PATCH_BYTES || !r->Check != (pass == 1) || r->Address < s->VirtualAddress ||
                r->Address - s->VirtualAddress + oxos::ini_file::PatchSpan(r) > s->VirtualSize)
                continue;
            auto target = (PUCHAR)r->Address;
            auto check = (PUCHAR)(r->Check ? r->Check : r->Address);
            // Already there first: a patch checked against another place must not be written twice (its match bytes may still be there when an earlier patch of
            // the section has not changed them).
            if (memcmp(target, oxos::ini_file::PatchData(r), r->Length) == 0) {
                XK_SERIAL("patches: %#lx: already there\n", r->Address);
            } else if (memcmp(check, oxos::ini_file::PatchOriginal(r), r->OriginalLength) == 0) {
                memcpy(target, oxos::ini_file::PatchData(r), r->Length);
                XK_SERIAL("patches: %#lx: %u bytes written\n", r->Address, r->Length);
            } else {
                XK_SERIAL("patches: %#lx: the %s bytes differ: not written\n", r->Address, r->Check ? "match" : "current");
            }
        }
    }
}

// --- Sections (refcounted; neighbouring sections share their head/tail pages)

static NTSTATUS LoadXbeSection(HANDLE file, XBE_SECTION_HEADER *s) {
    PVOID base = (PVOID)s->VirtualAddress;
    SIZE_T size = s->VirtualSize;
    NTSTATUS status = memory::CommitImageMemory(&base, &size);
    if (!NT_SUCCESS(status))
        return status;
    (*s->HeadSharedPageReferenceCount)++;
    (*s->TailSharedPageReferenceCount)++;
    if (s->SizeOfRawData)
        status = ReadWholeFile(file, (PVOID)s->VirtualAddress, s->SizeOfRawData, s->PointerToRawData);
    if (NT_SUCCESS(status)) {
        if (s->VirtualSize > s->SizeOfRawData)
            memset((PUCHAR)s->VirtualAddress + s->SizeOfRawData, 0, s->VirtualSize - s->SizeOfRawData);
        UCHAR ctx[116], digest[20];
        ULONG raw = s->SizeOfRawData;
        XcSHAInit(ctx);
        XcSHAUpdate(ctx, (PUCHAR)&raw, 4);
        XcSHAUpdate(ctx, (PUCHAR)s->VirtualAddress, raw);
        XcSHAFinal(ctx, digest);
        // Homebrew images carry no valid digests; like the header signature this is not enforced (policy, spec/stock/05).
        for (int i = 0; i < 20; i++) {
            if (digest[i] != s->SectionDigest[i]) {
                DigestMismatches = DigestMismatches + 1;
                break;
            }
        }
        ApplyPatches(s); // after the digest, which is of the image as it is on disc
    }
    if (!NT_SUCCESS(status)) {
        (*s->HeadSharedPageReferenceCount)--;
        (*s->TailSharedPageReferenceCount)--;
        return status;
    }
    s->SectionReferenceCount = 1;
    return STATUS_SUCCESS;
}

static void UnloadXbeSection(XBE_SECTION_HEADER *s) {
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
    NTSTATUS status = OpenImage(&h);
    if (NT_SUCCESS(status)) {
        status = LoadXbeSection(h, Section);
        NtClose(h);
    }
    return status;
}

XBAPI NTSTATUS NTAPI XeUnloadSection(XBE_SECTION_HEADER *Section) {
    if (!Section->SectionReferenceCount)
        return STATUS_INVALID_PARAMETER;
    if (--Section->SectionReferenceCount == 0)
        UnloadXbeSection(Section);
    return STATUS_SUCCESS;
}

#ifndef XK_TRACE_IMPORTS
#define XK_TRACE_IMPORTS 0
#endif

// --- Kernel thunks: entries are ordinals (low 16 bits), resolved through our own export directory.

ULONG KernelExportAddress(ULONG Ordinal) {
    auto image = (PUCHAR)0x80010000;
    PUCHAR nt = image + *(ULONG *)(image + 0x3C);
    ULONG exportRva = *(ULONG *)(nt + 0x78);
    if (!exportRva)
        return 0;
    PUCHAR exports = image + exportRva;
    ULONG index = Ordinal - *(ULONG *)(exports + 0x10);
    auto functions = (PULONG)(image + *(ULONG *)(exports + 0x1C));
    if (index >= *(ULONG *)(exports + 0x14) || !functions[index])
        return 0;
    return (ULONG)image + functions[index];
}

static NTSTATUS ResolveKernelThunks(PULONG thunk) {
    NTSTATUS status = STATUS_SUCCESS;
    for (; *thunk; thunk++) {
        ULONG ordinal = *thunk & 0xFFFF;
        ULONG target = KernelExportAddress(ordinal);
        if (!target) {
            LastMissingOrdinal = ordinal;
            status = STATUS_ORDINAL_NOT_FOUND; // keep going so the list of missing ordinals is complete
            continue;
        }
        // An extension's hook in the export's place (spec/oxos_internal/34 §7.1).
        *thunk = extensions::ResolveTitleImport(ordinal, target);
#if XK_TRACE_IMPORTS
        *thunk = XkTraceWrap((USHORT)ordinal, *thunk);
#endif
    }
    return status;
}

// --- Loading an image (spec/stock/05 §4)

// Media of the launch device (spec/stock/05 §4.10): 1 hard disk, CD 0xC, DVD variants otherwise.
static NTSTATUS MediaFlags(HANDLE file, ULONG *flags) {
    IO_STATUS_BLOCK iosb;
    ULONG dev[2];
    NTSTATUS status = NtQueryVolumeInformationFile(file, &iosb, dev, sizeof(dev), FileFsDeviceInformation);
    if (!NT_SUCCESS(status))
        return status;
    if (dev[0] != FILE_DEVICE_CD_ROM) {
        *flags = 1;
        return STATUS_SUCCESS;
    }
    // An authenticated Xbox game disc, or a disc image.
    if (TitleDiscAuthenticated || storage::IsDvdImageMounted()) {
        *flags = 2;
        return STATUS_SUCCESS;
    }
    ULONG in[6] = {}, out[6] = {};
    status = NtDeviceIoControlFile(file, nullptr, nullptr, nullptr, &iosb, 0x335140 /*IOCTL_DVD_READ_STRUCTURE*/, in, sizeof(in), out, sizeof(out));
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

static void ReleaseImage() {
    PVOID base = (PVOID)XBE_BASE;
    SIZE_T size = 0;
    NtFreeVirtualMemory(&base, &size, 0x8000 /*MEM_RELEASE*/);
}

static NTSTATUS LoadImageFile(PCSZ Path, BOOLEAN Dashboard) {
    if (XeImageFileName.Buffer)
        ExFreePool(XeImageFileName.Buffer);
    ULONG n = 0;
    while (Path[n])
        n++;
    XeImageFileName.Buffer = (PCHAR)ExAllocatePoolWithTag(n + 1, 'nFeX');
    if (!XeImageFileName.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    memcpy(XeImageFileName.Buffer, Path, n + 1);
    XeImageFileName.Length = (USHORT)n;
    XeImageFileName.MaximumLength = (USHORT)(n + 1);

    HANDLE h;
    NTSTATUS status = OpenImage(&h);
    XK_SERIAL("load: opened %#lx\n", status);
    if (!NT_SUCCESS(status))
        return status;
    auto head = (PUCHAR)ExAllocatePoolWithTag(0x1000, 'iHeX');
    ULONG got = 0;
    if (!head) {
        NtClose(h);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = ReadWholeFile(h, head, 0x1000, 0, &got);
    XK_SERIAL("load: first page read %#lx (%lu bytes)\n", status, got);
    ULONG sizeOfHeaders = *(ULONG *)(head + 0x108), sizeOfImage = *(ULONG *)(head + 0x10C);
    bool reserved = false;
    if (NT_SUCCESS(status) && (got < 0x178 || *(ULONG *)head != XBE_MAGIC || sizeOfHeaders < 0x179 || sizeOfImage < sizeOfHeaders ||
                               *(ULONG *)(head + 0x104) != XBE_BASE))
        status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(status)) {
        PVOID base = (PVOID)XBE_BASE;
        SIZE_T size = sizeOfImage;
        status = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
        XK_SERIAL("load: image reserved %#lx\n", status);
        reserved = NT_SUCCESS(status);
        size = sizeOfHeaders;
        if (reserved)
            status = memory::CommitImageMemory(&base, &size);
        XK_SERIAL("load: headers committed %#lx\n", status);
    }
    if (NT_SUCCESS(status)) {
        memcpy((PVOID)XBE_BASE, head, got < sizeOfHeaders ? got : sizeOfHeaders);
        XK_MARK("mark: load: first page copied");
        if (sizeOfHeaders > got)
            status = ReadWholeFile(h, (PVOID)(XBE_BASE + got), sizeOfHeaders - got, got);
    }
    ExFreePool(head);
    XK_SERIAL("load: headers %#lx\n", status);
    // The header fields only once the headers are in memory: a file that failed the first-page checks reserved nothing.
    bool loaded = NT_SUCCESS(status);
    ULONG cert = loaded ? XbeField(0x118) : 0, sections = loaded ? XbeField(0x120) : 0, count = loaded ? XbeField(0x11C) : 0;
    if (NT_SUCCESS(status) && (cert < XBE_BASE || cert + 0x1D0 > XBE_BASE + sizeOfHeaders || sections < XBE_BASE ||
                               sections + count * 0x38 > XBE_BASE + sizeOfHeaders || XbeField(0x15C)))
        status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(status)) {
        // Entry point and thunk table are XOR-encoded: the retail keys when they give an entry point and thunk table inside the image, else the debug keys. A
        // thunk table at 0 is an XBE with no kernel imports for the loader (packed XBEs such as EvolutionX's dashboard resolve their own), and is left
        // unresolved.
        ULONG entry = XbeField(0x128), thunk = XbeField(0x158);
        auto inside = [&](ULONG va) {
            return va >= XBE_BASE && va < XBE_BASE + sizeOfImage;
        };
        auto usable = [&](ULONG e, ULONG t) {
            return inside(e) && (!t || inside(t));
        };
        if (usable(entry ^ XBE_ENTRY_KEY_RETAIL, thunk ^ XBE_THUNK_KEY_RETAIL)) {
            entry ^= XBE_ENTRY_KEY_RETAIL;
            thunk ^= XBE_THUNK_KEY_RETAIL;
        } else {
            entry ^= XBE_ENTRY_KEY_DEBUG;
            thunk ^= XBE_THUNK_KEY_DEBUG;
        }
        *(ULONG *)(XBE_BASE + 0x128) = entry;
        *(ULONG *)(XBE_BASE + 0x158) = thunk;
        if (!usable(entry, thunk))
            status = STATUS_INVALID_IMAGE_FORMAT;
    }
    // Media/region policy: XK_RETAIL_MEDIA_POLICY=1 enforces both like a retail kernel; the default behaves like a modded BIOS (which patches the stock kernel
    // to allow any media and region), so discs that cannot be authenticated (backups, XISOs in emulators) still start. Authenticated discs are pinned either
    // way.
    ULONG region = NT_SUCCESS(status) ? *(ULONG *)(cert + 0xA0) : 0;
    if (NT_SUCCESS(status) && !(region & ConsoleGameRegion) && XK_RETAIL_MEDIA_POLICY)
        status = STATUS_REGION_MISMATCH;
    ULONG media = 0; // 1 = the hard disk (MediaFlags); only checked for titles
    if (NT_SUCCESS(status) && !Dashboard) {
        status = MediaFlags(h, &media);
        if (NT_SUCCESS(status) && !(*(ULONG *)(cert + 0x9C) & media) && XK_RETAIL_MEDIA_POLICY)
            status = STATUS_MEDIA_MISMATCH;
        if (NT_SUCCESS(status) && media == 2) { // an authenticated game disc runs one title until it is changed
            ULONG titleId = *(ULONG *)(cert + 8);
            if (!DiscTitleId)
                DiscTitleId = titleId;
            else if (DiscTitleId != titleId)
                status = STATUS_MEDIA_MISMATCH;
        }
    }
    auto sec = (XBE_SECTION_HEADER *)sections;
    if (NT_SUCCESS(status))
        LoadPatches(*(ULONG *)(cert + 8), *(ULONG *)(cert + 0xAC), sec, count);
    for (ULONG i = 0; NT_SUCCESS(status) && i < count; i++) {
        if (sec[i].Flags & XBE_SECTION_PRELOAD) {
            XK_SERIAL("load: section %lu\n", i);
            status = LoadXbeSection(h, &sec[i]);
        }
    }
    XK_SERIAL("load: sections %#lx\n", status);
    // SMC events from now on; an eject handled during boot sends a disc title back (to the dashboard), as the reference does (spec/stock/05 §4.16). A title on
    // the hard disk still starts: a console without a DVD drive reports an eject at every boot.
    if (NT_SUCCESS(status) && hardware::chipset::ArmSmcEvents(Dashboard) && !Dashboard && media != 1)
        status = (NTSTATUS)0xC000003A;
    XK_SERIAL("load: SMC events armed\n");
    // SMC "reset on eject" (spec/stock/05 §4.16): kept off (0x19 = 1, boot flag 8) for the dashboard, titles allowed on non-secure media, boot flag 4 and
    // manufacturing units; turned back on for everything else.
    if (NT_SUCCESS(status)) {
        bool keepOff = Dashboard || (*(ULONG *)(cert + 0x9C) & 0x80000000) || (hardware::chipset::BootFlags & 4) || (ConsoleGameRegion & 0x80000000);
        if (!keepOff && (!KiQuickRebootFlag || (hardware::chipset::BootFlags & 8))) {
            hardware::chipset::BootFlags &= ~8;
            while (!NT_SUCCESS(HalWriteSMBusValue(0x20, 0x19, FALSE, 0))) {
                // Retry until the SMC accepts the write.
            }
        } else if (keepOff && !KiQuickRebootFlag && !(hardware::chipset::BootFlags & 8)) {
            hardware::chipset::BootFlags |= 8;
            while (!NT_SUCCESS(HalWriteSMBusValue(0x20, 0x19, FALSE, 1))) {
                // Retry until the SMC accepts the write.
            }
        }
        XK_SERIAL("load: reset on eject set\n");
    }
    if (NT_SUCCESS(status) && XbeField(0x158)) // 0: no kernel thunk table
        status = ResolveKernelThunks((PULONG)XbeField(0x158));
    XK_SERIAL("load: thunks %#lx\n", status);
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
        XcHMAC(core::CertKey, 16, (PUCHAR)cert + 0xB0, 16, nullptr, 0, mac);
        memcpy(XboxLANKey, mac, 16);
        XcHMAC(core::CertKey, 16, (PUCHAR)cert + 0xC0, 16, nullptr, 0, mac);
        memcpy(XboxSignatureKey, mac, 16);
        for (int i = 0; i < 16; i++) {
            XcHMAC(core::CertKey, 16, (PUCHAR)cert + 0xD0 + 16 * i, 16, nullptr, 0, mac);
            memcpy(XboxAlternateSignatureKeys[i], mac, 16);
        }
        if (!(region & 0x80000000))
            memset(XboxEEPROMKey, 0, 16); // retail titles never see the EEPROM key
        // A 128 MB console's upper half, unless the XBE asks for the 64 MB limit (spec/stock/09 §10).
        if (!(XbeField(0x124) & XBE_INIT_LIMIT_64_MB))
            memory::AllowUpperMemory();
    }
    NtClose(h);
    if (!NT_SUCCESS(status) && reserved)
        ReleaseImage();
    LastLoadStatus = status;
    LoadHistory[LoadCount & 3] = status; // diagnostics: the last four load results
    LoadCount = LoadCount + 1;
    return status;
}

// Path's title-loading event (spec/oxos_internal/34 §7.2), then Path. Target (INI_REDIRECT_MAX_PATH + 1 bytes), for a launch an extension may redirect: when
// one names an XBE there (oxos_title_redirect), Path is not loaded and the result is STATUS_REPARSE.
NTSTATUS LoadImage(PCSZ Path, BOOLEAN Dashboard, char *Target) {
    XK_SERIAL("load %s: start\n", Path);
    if (Target)
        Target[0] = 0;
    extensions::TitleLoading(Path, Dashboard, Target, INI_REDIRECT_MAX_PATH + 1);
    if (Target && Target[0]) {
        XK_SERIAL("load %s: an extension starts %s in its place\n", Path, Target);
        return STATUS_REPARSE;
    }
    NTSTATUS status = LoadImageFile(Path, Dashboard);
    XK_SERIAL("load %s: %#lx\n", Path, status);
    return status;
}

// --- Launch (spec/stock/05 §1-3, §5)

volatile ULONG TitleLaunched; // 1 = disc title, 2 = dashboard, 3 = from the LaunchDataPage, 4 = from the File Manager, 5 = the boot video player
volatile ULONG LaunchReason;           // the cold-boot pre-launch check result (§1)
volatile ULONG DashboardLaunchData[3]; // diagnostics: {type, reason, code} as last written for the dashboard

// The dashboard (spec/oxos_internal/19_dashboard_choice.md): the ones C:\OXOS\oxos_settings.ini's [dashboard] lists, in order (the dashboard resolver module,
// kernel/modules/dashboard_resolver.cpp; not in safe mode, nor without the kernel modules), then the Xbox dashboard, C:\xboxdash.xbe, as the stock kernel
// starts it.
constexpr char STOCK_DASHBOARD[] = "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe";

// The launch data page's layout (one 4 KB page; the fields the kernel reads).
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

static void LaunchDashboard() {
    if ((kernel_modules::Modules && kernel_modules::Modules->LoadListedDashboard()) || NT_SUCCESS(LoadImage(STOCK_DASHBOARD, TRUE)))
        return;
    if (!KiQuickRebootFlag) {
        auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
        ULONG code = 13;
        if (ldp && ldp->LaunchDataType == 1)
            code += ldp->DashboardReason;
        else if (TitleDiscAuthenticated)
            code = 20;
        // The error's screen until X is pressed, then the File Manager (spec/oxos_internal/32 §1); the fatal error without a network card.
        user_interface::NoDashboardError(code);
        if (ServeFileManager())
            return;
        user_interface::ShowFatalError(code);
    }
    HalReturnToFirmware(4);
}

static void LaunchDashboardWithReason(ULONG Reason, ULONG Code) {
    if (!LaunchDataPage)
        LaunchDataPage = MmAllocateContiguousMemory(0x1000);
    if (LaunchDataPage) {
        MmPersistContiguousMemory(LaunchDataPage, 0x1000, TRUE);
        auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
        memset(ldp, 0, 0x1000);
        ldp->LaunchDataType = 1;
        ldp->DashboardReason = Reason;
        ldp->DashboardCode = Code;
        DashboardLaunchData[0] = 1;
        DashboardLaunchData[1] = Reason;
        DashboardLaunchData[2] = Code;
    }
    LaunchDashboard();
}

static void FreeLaunchDataPage() {
    PVOID page = LaunchDataPage;
    LaunchDataPage = nullptr;
    MmFreeContiguousMemory(page);
}

static bool ShowPlainDashboardError(NTSTATUS s) {
    return s == (NTSTATUS)0xC0000010 || s == (NTSTATUS)0xC0000013 || s == (NTSTATUS)0xC0000015 || s == (NTSTATUS)0xC0000034 || s == (NTSTATUS)0xC000003A ||
           s == (NTSTATUS)0xC00000A3 || s == (NTSTATUS)0xC000014F;
}

// The length of Path's folder: up to its last '\'.
static ULONG PathFolderLength(PCSZ Path) {
    ULONG folder = 0;
    for (ULONG i = 0; Path[i]; i++) {
        if (Path[i] == '\\')
            folder = i;
    }
    return folder;
}

// Loads Path with \??\D: linked to its first FolderLength characters, the folder it is started from (none when 0); the link is removed again when the image
// does not load. Redirectable (a launch through the launch data page, or the disc started without one): an extension may start another XBE in its place
// (spec/oxos_internal/34 §7.2), which is then loaded as a launch redirect's target is (spec/oxos_internal/33 §3: its own folder as D:), and Path as asked
// when it does not load.
static NTSTATUS LoadFromFolder(PCSZ Path, ULONG FolderLength, bool Redirectable = false) {
    char folder[256];
    bool linked = false;
    if (FolderLength && FolderLength < sizeof(folder)) {
        strlcpy(folder, Path, FolderLength + 1);
        linked = NT_SUCCESS(io::CreateLink("\\??\\D:", folder));
    }
    char target[INI_REDIRECT_MAX_PATH + 1];
    NTSTATUS status = LoadImage(Path, FALSE, Redirectable ? target : nullptr);
    if (status == STATUS_REPARSE) {
        if (linked)
            io::DeleteLink("\\??\\D:");
        status = LoadFromFolder(target, PathFolderLength(target));
        if (NT_SUCCESS(status))
            return status;
        XK_SERIAL("load: the extension's target did not load (%#lx); starting %s\n", status, Path);
        return LoadFromFolder(Path, FolderLength);
    }
    if (!NT_SUCCESS(status) && linked)
        io::DeleteLink("\\??\\D:");
    return status;
}

// --- Launch redirects from C:\OXOS\oxos_settings.ini's [launch_redirect] (spec/oxos_internal/33)

// Starts the redirect file's target in place of Path (the launch page's image, a device path) when a section names it (the launch redirect module,
// kernel/modules/launch_redirect.cpp): true once the target is loaded. False when there is no file, no section for Path or no kernel modules, or the target
// does not load (the caller then starts Path as asked, §4).
static bool RedirectLaunch(PCSZ Path) {
    char target[INI_REDIRECT_MAX_PATH + 1];
    if (!kernel_modules::Modules || !kernel_modules::Modules->FindLaunchRedirect(Path, target, sizeof(target)))
        return false;
    NTSTATUS status = LoadFromFolder(target, PathFolderLength(target));
    if (NT_SUCCESS(status))
        return true;
    XK_SERIAL("redirect: target did not load (%#lx); starting %s\n", status, Path);
    return false;
}

// --- The boot video (spec/oxos_internal/38 §2): its player, C:\OXOS\apps\oxos_boot_video.xbe, runs in place of the title; when it quick-reboots, the title
// is chosen as it would have been without it. The player leaves the launch data page as it found it. oxos_settings.ini's [boot_video] is read at the cold
// boot only, by the kernel settings module (ReadColdBootSwitches); its two switches are kept for the quick reboots.

constexpr char BOOT_VIDEO_PLAYER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\apps\\oxos_boot_video.xbe";

// [boot_video]'s switches (XK_BOOT_VIDEO_*), read at the cold boot by the kernel settings module; 0 in safe mode. They survive quick reboots.
XK_PERSIST volatile ULONG BootVideoSwitches;

// What the player runs in place of; it survives the player's quick reboot.
enum BOOT_VIDEO_PLACE : ULONG {
    BOOT_VIDEO_NONE = 0,         // the player is not running
    BOOT_VIDEO_COLD_BOOT = 1,    // the cold boot's title: the dashboard (boot flag 0x08) or the disc, else the dashboard
    BOOT_VIDEO_QUICK_REBOOT = 2, // a quick reboot's: the disc or the dashboard, as the launch data page says
    BOOT_VIDEO_TITLE = 3,        // the cold boot's LaunchTitle(reason): the disc, else the dashboard, with the first-boot reason in bits 8 and up
};
XK_PERSIST volatile ULONG BootVideoPlace;

// Loads the player (its folder as D:) in place of the title; true once it is loaded.
static bool LoadBootVideoPlayer(ULONG Place) {
    NTSTATUS status = LoadFromFolder(BOOT_VIDEO_PLAYER, PathFolderLength(BOOT_VIDEO_PLAYER));
    XK_SERIAL("boot video: player %#lx\n", status);
    if (!NT_SUCCESS(status))
        return false;
    BootVideoPlace = Place;
    TitleLaunched = 5;
    return true;
}

// §2. Returns with TitleLaunched set once an image is loaded (or does not return).
static void LaunchTitle(ULONG Reason) {
    NTSTATUS status;
    auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
    if (!Reason && ldp && (ldp->Flags & 1)) { // already tried once: do not loop
        FreeLaunchDataPage();
        ldp = nullptr;
    }
    if (!Reason && ldp) {
        // A. A title asked for another title.
        ldp->Flags |= 1;
        CHAR path[361];
        memcpy(path, ldp->LaunchPath, 360);
        path[360] = 0;
        if (ldp->LaunchDataType == 0xFFFFFFFF)
            FreeLaunchDataPage();
        if (!path[0]) {
            TitleLaunched = 2;
            return LaunchDashboard();
        }
        // "\Device\...\dir;name.xbe": D: is the directory, the image is dir\name.xbe.
        ULONG folder = 0;
        for (ULONG i = 0; path[i]; i++) {
            if (path[i] == ';') {
                path[i] = '\\';
                folder = i;
                break;
            }
        }
        // An image that is not on the disc (a dashboard starting a title from the hard disk): a disc image left mounted by an earlier launch is unmounted, so
        // this title's own restart or exit falls back to the dashboard, not to that image's title (spec/oxos_internal/16 §5).
        if (IsLaunchAwayFromDisc(path))
            storage::UnmountVirtualDiscs();
        // The redirect file's target in its place (spec/oxos_internal/33), else the image the page names.
        if (RedirectLaunch(path)) {
            TitleLaunched = 3;
            return;
        }
        status = LoadFromFolder(path, folder, true);
        if (NT_SUCCESS(status)) {
            TitleLaunched = 3;
            return;
        }
    } else {
        // B. A title delivered by netboot (titles/title_netboot.cpp), else the disc, else the dashboard. The netbooted title runs whatever the dashboard would
        // be asked to do (a console without a clock backup always has a reason: the clock is not set).
        if (memory::BootPagesPresent() && LaunchNetbootTitle()) {
            TitleLaunched = 1;
            return;
        }
        ULONG tray, before, after;
        HalReadSMCTrayState(&tray, &before);
        status = LoadFromFolder("\\Device\\CdRom0\\default.xbe", 0, !LaunchDataPage); // an extension may start its menu in the disc's place
        if (NT_SUCCESS(status)) {
            TitleLaunched = 1;
            return;
        }
        TitleLaunched = 2;
        if (Reason)
            return LaunchDashboardWithReason(3, Reason);
        HalReadSMCTrayState(&tray, &after);
        if (tray != 0x60)
            return LaunchDashboard();
        if (before != after)
            status = (NTSTATUS)0xC00000A3;
        if (ShowPlainDashboardError(status))
            return LaunchDashboard();
    }
    TitleLaunched = 2;
    LaunchDashboardWithReason(1, status == STATUS_REGION_MISMATCH ? 3 : status == STATUS_MEDIA_MISMATCH ? 5 : 1);
}

// A quick reboot's title. After the cold boot's boot video, the title that cold boot chose: the dashboard for boot flag 0x08, else the disc or the dashboard
// (as without launch data). Else, with boot_video_always = yes, the player first, unless a title asked for a title (a launch path not yet tried). Else what
// the launch data page asks for.
static void LaunchAfterQuickReboot() {
    ULONG place = BootVideoPlace;
    BootVideoPlace = BOOT_VIDEO_NONE;
    if ((place & 0xFF) == BOOT_VIDEO_TITLE) {
        LaunchReason = place >> 8;
        return LaunchTitle(LaunchReason);
    }
    if (place == BOOT_VIDEO_COLD_BOOT && (hardware::chipset::BootFlags & 0x08)) {
        TitleLaunched = 2;
        return LaunchDashboard();
    }
    auto ldp = (LAUNCH_DATA_PAGE *)LaunchDataPage;
    bool titleAsked = ldp && ldp->LaunchPath[0] && !(ldp->Flags & 1);
    // A title that ends with a quick reboot asking for nothing (no launch data page, or the one it was started with: how nxdk titles exit) has ended: a disc
    // image is unmounted, so the dashboard follows instead of the image's title again (spec/oxos_internal/16 §5). A return to the dashboard keeps it, and so
    // does an image that title mounted itself: an attach XBE mounts one and quick-reboots so that the disc, now the image, starts.
    if (place == BOOT_VIDEO_NONE && (!ldp || (ldp->Flags & 1)) && !storage::HasVirtualDiscFromLastTitle())
        storage::UnmountVirtualDiscs();
    if (place == BOOT_VIDEO_NONE && (BootVideoSwitches & XK_BOOT_VIDEO_ALWAYS) && !titleAsked && LoadBootVideoPlayer(BOOT_VIDEO_QUICK_REBOOT))
        return;
    LaunchTitle(0);
}

// Just before the title runs: the splash ends (for the boot video player its picture stays on screen until the player's mode set, spec/oxos_internal/38
// §2), and the LaunchDataPage moves below the kernel, out of the title's way (spec/stock/05 §5).
static void PrepareToEnterTitle() {
    boot_splash::EndSplash(BootVideoPlace != BOOT_VIDEO_NONE);
    storage::ForgetVirtualDiscMounters(); // images mounted until now are not this title's
    if (!LaunchDataPage)
        return;
    ULONG kernelPhys = MmGetPhysicalAddress((PVOID)0x80010000);
    PVOID page = MmAllocateContiguousMemoryEx(0x1000, 0, kernelPhys - 1, 0, PAGE_READWRITE);
    if (!page)
        return;
    memcpy(page, LaunchDataPage, 0x1000);
    MmPersistContiguousMemory(page, 0x1000, TRUE);
    PVOID old = LaunchDataPage;
    LaunchDataPage = page;
    MmFreeContiguousMemory(old);
}

// --- Launches asked for by extensions (spec/oxos_internal/34 §7.3), for in-game shortcuts (spec/oxos_internal/48)

static void NTAPI TitleThread(PVOID, PVOID);

// Suspends every thread of the running title (spec/oxos_internal/22 §9.3): those started in title code (below 0x80000000), and the title's main thread (the
// title launcher's thread, which entered the title). Then 50 ms, so each takes the suspend at its next safe point. Without it a title's own code ran on
// during the quick reboot and wrote to memory already given back.
static void SuspendTitleThreads() {
    PKTHREAD self = KeGetCurrentThread(), threads[64];
    ULONG count = 0;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    for (PLIST_ENTRY e = core::SystemProcess.ThreadListHead.Flink; e != &core::SystemProcess.ThreadListHead && count < 64; e = e->Flink) {
        auto t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        auto start = ((PETHREAD)t)->StartAddress;
        if (t != self && ((ULONG)start < 0x80000000 || start == (PVOID)TitleThread))
            threads[count++] = t;
    }
    KfLowerIrql(old);
    for (ULONG i = 0; i < count; i++)
        KeSuspendThread(threads[i]);
    LARGE_INTEGER wait;
    wait.QuadPart = -50 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    XK_SERIAL("launch: %lu title threads suspended\n", count);
}

NTSTATUS SwitchTitle(const char *Path) {
    ULONG length = Path ? strlen(Path) : 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return (NTSTATUS)0xC0000184; // STATUS_INVALID_DEVICE_STATE
    if (length >= 360) // the title loader reads 360 characters of the launch path
        return STATUS_INVALID_PARAMETER;
    if (Path && !io::PathExists(Path))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (InterlockedExchange(&hardware::chipset::ConsoleStopping, 1))
        return (NTSTATUS)0x80000011; // STATUS_DEVICE_BUSY
    auto page = (LAUNCH_DATA_PAGE *)LaunchDataPage;
    if (!page && (page = (LAUNCH_DATA_PAGE *)MmAllocateContiguousMemory(0x1000)) != nullptr)
        MmPersistContiguousMemory(page, 0x1000, TRUE);
    if (!page) {
        hardware::chipset::ConsoleStopping = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SuspendTitleThreads();
    memset(page, 0, 0x1000);
    page->LaunchDataType = 0xFFFFFFFF; // the dashboard: no launch data (the page is freed, spec/stock/05 §2)
    if (Path) { // "\Device\...\folder;name.xbe"
        page->LaunchDataType = 0;
        memcpy(page->LaunchPath, Path, length + 1);
        for (ULONG i = length; i-- > 0;) {
            if (page->LaunchPath[i] == '\\') {
                page->LaunchPath[i] = ';';
                break;
            }
        }
    }
    LaunchDataPage = page;
    XK_SERIAL("launch: an extension starts %s\n", Path ? Path : "the dashboard");
    HalReturnToFirmware(2);
    return (NTSTATUS)0xC0000001; // STATUS_UNSUCCESSFUL: not reached
}

// --- The cold boot's choice (spec/stock/03 §4): the pre-launch checks, the SMC boot flags, X for the File Manager and
// B for safe mode. Boot-only (spec/oxos_internal/18 §2): a quick reboot just launches.

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

// §1: bit 0 the clock was never set (CMOS signature, see NtSetSystemTime) or is outside [2001, 2101); bits 1/2 the user settings are unreadable or lack a
// language (+0x08) / time zone (+0x30).
static ULONG CheckFirstBoot() {
    ULONG reason = 0;
    if (hardware::chipset::CmosSignatureValid()) {
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

// The OXOS File Manager (spec/oxos_internal/31) until a browser launches an XBE that loads: true then. False when the network card cannot be used.
bool ServeFileManager() {
    char path[260];
    for (NTSTATUS last = STATUS_SUCCESS; webserver::Start(last, path, sizeof(path));) {
        last = LoadImage(path, FALSE);
        if (NT_SUCCESS(last)) {
            webserver::Stop();
            TitleLaunched = 4;
            return true;
        }
    }
    XK_SERIAL("filemanager: no network card\n");
    return false;
}

// Boot flag 0x40 (the tray was opened during boot, spec/stock/05 §3): wait while the tray is closing or settling, then the disc if one is in, otherwise the
// dashboard.
static void WaitForTray() {
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
        if (hardware::ide::DvdDeviceObject) // none on a console without a DVD drive
            IoDismountVolume(hardware::ide::DvdDeviceObject);
        LaunchTitle(0);
    } else {
        TitleLaunched = 2;
        LaunchDashboard();
    }
}

// The switches of oxos_settings.ini read at the cold boot (the kernel settings module), for this cold boot and its quick reboots: the boot video's
// ([boot_video], spec/oxos_internal/38 §1) and whether toasts are off ([toasts], spec/oxos_internal/36 §2). None in safe mode or without the kernel modules.
static void ReadColdBootSwitches() {
    ULONG switches = !extensions::SafeMode && kernel_modules::Modules ? kernel_modules::Modules->ReadColdBootSwitches() : 0;
    BootVideoSwitches = switches & (XK_BOOT_VIDEO_ENABLED | XK_BOOT_VIDEO_ALWAYS);
    toasts::ToastsOff = (switches & XK_TOASTS_OFF) != 0;
}

// LaunchTitle(Reason), after the boot video when [boot_video] turns it on (user, 2026-11-17: "if enabled, play the boot video always"): the player's
// quick reboot then makes this launch (LaunchAfterQuickReboot, BOOT_VIDEO_TITLE).
static void LaunchTitleAfterBootVideo(ULONG Reason) {
    if ((BootVideoSwitches & XK_BOOT_VIDEO_ENABLED) && LoadBootVideoPlayer(BOOT_VIDEO_TITLE | Reason << 8))
        return;
    LaunchTitle(Reason);
}

// Cold boot with X pressed during the splash (spec/oxos_internal/32): the File Manager instead of a title. False without the press, or when the network card
// cannot be used (the title is then chosen as usual).
static bool RunFileManager() {
    boot_splash::WaitForSplash();
    return gamepad::XButtonPressed && ServeFileManager();
}

void LaunchAtColdBoot() {
    if (hardware::chipset::BootFlags & 0x20) // the previous boot ended in a fatal error (scratch 2)
        user_interface::ShowFatalError(21);
    hardware::ide::CountDiskPowerCycle();
    diagnostics::CreateOXOSFolders(); // C:\OXOS and its log, extensions and cache folders exist from every boot on (spec/oxos_internal/34 §3, 35)
    kernel_modules::WriteKernelModulesCache(); // for the quick reboots (spec/oxos_internal/43)
    filesystems::ReadCachePartitions(); // an exFAT disk: X/Y/Z's formats from before a power-off (spec/oxos_internal/05 §3)
    if (RunFileManager())
        return;
    // B held during the splash: no extensions until the next cold boot (spec/oxos_internal/34 §3.3).
    extensions::SafeMode = gamepad::BButtonPressed != 0;
    ReadColdBootSwitches();
    extensions::LoadExtensions();
    ULONG reason = CheckFirstBoot();
    XK_SERIAL("launch: cold boot: boot flags %#lx, region %#lx, first-boot reason %#lx, boot video switches %#lx\n", hardware::chipset::BootFlags,
              ConsoleGameRegion, reason, BootVideoSwitches);
    if ((hardware::chipset::BootFlags & 4) || (ConsoleGameRegion & 0x80000000))
        return LaunchTitleAfterBootVideo(0);
    LaunchReason = reason;
    if (LaunchReason)
        return LaunchTitleAfterBootVideo(LaunchReason);
    // A title delivered by netboot runs instead of the dashboard the boot flags would choose (spec/oxos_internal/09_netboot.md §7).
    if (LaunchNetbootTitle()) {
        TitleLaunched = 1;
        return;
    }
    TitleLaunched = 2;
    if (hardware::chipset::BootFlags & 0x80)
        return LaunchDashboardWithReason(5, 0);
    if (hardware::chipset::BootFlags & 0x40)
        return WaitForTray();
    // The boot video's player first, when oxos_settings.ini asks for it (its quick reboot then starts this title, LaunchAfterQuickReboot).
    BootVideoPlace = BOOT_VIDEO_NONE;
    if ((BootVideoSwitches & XK_BOOT_VIDEO_ENABLED) && LoadBootVideoPlayer(BOOT_VIDEO_COLD_BOOT))
        return;
    if (hardware::chipset::BootFlags & 0x08)
        return LaunchDashboard();
    LaunchTitle(0);
}

#pragma code_seg()
#pragma const_seg()

static void NTAPI TitleThread(PVOID, PVOID) {
#if XK_BOOT_SELFTEST
    while (!KiQuickRebootFlag && !KiTestDvdDone) {
        LARGE_INTEGER t;
        t.QuadPart = -20 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
#endif
    storage::RestoreVirtualDiscMounts();
    // The kernel modules for this launch (spec/oxos_internal/43): the extension loader, the settings, launch redirects, the dashboards and title patches.
    kernel_modules::LoadKernelModules();
    if (KiQuickRebootFlag) {
        extensions::LoadExtensions();
        LaunchAfterQuickReboot();
    } else {
        LaunchAtColdBoot();
    }
    extensions::TitleLoaded();
    kernel_modules::UnloadKernelModules();
    filesystems::HDDFSRememberTitleFolder();
    PrepareToEnterTitle();
    hardware::chipset::SetBootLed(0x0F); // solid green: the title (dashboard or game) is starting
    diagnostics::BootMarksDone();
    CallTitleEntry(); // exactly `call [0x00010128]`, which trainer loaders hook (spec/oxos_internal/23)
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void StartTitleLauncher() {
    HANDLE h;
    if (NT_SUCCESS(PsCreateSystemThreadEx(&h, 0, 0x3000, 0, nullptr, nullptr, nullptr, FALSE, FALSE, TitleThread)))
        NtClose(h);
}

} // namespace xk::titles
