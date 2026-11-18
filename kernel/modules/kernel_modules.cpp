// The kernel modules' image (spec/oxos_internal/43): its entry point gives the kernel the table of the modules' functions. The kernel calls it once per
// launch, when it maps the image (kernel_modules/kernel_module_loader.cpp).
#include <kernel_modules/kernel_modules.h>
#include <extensions/extensions.h>
#include "modules.h"

namespace {

const XK_KERNEL_MODULES Table = {
    xk::extensions::LoadListedExtensions,
    xk::modules::ReadBootVideoSwitches,
    xk::modules::FindLaunchRedirect,
    xk::modules::LoadListedDashboard,
    xk::modules::BuildTitlePatches,
};

} // namespace

extern "C" const XK_KERNEL_MODULES *XkKernelModulesEntry() {
    return &Table;
}
