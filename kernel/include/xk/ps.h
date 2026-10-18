// Process structure: executive thread (spec/stock/07, spec/stock/10 §5).
#pragma once
#include <xk/ob.h>

// ETHREAD = KTHREAD + executive fields (0x140 bytes, plus the caller's extension).
typedef struct _ETHREAD {
    KTHREAD Tcb;
    LARGE_INTEGER CreateTime;   // 0x110
    LARGE_INTEGER ExitTime;     // 0x118
    NTSTATUS ExitStatus;        // 0x120
    LIST_ENTRY ReaperListEntry; // 0x124
    HANDLE UniqueThread;        // 0x12C
    PVOID StartAddress;         // 0x130
    LIST_ENTRY IrpList;         // 0x134
    PVOID DebugData;            // 0x13C
} ETHREAD, *PETHREAD;
XK_SIZE(ETHREAD, 0x140);
XK_OFFSET(ETHREAD, UniqueThread, 0x12C);
XK_OFFSET(ETHREAD, IrpList, 0x134);

static inline PETHREAD PsGetCurrentThread() { return (PETHREAD)KeGetCurrentThread(); }
