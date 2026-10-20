// XBE launcher (nxdk, open source). Reads D:\launch.txt (one path: a drive letter path such as
// F:\apps\demo\default.xbe, or a \Device\... path) and launches that XBE through the launch data page and a quick
// reboot, the way a dashboard does. If the file cannot be opened, it shows why and stays on screen.
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

// The retail drive letters and the extra exFAT drive F: (spec/oxos-internal/01-exfat-storage-spec.md).
static const char *PartitionOf(char letter) {
    switch (letter & ~0x20) {
    case 'C': return "\\Device\\Harddisk0\\Partition2";
    case 'E': return "\\Device\\Harddisk0\\Partition1";
    case 'X': return "\\Device\\Harddisk0\\Partition3";
    case 'Y': return "\\Device\\Harddisk0\\Partition4";
    case 'Z': return "\\Device\\Harddisk0\\Partition5";
    case 'F': return "\\Device\\Harddisk0\\Partition6";
    case 'G': return "\\Device\\Harddisk0\\Partition7";
    default: return NULL;
    }
}

static NTSTATUS OpenStatus(const char *path) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS status = NtOpenFile(&handle, GENERIC_READ, &attributes, &iosb, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE);
    if (NT_SUCCESS(status))
        NtClose(handle);
    return status;
}

static void Stay(void) {
    for (;;)
        Sleep(1000);
}

int main(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("OXOS XBE launcher\n\n");

    char text[300] = {0};
    FILE *f = fopen("D:\\launch.txt", "rb");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    text[n] = 0;
    char *start = text;
    while (*start == ' ' || *start == '\t')
        start++;
    char *end = start + strcspn(start, "\r\n");
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = 0;
    for (char *p = start; *p; p++)
        if (*p == '/')
            *p = '\\';
    if (!*start) {
        debugPrint("D:\\launch.txt is missing or empty.\n");
        Stay();
    }

    char path[300];
    if (!strncmp(start, "\\Device\\", 8)) {
        snprintf(path, sizeof(path), "%s", start);
    } else {
        const char *partition = start[1] == ':' ? PartitionOf(start[0]) : NULL;
        if (!partition) {
            debugPrint("Unknown drive in \"%s\" (C, E, F, G, X, Y, Z or a \\Device\\ path).\n", start);
            Stay();
        }
        snprintf(path, sizeof(path), "%s%s%s", partition, start[2] == '\\' ? "" : "\\", start + 2);
    }

    debugPrint("%s\n= %s\n\n", start, path);
    NTSTATUS status = OpenStatus(path);
    if (!NT_SUCCESS(status)) {
        debugPrint("Cannot open it: status 0x%08lx\n", (unsigned long)status);
        Stay();
    }
    debugPrint("Launching...\n");
    Sleep(500);
    XLaunchXBE(path);
    debugPrint("The launch failed.\n");
    Stay();
    return 0;
}
