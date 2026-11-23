// In-game shortcuts (spec/oxos_internal/48): the kernel's pad report decoding (src/hardware/usb/pad_reports.h, spec/oxos_internal/22 §9.2), and the
// oxos_igs extension's ini reading and firing rule (kernel/extensions/oxos_igs/shortcuts.h), on recorded pad states.
#include "kernel_test.h"

#include <hardware/usb/pad_reports.h>

#include "../../kernel/extensions/oxos_igs/shortcuts.h"

using namespace oxos::igs;

namespace {

constexpr ULONG DASHBOARD_BUTTONS = OXOS_PAD_LEFT_TRIGGER | OXOS_PAD_RIGHT_TRIGGER | OXOS_PAD_BACK | OXOS_PAD_BLACK;

// A shortcut as ReadShortcut makes it from Buttons and Action, which must be valid.
SHORTCUT MakeShortcut(const char *Name, const char *Buttons, const char *Action, const char *Target = nullptr) {
    SHORTCUT shortcut;
    EXPECT_EQ(ReadShortcut(Name, {Buttons, Action, Target, nullptr, nullptr, nullptr}, shortcut), nullptr);
    return shortcut;
}

} // namespace

TEST(PadReports, DecodesButtonsAnalogValuesAndSticks) {
    // 00 14, START and BACK, reserved, A full, BLACK past half way, WHITE just under, LT and RT full, sticks LX -2 LY 0x1234 RX 0 RY -32768.
    const UCHAR report[20] = {0x00, 0x14, 0x30, 0x00, 0xFF, 0, 0, 0, 0x81, 0x80, 0xFF, 0xFF, 0xFE, 0xFF, 0x34, 0x12, 0x00, 0x00, 0x00, 0x80};
    OXOS_PAD_STATE state = {};
    ASSERT_TRUE(xk::hardware::usb::DecodePadReport(report, state));
    EXPECT_EQ(state.Buttons, (ULONG)(OXOS_PAD_START | OXOS_PAD_BACK | OXOS_PAD_A | OXOS_PAD_BLACK | OXOS_PAD_LEFT_TRIGGER | OXOS_PAD_RIGHT_TRIGGER));
    EXPECT_EQ(state.Analog[4], 0x81);
    EXPECT_EQ(state.Analog[5], 0x80);
    EXPECT_EQ(state.ThumbLX, -2);
    EXPECT_EQ(state.ThumbLY, 0x1234);
    EXPECT_EQ(state.ThumbRY, -32768);
}

TEST(PadReports, RefusesWhatIsNotAPadReport) {
    UCHAR report[20] = {0x00, 0x06}; // another report of the pad (its rumble's length)
    OXOS_PAD_STATE state = {};
    EXPECT_FALSE(xk::hardware::usb::DecodePadReport(report, state));
    report[0] = 0x01;
    report[1] = 0x14;
    EXPECT_FALSE(xk::hardware::usb::DecodePadReport(report, state));
}

TEST(PadReports, RefusesThePadsCapabilities) {
    // A game asking for the pad's capabilities: the answer, as the done list had it on a console (2026-11-23). Read as buttons it held every one, the in-game
    // reset's combination included.
    UCHAR capabilities[20] = {0x00, 0x14, 0xFF, 0x00};
    for (int i = 4; i < 20; i++)
        capabilities[i] = 0xFF;
    OXOS_PAD_STATE state = {};
    EXPECT_FALSE(xk::hardware::usb::DecodePadReport(capabilities, state));
    UCHAR leftRight[20] = {0x00, 0x14, 0x0C};
    EXPECT_FALSE(xk::hardware::usb::DecodePadReport(leftRight, state));
    UCHAR upLeft[20] = {0x00, 0x14, 0x05};
    EXPECT_TRUE(xk::hardware::usb::DecodePadReport(upLeft, state)) << "a diagonal is a real d-pad state";
}

TEST(InGameShortcuts, ReadsTheShippedShortcuts) {
    SHORTCUT shortcut;
    ASSERT_EQ(ReadShortcut("dashboard", {"ltrigger + rtrigger + back + black", "coldboot", nullptr, "IN GAME RESET", nullptr, nullptr}, shortcut),
              nullptr);
    EXPECT_STREQ(shortcut.Name, "dashboard");
    EXPECT_EQ(shortcut.Buttons, DASHBOARD_BUTTONS);
    EXPECT_EQ(shortcut.Action, SHORTCUT_ACTION::COLD_BOOT);
    EXPECT_STREQ(shortcut.ToastMessage, "IN GAME RESET");
    EXPECT_EQ(shortcut.ToastSeconds, TOAST_TIMEOUT_DEFAULT);
    EXPECT_EQ(shortcut.LaunchDelaySeconds, 1u) << "the default: a second to read the toast";
    ASSERT_EQ(ReadShortcut("ftp", {"black + white", "direct", "C:\\OXOS\\apps\\oxos_ftp.xbe", nullptr, nullptr, nullptr}, shortcut), nullptr);
    EXPECT_EQ(shortcut.Buttons, (ULONG)(OXOS_PAD_BLACK | OXOS_PAD_WHITE));
    EXPECT_EQ(shortcut.Action, SHORTCUT_ACTION::DIRECT);
    EXPECT_STREQ(shortcut.Target, "\\Device\\Harddisk0\\Partition2\\OXOS\\apps\\oxos_ftp.xbe");
    EXPECT_STREQ(shortcut.ToastMessage, "");
    ASSERT_EQ(ReadShortcut("quick", {"a + b", "warmboot", nullptr, "Resetting", "5", "0"}, shortcut), nullptr);
    EXPECT_EQ(shortcut.ToastSeconds, 5u);
    EXPECT_EQ(shortcut.LaunchDelaySeconds, 0u) << "0: at once";
}

TEST(InGameShortcuts, ReadsEveryButtonNameInAnyCaseAndSpacing) {
    ULONG buttons;
    ASSERT_TRUE(ReadButtons("padup+paddown + padleft+ padright +start\t+ back + lthumb + rthumb + a + b + x + y + black + white + ltrigger + rtrigger",
                            buttons));
    EXPECT_EQ(buttons, 0xFFFFu);
    ASSERT_TRUE(ReadButtons(" LTrigger + RTRIGGER ", buttons));
    EXPECT_EQ(buttons, (ULONG)(OXOS_PAD_LEFT_TRIGGER | OXOS_PAD_RIGHT_TRIGGER));
    char text[160];
    DescribeButtons(DASHBOARD_BUTTONS, text, sizeof(text));
    EXPECT_STREQ(text, "back + black + ltrigger + rtrigger");
}

TEST(InGameShortcuts, LeavesOutBrokenShortcuts) {
    SHORTCUT shortcut;
    EXPECT_STREQ(ReadShortcut("s", {nullptr, "coldboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "no buttons = line");
    EXPECT_STREQ(ReadShortcut("s", {"black + select", "coldboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "buttons: a name that is not a button");
    EXPECT_STREQ(ReadShortcut("s", {"black white", "coldboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "buttons: a name that is not a button");
    EXPECT_STREQ(ReadShortcut("s", {"black + ", "coldboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "buttons: a name that is not a button");
    EXPECT_STREQ(ReadShortcut("s", {"black + black", "coldboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "buttons: fewer than two");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "reboot", nullptr, nullptr, nullptr, nullptr}, shortcut), "action: not coldboot, warmboot or direct");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", nullptr, nullptr, nullptr, nullptr, nullptr}, shortcut), "action: not coldboot, warmboot or direct");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "direct", "Q:\\a.xbe", nullptr, nullptr, nullptr}, shortcut),
                 "target: not on a drive OXOS knows (C D E F G X Y Z), or too long");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "direct", "apps\\a.xbe", nullptr, nullptr, nullptr}, shortcut),
                 "target: not on a drive OXOS knows (C D E F G X Y Z), or too long");
    std::string longMessage(43, 'x');
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "warmboot", nullptr, longMessage.c_str(), nullptr, nullptr}, shortcut),
                 "toast_message: longer than 42 characters");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "warmboot", nullptr, nullptr, "0", nullptr}, shortcut), "toast_timeout: not 1 to 30 (seconds)");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "warmboot", nullptr, nullptr, "31", nullptr}, shortcut), "toast_timeout: not 1 to 30 (seconds)");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "warmboot", nullptr, nullptr, nullptr, "6"}, shortcut), "launch_delay: not 0 to 5 (seconds)");
    EXPECT_STREQ(ReadShortcut("s", {"black + white", "warmboot", nullptr, nullptr, nullptr, "0.5"}, shortcut), "launch_delay: not 0 to 5 (seconds)");
}

TEST(InGameShortcuts, TurnsDrivePathsIntoDevicePaths) {
    char out[TARGET_MAX];
    ASSERT_TRUE(ReadTarget("e:/Apps/XBMC/default.xbe", out, sizeof(out)));
    EXPECT_STREQ(out, "\\Device\\Harddisk0\\Partition1\\Apps\\XBMC\\default.xbe");
    ASSERT_TRUE(ReadTarget("F:\\Titles\\a\\default.xbe", out, sizeof(out)));
    EXPECT_STREQ(out, "\\Device\\Harddisk0\\Partition6\\Titles\\a\\default.xbe");
    ASSERT_TRUE(ReadTarget("D:\\default.xbe", out, sizeof(out)));
    EXPECT_STREQ(out, "\\Device\\CdRom0\\default.xbe");
    ASSERT_TRUE(ReadTarget("\\Device\\Harddisk0\\Partition7\\a.xbe", out, sizeof(out)));
    EXPECT_STREQ(out, "\\Device\\Harddisk0\\Partition7\\a.xbe");
    EXPECT_FALSE(ReadTarget("C:a.xbe", out, sizeof(out)));
    std::string tooLong = "C:\\" + std::string(340, 'a');
    EXPECT_FALSE(ReadTarget(tooLong.c_str(), out, sizeof(out)));
}

TEST(InGameShortcuts, FiresOnExactlyItsButtonsOnceUntilReleased) {
    const SHORTCUT shortcuts[] = {MakeShortcut("dashboard", "ltrigger + rtrigger + back + black", "coldboot")};
    ULONG blocked = 0;
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, 0, blocked), -1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, OXOS_PAD_LEFT_TRIGGER | OXOS_PAD_RIGHT_TRIGGER, blocked), -1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS | OXOS_PAD_A, blocked), -1) << "one more button held: not its buttons";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, 0xFFFF, blocked), -1) << "everything held (the pad's capabilities read as buttons)";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS, blocked), 0);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS | OXOS_PAD_A, blocked), -1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS, blocked), -1) << "still held: no second firing";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, OXOS_PAD_BLACK, blocked), -1) << "one button still held";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS, blocked), -1) << "not all were released";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, 0, blocked), -1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 1, DASHBOARD_BUTTONS, blocked), 0) << "released, then held again";
}

TEST(InGameShortcuts, OnlyTheShortcutWithExactlyTheHeldButtonsFires) {
    const SHORTCUT shortcuts[] = {MakeShortcut("one", "black + white", "coldboot"), MakeShortcut("two", "white + black + a", "warmboot"),
                                  MakeShortcut("three", "a + b", "direct")};
    ULONG blocked = 0;
    EXPECT_EQ(FindFiringShortcut(shortcuts, 3, OXOS_PAD_BLACK | OXOS_PAD_WHITE | OXOS_PAD_A, blocked), 1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 3, OXOS_PAD_BLACK | OXOS_PAD_WHITE, blocked), -1) << "one was held all along (A let go)";
    EXPECT_EQ(FindFiringShortcut(shortcuts, 3, 0, blocked), -1);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 3, OXOS_PAD_BLACK | OXOS_PAD_WHITE, blocked), 0);
    EXPECT_EQ(FindFiringShortcut(shortcuts, 3, OXOS_PAD_A | OXOS_PAD_B, blocked), 2);
}
