// SPDX-License-Identifier: MIT
// example_title_event.xos: a title event, the state page, and a kernel function called from the run loop. When each title has been loaded, it logs the
// title's name and the one before it, which it keeps in its state page from one launch to the next. When the title is the one "led_title" names in
// C:\OXOS\oxos_extensions.ini (the sample names the OXOS FTP server), it turns the front LED orange a second later:
//
//   [example_title_event] Starting extension example_title_event version 1.0.0.
//   [example_title_event] "OXOS FTP server" is starting; the title before it was "Xbox Dashboard".
//   [example_title_event] "OXOS FTP server": the LED is orange.
//   [example_title_event] Terminating extension example_title_event version 1.0.0.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
static void ExtensionTerminate(void);

#define EXTENSION_NAME "example_title_event"
#define EXTENSION_VERSION "1.0.0"

// Define the extension's descriptor, which the kernel reads. It names the extension and its functions, and the kernel calls ExtensionStart before each title.
OXOS_DEFINE_EXTENSION(.Name = EXTENSION_NAME, .Version = EXTENSION_VERSION, .Start = ExtensionStart, .RunLoop = ExtensionRunLoop,
                      .Terminate = ExtensionTerminate);

// The front LED belongs to the SMC, on the SMBus at 0x20: register 0x08 is the pattern (bits 7-4 red, 3-0 green, one bit per quarter of its cycle) and
// register 0x07 = 1 shows it.
#define SMC_ADDRESS 0x20
#define SMC_LED_PATTERN 0x08
#define SMC_LED_MODE 0x07
#define LED_ORANGE 0xFF // red and green together

// The kernel turns the LED solid green just after the title-loaded event, as the title starts: the run loop waits this long, so the orange comes after it.
#define LED_DELAY_MS 1000

// What this example keeps in its state page, from one launch to the next until the next cold boot. The page is all zero at the first launch.
typedef struct LAST_TITLE {
    ULONG Launches; // titles started since the cold boot
    char Name[41];  // the name of the title before this one ("" at the first launch)
} LAST_TITLE;

// Set at each launch (the kernel loads the extension afresh before every title, so these start at 0 each time). The event sets them on the title's thread,
// the run loop reads them on its own.
static volatile BOOLEAN LedWanted; // this title is the one "led_title" names
static volatile ULONG LedWantedAt; // KeTickCount when the event saw it
static char LedTitle[41];          // its name, for the log

// The event: the kernel calls this once the title is loaded, before it starts. UserData is what ExtensionStart passed: the state page.
static void Example_TitleLoaded_Callback(const OXOS_TITLE_LOADED *Title, void *UserData) {
    LAST_TITLE *last = (LAST_TITLE *)UserData;

    // Log this title's name, and the one before it from the state page.
    if (last->Name[0])
        oxos_log("\"%s\" is starting; the title before it was \"%s\".", Title->TitleName, last->Name);
    else
        oxos_log("\"%s\" is starting, the first title since the cold boot.", Title->TitleName);

    // Keep this title's name in the state page for the next launch (its 41 bytes, the ending 0 included).
    memcpy(last->Name, Title->TitleName, sizeof(last->Name));

    // No "led_title" in oxos_extensions.ini: no LED.
    const char *ledTitle = oxos_setting("led_title");
    if (!ledTitle)
        return;

    // Is this the title "led_title" names (any case)? Compared with the kernel's own RtlEqualString. Then ask the run loop for the orange LED.
    ANSI_STRING loaded, wanted;
    RtlInitAnsiString(&loaded, Title->TitleName);
    RtlInitAnsiString(&wanted, ledTitle);
    if (RtlEqualString(&loaded, &wanted, TRUE)) {
        memcpy(LedTitle, Title->TitleName, sizeof(LedTitle));
        LedWantedAt = KeTickCount;
        LedWanted = TRUE;
    }
}

// Before each title starts.
static BOOLEAN ExtensionStart(void) {
    // Logging ExtensionStart message to the log file and serial port.
    oxos_log("Starting extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);

    // Getting the state page, which keeps its contents from one launch to the next, and counting this launch in it.
    LAST_TITLE *last = (LAST_TITLE *)oxos_state();
    last->Launches++;

    // Registering our Example_TitleLoaded_Callback function for the title-loaded event, with the state page as its UserData.
    // The kernel calls Example_TitleLoaded_Callback once the title is loaded, before it starts.
    BOOLEAN res = oxos_on_title_loaded(Example_TitleLoaded_Callback, last);
    return res;
}

// This will be called again and again by the kernel once every 1ms while the title runs.
static void ExtensionRunLoop(void) {
    // Nothing asked for, or not yet time: return at once, the other extensions wait for this call.
    if (!LedWanted || KeTickCount - LedWantedAt < LED_DELAY_MS)
        return;

    // Turning the LED orange, with the kernel's HalWriteSMBusValue (oxos_kernel.h), once.
    LedWanted = FALSE;
    HalWriteSMBusValue(SMC_ADDRESS, SMC_LED_PATTERN, FALSE, LED_ORANGE);
    HalWriteSMBusValue(SMC_ADDRESS, SMC_LED_MODE, FALSE, 1);
    oxos_log("\"%s\": the LED is orange.", LedTitle);
}

// The title is starting another title or the dashboard.
static void ExtensionTerminate(void) {
    oxos_log("Terminating extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);
}
