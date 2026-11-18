// The kernel's ini line reader (shared/ini_file/ini_file.cpp), shared by oxos_title_patches.ini (spec/oxos_internal/25 §3.1), oxos_settings.ini
// (spec/oxos_internal/19, 33 §1, 38) and oxos_extensions.ini (spec/oxos_internal/34).
#include "kernel_test.h"

#include <string>
#include <vector>

#include <ini_file/ini_file.h>

using namespace xk; // the kernel's subsystems as storage::, titles::, ...

namespace {

// Each line the reader returns, as "S:text", "K:key=value", "BS" or "BL".
std::vector<std::string> Lines(const std::string &text) {
    INI_READER reader = {text.data(), text.data() + text.size()};
    INI_LINE line;
    std::vector<std::string> out;
    while (oxos::ini_file::NextLine(&reader, &line)) {
        switch (line.Kind) {
        case INI_LINE_KIND::SECTION:
            out.push_back("S:" + std::string(line.Text, line.TextLength));
            break;
        case INI_LINE_KIND::KEY:
            out.push_back("K:" + std::string(line.Text, line.TextLength) + "=" +
                          std::string(line.Value, line.ValueLength));
            break;
        case INI_LINE_KIND::BAD_SECTION:
            out.push_back("BS");
            break;
        case INI_LINE_KIND::BAD_LINE:
            out.push_back("BL");
            break;
        }
    }
    return out;
}

using V = std::vector<std::string>;

} // namespace

TEST(IniFile, SectionsKeysAndComments) {
    EXPECT_EQ(Lines("; comment\r\n# also\r\n\r\n  [one]  ; note\r\nkey = value  \r\n\tother=x ; kept\r\n"), (V{"S:one", "K:key=value", "K:other=x ; kept"}));
}

TEST(IniFile, ValuesMayBeEmptyOrHoldEquals) {
    EXPECT_EQ(Lines("a =\nb = c=d\n"), (V{"K:a=", "K:b=c=d"}));
}

TEST(IniFile, BrokenLines) {
    EXPECT_EQ(Lines("[open\n[x] extra\nno equals\ntwo words = 1\n= 2\n"), (V{"BS", "BS", "BL", "BL", "BL"}));
}

TEST(IniFile, LastLineWithoutNewline) {
    EXPECT_EQ(Lines("[a]\nk = v"), (V{"S:a", "K:k=v"}));
}

TEST(IniFile, KeysMatchInAnyCase) {
    const std::string text = "SoUrCe = x";
    INI_READER reader = {text.data(), text.data() + text.size()};
    INI_LINE line;
    ASSERT_TRUE(oxos::ini_file::NextLine(&reader, &line));
    EXPECT_TRUE(oxos::ini_file::IsKey(line, "source"));
    EXPECT_FALSE(oxos::ini_file::IsKey(line, "sourc"));
    EXPECT_FALSE(oxos::ini_file::IsKey(line, "sources"));
}
