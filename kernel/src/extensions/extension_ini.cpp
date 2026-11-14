// The extension list file's parser (spec/oxos_internal/34 §3.1), on the ini line reader the kernel's other files use
// (titles/ini_file.cpp).
#include <xk/extension_ini.h>
#include <xk/ini_file.h>

namespace {

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool SameText(const XK_TEXT &A, const XK_TEXT &B) {
    if (A.Length != B.Length)
        return false;
    for (ULONG i = 0; i < A.Length; i++) {
        if (Lower(A.Text[i]) != Lower(B.Text[i]))
            return false;
    }
    return true;
}

// A value written in lower case letters, in any case.
bool ValueIs(const INI_LINE &Line, const char *Word) {
    ULONG i = 0;
    for (; i < Line.ValueLength && Word[i]; i++) {
        if (Lower(Line.Value[i]) != Word[i])
            return false;
    }
    return i == Line.ValueLength && !Word[i];
}

// A file name in the extensions folder: no path, not too long.
bool IsFileName(const INI_LINE &Line) {
    if (!Line.ValueLength || Line.ValueLength > XK_EXTENSION_FILE_NAME_MAX)
        return false;
    for (ULONG i = 0; i < Line.ValueLength; i++) {
        char c = Line.Value[i];
        if (c == '\\' || c == '/' || c == ':')
            return false;
    }
    return true;
}

// The section being read.
struct SECTION {
    bool Open;     // a [name] line was seen, and the section still fits in the list
    bool HasFile;
    bool Bad;      // a second extension = line: the section is left out
    XK_EXTENSION_ENTRY Entry;
};

// At the end of a section: keeps it in Entries when it is complete and names a file no earlier section names.
void CloseSection(const SECTION &Section, XK_EXTENSION_ENTRY *Entries, ULONG *Count, ULONG *Errors) {
    if (!Section.Open)
        return;
    bool keep = Section.HasFile && !Section.Bad;
    for (ULONG i = 0; keep && i < *Count; i++) {
        if (SameText(Entries[i].File, Section.Entry.File))
            keep = false;
    }
    if (!keep) {
        (*Errors)++;
        return;
    }
    Entries[(*Count)++] = Section.Entry;
}

} // namespace

ULONG XkParseExtensionList(const char *Text, ULONG Length, XK_EXTENSION_ENTRY *Entries, ULONG MaxEntries,
                           ULONG *Errors) {
    *Errors = 0;
    ULONG count = 0;
    SECTION section = {};
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (IniNextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::BAD_LINE) {
            (*Errors)++;
            continue;
        }
        // A section header ends the section before it; a broken one leaves the lines after it outside any section.
        if (line.Kind != INI_LINE_KIND::KEY) {
            CloseSection(section, Entries, &count, Errors);
            section = {};
            if (line.Kind == INI_LINE_KIND::BAD_SECTION || count == MaxEntries) {
                (*Errors)++;
                continue;
            }
            section.Open = true;
            section.Entry.Section = {line.Text, line.TextLength};
            section.Entry.Enabled = true;
            continue;
        }
        if (!section.Open) {
            (*Errors)++;
            continue;
        }
        XK_EXTENSION_ENTRY &entry = section.Entry;
        if (IniKeyIs(line, "extension")) {
            if (section.HasFile || !IsFileName(line)) {
                section.Bad = true;
                continue;
            }
            section.HasFile = true;
            entry.File = {line.Value, line.ValueLength};
        } else if (IniKeyIs(line, "enabled")) {
            if (ValueIs(line, "no"))
                entry.Enabled = false;
            else if (ValueIs(line, "yes"))
                entry.Enabled = true;
            else
                (*Errors)++;
        } else if (entry.SettingCount == XK_EXTENSION_SETTINGS_MAX || line.ValueLength > XK_EXTENSION_VALUE_MAX) {
            (*Errors)++;
        } else {
            entry.Keys[entry.SettingCount] = {line.Text, line.TextLength};
            entry.Values[entry.SettingCount] = {line.Value, line.ValueLength};
            entry.SettingCount++;
        }
    }
    CloseSection(section, Entries, &count, Errors);
    return count;
}
