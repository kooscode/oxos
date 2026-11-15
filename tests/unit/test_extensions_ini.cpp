// The extension list's parser (spec/oxos_internal/34 §3.1, kernel/src/ini_file/extensions_ini.cpp).
#include "kernel_test.h"

#include <string>
#include <vector>

#include <ini_file/extensions_ini.h>

using namespace xk; // the kernel's subsystems as storage::, extensions::, ...

namespace {

std::string Text(const XK_TEXT &t) {
    return std::string(t.Text, t.Length);
}

struct Parsed {
    std::vector<XK_EXTENSION_ENTRY> Entries;
    ULONG Errors;
};

Parsed Parse(const std::string &file, ULONG max = 8) {
    std::vector<XK_EXTENSION_ENTRY> entries(max);
    ULONG errors = 99;
    ULONG n = ini_file::ParseExtensions(file.data(), (ULONG)file.size(), entries.data(), max, &errors);
    entries.resize(n);
    return {entries, errors};
}

} // namespace

TEST(ExtensionList, SectionsInTheFilesOrderWithTheirSettings) {
    const std::string file = "; the extensions\r\n"
                             "[oxos_title_patches]\r\nextension = oxos_title_patches.xos\r\nenabled = yes\r\n"
                             "\r\n[netlog]\r\nExtension = NetLog.xos\r\nhost = 192.168.1.10\r\nport = 9000\r\n";
    Parsed p = Parse(file);
    ASSERT_EQ(p.Entries.size(), 2u);
    EXPECT_EQ(p.Errors, 0u);
    EXPECT_EQ(Text(p.Entries[0].Section), "oxos_title_patches");
    EXPECT_EQ(Text(p.Entries[0].File), "oxos_title_patches.xos");
    EXPECT_TRUE(p.Entries[0].Enabled);
    EXPECT_EQ(p.Entries[0].SettingCount, 0u);
    EXPECT_EQ(Text(p.Entries[1].File), "NetLog.xos");
    ASSERT_EQ(p.Entries[1].SettingCount, 2u);
    EXPECT_EQ(Text(p.Entries[1].Keys[0]), "host");
    EXPECT_EQ(Text(p.Entries[1].Values[0]), "192.168.1.10");
    EXPECT_EQ(Text(p.Entries[1].Keys[1]), "port");
    EXPECT_EQ(Text(p.Entries[1].Values[1]), "9000");
}

TEST(ExtensionList, OnlyEnabledYesSwitchesASectionOn) {
    Parsed p = Parse("[a]\nextension = a.xos\nenabled = NO\n[b]\nenabled = Yes\nextension = b.xos\n[c]\nextension = c.xos\n");
    ASSERT_EQ(p.Entries.size(), 3u);
    EXPECT_FALSE(p.Entries[0].Enabled);
    EXPECT_TRUE(p.Entries[1].Enabled);
    EXPECT_FALSE(p.Entries[2].Enabled); // no enabled = line: off
    EXPECT_EQ(p.Errors, 0u);
}

TEST(ExtensionList, AnEmptyFileListsNothing) {
    Parsed p = Parse("");
    EXPECT_TRUE(p.Entries.empty());
    EXPECT_EQ(p.Errors, 0u);
}

TEST(ExtensionList, BadSectionsAreLeftOutAndCounted) {
    const std::string file = "extension = outside.xos\n"           // outside a section
                             "[no file]\nnote = x\n"                // no extension =
                             "[path]\nextension = C:\\a.xos\n"      // a path, not a name
                             "[twice]\nextension = a.xos\nextension = b.xos\n"
                             "[good]\nextension = good.xos\n"
                             "[same file]\nextension = GOOD.xos\n"  // a file an earlier section names
                             "[maybe]\nextension = m.xos\nenabled = maybe\n"
                             "[unclosed\n";
    Parsed p = Parse(file);
    ASSERT_EQ(p.Entries.size(), 2u);
    EXPECT_EQ(Text(p.Entries[0].Section), "good");
    EXPECT_EQ(Text(p.Entries[1].Section), "maybe"); // the bad enabled = line is counted; the section stays off
    EXPECT_FALSE(p.Entries[1].Enabled);
    // outside, [no file], [path], [twice], [same file], enabled = maybe, [unclosed.
    EXPECT_EQ(p.Errors, 7u);
}

TEST(ExtensionList, TooManySettingsOrALongValueAreCounted) {
    std::string file = "[a]\nextension = a.xos\nlong = " + std::string(XK_EXTENSION_VALUE_MAX + 1, 'v') + "\n";
    for (ULONG i = 0; i < XK_EXTENSION_SETTINGS_MAX + 2; i++)
        file += "k" + std::to_string(i) + " = " + std::to_string(i) + "\n";
    Parsed p = Parse(file);
    ASSERT_EQ(p.Entries.size(), 1u);
    EXPECT_EQ(p.Entries[0].SettingCount, XK_EXTENSION_SETTINGS_MAX);
    EXPECT_EQ(Text(p.Entries[0].Keys[0]), "k0");
    EXPECT_EQ(p.Errors, 3u);
}

TEST(ExtensionList, SectionsPastTheLimitAreCounted) {
    Parsed p = Parse("[a]\nextension = a.xos\n[b]\nextension = b.xos\n[c]\nextension = c.xos\n", 2);
    ASSERT_EQ(p.Entries.size(), 2u);
    EXPECT_EQ(Text(p.Entries[1].File), "b.xos");
    EXPECT_EQ(p.Errors, 2u); // [c] and its extension = line, outside any kept section
}

TEST(ExtensionList, LongFileNamesAreRefused) {
    std::string name(XK_EXTENSION_FILE_NAME_MAX, 'n');
    Parsed p = Parse("[a]\nextension = " + name + "\n[b]\nextension = " + name + "x\n");
    ASSERT_EQ(p.Entries.size(), 1u);
    EXPECT_EQ(Text(p.Entries[0].File), name);
    EXPECT_EQ(p.Errors, 1u);
}

// An extension's own file (C:\OXOS\extensions\NAME.ini): its settings are the keys before the first section; the sections are its data.
TEST(ExtensionOwnIni, SettingsAreTheKeysBeforeTheFirstSection) {
    std::string file = "; settings\r\nbuttons = back black\nformat = png\n\n[4D530064]\nInfinite Ammo = on\nlater = no\n";
    XK_TEXT keys[XK_EXTENSION_SETTINGS_MAX], values[XK_EXTENSION_SETTINGS_MAX];
    ULONG errors = 99;
    ULONG n = ini_file::ParseOwnSettings(file.data(), (ULONG)file.size(), keys, values, XK_EXTENSION_SETTINGS_MAX, &errors);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(errors, 0u);
    EXPECT_EQ(Text(keys[0]), "buttons");
    EXPECT_EQ(Text(values[0]), "back black");
    EXPECT_EQ(Text(keys[1]), "format");
    EXPECT_EQ(Text(values[1]), "png");
}

TEST(ExtensionOwnIni, AFileOfOnlyDataHasNoSettings) {
    std::string file = "[first]\nkey = value\n";
    XK_TEXT keys[4], values[4];
    ULONG errors = 99;
    EXPECT_EQ(ini_file::ParseOwnSettings(file.data(), (ULONG)file.size(), keys, values, 4, &errors), 0u);
    EXPECT_EQ(errors, 0u);
}

TEST(ExtensionOwnIni, TooManySettingsALongValueOrABadLineAreCounted) {
    std::string file = "a = 1\nb = 2\nc = 3\nnot a setting\nd = " + std::string(XK_EXTENSION_VALUE_MAX + 1, 'x') + "\n";
    XK_TEXT keys[2], values[2];
    ULONG errors = 0;
    EXPECT_EQ(ini_file::ParseOwnSettings(file.data(), (ULONG)file.size(), keys, values, 2, &errors), 2u);
    EXPECT_EQ(errors, 3u); // c past the limit, the line without '=', d's long value
}
