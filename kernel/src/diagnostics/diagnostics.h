// Diagnostics (src/diagnostics/): what a console that freezes or faults leaves for the next boot, what the boot loader and the hardware report at start-up, the
// debug log (its ring and the serial port) and the log files in C:\OXOS\log. The CMOS bytes 0x70-0x7F hold the records; a serial build
// (spec/oxos_internal/03_debug_serial.md) prints them, and the reports, at the next boot.
#pragma once
#include <runtime/types.h>

namespace xk::diagnostics {

// --- Start-up marks (boot_marks.cpp). Until the title starts, every log point leaves its address in CMOS 0x70-0x76
// ('P', address, count) through XK_MARK / XK_SERIAL (below); a start-up that stalls for 3 s gets a watchdog record ('W', 0x77-0x7D) from the clock interrupt.

void BootMark(const char *Format);
void BootMarkWatchdog(ULONG Eip, UCHAR Irql); // each clock interrupt: the interrupted EIP and IRQL
void BootMarksDone();                         // the title is starting: the marks stop ('T')
// At kernel entry, before the first mark: the previous boot's mark and watchdog records are kept for the report, and the marks turned on unless a last-words
// record still waits to be reported.
void TakePreviousBootMarks();
void ReportPreviousBootMarks(); // serial builds: where the previous boot's start-up stopped
extern volatile BOOLEAN BootMarksOn;

// --- Last words (last_words.cpp): a record in CMOS that survives a power cycle.

// An exception that stops the kernel: 'X', the vector, EIP, and the machine-check status (unexpected exceptions, the NMI and double-fault tasks).
void SaveExceptionLastWords(ULONG Vector, ULONG Eip);
// A bug check: 'B', the code, a parameter and the caller.
void SaveBugCheckLastWords(ULONG Code, ULONG P1, ULONG Caller);
void ReportLastWords(); // serial builds: the previous boot's record, then the marks go on again

// --- Boot reports (boot_reports.cpp)

// At kernel entry: DRAM refresh (NV2A PFB 0x100210 bit 31) switched on if it is off (spec/oxos_internal/20).
void CheckDramRefresh();
// Before the memory manager unmaps page 2: the boot loader's records (refresh, GPU clock, X-code checks and snapshots, pad calibration); a quick reboot has
// none.
void TakeBootLoaderRecords(bool Quick);
void ReportMachineChecksAtBoot();               // the CPU signature, microcode and machine-check banks
void ReportMachineCheckBanks(const char *When); // the banks that hold an error
void ReportNvPads(const char *When);            // the NV2A memory pad registers, and the 2BL's calibration once
void ReportRefresh();                           // the DRAM refresh, GPU clock and X-code records
extern ULONG MachineCheckBanks;                 // the CPU's machine-check banks (0: none)

// --- The debug log (serial_log.cpp): the kernel's, titles' (DbgPrint, OutputDebugString) and extensions' (oxos_log) debug lines, to the in-memory ring and
// the debug serial port; SERIAL=1 builds only (a release kernel's LogLine does nothing)

// One line, Format as for vsnprintf (at most 255 characters): its start-up mark, then the ring and the serial port.
void LogLine(const char *Format, ...);
void LogLineArgs(const char *Format, __builtin_va_list Args); // the same, for a caller holding a va_list (DbgPrint)
#if XK_DEBUG_SERIAL
extern char DebugLog[XK_BOOT_SELFTEST ? 8192 : 256];           // the ring, readable from the emulator monitor
extern volatile ULONG DebugLogPosition;                        // characters written so far; the next goes at this % sizeof(DebugLog)
#endif

// --- Log files (file_log.cpp)

// Appends one line (Format as for vsnprintf, at most 253 characters, then CR LF) to C:\OXOS\log\LogName.log, creating the folder and the file when they are
// missing; a log that would pass 10 MB is rotated first (LogName_1.log the newest old part, spec/oxos_internal/35). PASSIVE_LEVEL only (it does nothing at a
// raised IRQL); nothing is written when the disk has no C: or refuses.
void AppendLogLine(const char *LogName, const char *Format, ...);
// C:\OXOS, C:\OXOS\log and C:\OXOS\extensions, created when missing: at every cold boot (titles::LaunchAtColdBoot), and by AppendLogLine when its log cannot
// be opened.
void CreateOxosFolders();

} // namespace xk::diagnostics

// --- Log points in kernel code

// Start-up marks (boot_marks.cpp): until the title starts, every log point also leaves an address of its own in CMOS 0x70-0x76 ('P', address, count), in every
// build. A console that freezes during start-up with no log (a release build) names the last log point it passed at the next boot (a serial build reports it).
// With the serial log the address is the format string's; without it (release builds) the text is not needed, so each log point has a one-byte mark instead,
// named in the map after the function that holds it (the strings would take about 5 KB of resident memory, spec/oxos_internal/18_boot_memory.md).
#define XK_MARK_ID() ([] { static const char XkMark = 0; return &XkMark; }())
#if XK_DEBUG_SERIAL
// A start-up mark that is never printed (hot paths: each disk transfer, each SMBus transaction).
#define XK_MARK(Label) ::xk::diagnostics::BootMark(Label)
// A line of the kernel's own in the debug log (LogLine).
#define XK_SERIAL(...) ::xk::diagnostics::LogLine(__VA_ARGS__)
#else
#define XK_MARK(Label) ::xk::diagnostics::BootMark(XK_MARK_ID())
#define XK_SERIAL(Format, ...) ::xk::diagnostics::BootMark(XK_MARK_ID())
#endif
