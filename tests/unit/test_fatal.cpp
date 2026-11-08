// The fatal error screen's text (spec/oxos_internal/29_error_screens.md, kernel/src/core/fatal_help.cpp).
#include "kernel_test.h"

#include <cstring>
#include <string>

#include <xk/core.h>

namespace {
// The screen: 640 pixels, 48 on each side, glyphs 6 * scale apart.
constexpr unsigned TEXT_WIDTH = 640 - 2 * 48;

unsigned Width(const char *text, unsigned scale) {
    unsigned n = (unsigned)strlen(text);
    return n ? n * 6 * scale - scale : 0;
}

void CheckText(const char *text) {
    for (const char *c = text; *c; c++)
        EXPECT_TRUE(*c >= 0x20 && *c <= 0x7E) << "not in the font: " << text;
}
} // namespace

TEST(FatalHelp, EveryCodeTheKernelRaisesHasItsOwnScreen) {
    struct {
        ULONG Code;
        const char *Title;
    } expected[] = {
        {6, "Could Not Unlock HDD"},   {7, "HDD Timeout"},          {8, "HDD IDENTIFY Failed"},
        {9, "No HDD Detected"},        {13, "No Dashboard Found"},  {19, "No Dashboard Found"},
        {20, "No Dashboard Found (Disc Inserted)"},                  {21, "Previous Boot Failed"},
    };
    for (auto &e : expected)
        EXPECT_STREQ(XkFatalHelp(e.Code)->Title, e.Title) << "code " << e.Code;
}

TEST(FatalHelp, OtherCodesGetTheGeneralScreen) {
    for (ULONG code : {0ul, 5ul, 10ul, 12ul, 22ul, 0x10E5Ful})
        EXPECT_STREQ(XkFatalHelp(code)->Title, "Fatal Error") << "code " << code;
}

TEST(FatalHelp, EveryLineFitsTheScreen) {
    for (ULONG code = 0; code < 32; code++) {
        const XK_FATAL_HELP *h = XkFatalHelp(code);
        CheckText(h->Title);
        EXPECT_LE(Width(h->Title, 2), TEXT_WIDTH) << h->Title;
        unsigned lines = 0;
        for (const char *const *l = h->Lines; *l; l++, lines++) {
            CheckText(*l);
            EXPECT_LE(strlen(*l), XK_FATAL_LINE) << *l;
            EXPECT_LE(Width(*l, 2), TEXT_WIDTH) << *l;
        }
        EXPECT_GE(lines, 2u) << h->Title;
        EXPECT_LE(lines, 6u) << h->Title << ": the details below must stay on screen";
    }
}
