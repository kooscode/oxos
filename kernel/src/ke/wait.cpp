// Waits and synchronization objects (spec/07 "Waits"; NT semantics).
// Not yet implemented: alerts/APC-interrupted waits (STATUS_ALERTED / STATUS_USER_APC / kernel-APC
// re-waits), queue objects, and priority boosts on wake — they arrive with APCs.
#include <xk/arch.h>
#include <xk/ke.h>

#define Prcb (KiPcr.PrcbData)

extern LIST_ENTRY KiWaitInListHead;
constexpr ULONG THREAD_WAIT_OBJECTS = 3, MAXIMUM_WAIT_OBJECTS = 64;
constexpr ULONG STATUS_MUTANT_LIMIT_EXCEEDED = 0xC0000191, STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
                STATUS_MUTANT_NOT_OWNED = 0xC0000046;

static bool KiIsSignaledFor(DISPATCHER_HEADER *o, PKTHREAD t) {
    if (o->Type == MutantObject)
        return o->SignalState > 0 || ((PKMUTANT)o)->OwnerThread == t;
    return o->SignalState > 0;
}

// Consume the signal for thread t (sync events/timers reset, semaphores count down, mutants are acquired).
static void KiWaitSatisfy(DISPATCHER_HEADER *o, PKTHREAD t) {
    switch (o->Type) {
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        o->SignalState = 0;
        break;
    case SemaphoreObject:
        o->SignalState--;
        break;
    case MutantObject: {
        auto m = (PKMUTANT)o;
        if (--o->SignalState == 0) {
            m->OwnerThread = t;
            InsertTailList(&t->MutantListHead, &m->MutantListEntry);
            if (m->Abandoned) {
                m->Abandoned = FALSE;
                t->WaitStatus = STATUS_ABANDONED;
            }
        }
        break;
    }
    default:
        break;
    }
}

void KiUnwaitThread(PKTHREAD Thread, LONG WaitStatus, LONG Increment) {
    Thread->WaitStatus |= WaitStatus;
    PKWAIT_BLOCK wb = Thread->WaitBlockList;
    do {
        RemoveEntryList(&wb->WaitListEntry);
        wb = wb->NextWaitBlock;
    } while (wb != Thread->WaitBlockList);
    KiRemoveTimerIfInserted(&Thread->Timer);
    RemoveEntryList(&Thread->WaitListEntry); // global wait list
    KiReadyThread(Thread);
}

static bool KiWaitAllSatisfiable(PKWAIT_BLOCK first) {
    PKWAIT_BLOCK wb = first;
    do {
        if (wb->WaitKey != (SHORT)STATUS_TIMEOUT && !KiIsSignaledFor((DISPATCHER_HEADER *)wb->Object, wb->Thread))
            return false;
        wb = wb->NextWaitBlock;
    } while (wb != first);
    return true;
}

void KiWaitTest(DISPATCHER_HEADER *Object, LONG Increment) {
    PLIST_ENTRY e = Object->WaitListHead.Flink;
    while (Object->SignalState > 0 && e != &Object->WaitListHead) {
        auto wb = CONTAINING_RECORD(e, KWAIT_BLOCK, WaitListEntry);
        PKTHREAD t = wb->Thread;
        e = e->Flink;
        if (wb->WaitType == WaitAny) {
            KiWaitSatisfy(Object, t);
            KiUnwaitThread(t, wb->WaitKey, Increment);
        } else if (KiWaitAllSatisfiable(wb)) {
            PKWAIT_BLOCK w = wb;
            do {
                if (w->WaitKey != (SHORT)STATUS_TIMEOUT)
                    KiWaitSatisfy((DISPATCHER_HEADER *)w->Object, t);
                w = w->NextWaitBlock;
            } while (w != wb);
            KiUnwaitThread(t, STATUS_WAIT_0, Increment);
        }
    }
}

static KIRQL KiBeginWait(PKTHREAD t) {
    if (t->WaitNext) {
        t->WaitNext = FALSE; // caller already holds DISPATCH_LEVEL (KeSetEvent(..., Wait=TRUE))
    } else {
        t->WaitIrql = KeRaiseIrqlToDpcLevel();
    }
    return t->WaitIrql;
}

static NTSTATUS KiEndWait(PKTHREAD t, NTSTATUS status) {
    KiUnlockDispatcherDatabase(t->WaitIrql);
    return status;
}

// Arm the thread timer for a wait timeout and chain its wait block. FALSE = already expired.
static bool KiArmWaitTimer(PKTHREAD t, PLARGE_INTEGER Timeout, PKWAIT_BLOCK chainAfter) {
    t->Timer.Header.SignalState = 0;
    if (!KiInsertTimerRelative(&t->Timer, Timeout->QuadPart))
        return false;
    InsertTailList(&t->Timer.Header.WaitListHead, &t->TimerWaitBlock.WaitListEntry);
    t->TimerWaitBlock.NextWaitBlock = chainAfter->NextWaitBlock;
    chainAfter->NextWaitBlock = &t->TimerWaitBlock;
    return true;
}

static NTSTATUS KiBlock(PKTHREAD t, KWAIT_REASON reason, KPROCESSOR_MODE mode, BOOLEAN alertable) {
    t->Alertable = alertable;
    t->WaitMode = mode;
    t->WaitReason = (UCHAR)reason;
    t->WaitTime = KeTickCount;
    t->State = Waiting;
    InsertTailList(&KiWaitInListHead, &t->WaitListEntry);
    NTSTATUS status = KiSwapThread();
    KfLowerIrql(t->WaitIrql);
    return status;
}

XBAPI NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                                           BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    PKTHREAD t = Prcb.CurrentThread;
    auto o = (DISPATCHER_HEADER *)Object;
    KiBeginWait(t);
    t->WaitStatus = 0;

    if (KiIsSignaledFor(o, t)) {
        if (o->Type == MutantObject && o->SignalState == (LONG)0x80000000)
            KeBugCheckEx(0x1E, STATUS_MUTANT_LIMIT_EXCEEDED, (ULONG)Object, 0, 0);
        KiWaitSatisfy(o, t);
        return KiEndWait(t, t->WaitStatus);
    }
    if (Timeout && Timeout->QuadPart == 0)
        return KiEndWait(t, STATUS_TIMEOUT);

    KWAIT_BLOCK wb;
    wb.Thread = t;
    wb.Object = Object;
    wb.WaitKey = 0;
    wb.WaitType = WaitAny;
    wb.NextWaitBlock = &wb;
    t->WaitBlockList = &wb;
    if (Timeout && !KiArmWaitTimer(t, Timeout, &wb))
        return KiEndWait(t, STATUS_TIMEOUT);
    InsertTailList(&o->WaitListHead, &wb.WaitListEntry);
    return KiBlock(t, WaitReason, WaitMode, Alertable);
}

XBAPI NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType,
                                              KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                              PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray) {
    KWAIT_BLOCK local[THREAD_WAIT_OBJECTS];
    if (!WaitBlockArray) {
        if (Count > THREAD_WAIT_OBJECTS)
            KeBugCheck(0x0C); // MAXIMUM_WAIT_OBJECTS_EXCEEDED
        WaitBlockArray = local;
    } else if (Count > MAXIMUM_WAIT_OBJECTS) {
        KeBugCheck(0x0C);
    }

    PKTHREAD t = Prcb.CurrentThread;
    KiBeginWait(t);
    t->WaitStatus = 0;

    if (WaitType == WaitAny) {
        for (ULONG i = 0; i < Count; i++) {
            auto o = (DISPATCHER_HEADER *)Object[i];
            if (KiIsSignaledFor(o, t)) {
                KiWaitSatisfy(o, t);
                return KiEndWait(t, t->WaitStatus | (NTSTATUS)i);
            }
        }
    } else {
        bool all = true;
        for (ULONG i = 0; i < Count && all; i++)
            all = KiIsSignaledFor((DISPATCHER_HEADER *)Object[i], t);
        if (all) {
            for (ULONG i = 0; i < Count; i++)
                KiWaitSatisfy((DISPATCHER_HEADER *)Object[i], t);
            return KiEndWait(t, t->WaitStatus);
        }
    }
    if (Timeout && Timeout->QuadPart == 0)
        return KiEndWait(t, STATUS_TIMEOUT);

    for (ULONG i = 0; i < Count; i++) {
        PKWAIT_BLOCK wb = &WaitBlockArray[i];
        wb->Thread = t;
        wb->Object = Object[i];
        wb->WaitKey = (SHORT)i;
        wb->WaitType = (SHORT)WaitType;
        wb->NextWaitBlock = i + 1 < Count ? &WaitBlockArray[i + 1] : &WaitBlockArray[0];
    }
    t->WaitBlockList = WaitBlockArray;
    if (Timeout && !KiArmWaitTimer(t, Timeout, &WaitBlockArray[Count - 1]))
        return KiEndWait(t, STATUS_TIMEOUT);
    for (ULONG i = 0; i < Count; i++)
        InsertTailList(&((DISPATCHER_HEADER *)Object[i])->WaitListHead, &WaitBlockArray[i].WaitListEntry);
    return KiBlock(t, WaitReason, WaitMode, Alertable);
}

XBAPI NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval) {
    PKTHREAD t = Prcb.CurrentThread;
    KiBeginWait(t);
    t->WaitStatus = 0;

    t->Timer.Header.SignalState = 0;
    if (!KiInsertTimerRelative(&t->Timer, Interval->QuadPart)) {
        // Already elapsed: yield to a ready thread of equal or higher priority, if any (spec/07).
        if (t->Priority < 16 && t->Priority != t->BasePriority) {
            t->PriorityDecrement = 0;
            KiSetPriorityThread(t, t->BasePriority);
        }
        if (KiSelectYieldThread(t)) {
            t->Preempted = FALSE; // back of its ready queue
            t->Quantum = t->ApcState.Process->ThreadQuantum;
        }
        return KiEndWait(t, STATUS_SUCCESS);
    }
    InsertTailList(&t->Timer.Header.WaitListHead, &t->TimerWaitBlock.WaitListEntry);
    t->TimerWaitBlock.NextWaitBlock = &t->TimerWaitBlock;
    t->WaitBlockList = &t->TimerWaitBlock;
    NTSTATUS status = KiBlock(t, DelayExecution, WaitMode, Alertable);
    return status == STATUS_TIMEOUT ? STATUS_SUCCESS : status;
}

// --- Semaphores

XBAPI void NTAPI KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit) {
    Semaphore->Header.Type = SemaphoreObject;
    Semaphore->Header.Size = sizeof(KSEMAPHORE) / sizeof(ULONG);
    Semaphore->Header.SignalState = Count;
    InitializeListHead(&Semaphore->Header.WaitListHead);
    Semaphore->Limit = Limit;
}

XBAPI LONG NTAPI KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait) {
    PKTHREAD t = Prcb.CurrentThread;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Semaphore->Header.SignalState;
    LONG next = previous + Adjustment;
    if (next > Semaphore->Limit || next < previous)
        KeBugCheckEx(0x1E, STATUS_SEMAPHORE_LIMIT_EXCEEDED, (ULONG)Semaphore, 0, 0);
    Semaphore->Header.SignalState = next;
    if (previous == 0 && !IsListEmpty(&Semaphore->Header.WaitListHead))
        KiWaitTest(&Semaphore->Header, Increment);
    if (Wait) {
        t->WaitNext = TRUE;
        t->WaitIrql = old;
    } else {
        KiUnlockDispatcherDatabase(old);
    }
    return previous;
}

// --- Mutants

XBAPI void NTAPI KeInitializeMutant(PKMUTANT Mutant, BOOLEAN InitialOwner) {
    Mutant->Header.Type = MutantObject;
    Mutant->Header.Size = sizeof(KMUTANT) / sizeof(ULONG);
    InitializeListHead(&Mutant->Header.WaitListHead);
    Mutant->Abandoned = FALSE;
    if (InitialOwner) {
        PKTHREAD t = Prcb.CurrentThread;
        Mutant->Header.SignalState = 0;
        Mutant->OwnerThread = t;
        KIRQL old = KeRaiseIrqlToDpcLevel();
        InsertTailList(&t->MutantListHead, &Mutant->MutantListEntry);
        KfLowerIrql(old);
    } else {
        Mutant->Header.SignalState = 1;
        Mutant->OwnerThread = nullptr;
    }
}

XBAPI LONG NTAPI KeReleaseMutant(PKMUTANT Mutant, LONG Increment, BOOLEAN Abandoned, BOOLEAN Wait) {
    PKTHREAD t = Prcb.CurrentThread;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Mutant->Header.SignalState;
    if (Abandoned) {
        Mutant->Header.SignalState = 1;
        Mutant->Abandoned = TRUE;
    } else {
        if (Mutant->OwnerThread != t)
            KeBugCheckEx(0x1E, STATUS_MUTANT_NOT_OWNED, (ULONG)Mutant, 0, 0);
        Mutant->Header.SignalState++;
    }
    if (Mutant->Header.SignalState == 1) {
        if (previous <= 0)
            RemoveEntryList(&Mutant->MutantListEntry);
        Mutant->OwnerThread = nullptr;
        if (!IsListEmpty(&Mutant->Header.WaitListHead))
            KiWaitTest(&Mutant->Header, Increment);
    }
    if (Wait) {
        t->WaitNext = TRUE;
        t->WaitIrql = old;
    } else {
        KiUnlockDispatcherDatabase(old);
    }
    return previous;
}
