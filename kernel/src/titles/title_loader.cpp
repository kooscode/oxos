// XBE loader and title launch (spec/stock/05 §2-4).
#include <titles/titles.h>
#include <diagnostics/diagnostics.h>
#include <user_interface/user_interface.h>
#include <io/io.h>
#include <extensions/extensions.h>
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

struct XBE_SECTION_HEADER {
    ULONG Flags, VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData, SectionName;
    ULONG SectionReferenceCount;
    USHORT *HeadSharedPageReferenceCount, *TailSharedPageReferenceCount;
    UCHAR SectionDigest[20];
};
static_assert(sizeof(XBE_SECTION_HEADER) == 0x38);

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

// --- Kernel files in C:\OXOS (oxos_title_patches.ini, oxos_launch_redirect.ini): C: on a FATX disk, /C/OXOS on an
// exFAT disk

// A whole file of at most MaxSize bytes, in pool memory the caller frees, with its length in *Got. Null when it does not open (*Status says why:
// STATUS_OBJECT_NAME_NOT_FOUND when it does not exist), is empty or larger, or cannot be read.
char *ReadOxosFile(PCSZ Path, ULONG MaxSize, ULONG Tag, ULONG *Got, NTSTATUS *Status) {
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

static PUCHAR PatchRecords; // this title's records (ini_file::ParseTitlePatches), or null
static ULONG PatchFileSize;

static void LoadPatches(ULONG TitleId, ULONG Version, const XBE_SECTION_HEADER *Sections, ULONG Count) {
    if (PatchRecords) {
        ExFreePool(PatchRecords);
        PatchRecords = nullptr;
        PatchFileSize = 0;
    }
    constexpr ULONG MAX_PATCH_FILE = 0x100000;
    ULONG got;
    NTSTATUS status;
    char *text = ReadOxosFile("\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_title_patches.ini", MAX_PATCH_FILE, 'tPeX', &got, &status);
    if (!text) {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
            XK_SERIAL("patches: oxos_title_patches.ini not read: %#lx\n", status);
        return; // no file: no patches
    }
    // A record is never more than twice its line ("patch=1:00" makes 12 bytes, "name=" 8).
    ULONG room = 2 * got + 16;
    auto out = (PUCHAR)ExAllocatePoolWithTag(room, 'rPeX');
    XE_PATCH_COUNTS counts = {};
    ULONG used = out ? ini_file::ParseTitlePatches(text, got, TitleId, Version, out, room, &counts) : 0;
    ExFreePool(text);
    XK_SERIAL("patches: title %08lx version %lu: %lu sections, %lu patches, %lu lines not understood\n", TitleId, Version, counts.Sections, counts.Patches,
              counts.Errors);
    if (!used) {
        if (out)
            ExFreePool(out);
        return;
    }
    for (ULONG at = 0; at < used;) {
        auto r = (XE_PATCH_RECORD *)(out + at);
        at += ini_file::PatchRecordSize(r);
        if (r->Kind == XE_PATCH_NAME) {
            CHAR line[XE_PATCH_MAX_NAME + 1];
            memcpy(line, r + 1, r->Length);
            line[r->Length] = 0;
            XK_SERIAL("patches: %s\n", line);
            continue;
        }
        // The patch, and the match bytes it is checked against, inside one section (so both are loaded when the patch is applied).
        bool inside = false;
        for (ULONG i = 0; i < Count && !inside; i++) {
            ULONG va = Sections[i].VirtualAddress, size = Sections[i].VirtualSize;
            inside = r->Address >= va && r->Address - va + ini_file::PatchSpan(r) <= size && (!r->Check || (r->Check >= va &&
                     r->Check - va + r->OriginalLength <= size));
        }
        if (!inside) {
            XK_SERIAL("patches: %#lx (%lu bytes) is not inside one section of the title%s: skipped\n", r->Address, ini_file::PatchSpan(r),
                      r->Check ? " with its match bytes" : "");
            r->Kind = 0xFFFF;
        }
    }
    PatchRecords = out;
    PatchFileSize = used;
}

// A patch is written only where its current bytes are: so never into another build of the title, nor into a launcher that carries the title's identity
// (spec/oxos_internal/25 §3.2). The patches checked against a section's `match =` bytes go first: another patch of the section may change those bytes (a
// section's first patch can sit at its match address).
static void ApplyPatches(const XBE_SECTION_HEADER *s) {
    for (ULONG pass = 0; pass < 2; pass++) {
        for (ULONG at = 0; at < PatchFileSize;) {
            auto r = (XE_PATCH_RECORD *)(PatchRecords + at);
            at += ini_file::PatchRecordSize(r);
            if (r->Kind != XE_PATCH_BYTES || !r->Check != (pass == 1) || r->Address < s->VirtualAddress ||
                r->Address - s->VirtualAddress + ini_file::PatchSpan(r) > s->VirtualSize)
                continue;
            auto target = (PUCHAR)r->Address;
            auto check = (PUCHAR)(r->Check ? r->Check : r->Address);
            // Already there first: a patch checked against another place must not be written twice (its match bytes may still be there when an earlier patch of
            // the section has not changed them).
            if (memcmp(target, ini_file::PatchData(r), r->Length) == 0) {
                XK_SERIAL("patches: %#lx: already there\n", r->Address);
            } else if (memcmp(check, ini_file::PatchOriginal(r), r->OriginalLength) == 0) {
                memcpy(target, ini_file::PatchData(r), r->Length);
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

NTSTATUS LoadImage(PCSZ Path, BOOLEAN Dashboard) {
    XK_SERIAL("load %s: start\n", Path);
    extensions::TitleLoading(Path, Dashboard);
    NTSTATUS status = LoadImageFile(Path, Dashboard);
    XK_SERIAL("load %s: %#lx\n", Path, status);
    return status;
}

// --- Launch (spec/stock/05 §1-3, §5)

volatile ULONG TitleLaunched; // 1 = disc title, 2 = dashboard, 3 = from the LaunchDataPage, 4 = from the File Manager
volatile ULONG LaunchReason;           // the cold-boot pre-launch check result (§1)
volatile ULONG DashboardLaunchData[3]; // diagnostics: {type, reason, code} as last written for the dashboard

// The dashboard: EvoX's when it is installed (C:\evoxdash.xbe, or E:\evoxdash.xbe), else the Xbox dashboard (C:\xboxdash.xbe), else C:\Dashboard\default.xbe
// (where the XBL Dashboard installs its loader). The first that loads is started (spec/oxos_internal/19_dashboard_choice.md).
static const char *const Dashboards[] = {
    "\\Device\\Harddisk0\\Partition2\\evoxdash.xbe",
    "\\Device\\Harddisk0\\Partition1\\evoxdash.xbe",
    "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe",
    "\\Device\\Harddisk0\\Partition2\\Dashboard\\default.xbe",
};

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
    for (const char *path : Dashboards) {
        if (NT_SUCCESS(LoadImage(path, TRUE)))
            return;
    }
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

// Loads Path with \??\D: linked to its first FolderLength characters, the folder it is started from (none when 0); the link is removed again when the image
// does not load.
static NTSTATUS LoadFromFolder(PCSZ Path, ULONG FolderLength) {
    char folder[256];
    bool linked = false;
    if (FolderLength && FolderLength < sizeof(folder)) {
        strlcpy(folder, Path, FolderLength + 1);
        linked = NT_SUCCESS(io::CreateLink("\\??\\D:", folder));
    }
    NTSTATUS status = LoadImage(Path, FALSE);
    if (!NT_SUCCESS(status) && linked)
        io::DeleteLink("\\??\\D:");
    return status;
}

// --- Launch redirects from C:\OXOS\oxos_launch_redirect.ini (spec/oxos_internal/33)

// Starts the redirect file's target in place of Path (the launch page's image, a device path) when a section names it: true once the target is loaded. False
// when there is no file or no section for Path, or the target does not load (the caller then starts Path as asked, §4).
static bool RedirectLaunch(PCSZ Path) {
    constexpr ULONG MAX_REDIRECT_FILE = 0x10000;
    ULONG got;
    NTSTATUS status;
    char *text = ReadOxosFile("\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_launch_redirect.ini", MAX_REDIRECT_FILE, 'rLeX', &got, &status);
    if (!text)
        return false;
    XE_REDIRECT redirect;
    ULONG errors;
    bool found = ini_file::LookUpRedirect(text, got, Path, &redirect, &errors);
    ExFreePool(text);
    if (errors)
        XK_SERIAL("redirect: oxos_launch_redirect.ini: %lu lines not understood\n", errors);
    if (!found)
        return false;
    XK_SERIAL("redirect: [%s] %s -> %s\n", redirect.Name, Path, redirect.Target);
    ULONG folder = 0;
    for (ULONG i = 0; redirect.Target[i]; i++) {
        if (redirect.Target[i] == '\\')
            folder = i;
    }
    status = LoadFromFolder(redirect.Target, folder);
    if (NT_SUCCESS(status))
        return true;
    XK_SERIAL("redirect: target did not load (%#lx); starting %s\n", status, Path);
    return false;
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
        // The redirect file's target in its place (spec/oxos_internal/33), else the image the page names.
        if (RedirectLaunch(path)) {
            TitleLaunched = 3;
            return;
        }
        status = LoadFromFolder(path, folder);
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
        status = LoadImage("\\Device\\CdRom0\\default.xbe", FALSE);
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

// Just before the title runs: the splash ends, and the LaunchDataPage moves below the kernel, out of the title's way (spec/stock/05 §5).
static void PrepareToEnterTitle() {
    boot_splash::EndSplash();
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
    diagnostics::CreateOxosFolders(); // C:\OXOS and its log, extensions and cache folders exist from every boot on (spec/oxos_internal/34 §3, 35)
    filesystems::ReadCachePartitions(); // an exFAT disk: X/Y/Z's formats from before a power-off (spec/oxos_internal/05 §3)
    if (RunFileManager())
        return;
    // B held during the splash: no extensions until the next cold boot (spec/oxos_internal/34 §3.3).
    extensions::SafeMode = gamepad::BButtonPressed != 0;
    extensions::LoadExtensions();
    if ((hardware::chipset::BootFlags & 4) || (ConsoleGameRegion & 0x80000000))
        return LaunchTitle(0);
    LaunchReason = CheckFirstBoot();
    if (LaunchReason)
        return LaunchTitle(LaunchReason);
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
    if (KiQuickRebootFlag) {
        extensions::LoadExtensions();
        LaunchTitle(0);
    } else {
        LaunchAtColdBoot();
    }
    extensions::TitleLoaded();
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
