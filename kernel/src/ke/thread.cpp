// Thread and process objects (spec/07 "Thread creation").
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/ob.h>
#include <xk/rtl.h>

#define Prcb (KiPcr.PrcbData)

void KiInitializeNpxArea(PKTHREAD t);
void KiNpxRelease(PKTHREAD t);
#if XK_TRACE_IMPORTS
extern "C" void XkTraceThreadExit(PKTHREAD Thread);
#endif
extern "C" void KiThreadStartup();
void NTAPI KiSuspendNop(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *);
void NTAPI KiSuspendThread(PVOID, PVOID, PVOID);

KPROCESS KiIdleProcess, KiSystemProcess;

static void KiInitializeProcess(PKPROCESS p, SCHAR basePriority, LONG quantum);

// Reference values (spec/01 §2.8): idle process base 0 / quantum 0x7F; system process base 8 / 0x3C.
void KiInitializeProcesses() {
    KiInitializeProcess(&KiIdleProcess, 0, 0x7F);
    KiInitializeProcess(&KiSystemProcess, 8, 0x3C);
}

static void KiInitializeProcess(PKPROCESS p, SCHAR basePriority, LONG quantum) {
    InitializeListHead(&p->ReadyListHead);
    InitializeListHead(&p->ThreadListHead);
    p->StackCount = 0;
    p->ThreadQuantum = quantum;
    p->BasePriority = basePriority;
    p->DisableBoost = FALSE;
    p->DisableQuantum = FALSE;
}

// Stack below StackBase: NPX area, TLS, then the switch frame KiSwapContext pops
// (ExceptionList, EFLAGS, edi, esi, ebx, ebp, return) followed by KiThreadStartup's arguments.
static void KiInitializeContextThread(PKTHREAD t, ULONG tlsDataSize, PKSYSTEM_ROUTINE systemRoutine,
                                      PVOID startRoutine, PVOID startContext) {
    KiInitializeNpxArea(t);
    ULONG tlsSize = (tlsDataSize + 3) & ~3u;
    ULONG *sp = (ULONG *)((UCHAR *)t->StackBase - NPX_FRAME_LENGTH - tlsSize);
    t->TlsData = tlsSize ? sp : nullptr;
    RtlZeroMemory(sp, tlsSize);

    *--sp = (ULONG)startContext;
    *--sp = (ULONG)startRoutine;
    *--sp = (ULONG)systemRoutine;
    *--sp = (ULONG)KiThreadStartup;
    *--sp = 0;     // ebp
    *--sp = 0;     // ebx
    *--sp = 0;     // esi
    *--sp = 0;     // edi
    *--sp = 0x202; // EFLAGS: IF set
    *--sp = 0xFFFFFFFF;
    t->KernelStack = sp;
}

void KeInitializeThread(PKTHREAD Thread, PVOID KernelStack, ULONG KernelStackSize, ULONG TlsDataSize,
                        PKSYSTEM_ROUTINE SystemRoutine, PVOID StartRoutine, PVOID StartContext, PKPROCESS Process) {
    Thread->Header.Type = ThreadObject;
    Thread->Header.Size = sizeof(KTHREAD) / sizeof(ULONG);
    Thread->Header.SignalState = 0;
    InitializeListHead(&Thread->Header.WaitListHead);
    InitializeListHead(&Thread->MutantListHead);
    InitializeListHead(&Thread->ApcState.ApcListHead[0]);
    InitializeListHead(&Thread->ApcState.ApcListHead[1]);
    Thread->ApcState.Process = Process;
    Thread->ApcState.ApcQueueable = TRUE;

    KeInitializeApc(&Thread->SuspendApc, Thread, KiSuspendNop, nullptr, KiSuspendThread, KernelMode, nullptr);
    KeInitializeSemaphore(&Thread->SuspendSemaphore, 0, 2);

    KeInitializeTimerEx(&Thread->Timer, NotificationTimer);
    Thread->TimerWaitBlock.Thread = Thread;
    Thread->TimerWaitBlock.Object = &Thread->Timer;
    Thread->TimerWaitBlock.WaitKey = (SHORT)STATUS_TIMEOUT;
    Thread->TimerWaitBlock.WaitType = WaitAny;

    Thread->StackBase = KernelStack;
    Thread->StackLimit = (UCHAR *)KernelStack - KernelStackSize;
    if (SystemRoutine)
        KiInitializeContextThread(Thread, TlsDataSize, SystemRoutine, StartRoutine, StartContext);

    Thread->Priority = Thread->BasePriority = Process->BasePriority;
    Thread->Quantum = Process->ThreadQuantum;
    Thread->DisableBoost = Process->DisableBoost;
    Thread->State = Initialized;

    KIRQL old = KeRaiseIrqlToDpcLevel();
    InsertTailList(&Process->ThreadListHead, &Thread->ThreadListEntry);
    Process->StackCount++;
    KfLowerIrql(old);
}

XBAPI PKTHREAD NTAPI KeGetCurrentThread() { return Prcb.CurrentThread; }

// Terminated threads: the next thread to run frees the stack and drops the thread object's
// "running" reference (spec/10 §6); the object lives on while handles or references remain.
static LIST_ENTRY KiDeadThreadListHead = {&KiDeadThreadListHead, &KiDeadThreadListHead};

void KiReapDeadThreads() {
    while (!IsListEmpty(&KiDeadThreadListHead)) {
        PKTHREAD t = CONTAINING_RECORD(RemoveHeadList(&KiDeadThreadListHead), KTHREAD, ThreadListEntry);
        MmDeleteKernelStack(t->StackBase, t->StackLimit);
        t->StackBase = t->StackLimit = t->KernelStack = nullptr;
        if (t != &KiIdleThread)
            ObfDereferenceObject(t);
    }
}

// Terminate the current thread: signal its object, wake joiners, switch away for good.
void KeTerminateThread() {
    KeRaiseIrqlToDpcLevel();
    PKTHREAD t = Prcb.CurrentThread;
#if XK_TRACE_IMPORTS
    XkTraceThreadExit(t); // it may have ended inside a traced call that will never return
#endif
    t->HasTerminated = TRUE;
    t->State = Terminated;
    t->Header.SignalState = 1;
    if (!IsListEmpty(&t->Header.WaitListHead))
        KiWaitTest(&t->Header, 0);
    RemoveEntryList(&t->ThreadListEntry);
    t->ApcState.Process->StackCount--;
    KiNpxRelease(t);
    InsertTailList(&KiDeadThreadListHead, &t->ThreadListEntry);
    KiSwapThread();
    KeBugCheck(0x12345678); // a terminated thread was resumed
    for (;;) {
    }
}


XBAPI LONG NTAPI KeSetPriorityThread(PKTHREAD Thread, LONG Priority) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Thread->Priority;
    Thread->Saturation = 0;
    Thread->PriorityDecrement = 0;
    Thread->Quantum = Thread->ApcState.Process->ThreadQuantum;
    KiSetPriorityThread(Thread, (SCHAR)Priority);
    KiUnlockDispatcherDatabase(old);
    return previous;
}

XBAPI LONG NTAPI KeQueryBasePriorityThread(PKTHREAD Thread) {
    return Thread->BasePriority - Thread->ApcState.Process->BasePriority;
}

// Base priority = process base + increment; +/-16 saturates to the top/bottom of the thread's class.
XBAPI LONG NTAPI KeSetBasePriorityThread(PKTHREAD Thread, LONG Increment) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Thread->BasePriority - Thread->ApcState.Process->BasePriority;
    if (Thread->Saturation)
        previous = (16 / 2) * Thread->Saturation;
    LONG processBase = Thread->ApcState.Process->BasePriority;
    LONG base = processBase + Increment;
    Thread->Saturation = 0;
    if (Increment >= 16 / 2 || Increment <= -(16 / 2)) {
        Thread->Saturation = (CHAR)(Increment > 0 ? 1 : -1);
        base = processBase >= 16 ? (Increment > 0 ? 31 : 16) : (Increment > 0 ? 15 : 1);
    } else if (processBase >= 16) {
        base = base < 16 ? 16 : base > 31 ? 31 : base;
    } else {
        base = base < 1 ? 1 : base > 15 ? 15 : base;
    }
    SCHAR oldBase = Thread->BasePriority;
    Thread->BasePriority = (SCHAR)base;
    Thread->Quantum = Thread->ApcState.Process->ThreadQuantum;
    Thread->PriorityDecrement = 0;
    LONG priority = base >= 16 ? base : Thread->Priority - oldBase + base;
    if (priority < base)
        priority = base;
    KiSetPriorityThread(Thread, (SCHAR)priority);
    KiUnlockDispatcherDatabase(old);
    return previous;
}

XBAPI BOOLEAN NTAPI KeSetDisableBoostThread(PKTHREAD Thread, BOOLEAN Disable) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN previous = Thread->DisableBoost;
    Thread->DisableBoost = Disable;
    KiUnlockDispatcherDatabase(old);
    return previous;
}

// Boost a variable-priority thread that has no outstanding decrement (e.g. after I/O completion).
XBAPI void NTAPI KeBoostPriorityThread(PKTHREAD Thread, KPRIORITY Increment) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (Thread->Priority < 16 && !Thread->PriorityDecrement && !Thread->DisableBoost) {
        LONG priority = Thread->BasePriority + Increment;
        if (priority > Thread->Priority) {
            if (priority > 15)
                priority = 15;
            Thread->PriorityDecrement = (SCHAR)(priority - Thread->BasePriority);
            KiSetPriorityThread(Thread, (SCHAR)priority);
        }
    }
    KiUnlockDispatcherDatabase(old);
}

// Change a process's base priority and shift its threads by the same amount within their class.
XBAPI KPRIORITY NTAPI KeSetPriorityProcess(PKPROCESS Process, KPRIORITY BasePriority) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KPRIORITY previous = Process->BasePriority;
    LONG delta = BasePriority - previous;
    Process->BasePriority = (SCHAR)BasePriority;
    for (PLIST_ENTRY e = Process->ThreadListHead.Flink; e != &Process->ThreadListHead; e = e->Flink) {
        PKTHREAD t = CONTAINING_RECORD(e, KTHREAD, ThreadListEntry);
        LONG low = BasePriority >= 16 ? 16 : 1, high = BasePriority >= 16 ? 31 : 15;
        LONG base = t->BasePriority + delta;
        base = base < low ? low : base > high ? high : base;
        t->BasePriority = (SCHAR)base;
        t->Quantum = Process->ThreadQuantum;
        t->PriorityDecrement = 0;
        KiSetPriorityThread(t, (SCHAR)base);
    }
    KiUnlockDispatcherDatabase(old);
    return previous;
}
