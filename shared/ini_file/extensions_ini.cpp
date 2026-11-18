// The extension list file's parser (spec/oxos_internal/34 §3.1), on the ini line reader the kernel's other files use (ini_file/ini_file.cpp).
#include <ini_file/extensions_ini.h>
#include <ini_file/ini_file.h>
#include <c_library.h>

namespace oxos::ini_file {

bool SameText(const INI_TEXT &A, const INI_TEXT &B) {
    return A.Length == B.Length && strncasecmp(A.Text, B.Text, A.Length) == 0;
}

namespace {

// The value is Word, in any case.
bool ValueIs(const INI_LINE &Line, const char *Word) {
    return Line.ValueLength == strlen(Word) && strncasecmp(Line.Value, Word, Line.ValueLength) == 0;
}

// A file name in the extensions folder: no path, not too long.
bool IsFileName(const INI_LINE &Line) {
    if (!Line.ValueLength || Line.ValueLength > INI_EXTENSION_FILE_NAME_MAX)
        return false;
    for (uint32_t i = 0; i < Line.ValueLength; i++) {
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
    INI_EXTENSION_ENTRY Entry;
};

// At the end of a section: keeps it in Entries when it is complete and names a file no earlier section names.
void CloseSection(const SECTION &Section, INI_EXTENSION_ENTRY *Entries, uint32_t *Count, uint32_t *Errors) {
    if (!Section.Open)
        return;
    bool keep = Section.HasFile && !Section.Bad;
    for (uint32_t i = 0; keep && i < *Count; i++) {
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

uint32_t ParseExtensions(const char *Text, uint32_t Length, INI_EXTENSION_ENTRY *Entries, uint32_t MaxEntries, uint32_t *Errors) {
    *Errors = 0;
    uint32_t count = 0;
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
            section.Entry.Enabled = false; // only enabled = yes loads it (user, 2026-11-15)
            continue;
        }
        if (!section.Open) {
            (*Errors)++;
            continue;
        }
        INI_EXTENSION_ENTRY &entry = section.Entry;
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
        } else if (entry.SettingCount == INI_EXTENSION_SETTINGS_MAX || line.ValueLength > INI_EXTENSION_VALUE_MAX) {
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

uint32_t ParseOwnSettings(const char *Text, uint32_t Length, INI_TEXT *Keys, INI_TEXT *Values, uint32_t MaxSettings, uint32_t *Errors) {
    *Errors = 0;
    uint32_t count = 0;
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::SECTION || line.Kind == INI_LINE_KIND::BAD_SECTION) // the data starts: not settings
            break;
        if (line.Kind == INI_LINE_KIND::BAD_LINE || count == MaxSettings || line.ValueLength > INI_EXTENSION_VALUE_MAX) {
            (*Errors)++;
            continue;
        }
        Keys[count] = {line.Text, line.TextLength};
        Values[count] = {line.Value, line.ValueLength};
        count++;
    }
    return count;
}

} // namespace oxos::ini_file
