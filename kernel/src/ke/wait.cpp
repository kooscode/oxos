// Waits and synchronization objects (spec/07 "Waits", spec/08; NT semantics with the Xbox's
// in-kernel delivery of user APCs).
#include <xk/arch.h>
#include <xk/hal.h>
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
        if (o->SignalState == (LONG)0x80000000)
            KeBugCheckEx(0x1E, STATUS_MUTANT_LIMIT_EXCEEDED, (ULONG)o, 0, 0);
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

// Wake a waiting thread (reference KiUnwaitThread): detach wait blocks and timer, credit its queue,
// apply the wake-up boost for variable-priority threads, then ready it.
void KiUnwaitThread(PKTHREAD Thread, LONG WaitStatus, LONG Increment) {
    Thread->WaitStatus |= WaitStatus;
    PKWAIT_BLOCK wb = Thread->WaitBlockList;
    do {
        RemoveEntryList(&wb->WaitListEntry);
        wb = wb->NextWaitBlock;
    } while (wb != Thread->WaitBlockList);
    RemoveEntryList(&Thread->WaitListEntry); // global wait list
    KiRemoveTimerIfInserted(&Thread->Timer);
    if (Thread->Queue)
        Thread->Queue->CurrentCount++;

    PKPROCESS process = Thread->ApcState.Process;
    if (Thread->Priority < 16) {
        if (!Thread->PriorityDecrement && !Thread->DisableBoost) {
            LONG boosted = Thread->BasePriority + Increment;
            if (boosted > Thread->Priority)
                Thread->Priority = (SCHAR)(boosted < 16 ? boosted : 15);
        }
        if (Thread->BasePriority < 14) {
            Thread->Quantum -= 10;
            if (Thread->Quantum <= 0) {
                Thread->Quantum = process->ThreadQuantum;
                Thread->Priority = (SCHAR)(Thread->Priority - 1 - Thread->PriorityDecrement);
                if (Thread->Priority < Thread->BasePriority)
                    Thread->Priority = Thread->BasePriority;
                Thread->PriorityDecrement = 0;
            }
        } else {
            Thread->Quantum = process->ThreadQuantum;
        }
    } else {
        Thread->Quantum = process->ThreadQuantum;
    }
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

// --- Common wait plumbing

// Timeouts are tracked as an interrupt-time deadline so a wait interrupted by a kernel APC resumes
// with the remaining time (absolute system times are passed through unchanged).
struct KiTimeout {
    bool present, absolute;
    LONGLONG value; // relative: deadline in interrupt time; absolute: system time
    LARGE_INTEGER current;

    explicit KiTimeout(PLARGE_INTEGER t) : present(t != nullptr), absolute(false), value(0) {
        if (!t)
            return;
        absolute = t->QuadPart > 0;
        value = absolute ? t->QuadPart : (LONGLONG)KeQueryInterruptTime() - t->QuadPart;
    }
    // Remaining timeout in KeSetTimer form; quadpart 0 means "already expired / poll".
    PLARGE_INTEGER Remaining() {
        if (absolute) {
            current.QuadPart = value;
        } else {
            LONGLONG left = value - (LONGLONG)KeQueryInterruptTime();
            current.QuadPart = left > 0 ? -left : 0;
        }
        return &current;
    }
};

// Raise to DISPATCH (unless the caller holds it via Wait=TRUE) and let pending kernel APCs run first.
static void KiBeginWait(PKTHREAD t) {
    for (;;) {
        if (t->WaitNext)
            t->WaitNext = FALSE;
        else
            t->WaitIrql = KeRaiseIrqlToDpcLevel();
        if (!t->ApcState.KernelApcPending || t->WaitIrql != PASSIVE_LEVEL)
            break;
        HalRequestSoftwareInterrupt(APC_LEVEL);  // pending until IRQL drops
        KiUnlockDispatcherDatabase(t->WaitIrql); // delivers the APC
    }
    t->WaitStatus = 0;
}

// Alertable/user-APC checks done before blocking (spec/07). 0 = go ahead and wait.
static NTSTATUS KiCheckAlertable(PKTHREAD t, KPROCESSOR_MODE mode, BOOLEAN alertable) {
    if (alertable) {
        if (t->Alerted[(UCHAR)mode]) {
            t->Alerted[(UCHAR)mode] = FALSE;
            return STATUS_ALERTED;
        }
        if (mode != KernelMode && !IsListEmpty(&t->ApcState.ApcListHead[UserMode])) {
            t->ApcState.UserApcPending = TRUE;
            return STATUS_USER_APC;
        }
        if (t->Alerted[KernelMode]) {
            t->Alerted[KernelMode] = FALSE;
            return STATUS_ALERTED;
        }
    } else if (mode != KernelMode && t->ApcState.UserApcPending) {
        return STATUS_USER_APC;
    }
    return 0;
}

// Arm the thread timer for a wait timeout and chain its wait block after `chainAfter`.
static bool KiArmWaitTimer(PKTHREAD t, PLARGE_INTEGER timeout, PKWAIT_BLOCK chainAfter) {
    t->Timer.Header.SignalState = 0;
    if (!KiInsertTimerRelative(&t->Timer, timeout->QuadPart))
        return false;
    InsertTailList(&t->Timer.Header.WaitListHead, &t->TimerWaitBlock.WaitListEntry);
    t->TimerWaitBlock.NextWaitBlock = chainAfter->NextWaitBlock;
    chainAfter->NextWaitBlock = &t->TimerWaitBlock;
    return true;
}

// Block; returns WaitStatus with IRQL still at DISPATCH_LEVEL.
static NTSTATUS KiBlock(PKTHREAD t, KWAIT_REASON reason, KPROCESSOR_MODE mode, BOOLEAN alertable) {
    if (t->Queue && reason != WrQueue)
        KiActivateWaiterQueue(t->Queue);
    t->Alertable = alertable;
    t->WaitMode = mode;
    t->WaitReason = (UCHAR)reason;
    t->WaitTime = KeTickCount;
    t->State = Waiting;
    InsertTailList(&KiWaitInListHead, &t->WaitListEntry);
    return KiSwapThread();
}

// Leave the wait: unlock at WaitIrql, run user APCs if that is why we woke.
static NTSTATUS KiFinishWait(PKTHREAD t, NTSTATUS status, bool unlocked = false) {
    if (!unlocked)
        KiUnlockDispatcherDatabase(t->WaitIrql);
    if (status == STATUS_USER_APC)
        KiExecuteUserApcs();
    return status;
}

XBAPI NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode,
                                           BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    PKTHREAD t = Prcb.CurrentThread;
    auto o = (DISPATCHER_HEADER *)Object;
    KiTimeout timeout(Timeout);
    for (;;) {
        KiBeginWait(t);
        if (KiIsSignaledFor(o, t)) {
            KiWaitSatisfy(o, t);
            return KiFinishWait(t, t->WaitStatus);
        }
        if (NTSTATUS s = KiCheckAlertable(t, WaitMode, Alertable))
            return KiFinishWait(t, s);
        PLARGE_INTEGER remaining = timeout.present ? timeout.Remaining() : nullptr;
        if (remaining && remaining->QuadPart == 0)
            return KiFinishWait(t, STATUS_TIMEOUT);

        KWAIT_BLOCK wb;
        wb.Thread = t;
        wb.Object = Object;
        wb.WaitKey = 0;
        wb.WaitType = WaitAny;
        wb.NextWaitBlock = &wb;
        t->WaitBlockList = &wb;
        if (remaining && !KiArmWaitTimer(t, remaining, &wb))
            return KiFinishWait(t, STATUS_TIMEOUT);
        InsertTailList(&o->WaitListHead, &wb.WaitListEntry);
        NTSTATUS status = KiBlock(t, WaitReason, WaitMode, Alertable);
        KfLowerIrql(t->WaitIrql);
        if (status != STATUS_KERNEL_APC)
            return KiFinishWait(t, status, true);
    }
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
    KiTimeout timeout(Timeout);
    for (;;) {
        KiBeginWait(t);
        if (WaitType == WaitAny) {
            for (ULONG i = 0; i < Count; i++) {
                auto o = (DISPATCHER_HEADER *)Object[i];
                if (KiIsSignaledFor(o, t)) {
                    KiWaitSatisfy(o, t);
                    return KiFinishWait(t, t->WaitStatus | (NTSTATUS)i);
                }
            }
        } else {
            bool all = true;
            for (ULONG i = 0; i < Count && all; i++)
                all = KiIsSignaledFor((DISPATCHER_HEADER *)Object[i], t);
            if (all) {
                for (ULONG i = 0; i < Count; i++)
                    KiWaitSatisfy((DISPATCHER_HEADER *)Object[i], t);
                return KiFinishWait(t, t->WaitStatus);
            }
        }
        if (NTSTATUS s = KiCheckAlertable(t, WaitMode, Alertable))
            return KiFinishWait(t, s);
        PLARGE_INTEGER remaining = timeout.present ? timeout.Remaining() : nullptr;
        if (remaining && remaining->QuadPart == 0)
            return KiFinishWait(t, STATUS_TIMEOUT);

        for (ULONG i = 0; i < Count; i++) {
            PKWAIT_BLOCK wb = &WaitBlockArray[i];
            wb->Thread = t;
            wb->Object = Object[i];
            wb->WaitKey = (SHORT)i;
            wb->WaitType = (SHORT)WaitType;
            wb->NextWaitBlock = i + 1 < Count ? &WaitBlockArray[i + 1] : &WaitBlockArray[0];
        }
        t->WaitBlockList = WaitBlockArray;
        if (remaining && !KiArmWaitTimer(t, remaining, &WaitBlockArray[Count - 1]))
            return KiFinishWait(t, STATUS_TIMEOUT);
        for (ULONG i = 0; i < Count; i++)
            InsertTailList(&((DISPATCHER_HEADER *)Object[i])->WaitListHead, &WaitBlockArray[i].WaitListEntry);
        NTSTATUS status = KiBlock(t, WaitReason, WaitMode, Alertable);
        KfLowerIrql(t->WaitIrql);
        if (status != STATUS_KERNEL_APC)
            return KiFinishWait(t, status, true);
    }
}

XBAPI NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval) {
    PKTHREAD t = Prcb.CurrentThread;
    KiTimeout timeout(Interval);
    for (;;) {
        KiBeginWait(t);
        if (NTSTATUS s = KiCheckAlertable(t, WaitMode, Alertable))
            return KiFinishWait(t, s);

        t->Timer.Header.SignalState = 0;
        if (!KiInsertTimerRelative(&t->Timer, timeout.Remaining()->QuadPart)) {
            // Already elapsed: yield to a ready thread of equal or higher priority (spec/07).
            if (t->Priority < 16 && t->Priority != t->BasePriority) {
                t->PriorityDecrement = 0;
                KiSetPriorityThread(t, t->BasePriority);
            }
            if (KiSelectYieldThread(t)) {
                t->Preempted = FALSE;
                t->Quantum = t->ApcState.Process->ThreadQuantum;
            }
            return KiFinishWait(t, STATUS_SUCCESS);
        }
        InsertTailList(&t->Timer.Header.WaitListHead, &t->TimerWaitBlock.WaitListEntry);
        t->TimerWaitBlock.NextWaitBlock = &t->TimerWaitBlock;
        t->WaitBlockList = &t->TimerWaitBlock;
        NTSTATUS status = KiBlock(t, DelayExecution, WaitMode, Alertable);
        KfLowerIrql(t->WaitIrql);
        if (status != STATUS_KERNEL_APC)
            return KiFinishWait(t, status == STATUS_TIMEOUT ? STATUS_SUCCESS : status, true);
    }
}

// --- Queue removal (a wait with queue concurrency control)

XBAPI PLIST_ENTRY NTAPI KeRemoveQueue(PKQUEUE Queue, KPROCESSOR_MODE WaitMode, PLARGE_INTEGER Timeout) {
    PKTHREAD t = Prcb.CurrentThread;
    KiTimeout timeout(Timeout);
    bool first = true;
    for (;;) {
        KiBeginWait(t);
        if (first) {
            first = false;
            // Associate the thread with this queue (it occupies a concurrency slot while running).
            if (t->Queue != Queue) {
                if (PKQUEUE oldQueue = t->Queue) {
                    RemoveEntryList(&t->QueueListEntry);
                    KiActivateWaiterQueue(oldQueue);
                }
                t->Queue = Queue;
                InsertTailList(&Queue->ThreadListHead, &t->QueueListEntry);
            } else {
                Queue->CurrentCount--;
            }
        }
        if (!IsListEmpty(&Queue->EntryListHead) && Queue->CurrentCount < Queue->MaximumCount) {
            Queue->Header.SignalState--;
            Queue->CurrentCount++;
            PLIST_ENTRY entry = RemoveHeadList(&Queue->EntryListHead);
            KiUnlockDispatcherDatabase(t->WaitIrql);
            return entry;
        }
        if (NTSTATUS s = KiCheckAlertable(t, WaitMode, FALSE)) {
            Queue->CurrentCount++;
            return (PLIST_ENTRY)KiFinishWait(t, s);
        }
        PLARGE_INTEGER remaining = timeout.present ? timeout.Remaining() : nullptr;
        if (remaining && remaining->QuadPart == 0) {
            Queue->CurrentCount++;
            return (PLIST_ENTRY)KiFinishWait(t, STATUS_TIMEOUT);
        }
        KWAIT_BLOCK wb;
        wb.Thread = t;
        wb.Object = Queue;
        wb.WaitKey = 0;
        wb.WaitType = WaitAny;
        wb.NextWaitBlock = &wb;
        t->WaitBlockList = &wb;
        if (remaining && !KiArmWaitTimer(t, remaining, &wb)) {
            Queue->CurrentCount++;
            return (PLIST_ENTRY)KiFinishWait(t, STATUS_TIMEOUT);
        }
        InsertTailList(&Queue->Header.WaitListHead, &wb.WaitListEntry);
        // KiUnwaitThread credits CurrentCount when the thread is released.
        NTSTATUS status = KiBlock(t, WrQueue, WaitMode, FALSE);
        KfLowerIrql(t->WaitIrql);
        if (status != STATUS_KERNEL_APC)
            return (PLIST_ENTRY)KiFinishWait(t, status, true);
        Queue->CurrentCount--; // retry after the APC: give the slot back first
    }
}

// --- Semaphores

XBAPI void NTAPI KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit) {
    Semaphore->Header.Type = SemaphoreObject;
    Semaphore->Header.Size = sizeof(KSEMAPHORE) / sizeof(ULONG);
    Semaphore->Header.SignalState = Count;
    InitializeListHead(&Semaphore->Header.WaitListHead);
    Semaphore->Limit = Limit;
}

static void KiFinishSignal(KIRQL old, BOOLEAN Wait) {
    if (Wait) {
        PKTHREAD t = Prcb.CurrentThread;
        t->WaitNext = TRUE;
        t->WaitIrql = old;
    } else {
        KiUnlockDispatcherDatabase(old);
    }
}

XBAPI LONG NTAPI KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Semaphore->Header.SignalState;
    LONG next = previous + Adjustment;
    if (next > Semaphore->Limit || next < previous)
        KeBugCheckEx(0x1E, STATUS_SEMAPHORE_LIMIT_EXCEEDED, (ULONG)Semaphore, 0, 0);
    Semaphore->Header.SignalState = next;
    if (previous == 0 && !IsListEmpty(&Semaphore->Header.WaitListHead))
        KiWaitTest(&Semaphore->Header, Increment);
    KiFinishSignal(old, Wait);
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
    KiFinishSignal(old, Wait);
    return previous;
}
