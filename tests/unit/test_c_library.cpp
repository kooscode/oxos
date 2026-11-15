// The kernel's C library (kernel/src/runtime/c_library.h), as OXOS's own code and kernel extensions use it. On the host the kernel's versions are named
// Xk_strlen, ... (the C library's own keep their names); each is checked against what the standard says.
#include "kernel_test.h"

#include <runtime/runtime.h>

#include <climits>
#include <cstring>

// --- Strings

TEST(CLibrary, LengthAndCompare) {
    EXPECT_EQ(Xk_strlen(""), 0u);
    EXPECT_EQ(Xk_strlen("OXOS"), 4u);
    EXPECT_EQ(Xk_strnlen("OXOS", 2), 2u);
    EXPECT_EQ(Xk_strnlen("OX", 8), 2u);
    EXPECT_EQ(Xk_strcmp("abc", "abc"), 0);
    EXPECT_LT(Xk_strcmp("abc", "abd"), 0);
    EXPECT_GT(Xk_strcmp("abcd", "abc"), 0);
    EXPECT_GT(Xk_strcmp("\xE9", "a"), 0); // bytes compare unsigned
    EXPECT_EQ(Xk_strncmp("abcX", "abcY", 3), 0);
    EXPECT_NE(Xk_strncmp("abcX", "abcY", 4), 0);
    EXPECT_EQ(Xk_strcasecmp("OXOS FTP server", "oxos ftp SERVER"), 0);
    EXPECT_LT(Xk_strcasecmp("apple", "BANANA"), 0);
    EXPECT_EQ(Xk_strncasecmp("HELLO there", "hello world", 6), 0);
    EXPECT_NE(Xk_strncasecmp("HELLO there", "hello world", 7), 0);
}

TEST(CLibrary, Search) {
    const char *text = "C:\\OXOS\\log\\extensions.log";
    EXPECT_EQ(Xk_strchr(text, '\\'), text + 2);
    EXPECT_EQ(Xk_strrchr(text, '\\'), text + 11);
    EXPECT_EQ(Xk_strchr(text, 0), text + strlen(text));
    EXPECT_EQ(Xk_strchr(text, 'Z'), nullptr);
    EXPECT_EQ(Xk_strstr(text, "log"), text + 8);
    EXPECT_EQ(Xk_strstr(text, ""), text);
    EXPECT_EQ(Xk_strstr(text, "logs"), nullptr);
    EXPECT_EQ(Xk_memchr(text, 'O', 8), text + 3);
    EXPECT_EQ(Xk_memchr(text, 'x', 8), nullptr);
}

TEST(CLibrary, BoundedCopy) {
    char b[8];
    EXPECT_EQ(Xk_strlcpy(b, "abc", sizeof(b)), 3u);
    EXPECT_STREQ(b, "abc");
    EXPECT_EQ(Xk_strlcpy(b, "abcdefghij", sizeof(b)), 10u); // cut short: the result says so
    EXPECT_STREQ(b, "abcdefg");
    EXPECT_EQ(Xk_strlcpy(b, "zz", 0), 2u);                  // nothing written
    EXPECT_STREQ(b, "abcdefg");
    Xk_strlcpy(b, "ab", sizeof(b));
    EXPECT_EQ(Xk_strlcat(b, "cd", sizeof(b)), 4u);
    EXPECT_STREQ(b, "abcd");
    EXPECT_EQ(Xk_strlcat(b, "efghij", sizeof(b)), 10u);
    EXPECT_STREQ(b, "abcdefg");
}

// --- Numbers and characters

TEST(CLibrary, Numbers) {
    char *end;
    EXPECT_EQ(Xk_strtoul("  42abc", &end, 10), 42ul);
    EXPECT_STREQ(end, "abc");
    EXPECT_EQ(Xk_strtoul("0x1F", nullptr, 0), 31ul);
    EXPECT_EQ(Xk_strtoul("0x1F", nullptr, 16), 31ul);
    EXPECT_EQ(Xk_strtoul("017", nullptr, 0), 15ul);
    EXPECT_EQ(Xk_strtoul("ff", nullptr, 16), 255ul);
    EXPECT_EQ(Xk_strtoul("99999999999", nullptr, 10), ULONG_MAX); // too large
    const char *none = "  x";
    EXPECT_EQ(Xk_strtoul(none, &end, 10), 0ul);
    EXPECT_EQ(end, none);                                        // nothing read
    EXPECT_EQ(Xk_strtol("-123", &end, 10), -123l);
    EXPECT_EQ(*end, 0);
    EXPECT_EQ(Xk_strtol("+7", nullptr, 10), 7l);
    EXPECT_EQ(Xk_strtol("-99999999999", nullptr, 10), LONG_MIN);
    EXPECT_EQ(Xk_strtol("99999999999", nullptr, 10), LONG_MAX);
    EXPECT_EQ(Xk_strtol("-", &end, 10), 0l);
    EXPECT_STREQ(end, "-");
    EXPECT_EQ(Xk_atoi(" 10 s"), 10);
    for (int c = 0; c < 256; c++) {
        EXPECT_EQ(!!Xk_isdigit(c), !!isdigit(c)) << c;
        EXPECT_EQ(!!Xk_isspace(c), !!isspace(c)) << c;
        EXPECT_EQ(!!Xk_isalpha(c), c < 128 && !!isalpha(c)) << c;
        EXPECT_EQ(!!Xk_isalnum(c), c < 128 && !!isalnum(c)) << c;
        EXPECT_EQ(Xk_tolower(c), c < 128 ? tolower(c) : c) << c;
        EXPECT_EQ(Xk_toupper(c), c < 128 ? toupper(c) : c) << c;
    }
}

// --- Formatting: C99's rules (the titles' RtlSnprintf keeps _snprintf's, test_rtl.cpp)

TEST(CLibrary, Snprintf) {
    char b[8];
    EXPECT_EQ(Xk_snprintf(b, sizeof(b), "%d|%s", 42, "ab"), 5);
    EXPECT_STREQ(b, "42|ab");
    EXPECT_EQ(Xk_snprintf(b, sizeof(b), "%s", "abcdefghij"), 10); // cut short: still ends in 0
    EXPECT_STREQ(b, "abcdefg");
    EXPECT_EQ(Xk_snprintf(b, 0, "%s", "abc"), 3);                 // nothing written
    EXPECT_STREQ(b, "abcdefg");
    char exact[4];
    EXPECT_EQ(Xk_snprintf(exact, sizeof(exact), "abc"), 3);
    EXPECT_STREQ(exact, "abc");
}
