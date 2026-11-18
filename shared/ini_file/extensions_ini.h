// The extension list, C:\OXOS\oxos_extensions.ini (spec/oxos_internal/34 §3.1): the parser. It makes no kernel calls, so the host unit tests run it unchanged
// (tests/unit/test_extensions_ini.cpp).
//
//   [name]                       one extension per section, loaded in the file's order
//   extension = name.xos         its file in C:\OXOS\extensions\ (required)
//   enabled = yes                required to load it: missing, no or anything else leaves it off
//   key = value                  any other key: a setting for the extension (the older place; see ParseOwnSettings)
//
// An extension's own file, C:\OXOS\extensions\NAME.ini next to NAME.xos: its settings are the keys before the first [section]; the sections after them are
// its data, which the extension reads itself (the SDK's oxos_ini_section).
#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr uint32_t INI_EXTENSION_SETTINGS_MAX = 32;  // settings per extension
constexpr uint32_t INI_EXTENSION_VALUE_MAX = 255;    // characters in a setting's value
constexpr uint32_t INI_EXTENSION_FILE_NAME_MAX = 63; // characters in an extension's file name

// A piece of the file's text (not zero-terminated).
struct INI_TEXT {
    const char *Text;
    uint32_t Length;
};

// One extension the file names, enabled or not.
struct INI_EXTENSION_ENTRY {
    INI_TEXT Section;   // the name between the brackets
    INI_TEXT File;      // the value of extension =
    bool Enabled;      // enabled = yes
    uint32_t SettingCount;
    INI_TEXT Keys[INI_EXTENSION_SETTINGS_MAX];
    INI_TEXT Values[INI_EXTENSION_SETTINGS_MAX];
};

namespace oxos::ini_file {

// The two texts are the same, in any case.
bool SameText(const INI_TEXT &A, const INI_TEXT &B);

// Reads the list in Text (Length bytes) into Entries, in the file's order, at most MaxEntries of them; returns how many. A section without a valid extension =
// line, or naming a file an earlier section names, is left out. Every line that cannot be used (left-out sections, keys outside a section, a bad enabled =
// value, too many settings or a value too long, sections past MaxEntries) counts one in *Errors. A section is Enabled only with enabled = yes.
uint32_t ParseExtensions(const char *Text, uint32_t Length, INI_EXTENSION_ENTRY *Entries, uint32_t MaxEntries, uint32_t *Errors);

// Reads the settings at the top of an extension's own ini file (Text, Length bytes): the keys before its first section header, at most MaxSettings, into Keys
// and Values; returns how many. A line that cannot be read, a key past MaxSettings or a value longer than INI_EXTENSION_VALUE_MAX counts one in *Errors.
uint32_t ParseOwnSettings(const char *Text, uint32_t Length, INI_TEXT *Keys, INI_TEXT *Values, uint32_t MaxSettings, uint32_t *Errors);

} // namespace oxos::ini_file
