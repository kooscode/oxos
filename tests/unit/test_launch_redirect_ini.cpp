// Launch redirect file parser (spec/oxos_internal/33 §1, shared/ini_file/launch_redirect_ini.cpp).
#include "kernel_test.h"

#include <cstring>
#include <string>

#include <ini_file/launch_redirect_ini.h>

using namespace xk; // the kernel's subsystems as storage::, titles::, ...

namespace {

constexpr const char *LIVE_UPDATER = "\\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe";
constexpr const char *XBL_UPDATER = "\\Device\\Harddisk0\\Partition1\\Apps\\XBLive\\default.xbe";

struct Result {
    bool Found;
    std::string Target;
    std::string Name;
    uint32_t Errors;
};

Result Lookup(const std::string &text, const char *path) {
    INI_REDIRECT redirect = {};
    uint32_t errors = 99;
    bool found = oxos::ini_file::LookUpRedirect(text.data(), (uint32_t)text.size(), path, &redirect, &errors);
    return {found, found ? redirect.Target : "", found ? redirect.Name : "", errors};
}

std::string Device(const std::string &path) {
    char out[INI_REDIRECT_MAX_PATH + 1];
    return oxos::ini_file::RedirectDevicePath(path.data(), (uint32_t)path.size(), out) ? out : "(refused)";
}

} // namespace

TEST(LaunchRedirect, DriveLettersBecomeDevicePaths) {
    EXPECT_EQ(Device("C:\\xodash\\xonlinedash.xbe"), LIVE_UPDATER);
    EXPECT_EQ(Device("e:/Apps/XBLive/default.xbe"), XBL_UPDATER);
    EXPECT_EQ(Device("D:\\default.xbe"), "\\Device\\CdRom0\\default.xbe");
    EXPECT_EQ(Device("F:games\\a.xbe"), "\\Device\\Harddisk0\\Partition6\\games\\a.xbe");
    EXPECT_EQ(Device("G:\\a.xbe"), "\\Device\\Harddisk0\\Partition7\\a.xbe");
    EXPECT_EQ(Device("X:\\a"), "\\Device\\Harddisk0\\Partition3\\a");
    EXPECT_EQ(Device("Y:\\a"), "\\Device\\Harddisk0\\Partition4\\a");
    EXPECT_EQ(Device("Z:\\a"), "\\Device\\Harddisk0\\Partition5\\a");
    EXPECT_EQ(Device("\\Device\\CdRom0\\media\\second.xbe"), "\\Device\\CdRom0\\media\\second.xbe");
}

TEST(LaunchRedirect, UnknownDrivesAndBadPathsAreRefused) {
    EXPECT_EQ(Device("Q:\\a.xbe"), "(refused)");
    EXPECT_EQ(Device("a.xbe"), "(refused)");
    EXPECT_EQ(Device(""), "(refused)");
    std::string longest = "\\" + std::string(INI_REDIRECT_MAX_PATH - 1, 'a');
    EXPECT_EQ(Device(longest), longest);
    EXPECT_EQ(Device(longest + "a"), "(refused)");
}

TEST(LaunchRedirect, TheXboxLiveUpdaterIsRedirected) {
    const std::string file = "; C:\\OXOS\\oxos_settings.ini\r\n"
                             "[dashboard]\r\ndashboard_1 = C:\\evoxdash.xbe\r\n"
                             "[launch_redirect]\r\n"
                             "; replaces the legacy one\r\n"
                             "xbox_live_updater = C:\\xodash\\xonlinedash.xbe >> E:\\Apps\\XBLive\\default.xbe\r\n"
                             "[boot_video]\r\nboot_video_enabled = no\r\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, XBL_UPDATER);
    EXPECT_EQ(r.Name, "xbox_live_updater");
    EXPECT_EQ(r.Errors, 0u);
}

TEST(LaunchRedirect, BlanksAroundTheArrowAreOptional) {
    Result r = Lookup("[launch_redirect]\na=C:\\xodash\\xonlinedash.xbe>>E:\\Apps\\XBLive\\default.xbe\n", LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, XBL_UPDATER);
    r = Lookup("[launch_redirect]\nmy_xbl_redirect =   C:\\xodash\\xonlinedash.xbe  \t>>   E:\\Apps\\XBLive\\default.xbe\n", LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, XBL_UPDATER);
    EXPECT_EQ(r.Name, "my_xbl_redirect");
}

TEST(LaunchRedirect, LettersMatchInAnyCase) {
    const std::string file = "[Launch_Redirect]\nx = c:\\XODASH\\XOnlineDash.XBE >> e:\\x.xbe\n";
    EXPECT_TRUE(Lookup(file, LIVE_UPDATER).Found);
}

TEST(LaunchRedirect, OtherPathsAreNotRedirected) {
    const std::string file = "[launch_redirect]\nx = C:\\xodash\\xonlinedash.xbe >> E:\\x.xbe\n";
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe2").Found);
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition2\\xodash").Found);
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition1\\xodash\\xonlinedash.xbe").Found);
    EXPECT_FALSE(Lookup("", LIVE_UPDATER).Found);
}

TEST(LaunchRedirect, OnlyTheLaunchRedirectSectionCounts) {
    const std::string file = "top = C:\\xodash\\xonlinedash.xbe >> E:\\top.xbe\n"
                             "[dashboard]\nx = C:\\xodash\\xonlinedash.xbe >> E:\\dash.xbe\ndashboard_1 = C:\\evoxdash.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_FALSE(r.Found);
    EXPECT_EQ(r.Errors, 0u);
}

TEST(LaunchRedirect, TheFirstMatchingLineDecides) {
    const std::string file = "[launch_redirect]\nother = C:\\other.xbe >> E:\\zero.xbe\n"
                             "first = C:\\xodash\\xonlinedash.xbe >> E:\\one.xbe\n"
                             "second = C:\\xodash\\xonlinedash.xbe >> E:\\two.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_EQ(r.Name, "first");
    EXPECT_EQ(r.Target, "\\Device\\Harddisk0\\Partition1\\one.xbe");
}

TEST(LaunchRedirect, ValuesKeepSemicolons) {
    const std::string file = "[launch_redirect]\nx = C:\\odd;name.xbe >> E:\\x;y.xbe\n";
    Result r = Lookup(file, "\\Device\\Harddisk0\\Partition2\\odd;name.xbe");
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, "\\Device\\Harddisk0\\Partition1\\x;y.xbe");
}

TEST(LaunchRedirect, BadLinesAreCountedAndSkipped) {
    const std::string file = "[launch_redirect]\n"
                             "no arrow = C:\\xodash\\xonlinedash.xbe\n"
                             "no target = C:\\xodash\\xonlinedash.xbe >>\n"
                             "no source = >> E:\\x.xbe\n"
                             "one arrow = C:\\xodash\\xonlinedash.xbe > E:\\x.xbe\n"
                             "bad drive = Q:\\a.xbe >> E:\\x.xbe\n"
                             "bad target = C:\\xodash\\xonlinedash.xbe >> Q:\\x.xbe\n"
                             "a line without an equals sign\n"
                             "good = C:\\xodash\\xonlinedash.xbe >> E:\\Apps\\XBLive\\default.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Name, "good");
    EXPECT_EQ(r.Target, XBL_UPDATER);
    EXPECT_EQ(r.Errors, 7u);
}

TEST(LaunchRedirect, ErrorsAfterTheMatchStillCount) {
    const std::string file = "[launch_redirect]\na = C:\\xodash\\xonlinedash.xbe >> E:\\x.xbe\nb = C:\\y.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Errors, 1u);
}

TEST(LaunchRedirect, LongNamesAreCut) {
    const std::string file = "[launch_redirect]\n" + std::string(200, 'n') + " = C:\\xodash\\xonlinedash.xbe >> E:\\x\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_EQ(r.Name, std::string(INI_REDIRECT_MAX_NAME, 'n'));
}
