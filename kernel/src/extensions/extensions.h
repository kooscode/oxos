// Kernel extensions (spec/oxos_internal/34): .xos DLLs from C:\OXOS\extensions\, loaded in the order of C:\OXOS\oxos_extensions.ini before each title. What the
// title launcher and the rest of the kernel call. The loader is a kernel module (kernel/modules/extension_loader.cpp, spec/oxos_internal/43); the state the
// loader and the runtime share is in extension_state.h.
#pragma once
#include <core/core.h>
#include <runtime/types.h>

namespace xk::extensions {
// Runs the extension loader (§8.1) for this launch: it loads the extensions of oxos_extensions.ini, calls each one's Load and starts the run loop (§3.2, §4.3).
// Called once per launch by the title launcher, before the title is chosen; nothing in safe mode, without the list or without the kernel modules.
void LoadExtensions();
// The loader itself (kernel/modules/extension_loader.cpp, in the kernel modules' image): what LoadExtensions calls through the modules' table.
void LoadListedExtensions();
// The address a title's import of Ordinal is resolved to: the first extension's hook, else Target (§7.1).
ULONG ResolveTitleImport(ULONG Ordinal, ULONG Target);
// OXOS_EVENT_TITLE_LOADING for the XBE the kernel is about to load (§7.2); Dashboard as titles::LoadImage's. Target (TargetSize bytes, empty), when this load
// may be redirected: an extension's oxos_title_redirect writes the XBE to load instead there.
void TitleLoading(PCSZ Path, BOOLEAN Dashboard, char *Target, ULONG TargetSize);
// OXOS_EVENT_TITLE_LOADED for the XBE now at 0x00010000, before its entry point runs.
void TitleLoaded();
// The extension whose image holds Address (its index in LoadedExtensions), or 0xFFFFFFFF: whose call it is (a toast's owner, spec/oxos_internal/36 §5).
ULONG FindExtensionAt(PVOID Address);
// The title quick-reboots (HalReturnToFirmware(2)): OXOS_EVENT_TITLE_EXITING, then the run loop's end (up to 500 ms for the RunLoop call in progress) and each extension's Unload (§4.3).
void TitleExiting();

// Safe mode (B held at the cold boot's splash, §3.3): no extension loads until the next cold boot.
extern XK_PERSIST BOOLEAN SafeMode;
} // namespace xk::extensions
