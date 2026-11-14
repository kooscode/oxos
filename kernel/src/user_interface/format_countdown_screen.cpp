// The countdown before the kernel formats a blank or foreign disk (spec/oxos_internal/30 §4): the disk, what it holds
// and the seconds left, in large digits and in the text; drawn once, then only the seconds, until the countdown ends.
#include <user_interface/user_interface.h>
#include <storage/storage.h>
#include <graphics/graphics.h>
#include <runtime/runtime.h>
#include <memory/memory.h>

namespace xk::user_interface {

static ULONG *CountdownScreen; // the countdown's frame buffer while it is shown

// The line with the seconds (line 5 of the text) and the seconds in large digits.
constexpr ULONG COUNTDOWN_LINE = 244 + 4 * graphics::SCREEN_LINE;

static void DrawSeconds(ULONG *Screen, ULONG Seconds) {
    graphics::ClearRectangle(Screen, 250, 72, 140, 110);
    graphics::DrawLargeDigit(Screen, 330, 72, Seconds % 10);
    graphics::DrawLargeDigit(Screen, 250, 72, Seconds / 10 % 10);
    graphics::ClearRectangle(Screen, graphics::SCREEN_LEFT, COUNTDOWN_LINE,
                             graphics::SCREEN_WIDTH - 2 * graphics::SCREEN_LEFT, 16);
    char line[graphics::SCREEN_LINE_CHARS + 1];
    RtlSnprintf(line, sizeof(line), "In %lu s OXOS erases it and formats it as", Seconds);
    graphics::DrawText(Screen, graphics::SCREEN_WIDTH, graphics::SCREEN_LEFT, COUNTDOWN_LINE, line, 2,
                       graphics::COLOR_WHITE);
}

void ShowFormatCountdown(const char *Title, const char *What, ULONG Seconds) {
    if (!Title)
        return graphics::CloseScreen(&CountdownScreen);
    if (CountdownScreen)
        return DrawSeconds(CountdownScreen, Seconds);
    ULONG *screen = graphics::OpenScreen();
    if (!screen)
        return;
    graphics::DrawScreenTitle(screen, 204, Title);
    char lines[8][graphics::SCREEN_LINE_CHARS + 1] = {};
    hardware::ide::DiskModelLine(lines[0], sizeof(lines[0]));
    RtlSnprintf(lines[1], sizeof(lines[1]), "Size: %lu GB", (ULONG)(hardware::ide::DiskLengthInSectors / 1953125));
    RtlSnprintf(lines[2], sizeof(lines[2]), "Holds: %s", What);
    RtlSnprintf(lines[5], sizeof(lines[5]), "exFAT, with all required folders.");
    RtlSnprintf(lines[7], sizeof(lines[7]), "  !! TURN OFF POWER TO SKIP FORMATTING !!");
    for (ULONG i = 0; i < 8; i++) {
        graphics::DrawText(screen, graphics::SCREEN_WIDTH, graphics::SCREEN_LEFT, 244 + i * graphics::SCREEN_LINE,
                           lines[i], 2, i < 3 ? graphics::COLOR_TEXT : graphics::COLOR_WHITE);
    }
    DrawSeconds(screen, Seconds);
    graphics::ShowScreen(MmGetPhysicalAddress(screen));
    CountdownScreen = screen;
}

} // namespace xk::user_interface
