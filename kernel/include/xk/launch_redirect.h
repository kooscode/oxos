// Launch redirects from C:\OXOS\launch_redirect.ini (spec/oxos_internal/33): the parser. It makes no kernel calls, so
// the host unit tests run it unchanged (tests/unit/test_launch_redirect.cpp).
#pragma once
#include <xk/types.h>

// The longest path the file may name, without its terminating zero (the loader's copy of the launch page's path).
constexpr ULONG XE_REDIRECT_MAX_PATH = 359;
// The longest section name kept for the serial log, without its terminating zero.
constexpr ULONG XE_REDIRECT_MAX_NAME = 63;

// One match: the target as a device path, and the name of the section that gave it.
struct XE_REDIRECT {
    char Target[XE_REDIRECT_MAX_PATH + 1];
    char Name[XE_REDIRECT_MAX_NAME + 1];
};

// Writes the device path for a path from the file (Length characters) into Out: a drive letter (C E F G X Y Z, and D
// for the DVD drive) becomes its \Device\ path, and '/' becomes '\'. False when the drive is not one of those, or the
// result is longer than XE_REDIRECT_MAX_PATH.
bool XeRedirectDevicePath(const char *Path, ULONG Length, char (&Out)[XE_REDIRECT_MAX_PATH + 1]);

// Looks Path (a device path) up in the file's text: true, with the target and the section's name in *Found, when a
// section's source names Path (letters in any case); the first such section decides. Errors counts the lines that
// could not be read and the sections without both a source and a target, in the whole file.
bool XeRedirectLookup(const char *Text, ULONG Length, const char *Path, XE_REDIRECT *Found, ULONG *Errors);
