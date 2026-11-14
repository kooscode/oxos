// Test extension (spec/oxos_internal/34 §9.3), listed after extension_test.xos: hooks the same NtOpenFile, so every title call passes extension_test's hook,
// then this one, then the kernel; extension_test's own NtOpenFile calls come here too.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "chain", .Version = "1.0.0", .Start = ExtensionStart);

#define SAVED_MAGIC 0x4E414843UL // "CHAN"

// The state page as boot_test.py reads it.
struct SAVED {
    ULONG Magic;
    ULONG Launches;
    LONG OpenCalls; // NtOpenFile calls through this hook: the title's, and extension_test's own from its RunLoop
};

static SAVED *Saved;

static NTSTATUS NTAPI CountNtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    InterlockedIncrement(&Saved->OpenCalls);
    return oxos_next(NtOpenFile)(File, Access, Name, Iosb, Share, Options);
}

static BOOLEAN ExtensionStart(void) {
    Saved = (SAVED *)oxos_state();
    Saved->Magic = SAVED_MAGIC;
    Saved->Launches++;
    return oxos_hook(NtOpenFile, CountNtOpenFile);
}
