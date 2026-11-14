// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK: the NT base types of the Xbox kernel's interface (i386, Microsoft layout). C and C++.
#pragma once

typedef unsigned char UCHAR, BOOLEAN, KIRQL, *PUCHAR, *PBOOLEAN, *PKIRQL;
typedef char CHAR, CCHAR, *PCHAR;
typedef unsigned short USHORT, WCHAR, *PUSHORT, *PWSTR;
typedef short SHORT, CSHORT;
typedef unsigned long ULONG, *PULONG;
typedef long LONG, NTSTATUS, *PLONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG, *PLONGLONG;
typedef void VOID, *PVOID;
typedef PVOID HANDLE, *PHANDLE;
typedef const char *PCSZ;
typedef ULONG ULONG_PTR, SIZE_T, *PSIZE_T, ACCESS_MASK;
typedef ULONG PHYSICAL_ADDRESS;
typedef CHAR KPROCESSOR_MODE;

#define NTAPI __stdcall
#define FASTCALL __fastcall
#define TRUE 1
#define FALSE 0
#define KernelMode 0
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void *)0)
#endif
#endif

#ifdef __cplusplus
#define OXOS_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define OXOS_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

// Counted 8-bit strings: object names, paths.
typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} STRING, ANSI_STRING, OBJECT_STRING, *PSTRING, *PANSI_STRING, *POBJECT_STRING;

OXOS_STATIC_ASSERT(sizeof(LARGE_INTEGER) == 8 && sizeof(STRING) == 8, "the kernel's layout");
