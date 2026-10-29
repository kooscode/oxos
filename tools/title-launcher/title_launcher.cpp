// Title launcher (nxdk, open source): lists the titles in F:\games and F:\titles (and their game folders) and starts
// the one chosen with the controller.
//   D-pad up / down: move (held: repeats), A: start the selected title.
// Listed: .iso and .cci disc images (a split image only by its first part, name.1.iso / name.1.cci) and .xbe files.
// A disc image is mounted as the console's disc (tools/iso-launcher/mount_iso.h) and its default.xbe started; an
// .xbe is started from where it is. Starting goes through the launch data page and a quick reboot, as a dashboard.
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <usbh_lib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <xid_driver.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../iso-launcher/mount_iso.h"

namespace {

// The folders listed, on F: (exFAT partition 6); entries are kept as "folder\\file".
constexpr const char *FolderDevice = "\\Device\\Harddisk0\\Partition6"; // F:
constexpr const char *FolderShown = "F:";
constexpr const char *Folders[] = {"games", "titles"};
// nxdk's text output (hal/debug.c) fits about 23 lines of 65 characters on 640x480 (16-pixel font, 25-pixel
// margins) and clears the whole screen when text runs past the bottom: 3 header lines, the list, 2 message lines.
constexpr int VisibleRows = 16, NameWidth = 60;

// --- The list ----------------------------------------------------------------------------------------------------

bool HasExtension(const std::string &name, const char *extension) {
    return oxos_vdisc::EndsWith(name.c_str(), extension);
}

// A later part of a split image (name.2.iso, name.3.cci, ...): only the first part is listed.
bool IsLaterPart(const std::string &name) {
    std::size_t n = name.size();
    return n > 6 && name[n - 6] == '.' && name[n - 5] >= '0' && name[n - 5] <= '9' && name[n - 5] != '1' &&
           (HasExtension(name, ".iso") || HasExtension(name, ".cci"));
}

bool Listed(const std::string &name) {
    return (HasExtension(name, ".iso") || HasExtension(name, ".cci") || HasExtension(name, ".xbe")) &&
           !IsLaterPart(name);
}

bool LessNoCase(const std::string &a, const std::string &b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); i++) {
        char x = a[i] | 0x20, y = b[i] | 0x20;
        if (x != y)
            return x < y;
    }
    return a.size() < b.size();
}

// The files of one folder that can be started, added as "folder\\file". A game folder one level down
// (F:\\games\\Halo\\) is listed by its default.xbe (a launcher made by tools/oxos-isonator, or the game itself)
// when it has one, else by the images and XBEs in it.
NTSTATUS ReadFolder(const std::string &sub, std::vector<std::string> &list, int depth = 0) {
    char path[300];
    std::snprintf(path, sizeof(path), "%s\\%s", FolderDevice, sub.c_str());
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE folder;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    NTSTATUS status = NtOpenFile(&folder, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &iosb,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status))
        return status;
    alignas(8) static unsigned char buffer[4096];
    std::vector<std::string> folders;
    for (bool first = true;; first = false) {
        status = NtQueryDirectoryFile(folder, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer),
                                      FileDirectoryInformation, nullptr, first);
        if (!NT_SUCCESS(status))
            break; // STATUS_NO_MORE_FILES at the end
        for (ULONG offset = 0;;) {
            auto entry = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer + offset);
            std::string file(entry->FileName, entry->FileNameLength);
            if (!(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Listed(file))
                list.push_back(sub + "\\" + file);
            else if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && depth == 0)
                folders.push_back(sub + "\\" + file);
            if (!entry->NextEntryOffset)
                break;
            offset += entry->NextEntryOffset;
        }
    }
    NtClose(folder);
    for (const auto &game : folders) { // after closing: the buffer is shared
        std::vector<std::string> inside;
        ReadFolder(game, inside, depth + 1);
        std::string launcher = game + "\\default.xbe";
        bool hasLauncher = false;
        for (const auto &file : inside)
            hasLauncher |= !LessNoCase(file, launcher) && !LessNoCase(launcher, file); // the same, in any case
        if (hasLauncher)
            list.push_back(launcher);
        else
            list.insert(list.end(), inside.begin(), inside.end());
    }
    return STATUS_SUCCESS;
}

// The files in F:\games and F:\titles that can be started, sorted by name; a failure only when neither opens.
NTSTATUS ReadList(std::vector<std::string> &list) {
    list.clear();
    NTSTATUS status = STATUS_OBJECT_PATH_NOT_FOUND;
    for (const char *sub : Folders)
        if (NT_SUCCESS(ReadFolder(sub, list)))
            status = STATUS_SUCCESS;
    std::sort(list.begin(), list.end(), LessNoCase);
    return status;
}

// --- The controller ----------------------------------------------------------------------------------------------

// The latest report of the first controller connected (0 when none).
volatile unsigned short PadButtons;
volatile unsigned char PadA;
volatile unsigned int PadUid, PadPresent;

void ReadDone(UTR_T *utr) {
    auto dev = static_cast<xid_dev_t *>(utr->context);
    if (utr->status < 0 || !dev)
        return;
    if (PadPresent && dev->uid == PadUid && utr->xfer_len >= static_cast<int>(sizeof(xid_gamepad_in))) {
        auto in = reinterpret_cast<const xid_gamepad_in *>(utr->buff);
        PadButtons = in->dButtons;
        PadA = in->a;
    }
    utr->xfer_len = 0;
    utr->bIsTransferDone = 0;
    usbh_int_xfer(utr);
}

void Connected(xid_dev_t *dev, int) {
    if (PadPresent || usbh_xid_get_type(dev) >> 8 != XID_TYPE_GAMECONTROLLER)
        return;
    PadUid = dev->uid;
    PadPresent = 1;
    usbh_xid_read(dev, 0, reinterpret_cast<void *>(&ReadDone)); // the API takes the callback as void *
}

void Disconnected(xid_dev_t *dev, int) {
    if (PadPresent && dev->uid == PadUid) {
        PadPresent = 0;
        PadButtons = 0;
        PadA = 0;
    }
}

constexpr unsigned short DpadUp = 0x01, DpadDown = 0x02;
constexpr unsigned char APressed = 0x30; // analog button: pressed above this

// --- The screen --------------------------------------------------------------------------------------------------

void Draw(const std::vector<std::string> &list, int selected, int top, const char *message) {
    debugResetCursor();
    debugPrint("%-*s\n", NameWidth + 2, "OXOS title launcher - F:\\games, F:\\titles");
    debugPrint("%-*s\n\n", NameWidth + 2, "D-pad: move    A: start");
    for (int row = 0; row < VisibleRows; row++) {
        if (list.empty() && row == 0) {
            debugPrint("%-*s\n", NameWidth + 2, "  (no .iso, .cci or .xbe files)");
            continue;
        }
        int i = top + row;
        if (i < static_cast<int>(list.size()))
            debugPrint("%s %-*.*s\n", i == selected ? ">" : " ", NameWidth, NameWidth, list[i].c_str());
        else
            debugPrint("%-*s\n", NameWidth + 2, "");
    }
    debugPrint("\n%-*.*s\n", NameWidth + 2, NameWidth + 2, message ? message : "");
}

[[noreturn]] void Start(const std::string &file, std::vector<std::string> &list, int selected, int top) {
    static char message[200], path[300];
    if (HasExtension(file, ".xbe")) {
        std::snprintf(path, sizeof(path), "%s\\%s", FolderDevice, file.c_str());
        std::snprintf(message, sizeof(message), "Starting %s ...", file.c_str());
        Draw(list, selected, top, message);
        usbh_core_deinit(); // the USB host stops its DMA before the quick reboot hands memory to the title
        XLaunchXBE(path);
    } else {
        std::snprintf(path, sizeof(path), "%s\\%s", FolderShown, file.c_str());
        NTSTATUS status = MountISO(path);
        if (NT_SUCCESS(status)) {
            std::snprintf(message, sizeof(message), "Mounted %s, starting its title ...", file.c_str());
            Draw(list, selected, top, message);
            usbh_core_deinit();
            XLaunchXBE("\\Device\\CdRom0\\default.xbe");
        }
        std::snprintf(message, sizeof(message), "Cannot mount %s: status 0x%08lx", file.c_str(),
                      static_cast<unsigned long>(status));
        Draw(list, selected, top, message);
    }
    // XLaunchXBE returns only when the launch failed; stay on the message.
    for (;;)
        Sleep(1000);
}

} // namespace

int main() {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugClearScreen();
    usbh_core_init();
    usbh_xid_init();
    usbh_install_xid_conn_callback(Connected, Disconnected);

    std::vector<std::string> list;
    static char message[120];
    NTSTATUS status = ReadList(list);
    if (!NT_SUCCESS(status))
        std::snprintf(message, sizeof(message), "Neither F:\\games nor F:\\titles opens: status 0x%08lx",
                      static_cast<unsigned long>(status));

    int selected = 0, top = 0, held = 0;
    unsigned short lastButtons = 0;
    bool lastA = false, dirty = true;
    for (;;) {
        usbh_pooling_hubs();
        unsigned short buttons = PadButtons;
        bool a = PadA > APressed;
        int move = 0;
        if (buttons & (DpadUp | DpadDown)) {
            // Once on the press, then every 4 frames after holding for 20.
            if (!(lastButtons & (DpadUp | DpadDown)))
                held = 0;
            if (held == 0 || (held >= 20 && held % 4 == 0))
                move = buttons & DpadUp ? -1 : 1;
            held++;
        }
        lastButtons = buttons;
        int count = static_cast<int>(list.size());
        if (move && count) {
            selected = (selected + move + count) % count;
            if (selected < top)
                top = selected;
            if (selected >= top + VisibleRows)
                top = selected - VisibleRows + 1;
            message[0] = 0;
            dirty = true;
        }
        if (a && !lastA && count)
            Start(list[selected], list, selected, top);
        lastA = a;
        if (dirty) {
            Draw(list, selected, top, message);
            dirty = false;
        }
        XVideoWaitForVBlank();
    }
}
