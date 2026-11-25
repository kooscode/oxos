// Kernel modules (spec/oxos_internal/43): kernel code that runs only while a title is being launched, kept out of the resident kernel. The modules
// (kernel/modules/) are linked together into one image at XK_KERNEL_MODULES_BASE against this kernel's addresses (tools/link_kernel_modules.py), appended to
// the kernel image in its boot-only pages (tools/mkbios.py) and written to C:\OXOS\cache\oxos_kernel.cache at every cold boot. At every launch the kernel
// maps the image (from the kernel image at a cold boot, from the cache after a quick reboot), calls the modules through their table, and frees it before the
// title starts (kernel_module_loader.cpp).
#pragma once
#include <runtime/types.h>
#include <titles/titles.h>

constexpr ULONG XK_KERNEL_MODULES_BASE = 0xB0000000;   // the image's link address, mapped only during a launch
constexpr ULONG XK_KERNEL_MODULES_MAX_BYTES = 0x20000; // its largest size
constexpr ULONG XK_KERNEL_MODULES_MAGIC = 0x4D4B4F58;  // "XOKM"
constexpr ULONG XK_KERNEL_MODULES_HEADER_FIELD = 0x38; // in the kernel's DOS header: the header's RVA (0: none)

// In front of the image, in the kernel image and in the cache file.
struct XK_KERNEL_MODULES_HEADER {
    ULONG Magic;
    ULONG Stamp; // the kernel's PE TimeDateStamp it was linked against
    ULONG Base;  // XK_KERNEL_MODULES_BASE
    ULONG Size;  // bytes of the image after this header (its SizeOfImage)
    ULONG Entry; // the entry point's RVA: XkKernelModulesEntry
};
XK_SIZE(XK_KERNEL_MODULES_HEADER, 20);

// The switches of oxos_settings.ini the kernel reads at a cold boot, as the kernel settings module returns them: the boot video's ([boot_video],
// spec/oxos_internal/38 §2) and the toasts' ([toasts], spec/oxos_internal/36 §2).
constexpr ULONG XK_BOOT_VIDEO_ENABLED = 1; // boot_video_enabled = yes
constexpr ULONG XK_BOOT_VIDEO_ALWAYS = 2;  // boot_video_always = yes (only with boot_video_enabled = yes)
constexpr ULONG XK_TOASTS_OFF = 4;         // [toasts] enabled = no
constexpr ULONG XK_VIDEO_MODE_TOAST_OFF = 8; // [toasts] show_video_mode = no

// The modules' functions: the table the image's entry point returns.
struct XK_KERNEL_MODULES {
    // extension_loader.cpp: loads the extensions oxos_extensions.ini lists for this launch (spec/oxos_internal/34 §8).
    void (*LoadExtensions)();
    // kernel_settings.cpp: oxos_settings.ini's cold-boot switches, XK_BOOT_VIDEO_*, XK_TOASTS_OFF and XK_VIDEO_MODE_TOAST_OFF; 0 without the file.
    ULONG (*ReadColdBootSwitches)();
    // launch_redirect.cpp: the target oxos_launch_redirect.ini gives Path (a device path), as a device path in Target (TargetSize bytes); false: none
    // (spec/oxos_internal/33).
    bool (*FindLaunchRedirect)(PCSZ Path, char *Target, ULONG TargetSize);
    // dashboard_resolver.cpp: loads the first of the dashboards oxos_settings.ini's [dashboard] lists that loads; false: none did (spec/oxos_internal/19).
    // OnlyCheck: loads nothing, true when one of them is on the disk (§5).
    bool (*LoadListedDashboard)(bool OnlyCheck);
    // title_patcher.cpp: this title's patch records from oxos_title_patches.ini, in pool memory the caller frees (*Records, *Size bytes); none: *Records
    // null (spec/oxos_internal/25 §3).
    void (*BuildTitlePatches)(ULONG TitleId, ULONG Version, const xk::titles::XBE_SECTION_HEADER *Sections, ULONG Count, PUCHAR *Records, ULONG *Size);
    // sound_recovery.cpp (spec/oxos_internal/46 §3), at a quick reboot before the next title is chosen: a frozen codec saves the launch and powers the
    // console off and on (does not return); true when it is frozen again right after such a recovery (no second cycle: the caller shows a toast).
    bool (*CheckSoundAtQuickReboot)();
    // sound_recovery.cpp, at a cold boot: true when it follows a recovery's power cycle, with the saved launch data page and disc images put back: the
    // caller then launches as a quick reboot would have.
    bool (*RestoreSoundRecoveryLaunch)();
};

// The image's entry point (kernel/modules/kernel_modules.cpp): the table.
extern "C" const XK_KERNEL_MODULES *XkKernelModulesEntry();

namespace xk::kernel_modules {

// The modules while they are mapped (from LoadKernelModules to UnloadKernelModules), else null: every caller has a way without them.
extern const XK_KERNEL_MODULES *Modules;

// Maps the image at XK_KERNEL_MODULES_BASE for this launch: at a cold boot from the kernel image (still in its boot-only pages), after a quick reboot from
// the cache file. False (Modules null) when there is none, or it was linked against another kernel: the launch goes on without the modules.
bool LoadKernelModules();

// Unmaps the image and frees its pages; before the title starts.
void UnloadKernelModules();

// Cold boot, boot-only: writes the image from the kernel image to C:\OXOS\cache\oxos_kernel.cache (the folder made just before).
void WriteKernelModulesCache();

} // namespace xk::kernel_modules
