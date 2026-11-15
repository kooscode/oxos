// The kernel's debug log (spec/oxos_internal/03, spec/stock/13), in SERIAL=1 builds only: one formatted line at a time to its in-memory copy, a ring the
// emulator monitor can read (DebugLog / DebugLogPosition), and to the debug serial port (the UART driver: hardware/chipset/serial.cpp). Every debug line comes
// here: the kernel's own through XK_SERIAL (only in SERIAL=1 builds: a release kernel keeps a start-up mark of each instead), titles' through DbgPrint and
// OutputDebugString (core/debug.cpp), and kernel extensions' through oxos_log (extensions/extension_runtime.cpp). Files: file_log.cpp.
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <hardware/chipset/serial.h>
#include <runtime/runtime.h>

namespace xk::diagnostics {

#if XK_DEBUG_SERIAL

char DebugLog[XK_BOOT_SELFTEST ? 8192 : 256];
volatile ULONG DebugLogPosition;

void LogLineArgs(const char *Format, __builtin_va_list Args) {
    BootMark(Format);
    char line[256];
    int length = vsnprintf(line, sizeof(line), Format, Args);
    if (length < 0 || length >= (int)sizeof(line))
        length = sizeof(line) - 1;
    ULONG flags = save_flags_cli(); // whole lines: another processor or interrupt does not cut into one
    for (int i = 0; i < length; i++)
        DebugLog[(DebugLogPosition + i) % sizeof(DebugLog)] = line[i];
    DebugLogPosition = DebugLogPosition + length;
    hardware::chipset::WriteSerial(line, length);
    restore_flags(flags);
}

void LogLine(const char *Format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, Format);
    LogLineArgs(Format, args);
    __builtin_va_end(args);
}

#else

// A release kernel keeps no debug log, as the stock kernel prints nothing without a debugger: nothing could read it on a console, and it would take resident
// memory titles count on. DbgPrint and OutputDebugString still exist for titles; extensions' log lines go to their file.
void LogLineArgs(const char *Format, __builtin_va_list Args) {
    // Nothing kept.
}

void LogLine(const char *Format, ...) {
    // Nothing kept.
}

#endif

} // namespace xk::diagnostics
