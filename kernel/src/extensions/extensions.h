// Kernel extensions (spec/oxos_internal/34): .xos DLLs from C:\OXOS\extensions\, loaded in the order of C:\OXOS\oxos_extensions.ini before each title. What the
// title launcher and the rest of the kernel call. The loader's image is described in extension_loader.h, the state the loader and the runtime share in
// extension_state.h.
#pragma once
#include <core/core.h>
#include <runtime/types.h>

namespace xk::extensions {
// Runs the extension loader (§8.1) for this launch: it loads the extensions of oxos_extensions.ini, calls each one's Load and starts the run loop (§3.2, §4.3).
// Called once per launch by the title launcher, before the title is chosen; nothing in safe mode or without the list.
void LoadExtensions();
// The address a title's import of Ordinal is resolved to: the first extension's hook, else Target (§7.1).
ULONG ResolveTitleImport(ULONG Ordinal, ULONG Target);
// OXOS_EVENT_TITLE_LOADING for the XBE the kernel is about to load (§7.2); Dashboard as titles::LoadImage's. Target (TargetSize bytes, empty), when this load
// may be redirected: an extension's oxos_title_redirect writes the XBE to load instead there.
void TitleLoading(PCSZ Path, BOOLEAN Dashboard, char *Target, ULONG TargetSize);
// OXOS_EVENT_TITLE_LOADED for the XBE now at 0x00010000, before its entry point runs.
void TitleLoaded();
// The title quick-reboots (HalReturnToFirmware(2)): OXOS_EVENT_TITLE_EXITING, then the run loop's end (up to 500 ms for the RunLoop call in progress) and each extension's Unload (§4.3).
void TitleExiting();

// Safe mode (B held at the cold boot's splash, §3.3): no extension loads until the next cold boot.
extern XK_PERSIST BOOLEAN SafeMode;
} // namespace xk::extensions
