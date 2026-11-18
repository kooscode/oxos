// The dashboard resolver module (spec/oxos_internal/19, 43): the dashboards C:\OXOS\oxos_settings.ini's [dashboard] lists (dashboard_1 to dashboard_8),
// tried in order with the kernel's XBE loader. The kernel falls back to C:\xboxdash.xbe (titles/title_loader.cpp, LaunchDashboard).
#include "modules.h"
#include <extensions/extensions.h>
#include <ini_file/settings_ini.h>
#include <memory/memory.h>
#include <titles/titles.h>

namespace xk::modules {

namespace {

constexpr char SETTINGS_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_settings.ini";
constexpr ULONG MAX_SETTINGS_FILE = 0x10000;
constexpr ULONG POOL_TAG = 'dSeX';

} // namespace

bool LoadListedDashboard() {
    if (extensions::SafeMode)
        return false;
    ULONG got;
    NTSTATUS status;
    char *text = titles::ReadOXOSFile(SETTINGS_FILE, MAX_SETTINGS_FILE, POOL_TAG, &got, &status);
    if (!text)
        return false;
    bool loaded = false;
    for (ULONG n = 1; n <= INI_MAX_DASHBOARDS && !loaded; n++) {
        char path[INI_REDIRECT_MAX_PATH + 1];
        loaded = oxos::ini_file::FindDashboard(text, got, n, path) && NT_SUCCESS(titles::LoadImage(path, TRUE));
    }
    ExFreePool(text);
    return loaded;
}

} // namespace xk::modules
