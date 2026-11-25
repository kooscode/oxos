// C:\OXOS\oxos_settings.ini, the kernel's own options: the parser (shared/ini_file/settings_ini.h).
#include <ini_file/settings_ini.h>
#include <ini_file/ini_file.h>
#include <c_library.h>

namespace oxos::ini_file {

namespace {

constexpr char DASHBOARD_SECTION[] = "dashboard";
constexpr char DASHBOARD_KEY[] = "dashboard_"; // followed by its number, 1 to INI_MAX_DASHBOARDS

bool IsSection(const INI_LINE &Line, const char *Name) {
    return Line.TextLength == strlen(Name) && strncasecmp(Line.Text, Name, Line.TextLength) == 0;
}

// A value without the quotes around it.
void Unquote(const char *&Value, uint32_t &Length) {
    if (Length >= 2 && Value[0] == '"' && Value[Length - 1] == '"') {
        Value++;
        Length -= 2;
    }
}

} // namespace

bool FindDashboard(const char *Text, uint32_t Length, uint32_t Number, char (&Path)[INI_REDIRECT_MAX_PATH + 1]) {
    const uint32_t prefix = sizeof(DASHBOARD_KEY) - 1;
    if (Number < 1 || Number > INI_MAX_DASHBOARDS)
        return false;
    bool found = false;
    bool inSection = false;
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::SECTION || line.Kind == INI_LINE_KIND::BAD_SECTION) {
            inSection = line.Kind == INI_LINE_KIND::SECTION && IsSection(line, DASHBOARD_SECTION);
            continue;
        }
        if (!inSection || line.Kind != INI_LINE_KIND::KEY || line.TextLength != prefix + 1 || strncasecmp(line.Text, DASHBOARD_KEY, prefix) != 0 ||
            line.Text[prefix] != static_cast<char>('0' + Number))
            continue;
        const char *value = line.Value;
        uint32_t valueLength = line.ValueLength;
        Unquote(value, valueLength);
        found = valueLength && RedirectDevicePath(value, valueLength, Path);
    }
    return found;
}

namespace {

constexpr char BOOT_VIDEO_SECTION[] = "boot_video";
constexpr char KEY_ENABLED[] = "boot_video_enabled";
constexpr char KEY_ALWAYS[] = "boot_video_always";
constexpr char KEY_FILE[] = "boot_video_file";
constexpr char YES[] = "yes";

bool IsYes(const INI_LINE &Line) {
    return Line.ValueLength == 3 && strncasecmp(Line.Value, YES, 3) == 0;
}

// boot_video_file: the value without the quotes around it; empty when it is too long.
void CopyFileName(const INI_LINE &Line, char (&File)[BOOT_VIDEO_MAX_FILE + 1]) {
    const char *from = Line.Value;
    uint32_t length = Line.ValueLength;
    Unquote(from, length);
    File[0] = 0;
    // The value lies inside the file's text, which has no terminating 0: strlcpy would measure the source on past the value, to the buffer's end and beyond
    // (a bug check when the buffer ended just before an unmapped page).
    if (length <= BOOT_VIDEO_MAX_FILE) {
        memcpy(File, from, length);
        File[length] = 0;
    }
}

} // namespace

void ReadBootVideoSettings(const char *Text, uint32_t Length, BOOT_VIDEO_SETTINGS *Settings, uint32_t *Errors) {
    memset(Settings, 0, sizeof(*Settings));
    *Errors = 0;
    bool inSection = false;
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::SECTION || line.Kind == INI_LINE_KIND::BAD_SECTION) {
            inSection = line.Kind == INI_LINE_KIND::SECTION && IsSection(line, BOOT_VIDEO_SECTION);
            if (line.Kind == INI_LINE_KIND::BAD_SECTION)
                (*Errors)++;
            continue;
        }
        if (line.Kind == INI_LINE_KIND::BAD_LINE) {
            (*Errors)++;
            continue;
        }
        if (!inSection)
            continue;
        if (IsKey(line, KEY_ENABLED))
            Settings->Enabled = IsYes(line);
        else if (IsKey(line, KEY_ALWAYS))
            Settings->Always = IsYes(line);
        else if (IsKey(line, KEY_FILE))
            CopyFileName(line, Settings->File);
    }
}

constexpr char TOASTS_SECTION[] = "toasts";
constexpr char KEY_TOASTS_ENABLED[] = "enabled";
constexpr char KEY_SHOW_VIDEO_MODE[] = "show_video_mode";

// A [toasts] switch that is on unless the file says "Key = no".
static bool ReadToastsSwitch(const char *Text, uint32_t Length, const char *Key) {
    bool enabled = true, inSection = false;
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::SECTION || line.Kind == INI_LINE_KIND::BAD_SECTION) {
            inSection = line.Kind == INI_LINE_KIND::SECTION && IsSection(line, TOASTS_SECTION);
            continue;
        }
        if (inSection && line.Kind == INI_LINE_KIND::KEY && IsKey(line, Key))
            enabled = !(line.ValueLength == 2 && (line.Value[0] | 0x20) == 'n' && (line.Value[1] | 0x20) == 'o');
    }
    return enabled;
}

bool ReadToastsEnabled(const char *Text, uint32_t Length) {
    return ReadToastsSwitch(Text, Length, KEY_TOASTS_ENABLED);
}

bool ReadShowVideoMode(const char *Text, uint32_t Length) {
    return ReadToastsSwitch(Text, Length, KEY_SHOW_VIDEO_MODE);
}

} // namespace oxos::ini_file
