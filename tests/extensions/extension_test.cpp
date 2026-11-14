// Test extension for the kernel extension loader (spec/oxos_internal/34 §9.3), run by tests/boot_test.py --extensions: it records in its state page what the
// kernel did, for the test to read back after quick reboots.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
static void ExtensionTerminate(void);

OXOS_DEFINE_EXTENSION(.Name = "extension_test", .Version = "1.0.0", .Start = ExtensionStart, .RunLoop = ExtensionRunLoop, .Terminate = ExtensionTerminate);

#define SAVED_MAGIC 0x54534F58UL // "XOST"
#define OWN_FILE "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\extension_test.xos"

// The state page as boot_test.py reads it (words, in this order).
struct SAVED {
    ULONG Magic;
    ULONG Launches;           // Start calls since the cold boot
    ULONG Generation;         // oxos_launch_number() at the last Start
    ULONG GenerationMismatch; // a Start whose launch number was not Launches
    ULONG Terminates;         // ExtensionTerminate calls
    ULONG RunLoopCalls;       // ExtensionRunLoop calls, all launches
    ULONG LoopAfterTerminate; // ExtensionRunLoop calls after this launch's Terminate (must stay 0: the loop stops first)
    ULONG Loading;            // title-loading events
    ULONG Loaded;             // title-loaded events
    ULONG Exiting;            // title-exiting events
    LONG OpenCalls;           // the title's NtOpenFile calls through the hook (not its own: they start below it)
    ULONG LastTitleId;        // the title ID of the last title-loaded event
    ULONG SettingOk;          // oxos_setting("Answer") was "42" at every Start
    ULONG FileOk;             // oxos_read_file read the extension's own file
};

static SAVED *Saved;
static BOOLEAN OwnFileOpened; // this launch's RunLoop opened the extension's own file
static BOOLEAN Terminated;    // this launch's Terminate has run

static NTSTATUS NTAPI CountNtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    InterlockedIncrement(&Saved->OpenCalls);
    return oxos_next(NtOpenFile)(File, Access, Name, Iosb, Share, Options);
}

// UserData is the counter each event registered with.
static void CountLoading(const OXOS_TITLE_LOADING *Title, void *UserData) {
    ++*(ULONG *)UserData;
}

static void CountLoaded(const OXOS_TITLE_LOADED *Title, void *UserData) {
    ++*(ULONG *)UserData;
    Saved->LastTitleId = Title->TitleId;
}

static void CountExiting(const OXOS_TITLE_EXITING *Title, void *UserData) {
    ++*(ULONG *)UserData;
}

static BOOLEAN SameText(const char *A, const char *B) {
    while (A && *A && *A == *B) {
        A++;
        B++;
    }
    return A && *A == *B;
}

static BOOLEAN ExtensionStart(void) {
    Saved = (SAVED *)oxos_state();
    if (!Saved->Launches) { // the first launch: the page is all zero
        Saved->Magic = SAVED_MAGIC;
        Saved->SettingOk = 1;
    }
    Saved->Launches++;
    Saved->Generation = oxos_launch_number();
    if (oxos_launch_number() != Saved->Launches)
        Saved->GenerationMismatch++;
    if (!SameText(oxos_setting("Answer"), "42"))
        Saved->SettingOk = 0;
    ULONG size;
    char *file = oxos_read_file(OWN_FILE, 0x40000, &size);
    if (file) {
        Saved->FileOk = size > 2 && file[0] == 'M' && file[1] == 'Z';
        oxos_free_file(file);
    }
    oxos_on_title_loading(CountLoading, &Saved->Loading);
    oxos_on_title_loaded(CountLoaded, &Saved->Loaded);
    oxos_on_title_exiting(CountExiting, &Saved->Exiting);
    oxos_log("launch %lu, generation %lu", Saved->Launches, oxos_launch_number());
    return oxos_hook(NtOpenFile, CountNtOpenFile);
}

static void ExtensionRunLoop(void) {
    if (Terminated) {
        Saved->LoopAfterTerminate++;
        return;
    }
    Saved->RunLoopCalls++;
    if (OwnFileOpened)
        return;
    // One NtOpenFile of its own once the chains are linked: it must pass chain.xos's hook (loaded after this one).
    OwnFileOpened = TRUE;
    ULONG size;
    oxos_free_file(oxos_read_file(OWN_FILE, 0x40000, &size));
}

static void ExtensionTerminate(void) {
    Terminated = TRUE;
    Saved->Terminates++;
}
