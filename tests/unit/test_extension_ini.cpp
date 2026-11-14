// The extension list's parser (spec/oxos_internal/34 §3.1, kernel/src/extensions/extension_ini.cpp).
#include "kernel_test.h"

#include <string>
#include <vector>

#include <xk/extension_ini.h>

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
    ULONG n = XkParseExtensionList(file.data(), (ULONG)file.size(), entries.data(), max, &errors);
    entries.resize(n);
    return {entries, errors};
}

} // namespace

TEST(ExtensionList, SectionsInTheFilesOrderWithTheirSettings) {
    const std::string file = "; the extensions\r\n"
                             "[oxos_title_patches]\r\nextension = oxos_title_patches.xos\r\n"
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

TEST(ExtensionList, EnabledNoKeepsTheSectionSwitchedOff) {
    Parsed p = Parse("[a]\nextension = a.xos\nenabled = NO\n[b]\nenabled = yes\nextension = b.xos\n");
    ASSERT_EQ(p.Entries.size(), 2u);
    EXPECT_FALSE(p.Entries[0].Enabled);
    EXPECT_TRUE(p.Entries[1].Enabled);
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
    EXPECT_EQ(Text(p.Entries[1].Section), "maybe"); // the bad enabled = line is counted; the section stays enabled
    EXPECT_TRUE(p.Entries[1].Enabled);
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
