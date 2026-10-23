// Kernel core structures. Layouts are ABI: titles allocate these objects and XDK libraries read
// fs:[...] directly. Offsets from spec/stock/01, spec/stock/06 (observed in xboxkrnl 4627) and nxdk headers.
#pragma once
#include <xk/types.h>

#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define PROFILE_LEVEL 26
#define CLOCK_LEVEL 0x1C
#define SYNCH_LEVEL 0x1C
#define HIGH_LEVEL 0x1F

typedef char SCHAR;

// --- Dispatcher objects (spec/stock/06)

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

// --- Threads and processes (spec/stock/07). Offsets are checked against the reference kernel's writes.

struct _KTHREAD;

typedef struct _KWAIT_BLOCK {
    LIST_ENTRY WaitListEntry;
    struct _KTHREAD *Thread;
    PVOID Object;
    struct _KWAIT_BLOCK *NextWaitBlock;
    SHORT WaitKey;
    SHORT WaitType; // WaitAll 0, WaitAny 1
} KWAIT_BLOCK, *PKWAIT_BLOCK;
XK_SIZE(KWAIT_BLOCK, 0x18);

typedef enum _WAIT_TYPE { WaitAll, WaitAny } WAIT_TYPE;

typedef struct _KPROCESS {
    LIST_ENTRY ReadyListHead;
    LIST_ENTRY ThreadListHead;
    ULONG StackCount;
    LONG ThreadQuantum;
    SCHAR BasePriority;
    UCHAR DisableBoost;
    UCHAR DisableQuantum;
} KPROCESS, *PKPROCESS;
XK_OFFSET(KPROCESS, ThreadQuantum, 0x14);
XK_OFFSET(KPROCESS, BasePriority, 0x18);
XK_OFFSET(KPROCESS, DisableQuantum, 0x1A);

typedef struct _KAPC_STATE {
    LIST_ENTRY ApcListHead[2];
    PKPROCESS Process;
    BOOLEAN KernelApcInProgress;
    BOOLEAN KernelApcPending;
    BOOLEAN UserApcPending;
    BOOLEAN ApcQueueable;
} KAPC_STATE;
XK_SIZE(KAPC_STATE, 0x18);

typedef struct _KAPC {
    CSHORT Type;
    CHAR ApcMode;
    BOOLEAN Inserted;
    struct _KTHREAD *Thread;
    LIST_ENTRY ApcListEntry;
    PVOID KernelRoutine;
    PVOID RundownRoutine;
    PVOID NormalRoutine;
    PVOID NormalContext;
    PVOID SystemArgument1;
    PVOID SystemArgument2;
} KAPC, *PKAPC;
XK_SIZE(KAPC, 0x28);

typedef struct _KSEMAPHORE {
    DISPATCHER_HEADER Header;
    LONG Limit;
} KSEMAPHORE, *PKSEMAPHORE;
XK_SIZE(KSEMAPHORE, 0x14);

typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;
    LIST_ENTRY MutantListEntry;
    struct _KTHREAD *OwnerThread;
    BOOLEAN Abandoned;
} KMUTANT, *PKMUTANT;
XK_OFFSET(KMUTANT, OwnerThread, 0x18);
XK_OFFSET(KMUTANT, Abandoned, 0x1C);

typedef struct _KQUEUE {
    DISPATCHER_HEADER Header;
    LIST_ENTRY EntryListHead;
    ULONG CurrentCount;
    ULONG MaximumCount;
    LIST_ENTRY ThreadListHead;
} KQUEUE, *PKQUEUE;
XK_OFFSET(KQUEUE, CurrentCount, 0x18);
XK_SIZE(KQUEUE, 0x28);

typedef struct _KDEVICE_QUEUE {
    CSHORT Type;
    UCHAR Size;
    BOOLEAN Busy;
    LIST_ENTRY DeviceListHead;
} KDEVICE_QUEUE, *PKDEVICE_QUEUE;
XK_SIZE(KDEVICE_QUEUE, 0x0C);

typedef struct _KDEVICE_QUEUE_ENTRY {
    LIST_ENTRY DeviceListEntry;
    ULONG SortKey;
    BOOLEAN Inserted;
} KDEVICE_QUEUE_ENTRY, *PKDEVICE_QUEUE_ENTRY;
XK_SIZE(KDEVICE_QUEUE_ENTRY, 0x10);

enum KTHREAD_STATE : UCHAR { Initialized, Ready, Running, Standby, Terminated, Waiting };

typedef struct _KTHREAD {
    DISPATCHER_HEADER Header;      // 0x00
    LIST_ENTRY MutantListHead;     // 0x10
    ULONG KernelTime;              // 0x18
    PVOID StackBase;               // 0x1C
    PVOID StackLimit;              // 0x20
    PVOID KernelStack;             // 0x24
    PVOID TlsData;                 // 0x28
    UCHAR State;                   // 0x2C
    BOOLEAN Alerted[2];            // 0x2D
    BOOLEAN Alertable;             // 0x2F
    UCHAR NpxState;                // 0x30
    CHAR Saturation;               // 0x31
    SCHAR Priority;                // 0x32
    UCHAR Padding;                 // 0x33
    KAPC_STATE ApcState;           // 0x34
    ULONG ContextSwitches;         // 0x4C
    LONG WaitStatus;               // 0x50
    KIRQL WaitIrql;                // 0x54
    CHAR WaitMode;                 // 0x55
    BOOLEAN WaitNext;              // 0x56
    UCHAR WaitReason;              // 0x57
    PKWAIT_BLOCK WaitBlockList;    // 0x58
    LIST_ENTRY WaitListEntry;      // 0x5C (also the ready-list link)
    ULONG WaitTime;                // 0x64
    ULONG KernelApcDisable;        // 0x68
    LONG Quantum;                  // 0x6C
    SCHAR BasePriority;            // 0x70
    UCHAR DecrementCount;          // 0x71
    SCHAR PriorityDecrement;       // 0x72
    BOOLEAN DisableBoost;          // 0x73
    UCHAR NpxIrql;                 // 0x74
    CHAR SuspendCount;             // 0x75
    BOOLEAN Preempted;             // 0x76
    BOOLEAN HasTerminated;         // 0x77
    PKQUEUE Queue;                 // 0x78
    LIST_ENTRY QueueListEntry;     // 0x7C
    KTIMER Timer;                  // 0x88 (8-byte aligned)
    KWAIT_BLOCK TimerWaitBlock;    // 0xB0
    KAPC SuspendApc;               // 0xC8
    KSEMAPHORE SuspendSemaphore;   // 0xF0
    LIST_ENTRY ThreadListEntry;    // 0x104
} KTHREAD, *PKTHREAD;
XK_OFFSET(KTHREAD, TlsData, 0x28);
XK_OFFSET(KTHREAD, NpxState, 0x30);
XK_OFFSET(KTHREAD, Priority, 0x32);
XK_OFFSET(KTHREAD, ApcState, 0x34);
XK_OFFSET(KTHREAD, WaitIrql, 0x54);
XK_OFFSET(KTHREAD, WaitListEntry, 0x5C);
XK_OFFSET(KTHREAD, Quantum, 0x6C);
XK_OFFSET(KTHREAD, HasTerminated, 0x77);
XK_OFFSET(KTHREAD, Timer, 0x88);
XK_OFFSET(KTHREAD, TimerWaitBlock, 0xB0);
XK_OFFSET(KTHREAD, SuspendApc, 0xC8);
XK_OFFSET(KTHREAD, SuspendSemaphore, 0xF0);
XK_OFFSET(KTHREAD, ThreadListEntry, 0x104);
XK_SIZE(KTHREAD, 0x110);

constexpr ULONG NPX_FRAME_LENGTH = 0x210;
constexpr UCHAR NPX_STATE_LOADED = 0, NPX_STATE_NOT_LOADED = 0x0A;

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

typedef enum _KWAIT_REASON {
    Executive = 0, DelayExecution = 4, Suspended = 5, UserRequest = 6, WrEventPair = 0x0E, WrQueue = 0x0F
} KWAIT_REASON;

typedef void(NTAPI *PKNORMAL_ROUTINE)(PVOID NormalContext, PVOID SystemArgument1, PVOID SystemArgument2);
typedef void(NTAPI *PKKERNEL_ROUTINE)(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine, PVOID *NormalContext,
                                      PVOID *SystemArgument1, PVOID *SystemArgument2);
typedef void(NTAPI *PKRUNDOWN_ROUTINE)(PKAPC Apc);
typedef BOOLEAN(NTAPI *PKSYNCHRONIZE_ROUTINE)(PVOID SynchronizeContext);
typedef CHAR KPROCESSOR_MODE;
typedef LONG KPRIORITY;
#define KernelMode 0
#define UserMode 1

constexpr NTSTATUS STATUS_WAIT_0 = 0, STATUS_ABANDONED = 0x80, STATUS_USER_APC = 0xC0, STATUS_KERNEL_APC = 0x100,
                   STATUS_ALERTED = 0x101, STATUS_TIMEOUT = 0x102;

XBAPI PKTHREAD NTAPI KeGetCurrentThread();
XBAPI void NTAPI KeBugCheck(ULONG BugCheckCode) __attribute__((noreturn));
XBAPI void NTAPI KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
XBAPI NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                                           BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                              KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                              PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray);
XBAPI NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
XBAPI LONG NTAPI KePulseEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
XBAPI void NTAPI KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit);
XBAPI LONG NTAPI KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait);
XBAPI void NTAPI KeInitializeMutant(PKMUTANT Mutant, BOOLEAN InitialOwner);
XBAPI LONG NTAPI KeReleaseMutant(PKMUTANT Mutant, LONG Increment, BOOLEAN Abandoned, BOOLEAN Wait);
XBAPI LONG NTAPI KeSetPriorityThread(PKTHREAD Thread, LONG Priority);
XBAPI void NTAPI KeInitializeApc(PKAPC Apc, PKTHREAD Thread, PKKERNEL_ROUTINE KernelRoutine,
                                 PKRUNDOWN_ROUTINE RundownRoutine, PKNORMAL_ROUTINE NormalRoutine,
                                 KPROCESSOR_MODE ApcMode, PVOID NormalContext);
XBAPI BOOLEAN NTAPI KeInsertQueueApc(PKAPC Apc, PVOID SystemArgument1, PVOID SystemArgument2, LONG Increment);
XBAPI void NTAPI KeEnterCriticalRegion();
XBAPI void NTAPI KeLeaveCriticalRegion();
XBAPI BOOLEAN NTAPI KeTestAlertThread(KPROCESSOR_MODE AlertMode);
XBAPI BOOLEAN NTAPI KeAlertThread(PKTHREAD Thread, KPROCESSOR_MODE AlertMode);
XBAPI ULONG NTAPI KeAlertResumeThread(PKTHREAD Thread);
XBAPI ULONG NTAPI KeSuspendThread(PKTHREAD Thread);
XBAPI ULONG NTAPI KeResumeThread(PKTHREAD Thread);
XBAPI void NTAPI KeInitializeQueue(PKQUEUE Queue, ULONG Count);
XBAPI LONG NTAPI KeInsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry);
XBAPI LONG NTAPI KeInsertHeadQueue(PKQUEUE Queue, PLIST_ENTRY Entry);
XBAPI PLIST_ENTRY NTAPI KeRemoveQueue(PKQUEUE Queue, KPROCESSOR_MODE WaitMode, PLARGE_INTEGER Timeout);
XBAPI PLIST_ENTRY NTAPI KeRundownQueue(PKQUEUE Queue);
XBAPI void NTAPI KeBoostPriorityThread(PKTHREAD Thread, KPRIORITY Increment);
XBAPI LONG NTAPI KeSetBasePriorityThread(PKTHREAD Thread, LONG Increment);
XBAPI BOOLEAN NTAPI KeSetDisableBoostThread(PKTHREAD Thread, BOOLEAN Disable);
XBAPI KPRIORITY NTAPI KeSetPriorityProcess(PKPROCESS Process, KPRIORITY BasePriority);
XBAPI void NTAPI KeInitializeDeviceQueue(PKDEVICE_QUEUE DeviceQueue);
XBAPI BOOLEAN NTAPI KeInsertDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY DeviceQueueEntry);
XBAPI BOOLEAN NTAPI KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY DeviceQueueEntry,
                                             ULONG SortKey);
XBAPI PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveDeviceQueue(PKDEVICE_QUEUE DeviceQueue);
XBAPI PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, ULONG SortKey);
XBAPI BOOLEAN NTAPI KeRemoveEntryDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY DeviceQueueEntry);
XBAPI BOOLEAN NTAPI KeSynchronizeExecution(PKINTERRUPT Interrupt, PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                                           PVOID SynchronizeContext);
XBAPI LONG NTAPI KeQueryBasePriorityThread(PKTHREAD Thread);

// --- Internal
extern volatile ULONG KiBootStage;
extern KPROCESS KiIdleProcess, KiSystemProcess;
XBAPI void KiSystemStartup(ULONG arg0, const UCHAR *bootKeys);
void KiInitializeProcessor();
void KiInitializeTimers();
void KiInitializeScheduler(PVOID bootStackTop, PVOID bootStackLimit);
void KiIdleLoop() __attribute__((noreturn));
void KiClockInterrupt(KIRQL interruptedIrql);
void KiCheckTimers();
void KiDispatchInterrupt();
void KiRetireDpcList();
void KiDeliverSoftwareInterrupts(); // interrupts disabled on entry and exit

// Scheduler (dispatcher "lock" = DISPATCH_LEVEL on this uniprocessor).
void KiReadyThread(PKTHREAD Thread);
void KiSetPriorityThread(PKTHREAD Thread, SCHAR Priority);
bool KiSelectYieldThread(PKTHREAD Current);
LONG KiSwapThread(); // caller at DISPATCH_LEVEL with its wait state set up
XBAPI void FASTCALL KiUnlockDispatcherDatabase(KIRQL OldIrql);
void KiWaitTest(DISPATCHER_HEADER *Object, LONG Increment);
void KiUnwaitThread(PKTHREAD Thread, LONG WaitStatus, LONG Increment);
bool KiInsertQueueApc(PKAPC Apc, LONG Increment);
void KiDeliverApc();
void KiExecuteUserApcs();
void KiActivateWaiterQueue(PKQUEUE Queue);
LONG KiInsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry, bool Head);
bool KiInsertTimerRelative(PKTIMER Timer, LONGLONG DueTime);
void KiRemoveTimerIfInserted(PKTIMER Timer);

// Thread creation (Ps* exports wrap this once the object manager exists).
typedef void(NTAPI *PKSYSTEM_ROUTINE)(PVOID StartRoutine, PVOID StartContext);
void KeInitializeThread(PKTHREAD Thread, PVOID KernelStack, ULONG KernelStackSize, ULONG TlsDataSize,
                        PKSYSTEM_ROUTINE SystemRoutine, PVOID StartRoutine, PVOID StartContext, PKPROCESS Process);
PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KeTerminateThread() __attribute__((noreturn));

// Terminated threads whose kernel stacks are freed after the switch away from them.
void KiReapDeadThreads();
XBAPI void KiDispatchHardwareInterrupt(ULONG irq, PULONG frame);
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
