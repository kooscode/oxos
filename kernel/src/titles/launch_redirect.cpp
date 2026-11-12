// Launch redirect file parser (spec/oxos_internal/33 §1).
//
//   ; comment
//   [name]                                 one redirect per section
//   source = C:\xodash\xonlinedash.xbe     the XBE a title asks for (drive letter or \Device\ path)
//   target = E:\Apps\XBLive\default.xbe    the XBE started instead
#include <xk/launch_redirect.h>
#include <xk/ini_file.h>

namespace {

// The drives a path in the file may start with (spec/oxos_internal/33 §1): D: is the DVD drive, the others are the
// hard disk's partitions, PARTITIONS[i] for DRIVES[i].
constexpr char DVD_DRIVE[] = "\\Device\\CdRom0";
constexpr char PARTITION[] = "\\Device\\Harddisk0\\Partition";
constexpr char DRIVES[] = "CEFGXYZ";
constexpr char PARTITIONS[] = "2167345";

char Upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool SamePath(const char *A, const char *B) {
    for (; *A && *B; A++, B++) {
        if (Upper(*A) != Upper(*B))
            return false;
    }
    return *A == *B;
}

// One section while it is read: what it has given so far.
struct SECTION {
    bool Open;          // a [name] line was seen
    bool HasSource;
    bool HasTarget;
    bool SourceMatches; // its source is the path looked up
    XE_REDIRECT Redirect;
};

// At the end of a section: true when it decides the lookup. A section without both lines is an error.
bool CloseSection(const SECTION &Section, ULONG *Errors) {
    if (!Section.Open)
        return false;
    if (!Section.HasSource || !Section.HasTarget) {
        (*Errors)++;
        return false;
    }
    return Section.SourceMatches;
}

} // namespace

bool XeRedirectDevicePath(const char *Path, ULONG Length, char (&Out)[XE_REDIRECT_MAX_PATH + 1]) {
    ULONG n = 0;
    ULONG at = 0;
    if (Length >= 2 && Path[1] == ':') {
        const char letter = Upper(Path[0]);
        const char *device = letter == 'D' ? DVD_DRIVE : nullptr;
        char partition = 0;
        for (ULONG i = 0; DRIVES[i]; i++) {
            if (letter == DRIVES[i]) {
                device = PARTITION;
                partition = PARTITIONS[i];
            }
        }
        if (!device)
            return false;
        for (; *device; device++)
            Out[n++] = *device;
        if (partition)
            Out[n++] = partition;
        at = 2;
        // "C:name" is "C:\name".
        if (at < Length && Path[at] != '\\' && Path[at] != '/')
            Out[n++] = '\\';
    }
    for (; at < Length; at++) {
        if (n == XE_REDIRECT_MAX_PATH)
            return false;
        Out[n++] = Path[at] == '/' ? '\\' : Path[at];
    }
    Out[n] = 0;
    return n > 0 && Out[0] == '\\';
}

bool XeRedirectLookup(const char *Text, ULONG Length, const char *Path, XE_REDIRECT *Found, ULONG *Errors) {
    *Errors = 0;
    bool found = false;
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
            if (CloseSection(section, Errors) && !found) {
                *Found = section.Redirect;
                found = true;
            }
            section = {};
            if (line.Kind == INI_LINE_KIND::BAD_SECTION) {
                (*Errors)++;
                continue;
            }
            section.Open = true;
            ULONG n = line.TextLength < XE_REDIRECT_MAX_NAME ? line.TextLength : XE_REDIRECT_MAX_NAME;
            for (ULONG i = 0; i < n; i++)
                section.Redirect.Name[i] = line.Text[i];
            section.Redirect.Name[n] = 0;
            continue;
        }
        bool isSource = IniKeyIs(line, "source");
        bool isTarget = IniKeyIs(line, "target");
        if (!section.Open) {
            (*Errors)++;
            continue;
        }
        if (!isSource && !isTarget)
            continue; // another key: ignored, for later additions
        char device[XE_REDIRECT_MAX_PATH + 1];
        bool twice = isSource ? section.HasSource : section.HasTarget;
        if (twice || !XeRedirectDevicePath(line.Value, line.ValueLength, device)) {
            (*Errors)++;
            continue;
        }
        if (isSource) {
            section.HasSource = true;
            section.SourceMatches = SamePath(device, Path);
        } else {
            section.HasTarget = true;
            for (ULONG i = 0; i <= XE_REDIRECT_MAX_PATH; i++)
                section.Redirect.Target[i] = device[i];
        }
    }
    if (CloseSection(section, Errors) && !found) {
        *Found = section.Redirect;
        found = true;
    }
    return found;
}
