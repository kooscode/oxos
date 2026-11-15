// SPDX-License-Identifier: MIT
// example_run_loop.xos: work while a title runs. ExtensionRunLoop is called about every millisecond on the kernel's one loop thread, which every extension
// shares, so it does a little and returns; here it logs once every "interval" seconds (its setting, in C:\OXOS\extensions\example_run_loop.ini), and how long the title ran when it ends:
//
//   [example_run_loop] Starting extension example_run_loop version 1.0.0.
//   [example_run_loop] 10 s into this title.
//   [example_run_loop] 20 s into this title.
//   [example_run_loop] This title ran 20 s.
//   [example_run_loop] Terminating extension example_run_loop version 1.0.0.
//
// It keeps its own time with the kernel's millisecond count (KeTickCount) and never waits: a RunLoop that takes over 500 ms holds up every other extension,
// and the kernel logs it in C:\OXOS\log\extensions.log.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
static void ExtensionTerminate(void);

#define EXTENSION_NAME "example_run_loop"
#define EXTENSION_VERSION "1.0.0"

// Define the extension's descriptor, which the kernel reads. It names the extension and its functions, and the kernel calls ExtensionStart before each title.
OXOS_DEFINE_EXTENSION(.Name = EXTENSION_NAME, .Version = EXTENSION_VERSION, .Start = ExtensionStart, .RunLoop = ExtensionRunLoop,
                      .Terminate = ExtensionTerminate);

// Set at each launch (the kernel loads the extension afresh before every title, so these start at 0 each time).
static ULONG IntervalMs;     // the "interval" setting, in milliseconds
static ULONG LastBeat;       // KeTickCount at the start, then at each heartbeat
static ULONG SecondsInTitle; // how long this title has run, counted by the heartbeats

// Before each title starts.
static BOOLEAN ExtensionStart(void) {
    // Logging ExtensionStart message to the log file and serial port.
    oxos_log("Starting extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);

    // Reading the "interval" setting from example_run_loop.ini, in seconds (10 when it is missing or 0).
    ULONG seconds = oxos_setting_number("interval", 10);
    IntervalMs = (seconds ? seconds : 10) * 1000;

    // Starting the clock: the first heartbeat is one interval from now.
    LastBeat = KeTickCount;
    return TRUE;
}

// This will be called again and again by the kernel once every 1ms while the title runs.
static void ExtensionRunLoop(void) {
    // Not yet time for a heartbeat: return at once, the other extensions wait for this call.
    if (KeTickCount - LastBeat < IntervalMs)
        return;

    // A heartbeat: count the time and log it.
    LastBeat += IntervalMs;
    SecondsInTitle += IntervalMs / 1000;
    oxos_log("%lu s into this title.", SecondsInTitle);
}

// The title is starting another title or the dashboard.
static void ExtensionTerminate(void) {
    oxos_log("This title ran %lu s.", SecondsInTitle);
    oxos_log("Terminating extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);
}
