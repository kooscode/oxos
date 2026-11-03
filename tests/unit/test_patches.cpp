// Title patch file parser (spec/oxos-internal/25 §3, kernel/src/xe/patchfile.cpp).
#include "kernel_test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <xk/patches.h>

namespace {
struct Rec {
    USHORT Kind;
    ULONG Address;
    std::vector<UCHAR> Original, Data;
    ULONG Check;
};

std::vector<Rec> Parse(const std::string &text, ULONG title, ULONG version, XE_PATCH_COUNTS *c = nullptr,
                       ULONG room = 4096) {
    std::vector<UCHAR> out(room + 1, 0xCC);
    XE_PATCH_COUNTS counts;
    ULONG used = XePatchParse(text.data(), (ULONG)text.size(), title, version, out.data(), room, &counts);
    EXPECT_LE(used, room);
    EXPECT_EQ(out[room], 0xCC) << "wrote past the output";
    if (c)
        *c = counts;
    std::vector<Rec> recs;
    for (ULONG at = 0; at < used;) {
        auto r = (const XE_PATCH_RECORD *)(out.data() + at);
        auto o = XePatchOriginal(r), d = XePatchData(r);
        recs.push_back({r->Kind, r->Address, std::vector<UCHAR>(o, o + r->OriginalLength),
                        std::vector<UCHAR>(d, d + r->Length), r->Check});
        at += XePatchRecordSize(r);
    }
    return recs;
}

std::vector<UCHAR> B(std::initializer_list<int> v) {
    std::vector<UCHAR> out;
    for (int x : v)
        out.push_back((UCHAR)x);
    return out;
}

const char *const File = "; OXOS title patches\r\n"
                         "[4D4A000C]   ; every version\r\n"
                         "name = Ultra Bust-A-Move: intro\r\n"
                         "patch = 506D7: E8 D4 64 00 > EB 08\r\n"
                         "\r\n"
                         "[56550039/3]\n"
                         "name=Hulk\n"
                         "patch=0x209A8E:558BEC83>B8000800 00C20400 ; mov eax, 0x800; ret 4\n"
                         "[56550039/0x4]\n"
                         "patch = 20A53E: 55 8B EC 83 > C3\n";
} // namespace

TEST(Patches, TitleWithoutVersionMatchesEveryVersion) {
    XE_PATCH_COUNTS c;
    auto r = Parse(File, 0x4D4A000C, 7, &c);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].Kind, XE_PATCH_NAME);
    EXPECT_EQ(std::string(r[0].Data.begin(), r[0].Data.end()), "Ultra Bust-A-Move: intro");
    EXPECT_EQ(r[1].Kind, XE_PATCH_BYTES);
    EXPECT_EQ(r[1].Address, 0x506D7u);
    EXPECT_EQ(r[1].Original, B({0xE8, 0xD4, 0x64, 0x00}));
    EXPECT_EQ(r[1].Data, B({0xEB, 0x08}));
    EXPECT_EQ(c.Sections, 1u);
    EXPECT_EQ(c.Patches, 1u);
    EXPECT_EQ(c.Errors, 0u);
}

TEST(Patches, VersionedSectionsOnlyMatchTheirVersion) {
    auto v3 = Parse(File, 0x56550039, 3);
    ASSERT_EQ(v3.size(), 2u);
    EXPECT_EQ(v3[1].Address, 0x209A8Eu);
    EXPECT_EQ(v3[1].Original, B({0x55, 0x8B, 0xEC, 0x83}));
    EXPECT_EQ(v3[1].Data, B({0xB8, 0, 0x08, 0, 0, 0xC2, 0x04, 0})) << "bytes with and without spaces, comment";
    auto v4 = Parse(File, 0x56550039, 4);
    ASSERT_EQ(v4.size(), 1u);
    EXPECT_EQ(v4[0].Address, 0x20A53Eu);
    EXPECT_EQ(v4[0].Original.size(), 4u) << "an original longer than the new bytes";
    EXPECT_EQ(v4[0].Data, B({0xC3}));
    EXPECT_TRUE(Parse(File, 0x56550039, 5).empty());
    EXPECT_TRUE(Parse(File, 0x12345678, 3).empty());
}

TEST(Patches, BadLinesAreCountedAndSkipped) {
    XE_PATCH_COUNTS c;
    auto r = Parse("[ABC]\n"
                   "patch = 1000: E > 90\n"     // odd digit count
                   "patch = 1000 90 > 90\n"     // no colon
                   "patch = 1000: 90\n"         // no original bytes
                   "patch = 1000: > 90\n"       // empty original
                   "patch = 1000: 90 >\n"       // empty new bytes
                   "patch = 1000: 90 > 90 > 90\n" // two '>'
                   "patch = 1000: ZZ > 90\n"    // not hex
                   "nonsense\n"                 // no '='
                   "[123456789]\n"              // 9 hex digits
                   "[ABC/x]\n"                  // bad version
                   "other = ignored\n"          // unknown key: not an error
                   "[ABC]\n"
                   "patch = 2000: 8B FF > 90 90\n",
                   0xABC, 0, &c);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].Address, 0x2000u);
    EXPECT_EQ(c.Errors, 10u);
    EXPECT_EQ(c.Sections, 2u);
}

TEST(Patches, OutputThatDoesNotFitIsAnError) {
    XE_PATCH_COUNTS c;
    auto r = Parse("[1]\npatch=10:00>01\npatch=20:00>02\n", 1, 0, &c, 16); // room for one 16-byte record
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].Address, 0x10u);
    EXPECT_EQ(c.Errors, 1u);
}

TEST(Patches, LongestPatchAndName) {
    std::string bytes(2 * XE_PATCH_MAX_BYTES, 'A');
    std::string name(200, 'n');
    auto r = Parse("[1]\nname = " + name + "\npatch = 10: " + bytes + " > " + bytes + "\npatch = 10: " + bytes +
                       "AA > 90\npatch = 10: 90 > " + bytes + "AA\n",
                   1, 0, nullptr, 1 << 16);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].Data.size(), XE_PATCH_MAX_NAME) << "names are cut";
    EXPECT_EQ(r[1].Original.size(), XE_PATCH_MAX_BYTES);
    EXPECT_EQ(r[1].Data.size(), XE_PATCH_MAX_BYTES) << "one byte too many, on either side, is an error";
}

TEST(Patches, SpacesAndTabsBetweenBytesDoNotMatter) {
    auto want = B({0xF0, 0xF4, 0xB7, 0xC9});
    for (const char *line : {"patch = 10: F0 F4 B7 C9 > F0F4B7C9", "patch = 10: F0F4B7C9>F0 F4 B7 C9",
                             "patch\t=\t10:\tF0\tF4  \t B7C9   >   F0F4 B7\t\tC9\t; comment",
                             "patch=0x10:f0f4b7c9>f0f4b7c9\r"}) {
        XE_PATCH_COUNTS c;
        auto r = Parse(std::string("[1]\n") + line + "\n", 1, 0, &c);
        ASSERT_EQ(r.size(), 1u) << line;
        EXPECT_EQ(r[0].Address, 0x10u) << line;
        EXPECT_EQ(r[0].Original, want) << line;
        EXPECT_EQ(r[0].Data, want) << line;
        EXPECT_EQ(c.Errors, 0u) << line;
    }
    XE_PATCH_COUNTS c;
    EXPECT_TRUE(Parse("[1]\npatch = 10: F 0 > 90\n", 1, 0, &c).empty()) << "a byte's two digits stay together";
    EXPECT_EQ(c.Errors, 1u);
}

TEST(Patches, MatchLineChecksPatchesWithoutOriginalBytes) {
    XE_PATCH_COUNTS c;
    auto r = Parse("[1]\n"
                   "match = 1000: 8B F0 56 E8\n"
                   "patch = 1000: 8B F0 56 E8 00 F0 03 00 > B8 7C D0 07 00 90 90 90\n"
                   "patch = 2000: > 90 90\n"
                   "patch = 3000:>05\n"
                   "[2]\n"
                   "patch = 4000: > 90\n", // no match line in this section
                   1, 0, &c);
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].Check, 0u) << "a patch with its own original bytes";
    EXPECT_EQ(r[0].Original.size(), 8u);
    EXPECT_EQ(r[1].Address, 0x2000u);
    EXPECT_EQ(r[1].Check, 0x1000u);
    EXPECT_EQ(r[1].Original, B({0x8B, 0xF0, 0x56, 0xE8})) << "the match bytes";
    EXPECT_EQ(r[1].Data, B({0x90, 0x90}));
    EXPECT_EQ(r[2].Check, 0x1000u);
    EXPECT_EQ(r[2].Data, B({0x05}));
    EXPECT_EQ(c.Patches, 3u);
    EXPECT_EQ(c.Errors, 1u) << "a patch without original bytes and without a match line";
    auto other = Parse("[1]\nmatch = 1000: 8B\n[1]\npatch = 2000: > 90\n", 1, 0, &c);
    EXPECT_TRUE(other.empty()) << "a match line holds for its own section only";
    EXPECT_EQ(c.Errors, 1u);
    Parse("[1]\nmatch = 1000:\nmatch = 0: 90\nmatch 1000: 90\n", 1, 0, &c);
    EXPECT_EQ(c.Errors, 3u) << "no bytes, address 0, no '='";
}

TEST(Patches, NoNewlineAtTheEnd) {
    auto r = Parse("[1]\npatch = 10: CC > 90", 1, 0);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].Data, B({0x90}));
}

// The file shipped in kernel/title-patches parses without a single bad line, checks 4 bytes or more, and never has
// fewer new bytes than original ones (shorter NEW is allowed, but not used there, for readability; longer only where
// the rest of the original is not known).
TEST(Patches, ShippedFileParses) {
    std::string path = XK_KERNEL_DIR "/title-patches/title-patches.ini";
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr) << path;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
        text.append(buf, n);
    fclose(f);
    XE_PATCH_COUNTS c;
    auto r = Parse(text, 0x41560010, 2, &c, 1 << 16); // Wolfenstein: seven one-byte patches
    EXPECT_EQ(c.Errors, 0u);
    EXPECT_EQ(c.Sections, 1u);
    EXPECT_EQ(c.Patches, 7u);
    for (ULONG title : {0x4D4A000Cu, 0x56550013u, 0x4541038Bu, 0x4D530031u, 0x56550041u, 0x48450001u, 0x53430007u,
                        0x56550039u, 0x45410042u, 0x434D0050u}) {
        XE_PATCH_COUNTS t;
        for (auto &p : Parse(text, title, 1, &t, 1 << 16))
            if (p.Kind == XE_PATCH_BYTES) {
                EXPECT_GE(p.Original.size(), 4u) << std::hex << title << " " << p.Address << ": check 4 bytes or more";
                if (!p.Check) // a patch checked against its section's match line has no bytes of its own
                    EXPECT_GE(p.Data.size(), p.Original.size()) << std::hex << title << " " << p.Address
                                                                << ": the shipped file spells out every byte on both sides";
            }
        EXPECT_GT(t.Patches, 0u) << std::hex << title;
    }
}
