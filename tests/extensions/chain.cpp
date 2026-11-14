// Test extension (spec/oxos_internal/34 §9.3), listed after extension_test.xos: hooks the same NtOpenFile, so every
// title call passes extension_test's hook, then this one, then the kernel; extension_test's own NtOpenFile calls come
// here too.
#include <oxos_sdk.h>

namespace {

constexpr ULONG SAVED_MAGIC = 0x4E414843; // "CHAN"

// The state page as boot_test.py reads it.
struct SAVED {
    ULONG Magic;
    ULONG Launches;
    ULONG OpenCalls; // NtOpenFile calls through this hook: the title's, and extension_test's own from its Main
};

SAVED *Saved;
decltype(&oxos::NtOpenFile) NextNtOpenFile;

NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                              PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions) {
    __atomic_add_fetch(&Saved->OpenCalls, 1, __ATOMIC_RELAXED);
    return NextNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
}

NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    Saved = static_cast<SAVED *>(Context->State);
    if (Context->StateIsNew || Saved->Magic != SAVED_MAGIC)
        *Saved = {SAVED_MAGIC, 0, 0};
    Saved->Launches++;
    return Context->Api->HookExport(Context, OXOS_ORDINAL_NtOpenFile, reinterpret_cast<PVOID>(HookNtOpenFile),
                                    reinterpret_cast<PVOID *>(&NextNtOpenFile));
}

} // namespace

OXOS_DEFINE_EXTENSION(.Name = "chain", .Version = "1.0.0", .Load = Load);
