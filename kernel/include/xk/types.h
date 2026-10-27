// Basic NT-style types for the Xbox kernel ABI (i386, MSVC layout).
#pragma once

typedef unsigned char UCHAR, BOOLEAN, KIRQL, *PUCHAR, *PKIRQL, *PBOOLEAN;
typedef char CHAR, CCHAR, *PCHAR;
typedef unsigned short USHORT, WCHAR, *PUSHORT;
typedef short SHORT, CSHORT;
typedef unsigned long ULONG, *PULONG;
typedef long LONG, NTSTATUS, *PLONG;
#ifdef XK_HOST_TEST // i386 Linux aligns 64-bit fields to 4; the kernel's structures follow the Windows layout (8)
typedef unsigned long long ULONGLONG __attribute__((aligned(8)));
typedef long long LONGLONG __attribute__((aligned(8)));
typedef LONGLONG *PLONGLONG;
#else
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG, *PLONGLONG;
#endif
typedef void VOID, *PVOID;
typedef ULONG ULONG_PTR, SIZE_T, *PSIZE_T;
typedef ULONG PHYSICAL_ADDRESS;

#ifdef XK_HOST_TEST // host unit tests (tests/unit): a 32-bit Linux build of the core runtime
#define NTAPI __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))
#else
#define NTAPI __stdcall
#define FASTCALL __fastcall
#endif
#define XBAPI extern "C"
// State that survives a quick reboot (spec/stock/02 §1): neither re-copied from flash nor zeroed.
#define XK_PERSIST __attribute__((section(".persist")))
// Boot-only memory (spec/oxos-internal/18-boot-memory.md): code under #pragma code_seg(".xkb") and read-only data
// marked XK_BOOT_DATA (or under #pragma const_seg(".xka$y")) are used only during a cold boot; their pages are freed
// before the first title starts.
#define XK_BOOT_DATA __attribute__((section(".xka$y")))

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
