// The kernel's own screens (src/user_interface/, spec/oxos_internal/29_error_screens.md), drawn with the kernel's
// graphics (graphics/graphics.h): the fatal error screen (with its LED sequence and EEPROM error history), No Dashboard
// Found and the bug check screen (error_screens.cpp), the countdown before a disk is formatted
// (format_countdown_screen.cpp) and the OXOS File Manager's screen (file_manager_screen.cpp). The boot splash is a
// feature of its own (boot_splash/).
#pragma once
#include <runtime/types.h>
#include <graphics/graphics.h>

// The fatal error screen's text per code (error_screen_text.cpp): a title and lines of at most XK_FATAL_LINE
// characters, ending with nullptr.
constexpr ULONG XK_FATAL_LINE = xk::graphics::SCREEN_LINE_CHARS;
struct XK_FATAL_HELP {
    ULONG First, Last; // the codes it covers
    const char *Title;
    const char *const *Lines;
};

namespace xk::user_interface {

const XK_FATAL_HELP *FatalHelp(ULONG Code);

// The fatal error (spec/stock/05 §6): LED sequence, EEPROM error history, the error screen, then halt.
[[noreturn]] void ShowFatalError(ULONG Code);

// No Dashboard Found (13-20) on a cold boot (spec/oxos_internal/32 §1; boot-only): the fatal error's screen, LED and
// history, keeping the boot-only pages and the keys, until X is pressed; then it returns, the screen closed, for the
// File Manager.
void NoDashboardError(ULONG Code);

// The bug check screen, drawn into the frame buffer on screen (KeBugCheckEx, interrupts off).
void ShowBugCheck(ULONG Code, const ULONG_PTR *Parameters);

// The countdown before a blank or foreign disk is formatted (storage/disk_layout.cpp, spec/oxos_internal/30 §4): Title
// and What (what the disk holds) with Seconds left, redrawn with each call; Title null takes the screen down.
void ShowFormatCountdown(const char *Title, const char *What, ULONG Seconds);

// The File Manager's screen on the TV (webserver/webserver.cpp, spec/oxos_internal/32 §5; boot-only): the server's
// Address (null or "" while it has none) and a Status line, redrawn with each call; closed before the title it launches
// starts.
void ShowFileManagerScreen(const char *Address, const char *Status);
void CloseFileManagerScreen();

} // namespace xk::user_interface
