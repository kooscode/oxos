// Test extension for the kernel extension loader (spec/oxos_internal/34 §9.3), run by tests/boot_test.py --extensions: it records in its state page what the
// kernel did, for the test to read back after quick reboots.
#include <oxos_sdk.h>

namespace {

constexpr ULONG SAVED_MAGIC = 0x54534F58; // "XOST"
constexpr char OWN_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\extension_test.xos";

// The state page as boot_test.py reads it (words, in this order).
struct SAVED {
    ULONG Magic;
    ULONG Launches;           // Load calls since the cold boot
    ULONG Generation;         // OXOS_CONTEXT::Generation at the last Load
    ULONG GenerationMismatch; // a Load whose Generation was not Launches
    ULONG Unloads;            // Unload calls
    ULONG RunLoopCalls;       // RunLoop calls, all launches
    ULONG LoopAfterUnload;    // RunLoop calls after this launch's Unload (must stay 0: the loop stops first)
    ULONG Loading;            // OXOS_EVENT_TITLE_LOADING events
    ULONG Loaded;             // OXOS_EVENT_TITLE_LOADED events
    ULONG Exiting;            // OXOS_EVENT_TITLE_EXITING events
    ULONG OpenCalls;          // the title's NtOpenFile calls through the hook (not its own: they start below it)
    ULONG LastTitleId;        // the title ID of the last OXOS_EVENT_TITLE_LOADED
    ULONG SettingOk;          // Setting("answer") was "42" at every Load
    ULONG FileOk;             // oxos_read_file read the extension's own file
};

SAVED *Saved;
decltype(&oxos::NtOpenFile) NextNtOpenFile;
bool OwnFileOpened; // this launch's RunLoop opened the extension's own file (globals start afresh at each launch)
bool Unloaded;      // this launch's Unload has run

NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                              ULONG ShareAccess, ULONG OpenOptions) {
    __atomic_add_fetch(&Saved->OpenCalls, 1, __ATOMIC_RELAXED);
    return NextNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
}

void NTAPI OnEvent(OXOS_CONTEXT *Context, PVOID Which, PVOID Data) {
    auto event = reinterpret_cast<ULONG>(Which);
    if (event == OXOS_EVENT_TITLE_LOADING) {
        Saved->Loading++;
    } else if (event == OXOS_EVENT_TITLE_LOADED) {
        Saved->Loaded++;
        Saved->LastTitleId = static_cast<OXOS_TITLE_LOADED *>(Data)->TitleId;
    } else {
        Saved->Exiting++;
    }
}

bool SameText(const char *A, const char *B) {
    while (A && *A && *A == *B) {
        A++;
        B++;
    }
    return A && *A == *B;
}

NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    Saved = static_cast<SAVED *>(Context->State);
    if (!Saved)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (Context->StateIsNew || Saved->Magic != SAVED_MAGIC) {
        *Saved = {};
        Saved->Magic = SAVED_MAGIC;
        Saved->SettingOk = 1;
    }
    Saved->Launches++;
    Saved->Generation = Context->Generation;
    if (Context->Generation != Saved->Launches)
        Saved->GenerationMismatch++;
    if (!SameText(oxos_get_setting(Context, "Answer"), "42"))
        Saved->SettingOk = 0;
    ULONG size;
    char *file = oxos_read_file(OWN_FILE, 0x40000, &size);
    if (file) {
        Saved->FileOk = size > 2 && file[0] == 'M' && file[1] == 'Z';
        oxos::ExFreePool(file);
    }
    for (ULONG event = OXOS_EVENT_TITLE_LOADING; event <= OXOS_EVENT_TITLE_EXITING; event++)
        Context->Api->RegisterEvent(Context, event, reinterpret_cast<PVOID>(OnEvent), reinterpret_cast<PVOID>(event));
    oxos_log(Context, "launch %lu, generation %lu", Saved->Launches, Context->Generation);
    return Context->Api->HookExport(Context, OXOS_ORDINAL_NtOpenFile, reinterpret_cast<PVOID>(HookNtOpenFile), reinterpret_cast<PVOID *>(&NextNtOpenFile));
}

void NTAPI RunLoop(OXOS_CONTEXT *Context) {
    if (Unloaded) {
        Saved->LoopAfterUnload++;
        return;
    }
    Saved->RunLoopCalls++;
    if (OwnFileOpened)
        return;
    // One NtOpenFile of its own once the chains are linked: it must pass chain.xos's hook (loaded after this one).
    OwnFileOpened = true;
    ULONG size;
    char *file = oxos_read_file(OWN_FILE, 0x40000, &size);
    if (file)
        oxos::ExFreePool(file);
}

void NTAPI Unload(OXOS_CONTEXT *Context) {
    Unloaded = true;
    Saved->Unloads++;
}

} // namespace

OXOS_DEFINE_EXTENSION(.Name = "extension_test", .Version = "1.0.0", .Load = Load, .RunLoop = RunLoop, .Unload = Unload);
