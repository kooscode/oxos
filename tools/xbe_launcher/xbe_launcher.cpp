// XBE launcher (nxdk + SDL3, open source). Reads D:\launch.txt (one path: a drive letter path such as
// F:\apps\demo\default.xbe, or a \Device\... path) and launches that XBE through the launch data page and a quick
// reboot, the way a dashboard does. It leaves the screen alone (black after the kernel's splash), so the title starts
// with no video mode change of its own; only when something is wrong does it start SDL (tools/sdl_screen), say why
// and stay there.
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstring>
#include <string_view>

#include "sdl_screen.h"

namespace {

// The retail drive letters and the extra exFAT drives F: and G: (spec/oxos_internal/05_exfat_storage_spec.md).
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

NTSTATUS OpenStatus(const char *path) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    NTSTATUS status = NtOpenFile(&handle, GENERIC_READ, &attributes, &iosb, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE);
    if (NT_SUCCESS(status))
        NtClose(handle);
    return status;
}

// The first message: start the screen (the launcher leaves it alone otherwise) and show the heading.
void Heading() {
    if (sdl_screen::IsOpen())
        return;
    if (!sdl_screen::Open(false))
        DbgPrint("xbe launcher: SDL: %s\n", SDL_GetError());
    sdl_screen::Print("OXOS XBE launcher\n\n");
}


// The first line of D:\launch.txt, trimmed, with '/' as '\'.
std::size_t ReadLaunchLine(char *text, std::size_t capacity) {
    std::size_t n = 0;
    if (std::FILE *f = std::fopen("D:\\launch.txt", "rb")) {
        n = std::fread(text, 1, capacity - 1, f);
        std::fclose(f);
    }
    std::string_view line(text, n);
    line = line.substr(0, line.find_first_of("\r\n"));
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.remove_suffix(1);
    std::memmove(text, line.data(), line.size());
    text[line.size()] = 0;
    for (std::size_t i = 0; i < line.size(); i++) {
        if (text[i] == '/')
            text[i] = '\\';
    }
    return line.size();
}

} // namespace

int main() {
    char start[300] = {};
    if (!ReadLaunchLine(start, sizeof(start))) {
        Heading();
        sdl_screen::Print("D:\\launch.txt is missing or empty.\n");
        sdl_screen::ShowForever();
    }

    char path[300];
    if (!std::strncmp(start, "\\Device\\", 8)) {
        std::snprintf(path, sizeof(path), "%s", start);
    } else {
        const char *partition = start[1] == ':' ? PartitionOf(start[0]) : nullptr;
        if (!partition) {
            Heading();
            sdl_screen::Print("Unknown drive in \"%s\" (C, E, F, G, X, Y, Z or a \\Device\\ path).\n", start);
            sdl_screen::ShowForever();
        }
        std::snprintf(path, sizeof(path), "%s%s%s", partition, start[2] == '\\' ? "" : "\\", start + 2);
    }

    NTSTATUS status = OpenStatus(path);
    if (!NT_SUCCESS(status)) {
        Heading();
        sdl_screen::Print("%s\n= %s\n\nCannot open it: status 0x%08lx\n", start, path,
                          static_cast<unsigned long>(status));
        sdl_screen::ShowForever();
    }
    XLaunchXBE(path);
    Heading();
    sdl_screen::Print("%s\n= %s\n\nThe launch failed.\n", start, path);
    sdl_screen::ShowForever();
}
