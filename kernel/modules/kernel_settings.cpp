// The kernel settings module (spec/oxos_internal/43): OXOS's own options from C:\OXOS\oxos_settings.ini, read with the shared ini reader
// (shared/ini_file/settings_ini.cpp): the switches the kernel reads at a cold boot and keeps, the boot video's (spec/oxos_internal/38 §2) and the toasts'
// (spec/oxos_internal/36 §2).
#include "modules.h"
#include <diagnostics/diagnostics.h>
#include <ini_file/settings_ini.h>
#include <memory/memory.h>
#include <titles/titles.h>

namespace xk::modules {

namespace {

constexpr char SETTINGS_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_settings.ini";
constexpr ULONG MAX_SETTINGS_FILE = 0x10000;
constexpr ULONG POOL_TAG = 'sSeX';

} // namespace

ULONG ReadColdBootSwitches() {
    ULONG got;
    NTSTATUS status;
    char *text = titles::ReadOXOSFile(SETTINGS_FILE, MAX_SETTINGS_FILE, POOL_TAG, &got, &status);
    if (!text) {
        XK_SERIAL("settings: oxos_settings.ini not read (%#lx)\n", status);
        return 0;
    }
    BOOT_VIDEO_SETTINGS settings;
    uint32_t errors;
    oxos::ini_file::ReadBootVideoSettings(text, got, &settings, &errors);
    bool toasts = oxos::ini_file::ReadToastsEnabled(text, got);
    ExFreePool(text);
    if (errors)
        XK_SERIAL("settings: oxos_settings.ini: %u lines not understood\n", errors);
    XK_SERIAL("settings: oxos_settings.ini (%lu bytes): boot video %s, always %s; toasts %s\n", got, settings.Enabled ? "on" : "off",
              settings.Always ? "yes" : "no", toasts ? "on" : "off");
    ULONG switches = toasts ? 0 : XK_TOASTS_OFF;
    if (settings.Enabled)
        switches |= XK_BOOT_VIDEO_ENABLED | (settings.Always ? XK_BOOT_VIDEO_ALWAYS : 0);
    return switches;
}

} // namespace xk::modules
