// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK (spec/oxos_internal/34): what an extension's source includes, in C or C++. The kernel's
// types and functions (oxos/oxos_kernel.h), the structures the kernel and the extension exchange
// (oxos/oxos_interfaces.h), the descriptor macro and the runtime's helpers (in lib/oxos_sdk.lib).
//
//   static NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) { ...; return STATUS_SUCCESS; }
//   OXOS_DEFINE_EXTENSION(.Name = "hello", .Version = "1.0.0", .Load = Load);
//
// The fields after the macro's own (Magic, ApiVersion) are OXOS_EXTENSION's, in its order: Name, Version, Flags,
// Load, Main, Unload (C++ wants designated fields in declaration order).
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

#ifdef __cplusplus
extern "C" {
#endif

// One line to the debug serial port, after the extension's section name ("[name] ...").
void oxos_log(const OXOS_CONTEXT *Context, const char *Format, ...);

// The value of Key in the extension's section of oxos_extensions.ini (any case), or null.
const char *oxos_get_setting(const OXOS_CONTEXT *Context, const char *Key);

// Waits up to Milliseconds for the kernel's stop request (Main's end, §4.3); TRUE once it came.
BOOLEAN oxos_wait_for_stop(const OXOS_CONTEXT *Context, ULONG Milliseconds);

// Reads a whole file (a \Device\... path) of at most MaxSize bytes into pool memory the caller frees with ExFreePool;
// null when it does not open, is empty or larger, or cannot be read.
char *oxos_read_file(const char *Path, ULONG MaxSize, ULONG *Size);

#ifdef __cplusplus
}
#endif
