// Launch redirects, the [launch_redirect] section of C:\OXOS\oxos_settings.ini (spec/oxos_internal/33): the parser. It makes no kernel calls, so the host unit
// tests run it unchanged (tests/unit/test_launch_redirect_ini.cpp).
//
//   [launch_redirect]
//   xbox_live_updater = C:\xodash\xonlinedash.xbe >> E:\Apps\XBLive\default.xbe   any name = the XBE a title asks for >> the XBE started instead
#pragma once
#include <stddef.h>
#include <stdint.h>

// The longest path the file may name, without its terminating zero (the loader's copy of the launch page's path).
constexpr uint32_t INI_REDIRECT_MAX_PATH = 359;
// The longest redirect name (its key) kept for the serial log, without its terminating zero.
constexpr uint32_t INI_REDIRECT_MAX_NAME = 63;

// One match: the target as a device path, and the redirect's name (its key) for the serial log.
struct INI_REDIRECT {
    char Target[INI_REDIRECT_MAX_PATH + 1];
    char Name[INI_REDIRECT_MAX_NAME + 1];
};

namespace oxos::ini_file {

// Writes the device path for a path from the file (Length characters) into Out: a drive letter (C E F G X Y Z, and D for the DVD drive) becomes its \Device\
// path, and '/' becomes '\'. False when the drive is not one of those, or the result is longer than INI_REDIRECT_MAX_PATH.
bool RedirectDevicePath(const char *Path, uint32_t Length, char (&Out)[INI_REDIRECT_MAX_PATH + 1]);

// Looks Path (a device path) up in the file's [launch_redirect] section: true, with the target and the redirect's name in *Found, when a line's source names
// Path (letters in any case); the first such line decides. Errors counts the section's lines that could not be read: no '=', no ">>", or a path that is
// empty, too long or on no drive OXOS knows.
bool LookUpRedirect(const char *Text, uint32_t Length, const char *Path, INI_REDIRECT *Found, uint32_t *Errors);

} // namespace oxos::ini_file
