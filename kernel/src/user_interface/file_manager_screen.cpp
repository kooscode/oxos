// The OXOS File Manager's screen (spec/oxos_internal/32 §5): a folder with an arrow up on it (uploads), the title, the server's address and a status line;
// drawn once, then only the address and the status. Boot-only: the server (webserver/webserver.cpp) shows it before the first title starts.
#include <user_interface/user_interface.h>
#include <graphics/graphics.h>
#include <runtime/runtime.h>
#include <memory/memory.h>

namespace xk::user_interface {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

static ULONG *FileManagerScreen; // its frame buffer while it is shown

constexpr ULONG COLOR_GREEN = 0x88FF66, COLOR_DARK_GREEN = 0x55BB33;
// The lines that change: the address (large) and the status.
constexpr ULONG ADDRESS_LINE = 272, STATUS_LINE = 380;

// A folder in the place of an error's digits, with an arrow up on it: uploads.
static void DrawFolder(ULONG *Screen) {
    graphics::FillRectangle(Screen, 250, 76, 60, 16, COLOR_DARK_GREEN);  // the tab
    graphics::FillRectangle(Screen, 250, 90, 140, 92, COLOR_DARK_GREEN); // the back
    graphics::FillRectangle(Screen, 250, 104, 140, 78, COLOR_GREEN);     // the front
    graphics::FillRectangle(Screen, 314, 128, 12, 40, 0);                // the arrow's shaft
    for (ULONG row = 0; row < 18; row++)                                 // its head
        graphics::FillRectangle(Screen, 320 - row - 1, 112 + row, 2 * row + 2, 1, 0);
}

static void DrawFileManagerState(ULONG *Screen, const char *Address, const char *Status) {
    char line[graphics::SCREEN_LINE_CHARS + 1];
    graphics::ClearRectangle(Screen, 0, ADDRESS_LINE, graphics::SCREEN_WIDTH, 24);
    if (Address && Address[0])
        snprintf(line, sizeof(line), "http://%s/", Address);
    else
        snprintf(line, sizeof(line), "(no network address yet)");
    graphics::DrawCentredText(Screen, ADDRESS_LINE, line, 3, COLOR_GREEN);
    graphics::ClearRectangle(Screen, 0, STATUS_LINE, graphics::SCREEN_WIDTH, 16);
    graphics::DrawCentredText(Screen, STATUS_LINE, Status ? Status : "", 2, graphics::COLOR_GREY);
}

void ShowFileManagerScreen(const char *Address, const char *Status) {
    if (FileManagerScreen)
        return DrawFileManagerState(FileManagerScreen, Address, Status);
    ULONG *screen = graphics::OpenScreen();
    if (!screen)
        return;
    DrawFolder(screen);
    graphics::DrawCentredText(screen, 204, "OXOS File Manager", 3, COLOR_GREEN);
    graphics::DrawCentredText(screen, 244, "Open this address in a browser:", 2, graphics::COLOR_TEXT);
    graphics::DrawCentredText(screen, 316, "[ Upload files, manage folders, launch XBEs ]", 2, graphics::COLOR_TEXT);
    graphics::DrawCentredText(screen, 430, "Launch an XBE, or turn the console off.", 2, graphics::COLOR_GREY);
    DrawFileManagerState(screen, Address, Status);
    graphics::ShowScreen(MmGetPhysicalAddress(screen));
    FileManagerScreen = screen;
}

void CloseFileManagerScreen() {
    graphics::CloseScreen(&FileManagerScreen);
}

} // namespace xk::user_interface
