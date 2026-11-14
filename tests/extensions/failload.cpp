// Test extension (spec/oxos_internal/34 §9.3): hooks NtOpenFile, then returns FALSE from its start. The kernel must unload it and drop the hook: a title call
// through it would run freed memory.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "failload", .Version = "1.0.0", .Start = ExtensionStart);

static NTSTATUS NTAPI RefuseNtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    return STATUS_ACCESS_DENIED;
}

static BOOLEAN ExtensionStart(void) {
    oxos_hook(NtOpenFile, RefuseNtOpenFile);
    return FALSE;
}
