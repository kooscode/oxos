// Launch redirects, oxos_settings.ini's [launch_redirect] section (spec/oxos_internal/33 §1): the parser (shared/ini_file/launch_redirect_ini.h).
//
//   [launch_redirect]
//   ; comment
//   xbox_live_updater = C:\xodash\xonlinedash.xbe >> E:\Apps\XBLive\default.xbe    any name = the XBE a title asks for >> the XBE started instead
#include <ini_file/launch_redirect_ini.h>
#include <ini_file/ini_file.h>
#include <c_library.h>

namespace oxos::ini_file {

namespace {

// The drives a path in the file may start with (spec/oxos_internal/33 §1): D: is the DVD drive, the others are the hard disk's partitions, PARTITIONS[i] for
// DRIVES[i].
constexpr char DVD_DRIVE[] = "\\Device\\CdRom0";
constexpr char PARTITION[] = "\\Device\\Harddisk0\\Partition";
constexpr char DRIVES[] = "CEFGXYZ";
constexpr char PARTITIONS[] = "2167345";

constexpr char REDIRECT_SECTION[] = "launch_redirect";
// Between the source and the target in a redirect's value.
constexpr char REDIRECT_ARROW[] = ">>";

bool IsSection(const INI_LINE &Line, const char *Name) {
    return Line.TextLength == strlen(Name) && strncasecmp(Line.Text, Name, Line.TextLength) == 0;
}

bool IsBlank(char Character) {
    return Character == ' ' || Character == '\t';
}

// Text..End without the blanks around it.
void TrimBlanks(const char *&Text, const char *&End) {
    while (Text < End && IsBlank(*Text))
        Text++;
    while (End > Text && IsBlank(End[-1]))
        End--;
}

// Splits "SOURCE >> TARGET" and writes both as device paths. False when there is no ">>", or either path is empty, on no drive OXOS knows, or too long.
bool ReadRedirect(const INI_LINE &Line, char (&Source)[INI_REDIRECT_MAX_PATH + 1], char (&Target)[INI_REDIRECT_MAX_PATH + 1]) {
    const char *end = Line.Value + Line.ValueLength;
    const char *arrow = Line.Value;
    while (arrow + 1 < end && !(arrow[0] == REDIRECT_ARROW[0] && arrow[1] == REDIRECT_ARROW[1]))
        arrow++;
    if (arrow + 1 >= end)
        return false;
    const char *source = Line.Value;
    const char *sourceEnd = arrow;
    const char *target = arrow + 2;
    const char *targetEnd = end;
    TrimBlanks(source, sourceEnd);
    TrimBlanks(target, targetEnd);
    return RedirectDevicePath(source, static_cast<uint32_t>(sourceEnd - source), Source) &&
           RedirectDevicePath(target, static_cast<uint32_t>(targetEnd - target), Target);
}

} // namespace

bool RedirectDevicePath(const char *Path, uint32_t Length, char (&Out)[INI_REDIRECT_MAX_PATH + 1]) {
    uint32_t n = 0;
    uint32_t at = 0;
    if (Length >= 2 && Path[1] == ':') {
        const char letter = static_cast<char>(toupper(Path[0]));
        const char *device = letter == 'D' ? DVD_DRIVE : nullptr;
        char partition = 0;
        for (uint32_t i = 0; DRIVES[i]; i++) {
            if (letter == DRIVES[i]) {
                device = PARTITION;
                partition = PARTITIONS[i];
            }
        }
        if (!device)
            return false;
        n = strlcpy(Out, device, sizeof(Out));
        if (partition)
            Out[n++] = partition;
        at = 2;
        // "C:name" is "C:\name".
        if (at < Length && Path[at] != '\\' && Path[at] != '/')
            Out[n++] = '\\';
    }
    for (; at < Length; at++) {
        if (n == INI_REDIRECT_MAX_PATH)
            return false;
        Out[n++] = Path[at] == '/' ? '\\' : Path[at];
    }
    Out[n] = 0;
    return n > 0 && Out[0] == '\\';
}

bool LookUpRedirect(const char *Text, uint32_t Length, const char *Path, INI_REDIRECT *Found, uint32_t *Errors) {
    *Errors = 0;
    bool found = false;
    bool inSection = false;
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::SECTION || line.Kind == INI_LINE_KIND::BAD_SECTION) {
            inSection = line.Kind == INI_LINE_KIND::SECTION && IsSection(line, REDIRECT_SECTION);
            continue;
        }
        if (!inSection)
            continue;
        char source[INI_REDIRECT_MAX_PATH + 1];
        char target[INI_REDIRECT_MAX_PATH + 1];
        if (line.Kind == INI_LINE_KIND::BAD_LINE || !ReadRedirect(line, source, target)) {
            (*Errors)++;
            continue;
        }
        // The first line whose source is the launch decides; the lines after it are still read for the error count.
        if (found || strcasecmp(source, Path) != 0)
            continue;
        found = true;
        memcpy(Found->Target, target, sizeof(target));
        uint32_t n = line.TextLength < INI_REDIRECT_MAX_NAME ? line.TextLength : INI_REDIRECT_MAX_NAME;
        memcpy(Found->Name, line.Text, n);
        Found->Name[n] = 0;
    }
    return found;
}

} // namespace oxos::ini_file
