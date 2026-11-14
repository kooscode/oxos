// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK (spec/oxos_internal/34): what an extension's source includes, in C or C++. The kernel's types and functions (oxos/oxos_kernel.h),
// the structures the kernel and the extension exchange (oxos/oxos_interfaces.h), the descriptor macro and the helpers (inline, below).
//
//   static NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) { ...; return STATUS_SUCCESS; }
//   OXOS_DEFINE_EXTENSION(.Name = "hello", .Version = "1.0.0", .Load = Load);
//
// The fields after the macro's own (Magic, ApiVersion) are OXOS_EXTENSION's, in its order: Name, Version, Flags, Load, RunLoop, Unload (C++ wants designated
// fields in declaration order).
#pragma once
#include <oxos/oxos_types.h>
#include <oxos/oxos_interfaces.h>
#include <oxos/oxos_kernel.h>

#ifdef __cplusplus
#define OXOS_EXTERN_C extern "C"
#else
#define OXOS_EXTERN_C
#endif

#define OXOS_DEFINE_EXTENSION(...)                                                                                    \
    OXOS_EXTERN_C __declspec(dllexport) const OXOS_EXTENSION oxos_extension = {                                      \
        .Magic = OXOS_EXTENSION_MAGIC, .ApiVersion = OXOS_API_VERSION, __VA_ARGS__}

// --- Helpers: inline, built on the kernel's exports, usable at any time (Load, RunLoop, hooks, events)

// The kernel's functions are global in C and in namespace oxos in C++ (oxos/oxos_kernel.h): the helpers' bodies see them by their plain names in both.
#ifdef __cplusplus
#define OXOS_KERNEL_NAMES using namespace oxos
#else
#define OXOS_KERNEL_NAMES ((void)0)
#endif

// Where oxos_log writes from now on: OXOS_LOG_SERIAL, OXOS_LOG_FILE (C:\OXOS\log\extensions.log), or both; at any time (Load, RunLoop, hooks, events). The
// kernel starts every extension at both (a release kernel has no serial output, so there the lines go to the file alone).
static inline void oxos_set_log_target(OXOS_CONTEXT *Context, ULONG Target) {
    Context->LogTarget = Target;
}

// One line, "[name] ..." after the extension's section name, at most 255 characters, to where oxos_set_log_target said. The file is written at PASSIVE_LEVEL
// only (file I/O): a line logged at a raised IRQL reaches only the serial port, when that is a target too.
static inline void oxos_log(const OXOS_CONTEXT *Context, const char *Format, ...) {
    OXOS_KERNEL_NAMES;
    char text[224];
    va_list args;
    __builtin_va_start(args, Format);
    RtlVsnprintf(text, sizeof(text), Format, args);
    __builtin_va_end(args);
    OXOSWriteExtensionLog(Context, text); // the kernel sends it where Context->LogTarget says
}

// The value of Key in the extension's section of oxos_extensions.ini (any case), or null.
static inline const char *oxos_get_setting(const OXOS_CONTEXT *Context, const char *Key) {
    for (ULONG i = 0; i < Context->SettingCount; i++) {
        const char *k = Context->Settings[i].Key;
        ULONG n = 0;
        while (k && k[n] && Key[n] && (k[n] | 0x20) == (Key[n] | 0x20))
            n++;
        if (k && !k[n] && !Key[n])
            return Context->Settings[i].Value;
    }
    return NULL;
}

// Reads a whole file (a \Device\... path) of at most MaxSize bytes into pool memory the caller frees with ExFreePool; null when it does not open, is empty or
// larger, or cannot be read.
static inline char *oxos_read_file(const char *Path, ULONG MaxSize, ULONG *Size) {
    OXOS_KERNEL_NAMES;
    *Size = 0;
    OBJECT_STRING name;
    RtlInitAnsiString(&name, Path);
    OBJECT_ATTRIBUTES attributes = {NULL, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    if (!NT_SUCCESS(NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
        return NULL;
    ULONG info[6] = {0}; // FILE_STANDARD_INFORMATION: EndOfFile at +8
    char *text = NULL;
    if (NT_SUCCESS(NtQueryInformationFile(file, &iosb, info, sizeof(info), FileStandardInformation)) && !info[3] && info[2] && info[2] <= MaxSize)
        text = (char *)ExAllocatePoolWithTag(info[2], 0x6472786F); // "oxrd"
    if (text) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        if (NT_SUCCESS(NtReadFile(file, NULL, NULL, NULL, &iosb, text, info[2], &at))) {
            *Size = iosb.Information;
        } else {
            ExFreePool(text);
            text = NULL;
        }
    }
    NtClose(file);
    return text;
}
