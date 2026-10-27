// Mount an Xbox disc image as the console's disc (OXOS virtual discs, spec/oxos-internal/16-virtual-discs.md).
// Header-only, for nxdk titles such as dashboards:
//
//     #include "mount_iso.h"
//     if (NT_SUCCESS(MountISO("F:\\titles\\Halo 2.iso")))
//         XLaunchXBE("\\Device\\CdRom0\\default.xbe");
//
// MountISO takes a drive path (C E F G X Y Z) or a \Device\ path to an .iso (XISO or full dump) or .cci image. For a
// split image, pass the first part (name.1.iso): name.2.iso is added. A mount replaces the one before it and stays
// through quick reboots until the console is switched off or UnmountISO is called. The same device interface works
// on Cerbios.
#pragma once
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstring>

namespace oxos_vdisc {

struct MountRequest { // the device's request layout (spec/cerbios/07 §2.1)
    UCHAR Parts, Type, Pad[2];
    ANSI_STRING Path[8];
    ANSI_STRING Link;
};
static_assert(sizeof(MountRequest) == 0x4C, "the request layout");

inline const char *PartitionOf(char letter) {
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

inline bool EndsWith(const char *s, const char *tail) {
    std::size_t n = std::strlen(s), t = std::strlen(tail);
    if (n < t)
        return false;
    for (std::size_t i = 0; i < t; i++)
        if ((s[n - t + i] | 0x20) != (tail[i] | 0x20))
            return false;
    return true;
}

inline bool FileExists(const char *path) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    if (!NT_SUCCESS(NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ,
                               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)))
        return false;
    NtClose(h);
    return true;
}

inline NTSTATUS Control(ULONG code, MountRequest *request) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    RtlInitAnsiString(&name, "\\Device\\CdRom1");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    NTSTATUS status = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ,
                                 FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status))
        return status; // STATUS_OBJECT_NAME_NOT_FOUND: this BIOS has no virtual discs
    status = NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb, code, request,
                                   request ? sizeof(*request) : 0, nullptr, 0);
    NtClose(h);
    return status;
}

} // namespace oxos_vdisc

// The image at Path becomes the console's disc; launch \Device\CdRom0\default.xbe next.
inline NTSTATUS MountISO(const char *Path) {
    using namespace oxos_vdisc;
    static char parts[2][300];
    if (!std::strncmp(Path, "\\Device\\", 8)) {
        std::snprintf(parts[0], sizeof(parts[0]), "%s", Path);
    } else {
        const char *partition = Path[0] && Path[1] == ':' ? PartitionOf(Path[0]) : nullptr;
        if (!partition)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        std::snprintf(parts[0], sizeof(parts[0]), "%s%s%s", partition, Path[2] == '\\' ? "" : "\\", Path + 2);
    }
    if (!FileExists(parts[0]))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    MountRequest request{};
    request.Parts = 1;
    if (EndsWith(parts[0], ".1.iso") || EndsWith(parts[0], ".1.cci")) { // a split image: add its second part
        std::snprintf(parts[1], sizeof(parts[1]), "%s", parts[0]);
        parts[1][std::strlen(parts[1]) - 5] = '2';
        if (FileExists(parts[1]))
            request.Parts = 2;
    }
    request.Type = EndsWith(parts[0], ".cci") ? 'd' : 0; // 0: a plain image that replaces \Device\CdRom0
    for (int i = 0; i < request.Parts; i++)
        RtlInitAnsiString(&request.Path[i], parts[i]);
    RtlInitAnsiString(&request.Link, "\\Device\\CdRom0");
    return Control(0x1EE7CD01, &request);
}

// No image any more: \Device\CdRom0 is the drive again.
inline NTSTATUS UnmountISO() { return oxos_vdisc::Control(0x1EE7CD02, nullptr); }
