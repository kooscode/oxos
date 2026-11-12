// Title launcher (nxdk + SDL3, open source): lists the titles in F:\games and F:\titles (and their game folders) and
// the applications in F:\Applications (one folder each, e.g. F:\Applications\ftp\ftp.xbe; F:\Tools, the folder's
// old name, is still read), and starts the one chosen with the controller.
//   D-pad up / down: move (held: repeats), A: start the selected title.
// Listed: .iso and .cci disc images (a split image only by its first part, name.1.iso / name.1.cci) and .xbe files.
// A disc image is mounted as the console's disc (tools/iso_launcher/mount_iso.h) and its default.xbe started; an
// .xbe is started from where it is. Starting goes through the launch data page and a quick reboot, as a dashboard.
// The screen and the controllers are SDL3's, through tools/sdl_screen.
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../iso_launcher/mount_iso.h"
#include "sdl_screen.h"

namespace {

// The folders listed, on F: (exFAT partition 6); entries are kept as "folder\\file".
constexpr const char *FolderDevice = "\\Device\\Harddisk0\\Partition6"; // F:
constexpr const char *FolderShown = "F:";
constexpr const char *Folders[] = {"games", "titles", "Applications", "Tools"};

// The screen's rows: 3 header rows, the list, 2 message rows; a name fills the row after the 2-character marker.
constexpr int HeaderRows = 3;
constexpr int VisibleRows = sdl_screen::ROWS - HeaderRows - 2;
constexpr int NameWidth = sdl_screen::COLUMNS - 2;

// Listed after the games: entries of these folders.
bool IsApplication(const std::string &entry) {
    return entry.rfind("Applications\\", 0) == 0 || entry.rfind("Tools\\", 0) == 0;
}

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
// (F:\\games\\Halo\\) is listed by its default.xbe (a launcher made by tools/oxos_isonator, or the game itself)
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

// The files in F:\games, F:\titles and F:\Applications that can be started, sorted by name, the applications after
// the games; a
// failure only when none opens.
NTSTATUS ReadList(std::vector<std::string> &list) {
    list.clear();
    NTSTATUS status = STATUS_OBJECT_PATH_NOT_FOUND;
    for (const char *sub : Folders) {
        if (NT_SUCCESS(ReadFolder(sub, list)))
            status = STATUS_SUCCESS;
    }
    std::sort(list.begin(), list.end(), [](const std::string &a, const std::string &b) {
        bool appA = IsApplication(a), appB = IsApplication(b);
        return appA != appB ? appB : LessNoCase(a, b);
    });
    return status;
}

// --- The controller ----------------------------------------------------------------------------------------------

// Frames a held D-pad waits before it repeats, and the frames between repeats.
constexpr int RepeatDelay = 20, RepeatEvery = 4;

// --- The screen --------------------------------------------------------------------------------------------------

void Draw(const std::vector<std::string> &list, int selected, int top, const char *message) {
    using sdl_screen::Print;
    sdl_screen::SetCursor(0);
    Print("OXOS title launcher - F:\\games, F:\\titles, F:\\Applications\n");
    Print("D-pad: move    A: start\n\n");
    for (int row = 0; row < VisibleRows; row++) {
        if (list.empty() && row == 0) {
            Print("  (no .iso, .cci or .xbe files)\n");
            continue;
        }
        int i = top + row;
        if (i < static_cast<int>(list.size()))
            Print("%s %.*s\n", i == selected ? ">" : " ", NameWidth, list[i].c_str());
        else
            Print("\n");
    }
    Print("\n%.*s\n", NameWidth + 2, message ? message : "");
    sdl_screen::SetHighlight(list.empty() ? -1 : HeaderRows + selected - top);
    sdl_screen::Present();
}

// Shows the message and starts the title: SDL and the USB host stop first, so nothing is left running into the title.
// Returns only when the launch failed; the screen is open again then.
void Launch(const char *path, const std::vector<std::string> &list, int selected, int top, const char *message) {
    Draw(list, selected, top, message);
    sdl_screen::Close();
    XLaunchXBE(path);
    sdl_screen::Open(true);
}

[[noreturn]] void Start(const std::string &file, std::vector<std::string> &list, int selected, int top) {
    static char message[200], path[300];
    if (HasExtension(file, ".xbe")) {
        std::snprintf(path, sizeof(path), "%s\\%s", FolderDevice, file.c_str());
        std::snprintf(message, sizeof(message), "Starting %s ...", file.c_str());
        Launch(path, list, selected, top, message);
    } else {
        std::snprintf(path, sizeof(path), "%s\\%s", FolderShown, file.c_str());
        NTSTATUS status = MountISO(path);
        if (NT_SUCCESS(status)) {
            std::snprintf(message, sizeof(message), "Mounted %s, starting its title ...", file.c_str());
            Launch("\\Device\\CdRom0\\default.xbe", list, selected, top, message);
        }
        std::snprintf(message, sizeof(message), "Cannot mount %s: status 0x%08lx", file.c_str(),
                      static_cast<unsigned long>(status));
        Draw(list, selected, top, message);
    }
    // XLaunchXBE returns only when the launch failed; stay on the message.
    sdl_screen::ShowForever();
}

} // namespace

int main() {
    if (!sdl_screen::Open(true)) {
        DbgPrint("title launcher: SDL: %s\n", SDL_GetError());
        return 1;
    }

    std::vector<std::string> list;
    static char message[120];
    NTSTATUS status = ReadList(list);
    if (!NT_SUCCESS(status)) {
        std::snprintf(message, sizeof(message), "None of F:\\games, F:\\titles, F:\\Applications opens: 0x%08lx",
                      static_cast<unsigned long>(status));
    }

    int selected = 0, top = 0, held = 0;
    for (;;) {
        sdl_screen::Poll();
        bool up = sdl_screen::Held(SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool down = sdl_screen::Held(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        int move = 0;
        if (up || down) {
            // Once on the press, then every RepeatEvery frames after holding for RepeatDelay.
            if (held == 0 || (held >= RepeatDelay && held % RepeatEvery == 0))
                move = up ? -1 : 1;
            held++;
        } else {
            held = 0;
        }
        int count = static_cast<int>(list.size());
        if (move && count) {
            selected = (selected + move + count) % count;
            if (selected < top)
                top = selected;
            if (selected >= top + VisibleRows)
                top = selected - VisibleRows + 1;
            message[0] = 0;
        }
        if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_SOUTH) && count)
            Start(list[selected], list, selected, top);
        Draw(list, selected, top, message);
    }
}
