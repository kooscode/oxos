// The launch redirect module (spec/oxos_internal/33, 43): the target C:\OXOS\oxos_settings.ini's [launch_redirect] section gives an XBE a title launches,
// read with the shared parser (shared/ini_file/launch_redirect_ini.cpp). The kernel loads the target (titles/title_loader.cpp, RedirectLaunch).
#include "modules.h"
#include <diagnostics/diagnostics.h>
#include <ini_file/launch_redirect_ini.h>
#include <memory/memory.h>
#include <runtime/c_library.h>
#include <titles/titles.h>

namespace xk::modules {

namespace {

constexpr char REDIRECT_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_settings.ini";
constexpr ULONG MAX_REDIRECT_FILE = 0x10000;
constexpr ULONG POOL_TAG = 'rLeX';

} // namespace

bool FindLaunchRedirect(PCSZ Path, char *Target, ULONG TargetSize) {
    ULONG got;
    NTSTATUS status;
    char *text = titles::ReadOXOSFile(REDIRECT_FILE, MAX_REDIRECT_FILE, POOL_TAG, &got, &status);
    if (!text)
        return false;
    INI_REDIRECT redirect;
    uint32_t errors;
    bool found = oxos::ini_file::LookUpRedirect(text, got, Path, &redirect, &errors);
    ExFreePool(text);
    if (errors)
        XK_SERIAL("redirect: oxos_settings.ini [launch_redirect]: %u lines not understood\n", errors);
    if (!found)
        return false;
    XK_SERIAL("redirect: [%s] %s -> %s\n", redirect.Name, Path, redirect.Target);
    strlcpy(Target, redirect.Target, TargetSize);
    return true;
}

} // namespace xk::modules
