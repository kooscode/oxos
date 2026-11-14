// Test extension (spec/oxos_internal/34 §9.3): hooks NtOpenFile, then fails its Load. The kernel must unload it and
// drop the hook: a title call through it would run freed memory.
#include <oxos_sdk.h>

namespace {

decltype(&oxos::NtOpenFile) NextNtOpenFile;

NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                              PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions) {
    return STATUS_ACCESS_DENIED;
}

NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    Context->Api->HookExport(Context, OXOS_ORDINAL_NtOpenFile, reinterpret_cast<PVOID>(HookNtOpenFile),
                             reinterpret_cast<PVOID *>(&NextNtOpenFile));
    return STATUS_UNSUCCESSFUL;
}

} // namespace

OXOS_DEFINE_EXTENSION(.Name = "failload", .Version = "1.0.0", .Load = Load);
