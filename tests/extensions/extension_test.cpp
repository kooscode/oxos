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
    ULONG FileOk;             // oxos_file_read read the extension's own file
    ULONG FileOps;            // the file and folder functions' round trip at the first launch: 1 passed, else the first step that failed (2, 3, ...)
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

// The file and folder functions (oxos_sdk.h §4), once: C:\OXOS\extension_test\ with a file and a folder in it, listed, renamed and deleted again. 1 when every
// step did what it should, else the number of the first one that did not.
#define TEST_FOLDER "\\Device\\Harddisk0\\Partition2\\OXOS\\extension_test"

// The folder's files are exactly the one named File, and its folders exactly the one named Folder.
static bool ListsOne(const char *File, const char *Folder) {
    char **names;
    uint32_t count;
    if (!NT_SUCCESS(oxos_file_list(TEST_FOLDER, names, count)))
        return false;
    bool ok = count == 1 && strcmp(names[0], File) == 0 && !names[1];
    oxos_list_free(names);
    if (!NT_SUCCESS(oxos_directory_list(TEST_FOLDER, names, count)))
        return false;
    ok &= count == 1 && strcmp(names[0], Folder) == 0;
    oxos_list_free(names);
    return ok;
}

static ULONG FileAndFolderRoundTrip(void) {
    ULONG size = 0;
    if (!NT_SUCCESS(oxos_directory_create(TEST_FOLDER)) || !oxos_directory_exists(TEST_FOLDER) || oxos_file_exists(TEST_FOLDER))
        return 2;
    if (oxos_directory_create(TEST_FOLDER) != (NTSTATUS)0xC0000035) // STATUS_OBJECT_NAME_COLLISION: it exists
        return 3;
    if (!NT_SUCCESS(oxos_file_write(TEST_FOLDER "\\a.txt", "hello", 5)) || !oxos_file_exists(TEST_FOLDER "\\a.txt") ||
        oxos_directory_exists(TEST_FOLDER "\\a.txt"))
        return 4;
    char data[64];
    if (!NT_SUCCESS(oxos_file_read(TEST_FOLDER "\\a.txt", data, sizeof(data), &size)) || size != 5 || memcmp(data, "hello", 5) != 0)
        return 5;
    if (!NT_SUCCESS(oxos_directory_create(TEST_FOLDER "\\sub")))
        return 6;
    if (!ListsOne("a.txt", "sub"))
        return 7;
    if (!NT_SUCCESS(oxos_file_rename(TEST_FOLDER "\\a.txt", "b.txt")) || oxos_file_exists(TEST_FOLDER "\\a.txt") ||
        !oxos_file_exists(TEST_FOLDER "\\b.txt"))
        return 8;
    if (!NT_SUCCESS(oxos_directory_rename(TEST_FOLDER "\\sub", "sub2")) || !oxos_directory_exists(TEST_FOLDER "\\sub2"))
        return 9;
    if (NT_SUCCESS(oxos_directory_delete(TEST_FOLDER))) // not empty
        return 10;
    if (!NT_SUCCESS(oxos_file_delete(TEST_FOLDER "\\b.txt")) || !NT_SUCCESS(oxos_directory_delete(TEST_FOLDER "\\sub2")) ||
        !NT_SUCCESS(oxos_directory_delete(TEST_FOLDER)) || oxos_directory_exists(TEST_FOLDER))
        return 11;
    return 1;
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
    // Its own .xos, read whole: its first bytes. A buffer of 2 bytes is too small, and says how large the file is.
    UCHAR head[2];
    ULONG size;
    Saved->FileOk = oxos_file_read(OWN_FILE, head, sizeof(head), &size) == (NTSTATUS)0xC0000023 && size > 1024;
    if (Saved->Launches == 1)
        Saved->FileOps = FileAndFolderRoundTrip();
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
    // One NtOpenFile of its own once the chains are linked: it must pass chain.xos's hook (loaded after this one). Called as the kernel function itself, since
    // that is what hooks see (oxos_file_open and oxos_file_read open inside the kernel).
    OwnFileOpened = TRUE;
    OBJECT_STRING name = {(USHORT)strlen(OWN_FILE), (USHORT)(strlen(OWN_FILE) + 1), (PCHAR)OWN_FILE};
    OBJECT_ATTRIBUTES attributes = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    if (NT_SUCCESS(NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT)))
        NtClose(file);
}

static void ExtensionTerminate(void) {
    Terminated = TRUE;
    Saved->Terminates++;
}
