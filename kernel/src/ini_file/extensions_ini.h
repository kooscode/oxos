// The extension list, C:\OXOS\oxos_extensions.ini (spec/oxos_internal/34 §3.1): the parser. It makes no kernel calls, so the host unit tests run it unchanged
// (tests/unit/test_extensions_ini.cpp).
//
//   [name]                       one extension per section, loaded in the file's order
//   extension = name.xos         its file in C:\OXOS\extensions\ (required)
//   enabled = no                 optional: skip it (yes is the default)
//   key = value                  any other key: a setting for the extension
#pragma once
#include <runtime/types.h>

constexpr ULONG XK_EXTENSION_SETTINGS_MAX = 32;  // settings per extension
constexpr ULONG XK_EXTENSION_VALUE_MAX = 255;    // characters in a setting's value
constexpr ULONG XK_EXTENSION_FILE_NAME_MAX = 63; // characters in an extension's file name

// A piece of the file's text (not zero-terminated).
struct XK_TEXT {
    const char *Text;
    ULONG Length;
};

// One extension the file names, enabled or not.
struct XK_EXTENSION_ENTRY {
    XK_TEXT Section;   // the name between the brackets
    XK_TEXT File;      // the value of extension =
    bool Enabled;
    ULONG SettingCount;
    XK_TEXT Keys[XK_EXTENSION_SETTINGS_MAX];
    XK_TEXT Values[XK_EXTENSION_SETTINGS_MAX];
};

namespace xk::ini_file {

// Reads the list in Text (Length bytes) into Entries, in the file's order, at most MaxEntries of them; returns how many. A section without a valid extension =
// line, or naming a file an earlier section names, is left out. Every line that cannot be used (left-out sections, keys outside a section, a bad enabled =
// value, too many settings or a value too long, sections past MaxEntries) counts one in *Errors.
ULONG ParseExtensions(const char *Text, ULONG Length, XK_EXTENSION_ENTRY *Entries, ULONG MaxEntries, ULONG *Errors);

} // namespace xk::ini_file
