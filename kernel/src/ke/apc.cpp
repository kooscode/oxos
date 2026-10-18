// Asynchronous procedure calls, alerts, suspend/resume (spec/stock/08).
//
// Kernel APCs (ApcListHead[KernelMode]) are delivered by the APC_LEVEL software interrupt. "User"
// APCs (ApcListHead[UserMode]) only run when an alertable user-mode wait returns STATUS_USER_APC:
// the Xbox has no user mode, so the wait routine itself runs them at PASSIVE_LEVEL before returning.
#include <xk/hal.h>
#include <xk/ke.h>

#define Prcb (KiPcr.PrcbData)

XBAPI void NTAPI KeInitializeApc(PKAPC Apc, PKTHREAD Thread, PKKERNEL_ROUTINE KernelRoutine,
                                 PKRUNDOWN_ROUTINE RundownRoutine, PKNORMAL_ROUTINE NormalRoutine,
                                 KPROCESSOR_MODE ApcMode, PVOID NormalContext) {
    Apc->Type = ApcObject;
    Apc->Thread = Thread;
    Apc->KernelRoutine = (PVOID)KernelRoutine;
    Apc->RundownRoutine = (PVOID)RundownRoutine;
    Apc->NormalRoutine = (PVOID)NormalRoutine;
    if (NormalRoutine) {
        Apc->ApcMode = ApcMode;
        Apc->NormalContext = NormalContext;
    } else { // special kernel APC
        Apc->ApcMode = KernelMode;
        Apc->NormalContext = nullptr;
    }
    Apc->Inserted = FALSE;
}

// At DISPATCH_LEVEL. Special APCs go after existing special APCs, normal APCs at the tail.
bool KiInsertQueueApc(PKAPC Apc, LONG Increment) {
    if (Apc->Inserted)
        return false;
    PKTHREAD t = Apc->Thread;
    PLIST_ENTRY head = &t->ApcState.ApcListHead[(UCHAR)Apc->ApcMode];
    if (!Apc->NormalRoutine) {
        PLIST_ENTRY e = head->Flink;
        while (e != head && !CONTAINING_RECORD(e, KAPC, ApcListEntry)->NormalRoutine)
            e = e->Flink;
        InsertTailList(e, &Apc->ApcListEntry); // insert before e
    } else {
        InsertTailList(head, &Apc->ApcListEntry);
    }
    Apc->Inserted = TRUE;

    if (Apc->ApcMode == KernelMode) {
        t->ApcState.KernelApcPending = TRUE;
        if (t->State == Running) {
            HalRequestSoftwareInterrupt(APC_LEVEL);
        } else if (t->State == Waiting && t->WaitIrql == PASSIVE_LEVEL &&
                   (!Apc->NormalRoutine || (!t->KernelApcDisable && !t->ApcState.KernelApcInProgress))) {
            KiUnwaitThread(t, STATUS_KERNEL_APC, Increment);
        }
    } else if (t->State == Waiting && t->WaitMode == UserMode && t->Alertable) {
        t->ApcState.UserApcPending = TRUE;
        KiUnwaitThread(t, STATUS_USER_APC, Increment);
    }
    return true;
}

XBAPI BOOLEAN NTAPI KeInsertQueueApc(PKAPC Apc, PVOID SystemArgument1, PVOID SystemArgument2, LONG Increment) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN inserted = FALSE;
    if (Apc->Thread->ApcState.ApcQueueable) {
        Apc->SystemArgument1 = SystemArgument1;
        Apc->SystemArgument2 = SystemArgument2;
        inserted = KiInsertQueueApc(Apc, Increment);
    }
    KiUnlockDispatcherDatabase(old);
    return inserted;
}

static PKAPC KiDequeueApc(PLIST_ENTRY head) {
    PKAPC apc = CONTAINING_RECORD(head->Flink, KAPC, ApcListEntry);
    RemoveEntryList(&apc->ApcListEntry);
    apc->Inserted = FALSE;
    return apc;
}

// APC_LEVEL software interrupt: deliver kernel-mode APCs of the current thread.
void KiDeliverApc() {
    PKTHREAD t = Prcb.CurrentThread;
    KIRQL old = KeRaiseIrqlToSynchLevel();
    t->ApcState.KernelApcPending = FALSE;
    PLIST_ENTRY head = &t->ApcState.ApcListHead[KernelMode];
    while (!IsListEmpty(head)) {
        PKAPC apc = CONTAINING_RECORD(head->Flink, KAPC, ApcListEntry);
        auto kernelRoutine = (PKKERNEL_ROUTINE)apc->KernelRoutine;
        auto normalRoutine = (PKNORMAL_ROUTINE)apc->NormalRoutine;
        PVOID ctx = apc->NormalContext, a1 = apc->SystemArgument1, a2 = apc->SystemArgument2;
        if (!normalRoutine) { // special APC: kernel routine only, at APC_LEVEL
            KiDequeueApc(head);
            KfLowerIrql(old);
            kernelRoutine(apc, &normalRoutine, &ctx, &a1, &a2);
        } else {
            if (t->ApcState.KernelApcInProgress || t->KernelApcDisable)
                break;
            KiDequeueApc(head);
            KfLowerIrql(old);
            kernelRoutine(apc, &normalRoutine, &ctx, &a1, &a2);
            if (normalRoutine) {
                t->ApcState.KernelApcInProgress = TRUE;
                KfLowerIrql(PASSIVE_LEVEL);
                normalRoutine(ctx, a1, a2);
                KfRaiseIrql(APC_LEVEL);
            }
        }
        KeRaiseIrqlToSynchLevel();
        t->ApcState.KernelApcInProgress = FALSE;
    }
    KfLowerIrql(old);
}

// Run queued user-mode APCs after an alertable wait returned STATUS_USER_APC (PASSIVE_LEVEL).
void KiExecuteUserApcs() {
    PKTHREAD t = Prcb.CurrentThread;
    KIRQL old = KeRaiseIrqlToSynchLevel();
    t->ApcState.UserApcPending = FALSE;
    PLIST_ENTRY head = &t->ApcState.ApcListHead[UserMode];
    while (!IsListEmpty(head)) {
        PKAPC apc = KiDequeueApc(head);
        auto kernelRoutine = (PKKERNEL_ROUTINE)apc->KernelRoutine;
        auto normalRoutine = (PKNORMAL_ROUTINE)apc->NormalRoutine;
        PVOID ctx = apc->NormalContext, a1 = apc->SystemArgument1, a2 = apc->SystemArgument2;
        KfLowerIrql(old);
        kernelRoutine(apc, &normalRoutine, &ctx, &a1, &a2);
        if (normalRoutine)
            normalRoutine(ctx, a1, a2);
        KeRaiseIrqlToSynchLevel();
    }
    KfLowerIrql(old);
}

XBAPI void NTAPI KeEnterCriticalRegion() { Prcb.CurrentThread->KernelApcDisable--; }

XBAPI void NTAPI KeLeaveCriticalRegion() {
    PKTHREAD t = Prcb.CurrentThread;
    if (++t->KernelApcDisable == 0 && !IsListEmpty(&t->ApcState.ApcListHead[KernelMode])) {
        t->ApcState.KernelApcPending = TRUE;
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }
}

// --- Alerts

XBAPI BOOLEAN NTAPI KeTestAlertThread(KPROCESSOR_MODE AlertMode) {
    PKTHREAD t = Prcb.CurrentThread;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN alerted = t->Alerted[(UCHAR)AlertMode];
    if (alerted)
        t->Alerted[(UCHAR)AlertMode] = FALSE;
    else if (AlertMode == UserMode && !IsListEmpty(&t->ApcState.ApcListHead[UserMode]))
        t->ApcState.UserApcPending = TRUE;
    KiUnlockDispatcherDatabase(old);
    return alerted;
}

static void KiAlert(PKTHREAD Thread, KPROCESSOR_MODE AlertMode) {
    if (Thread->Alerted[(UCHAR)AlertMode])
        return;
    if (Thread->State == Waiting && Thread->Alertable && AlertMode <= Thread->WaitMode)
        KiUnwaitThread(Thread, STATUS_ALERTED, 2);
    else
        Thread->Alerted[(UCHAR)AlertMode] = TRUE;
}

XBAPI BOOLEAN NTAPI KeAlertThread(PKTHREAD Thread, KPROCESSOR_MODE AlertMode) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN previous = Thread->Alerted[(UCHAR)AlertMode];
    KiAlert(Thread, AlertMode);
    KiUnlockDispatcherDatabase(old);
    return previous;
}

// --- Suspend / resume: the suspend APC's normal routine waits on the thread's suspend semaphore.

constexpr CHAR MAXIMUM_SUSPEND_COUNT = 0x7F;

void NTAPI KiSuspendNop(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {}

void NTAPI KiSuspendThread(PVOID, PVOID, PVOID) {
    PKTHREAD t = Prcb.CurrentThread;
    KeWaitForSingleObject(&t->SuspendSemaphore, Suspended, KernelMode, FALSE, nullptr);
}

static void KiResume(PKTHREAD Thread) {
    if (Thread->SuspendCount && --Thread->SuspendCount == 0) {
        Thread->SuspendSemaphore.Header.SignalState++;
        KiWaitTest(&Thread->SuspendSemaphore.Header, 0);
    }
}

XBAPI ULONG NTAPI KeSuspendThread(PKTHREAD Thread) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    CHAR previous = Thread->SuspendCount;
    if (previous == MAXIMUM_SUSPEND_COUNT)
        KeBugCheckEx(0x1E, 0xC000004A /*STATUS_SUSPEND_COUNT_EXCEEDED*/, (ULONG)Thread, 0, 0);
    if (Thread->ApcState.ApcQueueable) {
        Thread->SuspendCount++;
        if (previous == 0 && !KiInsertQueueApc(&Thread->SuspendApc, 0))
            Thread->SuspendSemaphore.Header.SignalState--;
    }
    KiUnlockDispatcherDatabase(old);
    return (ULONG)previous;
}

XBAPI ULONG NTAPI KeResumeThread(PKTHREAD Thread) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    CHAR previous = Thread->SuspendCount;
    KiResume(Thread);
    KiUnlockDispatcherDatabase(old);
    return (ULONG)previous;
}

XBAPI ULONG NTAPI KeAlertResumeThread(PKTHREAD Thread) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    KiAlert(Thread, KernelMode);
    CHAR previous = Thread->SuspendCount;
    KiResume(Thread);
    KiUnlockDispatcherDatabase(old);
    return (ULONG)previous;
}
