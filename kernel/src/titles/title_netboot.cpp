// A title delivered by netboot (OXOS addition; spec/oxos_internal/09_netboot.md §7). tools/netboot/netboot.py --title DIR appends the title's files (a package)
// to the image it serves and leaves a marker in the downloaded bank. At entry the kernel notes where the package is; the memory manager keeps those pages off
// the free lists; the title-launch thread writes the files to E:\<name>\ (the XBE as <name>.xbe), gives the pages back, and launches that XBE (D: = its folder)
// instead of the disc or the dashboard. A quick reboot finds no package (the marker is cleared once read), so a title's own reboots and launches behave as
// usual.
#include <titles/titles.h>
#include <io/io.h>
#include <core/core.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <runtime/runtime.h>
#include <diagnostics/diagnostics.h>

namespace xk::titles {

namespace {
constexpr ULONG BANK = 0x80800000;         // the downloaded image, still mapped by the 2BL at kernel entry
constexpr ULONG MARKER_AT = 0x5FE0;        // 'OXTI', package offset in the image, package size
constexpr ULONG PAYLOAD_MAGIC = 0x504B584F, MARKER_MAGIC = 0x4954584F, PACKAGE_MAGIC = 0x5054584F; // OXKP OXTI OXTP
constexpr ULONG MAX_IMAGE = 0x400000;      // the 2BL's limit (kernel/boot/netboot.cpp)

struct PackageHeader { // little-endian, at the package's start
    ULONG Magic, Version, Count;
    char Name[16];     // the folder on E: and the XBE's base name, NUL-padded
};
struct PackageEntry {
    char Name[64];     // file name inside the folder, NUL-padded
    ULONG Offset, Size; // from the package's start
};

ULONG NetbootPackagePhysical, NetbootPackageSize; // physical address and size of the package; 0 when there is none
ULONG NetbootMarkerSeen[5];              // diagnostics: payload magic, marker magic, offset, size, package magic
} // namespace

// At kernel entry (cold boot), before anything can reuse the downloaded bank's pages.
void ReadNetbootTitle() {
    auto bank = (volatile ULONG *)BANK;
    NetbootMarkerSeen[0] = bank[0x6000 / 4];
    NetbootMarkerSeen[1] = bank[MARKER_AT / 4];
    NetbootMarkerSeen[2] = bank[MARKER_AT / 4 + 1];
    NetbootMarkerSeen[3] = bank[MARKER_AT / 4 + 2];
    if (bank[0x6000 / 4] != PAYLOAD_MAGIC || bank[MARKER_AT / 4] != MARKER_MAGIC)
        return;
    ULONG offset = bank[MARKER_AT / 4 + 1], size = bank[MARKER_AT / 4 + 2];
    bank[MARKER_AT / 4] = 0; // read once: a quick reboot must not find it again
    if (offset < 0x40000 || offset % PAGE_SIZE || size < sizeof(PackageHeader) || offset + size > MAX_IMAGE)
        return;
    auto header = (const PackageHeader *)(BANK + offset);
    NetbootMarkerSeen[4] = header->Magic;
    if (header->Magic != PACKAGE_MAGIC || header->Version != 1 || !header->Count || header->Count > 64)
        return;
    NetbootPackagePhysical = (BANK & 0x0FFFFFFF) + offset;
    NetbootPackageSize = size;
}

// Memory-manager start-up: the package's pages stay off the free lists until the title is installed.
bool IsNetbootTitlePage(ULONG pfn) {
    return NetbootPackageSize && pfn >= (NetbootPackagePhysical >> PAGE_SHIFT) &&
           pfn < ((NetbootPackagePhysical + NetbootPackageSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

// --- Installing and launching the package: only at a cold boot (a quick reboot never has one), so boot-only
// (spec/oxos_internal/18 §2); the title launcher asks memory::BootPagesPresent() first.

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

static void ReleaseNetbootPackage() {
    ULONG first = NetbootPackagePhysical >> PAGE_SHIFT;
    ULONG last = (NetbootPackagePhysical + NetbootPackageSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
    NetbootPackageSize = 0;
    for (ULONG pfn = first; pfn < last; pfn++)
        memory::FreeContiguousPage(pfn);
}

static void PackageTitleName(char *out, const char *in, ULONG max) { // a NUL-padded name, '\\' and ':' refused
    ULONG i = 0;
    for (; i < max - 1 && in[i]; i++)
        out[i] = (in[i] == '\\' || in[i] == ':' || in[i] == '/') ? '_' : in[i];
    out[i] = 0;
}

// The title-launch thread, cold boot: install the package's files and launch its XBE. FALSE when there is no package or the launch failed (the usual disc /
// dashboard selection follows).
bool HasNetbootTitle() {
    return NetbootPackageSize != 0;
}

bool LaunchNetbootTitle() {
    if (!NetbootPackageSize) {
        if (NetbootMarkerSeen[1] == MARKER_MAGIC) { // a title was sent but not taken: say why
            XK_SERIAL("netboot title: refused (payload %08lx, marker %08lx, offset %#lx, size %#lx, package %08lx)\n", NetbootMarkerSeen[0],
                      NetbootMarkerSeen[1], NetbootMarkerSeen[2], NetbootMarkerSeen[3], NetbootMarkerSeen[4]);
        }
        return false;
    }
    auto base = (const UCHAR *)(0x80000000 | NetbootPackagePhysical);
    auto header = (const PackageHeader *)base;
    auto entries = (const PackageEntry *)(header + 1);
    char title[16], dir[96], path[192];
    PackageTitleName(title, header->Name, sizeof(title));
    snprintf(dir, sizeof(dir), "\\Device\\Harddisk0\\Partition1\\%s", title);
    HANDLE h;
    NTSTATUS status = io::CreateFile(dir, GENERIC_WRITE, 0, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &h);
    if (NT_SUCCESS(status))
        NtClose(h);
    XK_SERIAL("netboot title: %s, %lu files, %lu bytes -> E:\\%s (folder %#lx)\n", title, header->Count, NetbootPackageSize, title, status);
    bool written = NT_SUCCESS(status);
    for (ULONG i = 0; written && i < header->Count; i++) {
        const PackageEntry &e = entries[i];
        if (e.Offset + e.Size > NetbootPackageSize) {
            written = false;
            break;
        }
        char name[64];
        PackageTitleName(name, e.Name, sizeof(name));
        snprintf(path, sizeof(path), "%s\\%s", dir, name);
        status = io::CreateFile(path, GENERIC_WRITE, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &h);
        if (NT_SUCCESS(status)) {
            IO_STATUS_BLOCK iosb;
            LARGE_INTEGER at = {};
            status = e.Size ? NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, (PVOID)(base + e.Offset), e.Size, &at) : STATUS_SUCCESS;
            NtClose(h);
        }
        XK_SERIAL("netboot title: E:\\%s\\%s, %lu bytes: %#lx\n", title, name, e.Size, status);
        written = NT_SUCCESS(status);
    }
    ReleaseNetbootPackage();
    if (!written)
        return false;
    // D: is the title's folder, as for a title launched from the hard disk.
    bool linked = NT_SUCCESS(io::CreateLink("\\??\\D:", dir));
    snprintf(path, sizeof(path), "%s\\%s.xbe", dir, title);
    status = LoadImage(path, FALSE);
    XK_SERIAL("netboot title: launch %s: %#lx\n", path, status);
    if (NT_SUCCESS(status))
        return true;
    if (linked)
        io::DeleteLink("\\??\\D:");
    return false;
}

#pragma code_seg()
#pragma const_seg()

} // namespace xk::titles
