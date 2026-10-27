// ISO launcher (nxdk, open source): mounts an Xbox disc image from the hard disk as the console's disc and starts the
// game on it, the way a disc in the drive starts (spec/oxos-internal/16-virtual-discs.md).
//
// Which image:
//   - D:\iso.txt, one line: an image (F:\games\Halo 2.iso), a folder holding one (F:\games\Halo 2), or a \Device\...
//     path. "none" unmounts the image and starts the dashboard.
//   - Without iso.txt: the image in the launcher's own folder, so a copy of the launcher next to an image starts it.
// Images: XISO or a full disc dump (.iso), CCI (.cci), either one whole or split in two (name.1.iso + name.2.iso).
//
// The image is mounted through the virtual disc device (\Device\CdRom1, the interface Cerbios uses too), then
// \Device\CdRom0\default.xbe is launched through the launch data page and a quick reboot; the mount stays for the
// game's own reboots until the console is switched off (or the disc tray is opened).
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr ULONG IOCTL_VD_MOUNT = 0x1EE7CD01, IOCTL_VD_UNMOUNT = 0x1EE7CD02;
constexpr std::size_t PATH = 300;

// The retail drive letters and the extra exFAT drives F: and G: (spec/oxos-internal/05-exfat-storage-spec.md).
const char *PartitionOf(char letter) {
    switch (letter & ~0x20) {
    case 'C': return "\\Device\\Harddisk0\\Partition2";
    case 'E': return "\\Device\\Harddisk0\\Partition1";
    case 'X': return "\\Device\\Harddisk0\\Partition3";
    case 'Y': return "\\Device\\Harddisk0\\Partition4";
    case 'Z': return "\\Device\\Harddisk0\\Partition5";
    case 'F': return "\\Device\\Harddisk0\\Partition6";
    case 'G': return "\\Device\\Harddisk0\\Partition7";
    default: return nullptr;
    }
}

[[noreturn]] void Stay() {
    for (;;)
        Sleep(1000);
}

NTSTATUS Open(HANDLE *h, const char *path, ULONG options, ACCESS_MASK access = GENERIC_READ) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    return NtOpenFile(h, access | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ,
                      options | FILE_SYNCHRONOUS_IO_NONALERT);
}

bool IsFile(const char *path) {
    HANDLE h;
    if (!NT_SUCCESS(Open(&h, path, FILE_NON_DIRECTORY_FILE)))
        return false;
    NtClose(h);
    return true;
}

bool IsFolder(const char *path) {
    HANDLE h;
    if (!NT_SUCCESS(Open(&h, path, FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY)))
        return false;
    NtClose(h);
    return true;
}

bool EndsWith(std::string_view s, std::string_view tail) {
    if (s.size() < tail.size())
        return false;
    for (std::size_t i = 0; i < tail.size(); i++)
        if ((s[s.size() - tail.size() + i] | 0x20) != (tail[i] | 0x20))
            return false;
    return true;
}

// The first line of D:\iso.txt, trimmed, with '/' as '\'.
std::size_t ReadLine(const char *file, char *text, std::size_t capacity) {
    std::size_t n = 0;
    if (std::FILE *f = std::fopen(file, "rb")) {
        n = std::fread(text, 1, capacity - 1, f);
        std::fclose(f);
    }
    std::string_view line(text, n);
    line = line.substr(0, line.find_first_of("\r\n"));
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '"'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '"'))
        line.remove_suffix(1);
    std::memmove(text, line.data(), line.size());
    text[line.size()] = 0;
    for (std::size_t i = 0; i < line.size(); i++)
        if (text[i] == '/')
            text[i] = '\\';
    return line.size();
}

// A drive path (F:\games\x.iso) or a \Device\ path as a \Device\ path; false for an unknown drive.
bool DevicePath(const char *in, char *out) {
    if (!std::strncmp(in, "\\Device\\", 8)) {
        std::snprintf(out, PATH, "%s", in);
        return true;
    }
    const char *partition = in[0] && in[1] == ':' ? PartitionOf(in[0]) : nullptr;
    if (!partition)
        return false;
    std::snprintf(out, PATH, "%s%s%s", partition, in[2] == '\\' ? "" : "\\", in + 2);
    return true;
}

// The folder the launcher was started from, as a \Device\ path (D: is linked to it, but only until the reboot).
void OwnFolder(char *out) {
    std::snprintf(out, PATH, "%.*s", XeImageFileName->Length, XeImageFileName->Buffer);
    if (char *slash = std::strrchr(out, '\\'))
        *slash = 0;
}

// The images in a folder, best first: a split image's first part, a whole image; .iso before .cci.
struct Found {
    char Name[4][64];
    int Count = 0;
};

int Rank(std::string_view n) {
    if (EndsWith(n, ".2.iso") || EndsWith(n, ".2.cci"))
        return -1; // second parts come with their first
    if (EndsWith(n, ".iso"))
        return 2;
    if (EndsWith(n, ".cci"))
        return 1;
    return -1;
}

void ListImages(const char *folder, Found *found) {
    HANDLE h;
    if (!NT_SUCCESS(Open(&h, folder, FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY)))
        return;
    alignas(8) static char buffer[4096];
    for (BOOLEAN restart = TRUE;; restart = FALSE) {
        IO_STATUS_BLOCK iosb;
        NTSTATUS status = NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer),
                                               FileDirectoryInformation, nullptr, restart);
        if (!NT_SUCCESS(status))
            break;
        for (auto *e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer);;) {
            std::string_view name(e->FileName, e->FileNameLength);
            int rank = Rank(name);
            if (rank > 0 && found->Count < 4 && name.size() < sizeof(found->Name[0])) {
                // Keep the list ordered by rank (stable).
                int at = found->Count;
                while (at > 0 && Rank(found->Name[at - 1]) < rank) {
                    std::memcpy(found->Name[at], found->Name[at - 1], sizeof(found->Name[0]));
                    at--;
                }
                std::snprintf(found->Name[at], sizeof(found->Name[0]), "%.*s", int(name.size()), name.data());
                found->Count++;
            }
            if (!e->NextEntryOffset)
                break;
            e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(reinterpret_cast<char *>(e) + e->NextEntryOffset);
        }
    }
    NtClose(h);
}

// Up to 24 entries of a folder on screen: folders with a trailing '\\', files with their size.
void ListFolder(const char *folder) {
    HANDLE h;
    if (!NT_SUCCESS(Open(&h, folder, FILE_DIRECTORY_FILE, FILE_LIST_DIRECTORY)))
        return;
    alignas(8) static char buffer[4096];
    int shown = 0;
    for (BOOLEAN restart = TRUE; shown < 24; restart = FALSE) {
        IO_STATUS_BLOCK iosb;
        if (!NT_SUCCESS(NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer),
                                             FileDirectoryInformation, nullptr, restart)))
            break;
        for (auto *e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer); shown < 24; shown++) {
            if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                debugPrint("  %.*s\\\n", int(e->FileNameLength), e->FileName);
            else
                debugPrint("  %.*s  (%lu MB)\n", int(e->FileNameLength), e->FileName,
                           static_cast<unsigned long>(e->EndOfFile.QuadPart >> 20));
            if (!e->NextEntryOffset)
                break;
            e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(reinterpret_cast<char *>(e) + e->NextEntryOffset);
        }
    }
    NtClose(h);
}

// The mount request (spec/cerbios/07 §2.1): part count, type, the parts' paths and a link name, as counted strings.
struct MountRequest {
    UCHAR Parts, Type, Pad[2];
    ANSI_STRING Path[8];
    ANSI_STRING Link;
};
static_assert(sizeof(MountRequest) == 0x4C, "the Cerbios request layout");

NTSTATUS Control(ULONG code, MountRequest *request) {
    HANDLE h;
    NTSTATUS status = Open(&h, "\\Device\\CdRom1", 0);
    if (!NT_SUCCESS(status))
        return status;
    IO_STATUS_BLOCK iosb;
    status = NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb, code, request,
                                   request ? sizeof(*request) : 0, nullptr, 0);
    NtClose(h);
    return status;
}

// The title name from the image's default.xbe certificate (UTF-16, 40 characters), as ASCII.
void TitleName(char *out, std::size_t size) {
    out[0] = 0;
    HANDLE h;
    if (!NT_SUCCESS(Open(&h, "\\Device\\CdRom0\\default.xbe", FILE_NON_DIRECTORY_FILE)))
        return;
    alignas(4) static unsigned char head[0x1000];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at{};
    NTSTATUS status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, head, sizeof(head), &at);
    NtClose(h);
    if (!NT_SUCCESS(status) || std::memcmp(head, "XBEH", 4))
        return;
    ULONG base = *reinterpret_cast<ULONG *>(head + 0x104), cert = *reinterpret_cast<ULONG *>(head + 0x118) - base;
    if (cert + 0x0C + 80 > sizeof(head))
        return;
    std::size_t n = 0;
    for (std::size_t i = 0; i < 40 && n + 1 < size; i++) {
        unsigned c = head[cert + 0x0C + 2 * i] | head[cert + 0x0C + 2 * i + 1] << 8;
        if (!c)
            break;
        out[n++] = c < 0x80 ? char(c) : '?';
    }
    out[n] = 0;
}

} // namespace

int main() {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("OXOS ISO launcher\n\n");

    static char line[PATH], path[PATH], folder[PATH], parts[2][PATH];
    std::size_t length = ReadLine("D:\\iso.txt", line, sizeof(line));
    if (length && (!std::strcmp(line, "none") || !std::strcmp(line, "NONE"))) {
        NTSTATUS status = Control(IOCTL_VD_UNMOUNT, nullptr);
        debugPrint("Unmount: status 0x%08lx. Starting the dashboard...\n", static_cast<unsigned long>(status));
        Sleep(1000);
        XLaunchXBE(nullptr);
        Stay();
    }
    if (length) {
        if (!DevicePath(line, path)) {
            debugPrint("Unknown drive in \"%s\" (C, E, F, G, X, Y, Z or a \\Device\\ path).\n", line);
            Stay();
        }
    } else {
        OwnFolder(path);
    }

    // A folder: the best image in it.
    if (IsFolder(path)) {
        Found found;
        ListImages(path, &found);
        if (!found.Count) {
            debugPrint("No .iso or .cci image in %s\n", length ? line : path);
            Stay();
        }
        std::snprintf(folder, sizeof(folder), "%s", path);
        std::snprintf(path, sizeof(path), "%s\\%s", folder, found.Name[0]);
    } else if (!IsFile(path)) {
        debugPrint("Cannot find %s\n= %s\n", length ? line : "an image", path);
        // Show what is there: the nearest folder above the path that exists.
        std::snprintf(folder, sizeof(folder), "%s", path);
        while (char *slash = std::strrchr(folder, '\\')) {
            *slash = 0;
            if (std::strlen(folder) <= 8 || IsFolder(folder)) // stop at \Device
                break;
        }
        if (IsFolder(folder)) {
            debugPrint("\n%s holds:\n", folder);
            ListFolder(folder);
        }
        Stay();
    }

    // A split image: name.1.iso + name.2.iso.
    std::string_view p(path);
    bool cci = EndsWith(p, ".cci");
    int count = 1;
    std::snprintf(parts[0], PATH, "%s", path);
    if (EndsWith(p, ".1.iso") || EndsWith(p, ".1.cci")) {
        std::snprintf(parts[1], PATH, "%s", path);
        parts[1][p.size() - 5] = '2';
        if (IsFile(parts[1]))
            count = 2;
    }
    debugPrint("Image: %s%s\n", parts[0], count == 2 ? " (+ part 2)" : "");

    // Mount: a plain image as the drive (type 0, which Cerbios also takes as "replace \Device\CdRom0"); CCI as 'd'.
    MountRequest request{};
    request.Parts = UCHAR(count);
    request.Type = cci ? 'd' : 0;
    for (int i = 0; i < count; i++)
        RtlInitAnsiString(&request.Path[i], parts[i]);
    RtlInitAnsiString(&request.Link, "\\Device\\CdRom0");
    NTSTATUS status = Control(IOCTL_VD_MOUNT, &request);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        debugPrint("\nThis BIOS cannot mount disc images (no \\Device\\CdRom1).\n");
        Stay();
    }
    if (!NT_SUCCESS(status)) {
        debugPrint("\nThe mount failed: status 0x%08lx (not a disc image?)\n", static_cast<unsigned long>(status));
        Stay();
    }

    char title[48];
    TitleName(title, sizeof(title));
    if (title[0])
        debugPrint("Title: %s\n", title);
    debugPrint("\nLaunching...\n");
    Sleep(500);
    XLaunchXBE("\\Device\\CdRom0\\default.xbe");
    debugPrint("The launch failed.\n");
    Stay();
}
