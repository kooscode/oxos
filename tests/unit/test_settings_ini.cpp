// oxos_settings.ini parser (spec/oxos_internal/38 §1, kernel/src/ini_file/settings_ini.cpp).
#include "kernel_test.h"

#include <cstring>
#include <string>

#include <ini_file/settings_ini.h>

using namespace xk; // the kernel's subsystems as storage::, titles::, ...

namespace {

struct Result {
    BOOT_VIDEO_SETTINGS Settings;
    uint32_t Errors;
};

Result Read(const std::string &text) {
    Result result;
    memset(&result.Settings, 0x55, sizeof(result.Settings));
    result.Errors = 99;
    oxos::ini_file::ReadBootVideoSettings(text.data(), (uint32_t)text.size(), &result.Settings, &result.Errors);
    return result;
}

} // namespace

TEST(SettingsIni, BootVideoAsTheSpecShowsIt) {
    Result r = Read("; C:\\OXOS\\oxos_settings.ini\r\n[boot_video]\r\n; on\r\nboot_video_enabled = yes\r\n"
                    "boot_video_file = intro.mpg\r\nboot_video_always = no\r\n");
    EXPECT_TRUE(r.Settings.Enabled);
    EXPECT_FALSE(r.Settings.Always);
    EXPECT_STREQ(r.Settings.File, "intro.mpg");
    EXPECT_EQ(r.Errors, 0u);
}

TEST(SettingsIni, NothingSaidIsOff) {
    Result r = Read("");
    EXPECT_FALSE(r.Settings.Enabled);
    EXPECT_FALSE(r.Settings.Always);
    EXPECT_STREQ(r.Settings.File, "");
    r = Read("[dashboard]\norder = C:\\xboxdash.xbe\n");
    EXPECT_FALSE(r.Settings.Enabled);
    EXPECT_EQ(r.Errors, 0u);
}

TEST(SettingsIni, OnlyYesTurnsASwitchOn) {
    EXPECT_TRUE(Read("[boot_video]\nboot_video_enabled = YES\n").Settings.Enabled);
    EXPECT_TRUE(Read("[Boot_Video]\nBOOT_VIDEO_ALWAYS=Yes\n").Settings.Always);
    EXPECT_FALSE(Read("[boot_video]\nboot_video_enabled = true\n").Settings.Enabled);
    EXPECT_FALSE(Read("[boot_video]\nboot_video_enabled = yes ; on\n").Settings.Enabled); // a ';' after a value is part of it
    EXPECT_FALSE(Read("[boot_video]\nboot_video_enabled =\n").Settings.Enabled);
}

TEST(SettingsIni, KeysOutsideTheSectionAreIgnored) {
    Result r = Read("boot_video_enabled = yes\n[other]\nboot_video_enabled = yes\n[boot_video]\nboot_video_file = a.mpg\n[after]\nboot_video_always = yes\n");
    EXPECT_FALSE(r.Settings.Enabled);
    EXPECT_FALSE(r.Settings.Always);
    EXPECT_STREQ(r.Settings.File, "a.mpg");
}

TEST(SettingsIni, TheFileLosesItsQuotes) {
    EXPECT_STREQ(Read("[boot_video]\nboot_video_file = \"my intro.mpg\"\n").Settings.File, "my intro.mpg");
    EXPECT_STREQ(Read("[boot_video]\nboot_video_file = E:\\Videos\\x.mpg\n").Settings.File, "E:\\Videos\\x.mpg");
    EXPECT_STREQ(Read("[boot_video]\nboot_video_file = \"half.mpg\n").Settings.File, "\"half.mpg");
    std::string longest(BOOT_VIDEO_MAX_FILE, 'a');
    EXPECT_STREQ(Read("[boot_video]\nboot_video_file = " + longest + "\n").Settings.File, longest.c_str());
    EXPECT_STREQ(Read("[boot_video]\nboot_video_file = " + longest + "a\n").Settings.File, "");
}

TEST(SettingsIni, TheLastOfTwoKeysCounts) {
    Result r = Read("[boot_video]\nboot_video_enabled = yes\nboot_video_enabled = no\n[boot_video]\nboot_video_always = no\nboot_video_always = yes\n");
    EXPECT_FALSE(r.Settings.Enabled);
    EXPECT_TRUE(r.Settings.Always);
}

TEST(SettingsIni, UnreadableLinesAreCounted) {
    Result r = Read("[boot_video\nnot a key\n[boot_video]\nboot_video_enabled = yes\n= value\n");
    EXPECT_TRUE(r.Settings.Enabled);
    EXPECT_EQ(r.Errors, 3u);
}

namespace {

std::string Dashboard(const std::string &text, uint32_t number) {
    char path[INI_REDIRECT_MAX_PATH + 1];
    return oxos::ini_file::FindDashboard(text.data(), (uint32_t)text.size(), number, path) ? path : "(none)";
}

} // namespace

TEST(SettingsIni, DashboardsAsTheShippedFileListsThem) {
    std::string text = "; C:\\OXOS\\oxos_settings.ini\r\n[dashboard]\r\n; the order\r\ndashboard_1 = C:\\evoxdash.xbe\r\ndashboard_2 = E:\\evoxdash.xbe\r\n"
                       "dashboard_3 = C:\\xboxdash.xbe\r\ndashboard_4 = C:\\Dashboard\\default.xbe\r\n[boot_video]\r\nboot_video_enabled = no\r\n";
    EXPECT_EQ(Dashboard(text, 1), "\\Device\\Harddisk0\\Partition2\\evoxdash.xbe");
    EXPECT_EQ(Dashboard(text, 2), "\\Device\\Harddisk0\\Partition1\\evoxdash.xbe");
    EXPECT_EQ(Dashboard(text, 3), "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe");
    EXPECT_EQ(Dashboard(text, 4), "\\Device\\Harddisk0\\Partition2\\Dashboard\\default.xbe");
    EXPECT_EQ(Dashboard(text, 5), "(none)");
}

TEST(SettingsIni, DashboardNumbersOutsideTheListAreNone) {
    std::string text = "[dashboard]\ndashboard_0 = C:\\a.xbe\ndashboard_9 = C:\\b.xbe\ndashboard_10 = C:\\c.xbe\n";
    EXPECT_EQ(Dashboard(text, 0), "(none)");
    EXPECT_EQ(Dashboard(text, 9), "(none)");
    EXPECT_EQ(Dashboard(text, 1), "(none)");
}

TEST(SettingsIni, DashboardKeysOnlyInTheirSection) {
    std::string text = "dashboard_1 = C:\\top.xbe\n[boot_video]\ndashboard_1 = C:\\video.xbe\n[Dashboard]\nDASHBOARD_2 = \"F:\\Apps\\XBMC\\default.xbe\"\n";
    EXPECT_EQ(Dashboard(text, 1), "(none)");
    EXPECT_EQ(Dashboard(text, 2), "\\Device\\Harddisk0\\Partition6\\Apps\\XBMC\\default.xbe");
}

TEST(SettingsIni, DashboardPathsOnUnknownDrivesAreNone) {
    std::string text = "[dashboard]\ndashboard_1 = Q:\\x.xbe\ndashboard_2 =\ndashboard_3 = e:/evoxdash.xbe\ndashboard_3 = C:\\last.xbe\n";
    EXPECT_EQ(Dashboard(text, 1), "(none)");
    EXPECT_EQ(Dashboard(text, 2), "(none)");
    EXPECT_EQ(Dashboard(text, 3), "\\Device\\Harddisk0\\Partition2\\last.xbe");
}

TEST(SettingsIni, ToastsAreOnUnlessTurnedOff) {
    const std::string off = "[boot_video]\nenabled = no\n[toasts]\nenabled = No\n";
    EXPECT_FALSE(oxos::ini_file::ReadToastsEnabled(off.data(), (uint32_t)off.size()));
    const std::string on = "[toasts]\nenabled = yes\n";
    EXPECT_TRUE(oxos::ini_file::ReadToastsEnabled(on.data(), (uint32_t)on.size()));
    const std::string other = "[boot_video]\nenabled = no\n";
    EXPECT_TRUE(oxos::ini_file::ReadToastsEnabled(other.data(), (uint32_t)other.size())) << "another section's enabled";
    EXPECT_TRUE(oxos::ini_file::ReadToastsEnabled("", 0));
}

TEST(SettingsIni, VideoModeToastIsOnUnlessTurnedOff) {
    const std::string off = "[toasts]\nenabled = yes\nshow_video_mode = NO\n";
    EXPECT_FALSE(oxos::ini_file::ReadShowVideoMode(off.data(), (uint32_t)off.size()));
    EXPECT_TRUE(oxos::ini_file::ReadToastsEnabled(off.data(), (uint32_t)off.size())) << "only its own key";
    const std::string on = "[toasts]\nshow_video_mode = yes\n";
    EXPECT_TRUE(oxos::ini_file::ReadShowVideoMode(on.data(), (uint32_t)on.size()));
    const std::string other = "[boot_video]\nshow_video_mode = no\n[toasts]\nenabled = no\n";
    EXPECT_TRUE(oxos::ini_file::ReadShowVideoMode(other.data(), (uint32_t)other.size())) << "another section's key, or enabled";
    EXPECT_TRUE(oxos::ini_file::ReadShowVideoMode("", 0));
}
