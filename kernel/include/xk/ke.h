// Kernel core structures. Layouts are ABI: titles and XDK libraries read fs:[...] directly.
// Offsets are from spec/01-boot.md (observed in xboxkrnl 4627) and nxdk headers.
#pragma once
#include <xk/types.h>

#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define SYNCH_LEVEL 0x1C
#define HIGH_LEVEL 0x1F

typedef struct _NT_TIB {
    PVOID ExceptionList;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID SubSystemTib;
    PVOID FiberData;
    PVOID ArbitraryUserPointer;
    struct _NT_TIB *Self;
} NT_TIB;
XK_SIZE(NT_TIB, 0x1C);

// Placeholder thread object until the scheduler exists (milestone 2).
typedef struct _KTHREAD {
    UCHAR Opaque[0x110];
} KTHREAD, *PKTHREAD;

typedef struct _KPRCB {
    PKTHREAD CurrentThread; // +0x00
    PKTHREAD NextThread;    // +0x04
    PKTHREAD IdleThread;    // +0x08
    UCHAR Rest[0x250 - 0x0C];
} KPRCB;

typedef struct _KPCR {
    NT_TIB NtTib;           // +0x00
    struct _KPCR *SelfPcr;  // +0x1C
    KPRCB *Prcb;            // +0x20
    KIRQL Irql;             // +0x24
    UCHAR Pad[3];
    KPRCB PrcbData;         // +0x28
} KPCR;
XK_OFFSET(KPCR, SelfPcr, 0x1C);
XK_OFFSET(KPCR, Prcb, 0x20);
XK_OFFSET(KPCR, Irql, 0x24);
XK_OFFSET(KPCR, PrcbData, 0x28);

typedef struct _KSYSTEM_TIME {
    ULONG LowPart;
    LONG High1Time;
    LONG High2Time;
} KSYSTEM_TIME;

typedef struct _XBOX_KRNL_VERSION {
    USHORT Major, Minor, Build, Qfe;
} XBOX_KRNL_VERSION;

typedef struct _XBOX_HARDWARE_INFO {
    ULONG Flags;
    UCHAR GpuRevision;
    UCHAR McpRevision;
    UCHAR Unknown3;
    UCHAR Unknown4;
} XBOX_HARDWARE_INFO;

extern KPCR KiPcr;
extern KTHREAD KiIdleThread;

// Exported data (ordinals in xboxkrnl.def).
XBAPI volatile ULONG KeTickCount;
XBAPI volatile KSYSTEM_TIME KeInterruptTime;
XBAPI volatile KSYSTEM_TIME KeSystemTime;
XBAPI ULONG KeTimeIncrement;
XBAPI XBOX_KRNL_VERSION XboxKrnlVersion;
XBAPI XBOX_HARDWARE_INFO XboxHardwareInfo;
XBAPI UCHAR XboxEEPROMKey[16];

// Internal.
extern volatile ULONG KiBootStage;
XBAPI void KiSystemStartup(const UCHAR *bootKeys);
void KiInitializeProcessor();
void KiIdleLoop() __attribute__((noreturn));
XBAPI void KiClockInterrupt();
XBAPI void KiUnexpectedException(ULONG vector, ULONG errorCode, ULONG eip);

XBAPI KIRQL NTAPI KeGetCurrentIrql();
XBAPI KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql);
XBAPI void FASTCALL KfLowerIrql(KIRQL NewIrql);
XBAPI void NTAPI KeStallExecutionProcessor(ULONG MicroSeconds);
