// The kernel modules' own functions (spec/oxos_internal/43), which kernel_modules.cpp puts in the table the kernel calls (kernel_modules/kernel_modules.h
// says what each does). The extension loader's is extensions::LoadListedExtensions (extension_loader.cpp).
#pragma once
#include <kernel_modules/kernel_modules.h>

namespace xk::modules {

// kernel_settings.cpp
ULONG ReadColdBootSwitches();
// launch_redirect.cpp
bool FindLaunchRedirect(PCSZ Path, char *Target, ULONG TargetSize);
// dashboard_resolver.cpp
bool LoadListedDashboard(bool OnlyCheck);
// sound_recovery.cpp
bool CheckSoundAtQuickReboot();
bool RestoreSoundRecoveryLaunch();
// title_patcher.cpp
void BuildTitlePatches(ULONG TitleId, ULONG Version, const titles::XBE_SECTION_HEADER *Sections, ULONG Count, PUCHAR *Records, ULONG *Size);

} // namespace xk::modules
