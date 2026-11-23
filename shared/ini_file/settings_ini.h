// C:\OXOS\oxos_settings.ini, the kernel's own options (spec/oxos_internal/19, 38 §1, 39): the parser. It makes no kernel calls, so the host unit tests run
// it unchanged (tests/unit/test_settings_ini.cpp), and the boot video player (apps/oxos_boot_video) reads the file with it too.
//
//   [dashboard]
//   dashboard_1 = C:\evoxdash.xbe     the dashboards to start, tried in order (dashboard_1 to dashboard_8); then C:\xboxdash.xbe
//   dashboard_2 = C:\Dashboard\default.xbe
//
//   [boot_video]
//   boot_video_enabled = yes          yes: a boot video plays; no or missing: none
//   boot_video_file = intro.mpg       a file in C:\OXOS\boot_videos\, or a full path; quotes around it are allowed
//   boot_video_always = no            yes: after every reboot that starts the dashboard or the disc; no or missing: after a cold boot only
//
//   [toasts]
//   enabled = no                      no: no toasts over titles (spec/oxos_internal/36); yes or missing: toasts on
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <ini_file/launch_redirect_ini.h>

// The dashboards [dashboard] may list: dashboard_1 to dashboard_8.
constexpr uint32_t INI_MAX_DASHBOARDS = 8;

// The longest boot_video_file kept, without its terminating zero; a longer one is taken as missing.
constexpr uint32_t BOOT_VIDEO_MAX_FILE = 259;

// The [boot_video] section.
struct BOOT_VIDEO_SETTINGS {
    bool Enabled;                     // boot_video_enabled = yes
    bool Always;                      // boot_video_always = yes
    char File[BOOT_VIDEO_MAX_FILE + 1];  // boot_video_file without quotes; empty when it is missing or too long
};

namespace oxos::ini_file {

// [dashboard]'s dashboard_<Number> (1 to INI_MAX_DASHBOARDS) as a device path in Path (its drive letter as the launch redirect file's, spec/oxos_internal/33
// §1; quotes around it are allowed); false when the file has none, or its path names no drive OXOS knows. A key given twice: the last one counts.
bool FindDashboard(const char *Text, uint32_t Length, uint32_t Number, char (&Path)[INI_REDIRECT_MAX_PATH + 1]);

// Reads [boot_video] from the file's text into *Settings: off and empty unless the file says otherwise ("yes" in any case turns a switch on; a key given twice,
// the last one counts). Errors counts the lines that could not be read, in the whole file.
void ReadBootVideoSettings(const char *Text, uint32_t Length, BOOT_VIDEO_SETTINGS *Settings, uint32_t *Errors);

// [toasts]'s enabled (spec/oxos_internal/36 §2): true unless the file says "enabled = no" (any case; a key given twice, the last one counts).
bool ReadToastsEnabled(const char *Text, uint32_t Length);

} // namespace oxos::ini_file
