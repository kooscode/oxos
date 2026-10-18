// Basic NT-style types for the Xbox kernel ABI (i386, MSVC layout).
#pragma once

typedef unsigned char UCHAR, BOOLEAN, KIRQL, *PUCHAR, *PKIRQL, *PBOOLEAN;
typedef char CHAR, CCHAR, *PCHAR;
typedef unsigned short USHORT, WCHAR, *PUSHORT;
typedef short SHORT, CSHORT;
typedef unsigned long ULONG, *PULONG;
typedef long LONG, NTSTATUS, *PLONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG, *PLONGLONG;
typedef void VOID, *PVOID;
typedef ULONG ULONG_PTR, SIZE_T, *PSIZE_T;
typedef ULONG PHYSICAL_ADDRESS;

#define NTAPI __stdcall
#define FASTCALL __fastcall
#define XBAPI extern "C"
// State that survives a quick reboot (spec/stock/02 §1): neither re-copied from flash nor zeroed.
#define XK_PERSIST __attribute__((section(".persist")))

#define TRUE 1
#define FALSE 0
#define NULL nullptr

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

// Compile-time ABI layout checks.
#define XK_OFFSET(type, field, off) static_assert(__builtin_offsetof(type, field) == (off), #type "." #field)
#define XK_SIZE(type, size) static_assert(sizeof(type) == (size), "sizeof " #type)

XK_SIZE(LARGE_INTEGER, 8);
XK_SIZE(LIST_ENTRY, 8);
