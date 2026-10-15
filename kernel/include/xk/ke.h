// Kernel core structures. Layouts are ABI: titles allocate these objects and XDK libraries read
// fs:[...] directly. Offsets from spec/01, spec/06 (observed in xboxkrnl 4627) and nxdk headers.
#pragma once
#include <xk/types.h>

#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define PROFILE_LEVEL 26
#define CLOCK_LEVEL 26
#define SYNCH_LEVEL 0x1C
#define HIGH_LEVEL 0x1F

typedef char SCHAR;

// --- Dispatcher objects (spec/06)

enum KOBJECTS : UCHAR {
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    ProcessObject = 3,
    QueueObject = 4,
    SemaphoreObject = 5,
    ThreadObject = 6,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
    ApcObject = 0x12,
    DpcObject = 0x13,
    DeviceQueueObject = 0x14,
};

typedef struct _DISPATCHER_HEADER {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size; // in dwords
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;
XK_SIZE(DISPATCHER_HEADER, 0x10);

struct _KDPC;
typedef void(NTAPI *PKDEFERRED_ROUTINE)(struct _KDPC *Dpc, PVOID DeferredContext, PVOID SystemArgument1,
                                         PVOID SystemArgument2);

typedef struct _KDPC {
    CSHORT Type;
    BOOLEAN Inserted;
    UCHAR Padding;
    LIST_ENTRY DpcListEntry;
    PKDEFERRED_ROUTINE DeferredRoutine;
    PVOID DeferredContext;
    PVOID SystemArgument1;
    PVOID SystemArgument2;
} KDPC, *PKDPC;
XK_SIZE(KDPC, 0x1C);

typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    ULONGLONG DueTime;
    LIST_ENTRY TimerListEntry;
    PKDPC Dpc;
    LONG Period;
} KTIMER, *PKTIMER;
XK_SIZE(KTIMER, 0x28);
XK_OFFSET(KTIMER, DueTime, 0x10);
XK_OFFSET(KTIMER, Dpc, 0x20);

typedef enum _TIMER_TYPE { NotificationTimer, SynchronizationTimer } TIMER_TYPE;
typedef enum _EVENT_TYPE { NotificationEvent, SynchronizationEvent } EVENT_TYPE;

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT;

typedef enum _KINTERRUPT_MODE { LevelSensitive, Latched } KINTERRUPT_MODE;

struct _KINTERRUPT;
typedef BOOLEAN(NTAPI *PKSERVICE_ROUTINE)(struct _KINTERRUPT *Interrupt, PVOID ServiceContext);

typedef struct _KINTERRUPT {
    PKSERVICE_ROUTINE ServiceRoutine;
    PVOID ServiceContext;
    ULONG BusInterruptLevel;
    ULONG Irql;
    BOOLEAN Connected;
    BOOLEAN ShareVector;
    UCHAR Mode;
    UCHAR Padding7;
    ULONG ServiceCount;
    ULONG DispatchCode[22];
} KINTERRUPT, *PKINTERRUPT;
XK_SIZE(KINTERRUPT, 0x70);
XK_OFFSET(KINTERRUPT, DispatchCode, 0x18);

// --- Processor and thread

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

// Placeholder until the scheduler exists (milestone 2b), sized like the reference KTHREAD.
typedef struct _KTHREAD {
    UCHAR Opaque[0x110];
} KTHREAD, *PKTHREAD;

typedef struct _KPRCB {
    PKTHREAD CurrentThread;      // +0x00
    PKTHREAD NextThread;         // +0x04
    PKTHREAD IdleThread;         // +0x08
    PKTHREAD NpxThread;          // +0x0C
    ULONG InterruptCount;        // +0x10
    ULONG DpcTime;               // +0x14
    ULONG InterruptTime;         // +0x18
    ULONG DebugDpcTime;          // +0x1C
    ULONG KeContextSwitches;     // +0x20
    ULONG DpcInterruptRequested; // +0x24
    LIST_ENTRY DpcListHead;      // +0x28
    ULONG DpcRoutineActive;      // +0x30
    PVOID DpcStack;              // +0x34
    ULONG QuantumEnd;            // +0x38
    UCHAR NpxSaveArea[0x210];    // +0x3C
    PVOID DmEnetFunc;            // +0x24C
    PVOID DebugMonitorData;      // +0x250
    PVOID DebugHaltThread;       // +0x254
    PVOID DebugDoubleFault;      // +0x258
} KPRCB;
XK_OFFSET(KPRCB, DpcInterruptRequested, 0x24);
XK_OFFSET(KPRCB, DpcListHead, 0x28);
XK_OFFSET(KPRCB, DpcRoutineActive, 0x30);
XK_OFFSET(KPRCB, DpcStack, 0x34);

typedef struct _KPCR {
    NT_TIB NtTib;          // +0x00
    struct _KPCR *SelfPcr; // +0x1C
    KPRCB *Prcb;           // +0x20
    KIRQL Irql;            // +0x24
    UCHAR Pad[3];
    KPRCB PrcbData;        // +0x28
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

// --- Exported data (ordinals in xboxkrnl.def)
XBAPI volatile ULONG KeTickCount;
XBAPI volatile KSYSTEM_TIME KeInterruptTime;
XBAPI volatile KSYSTEM_TIME KeSystemTime;
XBAPI ULONG KeTimeIncrement;
XBAPI XBOX_KRNL_VERSION XboxKrnlVersion;
XBAPI XBOX_HARDWARE_INFO XboxHardwareInfo;
XBAPI UCHAR XboxEEPROMKey[16];

// --- Exported routines
XBAPI KIRQL NTAPI KeGetCurrentIrql();
XBAPI KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql);
XBAPI void FASTCALL KfLowerIrql(KIRQL NewIrql);
XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel();
XBAPI KIRQL NTAPI KeRaiseIrqlToSynchLevel();
XBAPI void NTAPI KeStallExecutionProcessor(ULONG MicroSeconds);
XBAPI ULONGLONG NTAPI KeQueryInterruptTime();
XBAPI void NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime);

XBAPI void NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
XBAPI BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
XBAPI BOOLEAN NTAPI KeRemoveQueueDpc(PKDPC Dpc);
XBAPI BOOLEAN NTAPI KeIsExecutingDpc();

XBAPI void NTAPI KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type);
XBAPI BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
XBAPI BOOLEAN NTAPI KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc);
XBAPI BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer);

XBAPI void NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
XBAPI LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
XBAPI LONG NTAPI KeResetEvent(PKEVENT Event);

XBAPI void NTAPI KeInitializeInterrupt(PKINTERRUPT Interrupt, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext,
                                       ULONG Vector, KIRQL Irql, KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector);
XBAPI BOOLEAN NTAPI KeConnectInterrupt(PKINTERRUPT Interrupt);
XBAPI BOOLEAN NTAPI KeDisconnectInterrupt(PKINTERRUPT Interrupt);

// --- Internal
extern volatile ULONG KiBootStage;
XBAPI void KiSystemStartup(const UCHAR *bootKeys);
void KiInitializeProcessor();
void KiInitializeTimers();
void KiIdleLoop() __attribute__((noreturn));
void KiClockInterrupt();
void KiCheckTimers();
void KiDispatchInterrupt();
void KiRetireDpcList();
void KiDeliverSoftwareInterrupts(); // interrupts disabled on entry and exit
XBAPI void KiDispatchHardwareInterrupt(ULONG irq);
XBAPI void KiUnexpectedException(ULONG vector, ULONG errorCode, ULONG eip);

// Self-tests run at boot (milestone checks read their counters over QMP).
void KiRunBootSelfTests();

// List helpers (NT semantics).
static inline void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static inline bool IsListEmpty(const LIST_ENTRY *h) { return h->Flink == h; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e) {
    e->Flink = h;
    e->Blink = h->Blink;
    h->Blink->Flink = e;
    h->Blink = e;
}
static inline void InsertHeadList(PLIST_ENTRY h, PLIST_ENTRY e) {
    e->Flink = h->Flink;
    e->Blink = h;
    h->Flink->Blink = e;
    h->Flink = e;
}
static inline void RemoveEntryList(PLIST_ENTRY e) {
    e->Blink->Flink = e->Flink;
    e->Flink->Blink = e->Blink;
}
static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h) {
    PLIST_ENTRY e = h->Flink;
    RemoveEntryList(e);
    return e;
}
#define CONTAINING_RECORD(addr, type, field) ((type *)((UCHAR *)(addr) - __builtin_offsetof(type, field)))
