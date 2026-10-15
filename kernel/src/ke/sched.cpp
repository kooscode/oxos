// Scheduler (spec/07 "Scheduling"). Uniprocessor: the dispatcher database is protected by running
// at DISPATCH_LEVEL; KiUnlockDispatcherDatabase performs any pending switch when IRQL drops.
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>

#define Prcb (KiPcr.PrcbData)

void KiInitializeNpxArea(PKTHREAD t);
void KiNpxPrepareSwitch(PKTHREAD next);
void KiInitializeNpx();
extern "C" void FASTCALL KiSwapContext(PKTHREAD Old, PKTHREAD New);
extern "C" void KiCallOnStack(void (*fn)(), PVOID stackTop);

static LIST_ENTRY KiDispatcherReadyListHead[32];
static ULONG KiReadySummary;
LIST_ENTRY KiWaitInListHead;

static void KiInsertReady(PKTHREAD t, bool atHead) {
    PLIST_ENTRY head = &KiDispatcherReadyListHead[(UCHAR)t->Priority];
    if (atHead)
        InsertHeadList(head, &t->WaitListEntry);
    else
        InsertTailList(head, &t->WaitListEntry);
    KiReadySummary |= 1u << t->Priority;
}

static void KiRemoveReady(PKTHREAD t) {
    RemoveEntryList(&t->WaitListEntry);
    if (IsListEmpty(&KiDispatcherReadyListHead[(UCHAR)t->Priority]))
        KiReadySummary &= ~(1u << t->Priority);
}

// Highest-priority ready thread with priority >= MinPriority, removed from its list.
static PKTHREAD KiFindReadyThread(LONG MinPriority) {
    ULONG mask = KiReadySummary & ~((1u << MinPriority) - 1);
    if (!mask)
        return nullptr;
    ULONG priority = 31 - __builtin_clz(mask);
    PKTHREAD t = CONTAINING_RECORD(KiDispatcherReadyListHead[priority].Flink, KTHREAD, WaitListEntry);
    KiRemoveReady(t);
    return t;
}

void KiReadyThread(PKTHREAD Thread) {
    bool preempted = Thread->Preempted;
    Thread->Preempted = FALSE;
    Thread->WaitTime = KeTickCount;
    for (;;) {
        PKTHREAD next = Prcb.NextThread;
        if (!next) {
            if (Thread->Priority > Prcb.CurrentThread->Priority) {
                Prcb.CurrentThread->Preempted = TRUE;
                Prcb.NextThread = Thread;
                Thread->State = Standby;
                return;
            }
        } else if (Thread->Priority > next->Priority) {
            // Thread displaces the standby thread, which goes back to the head of its ready list.
            next->Preempted = TRUE;
            Prcb.NextThread = Thread;
            Thread->State = Standby;
            Thread = next;
            preempted = true;
            Thread->Preempted = FALSE;
            continue;
        }
        Thread->State = Ready;
        KiInsertReady(Thread, preempted);
        return;
    }
}

void KiSetPriorityThread(PKTHREAD Thread, SCHAR Priority) {
    SCHAR old = Thread->Priority;
    if (old == Priority)
        return;
    switch (Thread->State) {
    case Ready:
        KiRemoveReady(Thread);
        Thread->Priority = Priority;
        KiReadyThread(Thread);
        break;
    case Running:
        Thread->Priority = Priority;
        if (Priority < old && !Prcb.NextThread)
            if (PKTHREAD t = KiFindReadyThread(Priority + 1)) {
                t->State = Standby;
                Prcb.NextThread = t;
            }
        break;
    default:
        Thread->Priority = Priority;
        break;
    }
}

// Make a ready thread of equal or higher priority the next thread (yield). TRUE if one exists.
bool KiSelectYieldThread(PKTHREAD Current) {
    if (!Prcb.NextThread)
        if (PKTHREAD t = KiFindReadyThread(Current->Priority)) {
            t->State = Standby;
            Prcb.NextThread = t;
        }
    return Prcb.NextThread != nullptr;
}

// Switch from the current thread to Next. Caller is at DISPATCH_LEVEL, not in a DPC, and has
// already set the old thread's state (Ready/Waiting/Terminated).
static void KiSwapToThread(PKTHREAD Old, PKTHREAD Next) {
    if (Prcb.DpcRoutineActive)
        KeBugCheck(0xB8); // ATTEMPTED_SWITCH_FROM_DPC
    Prcb.CurrentThread = Next;
    Next->State = Running;
    Next->ContextSwitches++;
    Prcb.KeContextSwitches++;
    KiNpxPrepareSwitch(Next);
    // Like the reference KiSwapContext: an incoming thread with kernel APCs queued (including a new thread
    // created suspended) gets the APC interrupt as soon as it drops below APC_LEVEL.
    if (Next->ApcState.KernelApcPending)
        HalRequestSoftwareInterrupt(APC_LEVEL);
    KiSwapContext(Old, Next);
    // Back on Old's stack. Free stacks of threads that terminated (we're no longer running on them).
    KiReapDeadThreads();
    // A kernel APC queued while it was switched out is delivered when its IRQL drops.
    if (Old->ApcState.KernelApcPending)
        HalRequestSoftwareInterrupt(APC_LEVEL);
}

// Block the current thread (its wait state is already set). Returns its WaitStatus when resumed;
// the caller then lowers IRQL to WaitIrql.
LONG KiSwapThread() {
    PKTHREAD old = Prcb.CurrentThread;
    PKTHREAD next = Prcb.NextThread;
    if (next)
        Prcb.NextThread = nullptr;
    else if (!(next = KiFindReadyThread(0)))
        next = Prcb.IdleThread;
    KiSwapToThread(old, next);
    return old->WaitStatus;
}

XBAPI void FASTCALL KiUnlockDispatcherDatabase(KIRQL OldIrql) {
    if (Prcb.NextThread) {
        if (OldIrql < DISPATCH_LEVEL && !Prcb.DpcRoutineActive) {
            PKTHREAD old = Prcb.CurrentThread, next = Prcb.NextThread;
            Prcb.NextThread = nullptr;
            old->WaitIrql = OldIrql;
            if (old != Prcb.IdleThread)
                KiReadyThread(old);
            KiSwapToThread(old, next);
            KfLowerIrql(old->WaitIrql);
            return;
        }
        if (!Prcb.DpcRoutineActive)
            HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
    KfLowerIrql(OldIrql);
}

// Quantum expired (spec/07 KiQuantumEnd): refill, decay priority, pick a same-or-higher thread.
static void KiQuantumEnd() {
    PKTHREAD t = Prcb.CurrentThread;
    if (t == Prcb.IdleThread || t->Quantum > 0)
        return;
    PKPROCESS p = t->ApcState.Process;
    if (p->DisableQuantum && t->Priority >= 16) {
        t->Quantum = 0x7FFFFFFF;
        return;
    }
    t->Quantum = p->ThreadQuantum;
    SCHAR priority = t->Priority, newPriority = priority;
    if (priority < 16) {
        newPriority = (SCHAR)(priority - t->PriorityDecrement - 1);
        if (newPriority < t->BasePriority)
            newPriority = t->BasePriority;
        t->PriorityDecrement = 0;
    }
    if (newPriority != priority) {
        KiSetPriorityThread(t, newPriority);
    } else if (!Prcb.NextThread) {
        if (PKTHREAD r = KiFindReadyThread(priority)) {
            r->State = Standby;
            Prcb.NextThread = r;
        }
    } else {
        t->Preempted = FALSE;
    }
}

static void KiRetireDpcListOnDpcStack() {
    PVOID exceptionList = KiPcr.NtTib.ExceptionList;
    KiPcr.NtTib.ExceptionList = (PVOID)0xFFFFFFFF;
    KiCallOnStack(KiRetireDpcList, Prcb.DpcStack);
    KiPcr.NtTib.ExceptionList = exceptionList;
}

// DISPATCH_LEVEL software interrupt: drain DPCs on the DPC stack, then reschedule if needed.
void KiDispatchInterrupt() {
    if (!IsListEmpty(&Prcb.DpcListHead))
        KiRetireDpcListOnDpcStack();
    if (Prcb.QuantumEnd) {
        Prcb.QuantumEnd = 0;
        KiQuantumEnd();
    }
    PKTHREAD next = Prcb.NextThread;
    if (!next)
        return;
    PKTHREAD old = Prcb.CurrentThread;
    Prcb.NextThread = nullptr;
    if (old != Prcb.IdleThread)
        KiReadyThread(old);
    KiSwapToThread(old, next);
}

// Clock tick bookkeeping (spec/07 "Clock tick"), at CLOCK_LEVEL.
void KiUpdateRunTime(KIRQL interruptedIrql) {
    PKTHREAD t = Prcb.CurrentThread;
    if (interruptedIrql > DISPATCH_LEVEL)
        Prcb.InterruptTime++;
    else if (interruptedIrql == DISPATCH_LEVEL && Prcb.DpcRoutineActive)
        Prcb.DpcTime++;
    else
        t->KernelTime++;
    t->Quantum -= 3;
    if (t->Quantum <= 0 && t != Prcb.IdleThread) {
        Prcb.QuantumEnd = 1;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }
}

// The idle thread runs at DISPATCH_LEVEL: it drains DPCs and switches to ready threads itself.
void KiIdleLoop() {
    PKTHREAD idle = Prcb.IdleThread;
    for (;;) {
        cli();
        if (!IsListEmpty(&Prcb.DpcListHead)) {
            HalClearSoftwareInterrupt(DISPATCH_LEVEL);
            sti();
            KiRetireDpcListOnDpcStack();
            continue;
        }
        if (PKTHREAD next = Prcb.NextThread) {
            Prcb.NextThread = nullptr;
            sti();
            KiSwapToThread(idle, next);
            continue;
        }
        Prcb.QuantumEnd = 0;
        __asm__ volatile("sti\n\thlt" ::: "memory");
    }
}

// The boot thread becomes the idle thread (priority 0, idle process).
void KiInitializeScheduler(PVOID bootStackTop, PVOID bootStackLimit) {
    for (auto &head : KiDispatcherReadyListHead)
        InitializeListHead(&head);
    InitializeListHead(&KiWaitInListHead);

    extern void KiInitializeProcesses();
    KiInitializeProcesses();

    PKTHREAD idle = &KiIdleThread;
    KeInitializeThread(idle, bootStackTop, (UCHAR *)bootStackTop - (UCHAR *)bootStackLimit, 0, nullptr, nullptr,
                       nullptr, &KiIdleProcess);
    KiInitializeNpxArea(idle);
    idle->Priority = idle->BasePriority = 0;
    idle->State = Running;
    idle->WaitIrql = DISPATCH_LEVEL;
    Prcb.CurrentThread = Prcb.IdleThread = idle;
    Prcb.NpxThread = nullptr;
    KiInitializeNpx();

    Prcb.DpcStack = MmCreateKernelStack(0x3000, FALSE);
}
