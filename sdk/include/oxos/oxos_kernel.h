// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK: the Xbox kernel's types and the functions an extension calls (spec/oxos_internal/34 §5.1, §7.1). C and C++.
//
// - In C the kernel functions have their own names: NtOpenFile(...).
// - In C++ they are in namespace oxos: oxos::NtOpenFile(...). A member function named like the kernel function (the extension class, §4.4) calls the kernel's
//   with oxos::, not itself.
// Either way the call goes through the extension's import of the function, so a hooked function passes the call on down the chain (§7.1). This first version
// declares the functions the SDK and the test extensions use; the complete table is generated from the kernel's exports (spec/oxos_internal/34 §9).
#pragma once
#include <oxos/oxos_types.h>

// --- Types

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _OBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    POBJECT_STRING ObjectName;
    ULONG Attributes;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _FILE_NETWORK_OPEN_INFORMATION {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile;
    ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION, *PFILE_NETWORK_OPEN_INFORMATION;

typedef struct _DISPATCHER_HEADER {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size;
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT;

typedef void(NTAPI *PIO_APC_ROUTINE)(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved);
typedef __builtin_va_list va_list;
#define va_start(args, last) __builtin_va_start(args, last)
#define va_end(args) __builtin_va_end(args)

#define OBJ_CASE_INSENSITIVE 0x40UL
#define GENERIC_READ 0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define SYNCHRONIZE 0x00100000UL
#define FILE_SHARE_READ 1UL
#define FILE_SHARE_WRITE 2UL
#define FILE_OPEN 1UL
#define FILE_OVERWRITE_IF 5UL
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20UL
#define FILE_NON_DIRECTORY_FILE 0x40UL
#define FileStandardInformation 5UL
#define Executive 0UL // KWAIT_REASON

// --- The kernel's memory functions (OXOS's own exports, 402-405): also what the compiler calls for copies

#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *Destination, const void *Source, SIZE_T Count);
void *memmove(void *Destination, const void *Source, SIZE_T Count);
void *memset(void *Destination, int Value, SIZE_T Count);
int memcmp(const void *A, const void *B, SIZE_T Count);
#ifdef __cplusplus
}
#endif

// --- Functions (ordinals in the SDK source's src/xboxkrnl.def)

#ifdef __cplusplus
#define OXOS_KERNEL_FUNCTIONS_BEGIN                                                                                    \
    namespace oxos {                                                                                                   \
    extern "C" {
#define OXOS_KERNEL_FUNCTIONS_END                                                                                      \
    }                                                                                                                  \
    }
#else
#define OXOS_KERNEL_FUNCTIONS_BEGIN
#define OXOS_KERNEL_FUNCTIONS_END
#endif
#define OXOS_IMPORT __declspec(dllimport)

OXOS_KERNEL_FUNCTIONS_BEGIN
OXOS_IMPORT ULONG __cdecl DbgPrint(const char *Format, ...);
OXOS_IMPORT PVOID NTAPI ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag);
OXOS_IMPORT void NTAPI ExFreePool(PVOID P);
OXOS_IMPORT NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
OXOS_IMPORT NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
OXOS_IMPORT NTSTATUS NTAPI NtClose(HANDLE Handle);
OXOS_IMPORT NTSTATUS NTAPI NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                                        PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions);
OXOS_IMPORT NTSTATUS NTAPI NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                                      ULONG ShareAccess, ULONG OpenOptions);
OXOS_IMPORT NTSTATUS NTAPI NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes, PFILE_NETWORK_OPEN_INFORMATION FileInformation);
OXOS_IMPORT NTSTATUS NTAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
                                                  ULONG FileInformationClass);
OXOS_IMPORT NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                                      PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset);
OXOS_IMPORT NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                                       PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset);
OXOS_IMPORT void NTAPI RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString);
OXOS_IMPORT int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args);
// OXOS's own (416): one line of an extension's log, "[SECTION] Text", to where Context->LogTarget says (oxos_log calls it).
OXOS_IMPORT void NTAPI OXOSWriteExtensionLog(const OXOS_CONTEXT *Context, PCSZ Text);

// Data: milliseconds since the console started (it counts up by one each millisecond, and wraps after 49 days).
OXOS_IMPORT extern volatile ULONG KeTickCount;
OXOS_KERNEL_FUNCTIONS_END

// --- Ordinals of the functions above that extensions hook (kernel/exports.tsv)

#define OXOS_ORDINAL_NtOpenFile 202UL
#define OXOS_ORDINAL_NtQueryFullAttributesFile 210UL
