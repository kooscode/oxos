// What the core sources reference outside the files under test: boot-time data from entry.S / interrupt_stubs.S, the serial log, and the HAL and executive
// pieces the tests do not cover. Each stub is the smallest correct behaviour.
#include "host.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>

#include <arch/arch.h>
#include <core/core.h>
#include <runtime/runtime.h>
#include <runtime/types.h>
#include <boot_splash/boot_splash.h>
#include <hardware/usb/pad_reports.h>
#include <toasts/toasts.h>
#include <extensions/extensions.h>
#include <io/io.h>
#include <hardware/chipset/chipset.h>
#include <extensions/extension_state.h>
#include <kernel_modules/kernel_modules.h>

using namespace xk; // the kernel's subsystems as storage::, core::, ...

// entry.S / interrupt_stubs.S
extern "C" {
alignas(16) unsigned char KiBootStack[KI_BOOT_STACK_SIZE];
void *KiTrapStubs[32];
void *KiIrqStubs[16];
void KiIgnoreInterrupt() {
    // The harness raises only the interrupts it models.
}

void KiDoubleFaultTask() {
    // No task switches on the host.
}

void KiNmiTask() {
    // No task switches on the host.
}

void KiTrap45() {
    // Debug service (int 2Dh): not raised in host tests.
}
}

// hardware/chipset/serial.cpp: the kernel log goes to the harness (shown for failing tests, or with OXOS_TEST_LOG=1).
namespace xk::hardware::chipset {

volatile BOOLEAN SerialPresent = TRUE;

void InitSerial() {
    // Nothing to set up: WriteSerial hands the text to the harness.
}

void WriteSerial(const char *Text, ULONG Length) {
    HostLogWrite(Text, Length);
}

} // namespace xk::hardware::chipset

// diagnostics/: the start-up marks and last words (CMOS records) are not under test here; the debug log goes to the harness's log.
// The debug port's image list (debugger/debug_port.cpp): no kernel modules mapped and no extension loaded in host tests.
namespace xk::kernel_modules {
const XK_KERNEL_MODULES *Modules;
} // namespace xk::kernel_modules

namespace xk::extensions {
XK_EXTENSIONS_STATE *LoadedExtensions;
} // namespace xk::extensions

namespace xk::diagnostics {
void LogLineArgs(const char *Format, va_list Args) {
    char line[256];
    int n = RtlVsnprintf(line, sizeof(line), Format, Args);
    HostLogWrite(line, n < 0 ? 0 : n >= (int)sizeof(line) ? sizeof(line) - 1 : (size_t)n);
}

void LogLine(const char *Format, ...) {
    va_list args;
    va_start(args, Format);
    LogLineArgs(Format, args);
    va_end(args);
}

// The hang dump's capture (interrupts.cpp): not under test; nothing is captured, and printing goes to the harness's log.
void StartLogCapture(char *, ULONG) {
    // Not under test.
}

ULONG StopLogCapture() {
    return 0;
}

void PrintLogText(const char *Text, ULONG Length) {
    HostLogWrite(Text, Length);
}

volatile BOOLEAN BootMarksOn;

void BootMark(const char *) {
    // Not under test.
}

void BootMarkWatchdog(ULONG, UCHAR) {
    // Not under test.
}

void SaveBugCheckLastWords(ULONG, ULONG, ULONG) {
    // Not under test.
}

void SaveExceptionLastWords(ULONG, ULONG) {
    // Not under test.
}
} // namespace xk::diagnostics

// filesystems/cache.cpp: the file-system cache is not under test (no cache pages exist, so no page has this owner).
namespace xk::filesystems {
ULONG *CachePte(ULONG) {
    return nullptr;
}
} // namespace xk::filesystems

// hardware/usb/pad_reports.cpp: no USB host controllers on the host.
void xk::hardware::usb::ReadPadReports(ULONG) {
}

void xk::hardware::usb::CheckPadLastResort() {
}

// toasts/toast_overlay.cpp: no GPU on the host (the queue, toast_queue.cpp, is tested).
void xk::toasts::HideToast() {
}

// extensions/extension_runtime.cpp: every call is extension 0's (tests/unit/test_toasts.cpp reads the toasts' owner).
ULONG xk::extensions::FindExtensionAt(PVOID) {
    return 0;
}

// boot_splash/splash.cpp: no splash copy in host tests (no page holds one).
bool xk::boot_splash::IsSplashPage(ULONG) {
    return false;
}

// titles/title_netboot.cpp: no network-delivered title in host tests (no page holds one).
namespace xk::titles {
bool IsNetbootTitlePage(ULONG) {
    return false;
}
} // namespace xk::titles

// io/device.cpp: no device objects in host tests; the type is only compared against (objects::CreateSymbolicLink).
XBAPI OBJECT_TYPE IoDeviceObjectType = {};

// io/file.cpp: no file systems in host tests, so nothing can be opened (objects::CreateSymbolicLink opens a title's path-link folder with it).
XBAPI NTSTATUS NTAPI NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG) {
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
