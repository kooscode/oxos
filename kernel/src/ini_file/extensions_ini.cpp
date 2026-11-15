// The extension list file's parser (spec/oxos_internal/34 §3.1), on the ini line reader the kernel's other files use (ini_file/ini_file.cpp).
#include <ini_file/extensions_ini.h>
#include <ini_file/ini_file.h>
#include <runtime/c_library.h>

namespace xk::ini_file {

namespace {

bool SameText(const XK_TEXT &A, const XK_TEXT &B) {
    return A.Length == B.Length && strncasecmp(A.Text, B.Text, A.Length) == 0;
}

// The value is Word, in any case.
bool ValueIs(const INI_LINE &Line, const char *Word) {
    return Line.ValueLength == strlen(Word) && strncasecmp(Line.Value, Word, Line.ValueLength) == 0;
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

ULONG ParseExtensions(const char *Text, ULONG Length, XK_EXTENSION_ENTRY *Entries, ULONG MaxEntries, ULONG *Errors) {
    *Errors = 0;
    ULONG count = 0;
    SECTION section = {};
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
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
        if (IsKey(line, "extension")) {
            if (section.HasFile || !IsFileName(line)) {
                section.Bad = true;
                continue;
            }
            section.HasFile = true;
            entry.File = {line.Value, line.ValueLength};
        } else if (IsKey(line, "enabled")) {
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

} // namespace xk::ini_file
