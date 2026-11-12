// Launch redirect file parser (spec/oxos_internal/33 §1, kernel/src/titles/launch_redirect.cpp).
#include "kernel_test.h"

#include <cstring>
#include <string>

#include <xk/launch_redirect.h>

namespace {

constexpr const char *LIVE_UPDATER = "\\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe";
constexpr const char *XBL_UPDATER = "\\Device\\Harddisk0\\Partition1\\Apps\\XBLive\\default.xbe";

struct Result {
    bool Found;
    std::string Target;
    std::string Name;
    ULONG Errors;
};

Result Lookup(const std::string &text, const char *path) {
    XE_REDIRECT redirect = {};
    ULONG errors = 99;
    bool found = XeRedirectLookup(text.data(), (ULONG)text.size(), path, &redirect, &errors);
    return {found, found ? redirect.Target : "", found ? redirect.Name : "", errors};
}

std::string Device(const std::string &path) {
    char out[XE_REDIRECT_MAX_PATH + 1];
    return XeRedirectDevicePath(path.data(), (ULONG)path.size(), out) ? out : "(refused)";
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
    std::string longest = "\\" + std::string(XE_REDIRECT_MAX_PATH - 1, 'a');
    EXPECT_EQ(Device(longest), longest);
    EXPECT_EQ(Device(longest + "a"), "(refused)");
}

TEST(LaunchRedirect, TheXboxLiveUpdaterIsRedirected) {
    const std::string file = "; XBL\r\n"
                             "[Xbox Live updater]   ; replaces the legacy one\r\n"
                             "source = C:\\xodash\\xonlinedash.xbe\r\n"
                             "target = E:\\Apps\\XBLive\\default.xbe\r\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, XBL_UPDATER);
    EXPECT_EQ(r.Name, "Xbox Live updater");
    EXPECT_EQ(r.Errors, 0u);
}

TEST(LaunchRedirect, LettersMatchInAnyCase) {
    const std::string file = "[a]\nsource = c:\\XODASH\\XOnlineDash.XBE\ntarget = e:\\x.xbe\n";
    EXPECT_TRUE(Lookup(file, LIVE_UPDATER).Found);
}

TEST(LaunchRedirect, OtherPathsAreNotRedirected) {
    const std::string file = "[a]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\x.xbe\n";
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition2\\xodash\\xonlinedash.xbe2").Found);
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition2\\xodash").Found);
    EXPECT_FALSE(Lookup(file, "\\Device\\Harddisk0\\Partition1\\xodash\\xonlinedash.xbe").Found);
    EXPECT_FALSE(Lookup("", LIVE_UPDATER).Found);
}

TEST(LaunchRedirect, TheFirstMatchingSectionDecides) {
    const std::string file = "[first]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\one.xbe\n"
                             "[second]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\two.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_EQ(r.Name, "first");
    EXPECT_EQ(r.Target, "\\Device\\Harddisk0\\Partition1\\one.xbe");
}

TEST(LaunchRedirect, TargetBeforeSourceAndOtherKeys) {
    const std::string file = "[a]\ntarget = E:\\x.xbe\nnote = later use\nsource = C:\\xodash\\xonlinedash.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Errors, 0u);
}

TEST(LaunchRedirect, ValuesKeepSemicolons) {
    const std::string file = "[a]\nsource = C:\\odd;name.xbe\ntarget = E:\\x;y.xbe\n";
    Result r = Lookup(file, "\\Device\\Harddisk0\\Partition2\\odd;name.xbe");
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Target, "\\Device\\Harddisk0\\Partition1\\x;y.xbe");
}

TEST(LaunchRedirect, IncompleteSectionsAndBadLinesAreCountedAndSkipped) {
    const std::string file = "source = C:\\before\\any\\section.xbe\n" // outside a section
                             "[no target]\nsource = C:\\xodash\\xonlinedash.xbe\n"
                             "[no source]\ntarget = E:\\x.xbe\n"
                             "[bad drive]\nsource = Q:\\a.xbe\ntarget = E:\\x.xbe\n"
                             "[twice]\nsource = C:\\a.xbe\nsource = C:\\b.xbe\ntarget = E:\\x.xbe\n"
                             "[unclosed\n"
                             "a line without an equals sign\n"
                             "[good]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\Apps\\XBLive\\default.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Name, "good");
    EXPECT_EQ(r.Target, XBL_UPDATER);
    // The line outside a section, [no target], [no source], the Q: line and [bad drive] itself, the second source,
    // [unclosed (no ']') and the line without '=' after it.
    EXPECT_EQ(r.Errors, 8u);
}

TEST(LaunchRedirect, ErrorsAfterTheMatchStillCount) {
    const std::string file = "[a]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\x.xbe\n[b]\nsource = C:\\y.xbe\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_TRUE(r.Found);
    EXPECT_EQ(r.Errors, 1u);
}

TEST(LaunchRedirect, LongNamesAreCut) {
    const std::string file = "[" + std::string(200, 'n') + "]\nsource = C:\\xodash\\xonlinedash.xbe\ntarget = E:\\x\n";
    Result r = Lookup(file, LIVE_UPDATER);
    EXPECT_EQ(r.Name, std::string(XE_REDIRECT_MAX_NAME, 'n'));
}
