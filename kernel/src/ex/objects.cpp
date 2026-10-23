// Executive objects behind handles: events, mutants, semaphores, timers, and the Nt wait calls (spec/stock/10 §5).
#include <xk/ob.h>
#include <xk/rtl.h>

constexpr NTSTATUS STATUS_INVALID_PARAMETER = (NTSTATUS)0xC000000D, STATUS_MUTANT_NOT_OWNED = (NTSTATUS)0xC0000046,
                   STATUS_SEMAPHORE_LIMIT_EXCEEDED = (NTSTATUS)0xC0000047;
constexpr LONG EVENT_INCREMENT = 1, MUTANT_INCREMENT = 1, SEMAPHORE_INCREMENT = 1;

struct ETIMER {
    KTIMER KeTimer;
    KAPC TimerApc;
    KDPC TimerDpc;
    BOOLEAN ApcAssociated;
};

static void NTAPI ExpDeleteMutant(PVOID Object) {
    auto m = (PKMUTANT)Object;
    if (m->OwnerThread)
        KeReleaseMutant(m, MUTANT_INCREMENT, TRUE, FALSE);
}

static void NTAPI ExpDeleteTimer(PVOID Object) {
    auto t = (ETIMER *)Object;
    KeCancelTimer(&t->KeTimer);
    KeRemoveQueueDpc(&t->TimerDpc);
}

OBJECT_TYPE ExEventObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr, nullptr, (PVOID)0, 'vevE'};
OBJECT_TYPE ExMutantObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, ExpDeleteMutant, nullptr, (PVOID)0, 'atuM'};
OBJECT_TYPE ExSemaphoreObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr, nullptr, (PVOID)0, 'ameS'};
OBJECT_TYPE ExTimerObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, ExpDeleteTimer, nullptr, (PVOID)0, 'emiT'};

template <typename T> static NTSTATUS ExpCreate(POBJECT_TYPE type, POBJECT_ATTRIBUTES attrs, PHANDLE handle, T init) {
    PVOID o;
    NTSTATUS status = ObCreateObject(type, attrs, sizeof(*init(nullptr)), &o);
    if (!NT_SUCCESS(status)) {
        *handle = nullptr;
        return status;
    }
    init(o);
    return ObInsertObject(o, attrs, 0, handle);
}

// --- Events

XBAPI NTSTATUS NTAPI NtCreateEvent(PHANDLE EventHandle, POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType,
                                   BOOLEAN InitialState) {
    if (EventType != NotificationEvent && EventType != SynchronizationEvent)
        return STATUS_INVALID_PARAMETER;
    return ExpCreate(&ExEventObjectType, ObjectAttributes, EventHandle, [&](PVOID o) {
        if (o)
            KeInitializeEvent((PKEVENT)o, EventType, InitialState);
        return (PKEVENT)o;
    });
}

template <typename F> static NTSTATUS ExpWithObject(HANDLE h, POBJECT_TYPE type, F f) {
    PVOID o;
    NTSTATUS status = ObReferenceObjectByHandle(h, type, &o);
    if (!NT_SUCCESS(status))
        return status;
    status = f(o);
    ObfDereferenceObject(o);
    return status;
}

XBAPI NTSTATUS NTAPI NtSetEvent(HANDLE EventHandle, PLONG PreviousState) {
    return ExpWithObject(EventHandle, &ExEventObjectType, [&](PVOID o) {
        LONG prev = KeSetEvent((PKEVENT)o, EVENT_INCREMENT, FALSE);
        if (PreviousState)
            *PreviousState = prev;
        return STATUS_SUCCESS;
    });
}

XBAPI NTSTATUS NTAPI NtClearEvent(HANDLE EventHandle) {
    return ExpWithObject(EventHandle, &ExEventObjectType, [](PVOID o) {
        ((PKEVENT)o)->Header.SignalState = 0;
        return STATUS_SUCCESS;
    });
}

XBAPI NTSTATUS NTAPI NtPulseEvent(HANDLE EventHandle, PLONG PreviousState) {
    return ExpWithObject(EventHandle, &ExEventObjectType, [&](PVOID o) {
        LONG prev = KePulseEvent((PKEVENT)o, EVENT_INCREMENT, FALSE);
        if (PreviousState)
            *PreviousState = prev;
        return STATUS_SUCCESS;
    });
}

struct EVENT_BASIC_INFORMATION {
    EVENT_TYPE EventType;
    LONG EventState;
};

XBAPI NTSTATUS NTAPI NtQueryEvent(HANDLE EventHandle, EVENT_BASIC_INFORMATION *EventInformation) {
    return ExpWithObject(EventHandle, &ExEventObjectType, [&](PVOID o) {
        auto e = (PKEVENT)o;
        EventInformation->EventType = (EVENT_TYPE)e->Header.Type;
        EventInformation->EventState = e->Header.SignalState;
        return STATUS_SUCCESS;
    });
}

// --- Mutants

XBAPI NTSTATUS NTAPI NtCreateMutant(PHANDLE MutantHandle, POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner) {
    return ExpCreate(&ExMutantObjectType, ObjectAttributes, MutantHandle, [&](PVOID o) {
        if (o)
            KeInitializeMutant((PKMUTANT)o, InitialOwner);
        return (PKMUTANT)o;
    });
}

XBAPI NTSTATUS NTAPI NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount) {
    return ExpWithObject(MutantHandle, &ExMutantObjectType, [&](PVOID o) {
        auto m = (PKMUTANT)o;
        if (m->OwnerThread != KeGetCurrentThread())
            return STATUS_MUTANT_NOT_OWNED;
        LONG prev = KeReleaseMutant(m, MUTANT_INCREMENT, FALSE, FALSE);
        if (PreviousCount)
            *PreviousCount = prev;
        return STATUS_SUCCESS;
    });
}

struct MUTANT_BASIC_INFORMATION {
    LONG CurrentCount;
    BOOLEAN OwnedByCaller;
    BOOLEAN AbandonedState;
};

XBAPI NTSTATUS NTAPI NtQueryMutant(HANDLE MutantHandle, MUTANT_BASIC_INFORMATION *MutantInformation) {
    return ExpWithObject(MutantHandle, &ExMutantObjectType, [&](PVOID o) {
        auto m = (PKMUTANT)o;
        MutantInformation->CurrentCount = m->Header.SignalState;
        MutantInformation->OwnedByCaller = m->OwnerThread == KeGetCurrentThread();
        MutantInformation->AbandonedState = m->Abandoned;
        return STATUS_SUCCESS;
    });
}

// --- Semaphores

XBAPI NTSTATUS NTAPI NtCreateSemaphore(PHANDLE SemaphoreHandle, POBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount,
                                       LONG MaximumCount) {
    if (MaximumCount <= 0 || InitialCount < 0 || InitialCount > MaximumCount)
        return STATUS_INVALID_PARAMETER;
    return ExpCreate(&ExSemaphoreObjectType, ObjectAttributes, SemaphoreHandle, [&](PVOID o) {
        if (o)
            KeInitializeSemaphore((PKSEMAPHORE)o, InitialCount, MaximumCount);
        return (PKSEMAPHORE)o;
    });
}

XBAPI NTSTATUS NTAPI NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount) {
    if (ReleaseCount <= 0)
        return STATUS_INVALID_PARAMETER;
    return ExpWithObject(SemaphoreHandle, &ExSemaphoreObjectType, [&](PVOID o) {
        auto s = (PKSEMAPHORE)o;
        LONG cur = s->Header.SignalState;
        if (cur + ReleaseCount > s->Limit || cur + ReleaseCount < cur)
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        LONG prev = KeReleaseSemaphore(s, SEMAPHORE_INCREMENT, ReleaseCount, FALSE);
        if (PreviousCount)
            *PreviousCount = prev;
        return STATUS_SUCCESS;
    });
}

struct SEMAPHORE_BASIC_INFORMATION {
    LONG CurrentCount;
    LONG MaximumCount;
};

XBAPI NTSTATUS NTAPI NtQuerySemaphore(HANDLE SemaphoreHandle, SEMAPHORE_BASIC_INFORMATION *SemaphoreInformation) {
    return ExpWithObject(SemaphoreHandle, &ExSemaphoreObjectType, [&](PVOID o) {
        auto s = (PKSEMAPHORE)o;
        SemaphoreInformation->CurrentCount = s->Header.SignalState;
        SemaphoreInformation->MaximumCount = s->Limit;
        return STATUS_SUCCESS;
    });
}

// --- Timers (optional APC to the thread that set the timer)

static void NTAPI ExpTimerApcKernelRoutine(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {}

static void NTAPI ExpTimerDpcRoutine(PKDPC, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2) {
    auto t = (ETIMER *)Context;
    if (t->ApcAssociated)
        KeInsertQueueApc(&t->TimerApc, SystemArgument1, SystemArgument2, 0);
}

XBAPI NTSTATUS NTAPI NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES ObjectAttributes, TIMER_TYPE TimerType) {
    if (TimerType != NotificationTimer && TimerType != SynchronizationTimer)
        return STATUS_INVALID_PARAMETER;
    return ExpCreate(&ExTimerObjectType, ObjectAttributes, TimerHandle, [&](PVOID o) {
        auto t = (ETIMER *)o;
        if (t) {
            KeInitializeTimerEx(&t->KeTimer, TimerType);
            KeInitializeDpc(&t->TimerDpc, ExpTimerDpcRoutine, t);
            t->ApcAssociated = FALSE;
        }
        return t;
    });
}

typedef void(NTAPI *PTIMER_APC_ROUTINE)(PVOID TimerContext, ULONG TimerLowValue, LONG TimerHighValue);

XBAPI NTSTATUS NTAPI NtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE TimerApcRoutine,
                                  KPROCESSOR_MODE ApcMode, PVOID TimerContext, BOOLEAN ResumeTimer, LONG Period,
                                  PBOOLEAN PreviousState) {
    if (Period < 0)
        return STATUS_INVALID_PARAMETER;
    return ExpWithObject(TimerHandle, &ExTimerObjectType, [&](PVOID o) {
        auto t = (ETIMER *)o;
        KIRQL old = KeRaiseIrqlToDpcLevel();
        BOOLEAN state = t->KeTimer.Header.SignalState != 0;
        KeCancelTimer(&t->KeTimer);
        KeRemoveQueueDpc(&t->TimerDpc);
        t->ApcAssociated = TimerApcRoutine != nullptr;
        if (TimerApcRoutine)
            KeInitializeApc(&t->TimerApc, KeGetCurrentThread(), ExpTimerApcKernelRoutine, nullptr,
                            (PKNORMAL_ROUTINE)TimerApcRoutine, ApcMode, TimerContext);
        KeSetTimerEx(&t->KeTimer, *DueTime, Period, &t->TimerDpc);
        KfLowerIrql(old);
        if (PreviousState)
            *PreviousState = state;
        return STATUS_SUCCESS;
    });
}

XBAPI NTSTATUS NTAPI NtCancelTimer(HANDLE TimerHandle, PBOOLEAN CurrentState) {
    return ExpWithObject(TimerHandle, &ExTimerObjectType, [&](PVOID o) {
        auto t = (ETIMER *)o;
        KeCancelTimer(&t->KeTimer);
        KeRemoveQueueDpc(&t->TimerDpc);
        if (CurrentState)
            *CurrentState = t->KeTimer.Header.SignalState != 0;
        return STATUS_SUCCESS;
    });
}

struct TIMER_BASIC_INFORMATION {
    LARGE_INTEGER RemainingTime;
    BOOLEAN TimerState;
};

XBAPI NTSTATUS NTAPI NtQueryTimer(HANDLE TimerHandle, TIMER_BASIC_INFORMATION *TimerInformation) {
    return ExpWithObject(TimerHandle, &ExTimerObjectType, [&](PVOID o) {
        auto t = (ETIMER *)o;
        LONGLONG remaining = 0;
        if (t->KeTimer.Header.Inserted)
            remaining = (LONGLONG)t->KeTimer.DueTime - (LONGLONG)KeQueryInterruptTime();
        TimerInformation->RemainingTime.QuadPart = remaining > 0 ? remaining : 0;
        TimerInformation->TimerState = t->KeTimer.Header.SignalState != 0;
        return STATUS_SUCCESS;
    });
}

// --- Waits through handles (WaitReason UserRequest)

XBAPI NTSTATUS NTAPI NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                             PLARGE_INTEGER Timeout) {
    PVOID o;
    NTSTATUS status = ObReferenceObjectByHandle(Handle, nullptr, &o);
    if (!NT_SUCCESS(status))
        return status;
    status = KeWaitForSingleObject(ObpGetWaitableObject(o), UserRequest, WaitMode, Alertable, Timeout);
    ObfDereferenceObject(o);
    return status;
}

XBAPI NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    return NtWaitForSingleObjectEx(Handle, UserMode, Alertable, Timeout);
}

XBAPI NTSTATUS NTAPI NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], WAIT_TYPE WaitType,
                                                KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    if (Count == 0 || Count > 64 || (WaitType != WaitAll && WaitType != WaitAny))
        return STATUS_INVALID_PARAMETER;
    PVOID objects[64], waitables[64];
    KWAIT_BLOCK blocks[64];
    NTSTATUS status = STATUS_SUCCESS;
    ULONG n = 0;
    for (; n < Count; n++) {
        status = ObReferenceObjectByHandle(Handles[n], nullptr, &objects[n]);
        if (!NT_SUCCESS(status))
            break;
        waitables[n] = ObpGetWaitableObject(objects[n]);
    }
    if (NT_SUCCESS(status))
        status = KeWaitForMultipleObjects(Count, waitables, WaitType, UserRequest, WaitMode, Alertable, Timeout, blocks);
    while (n)
        ObfDereferenceObject(objects[--n]);
    return status;
}

XBAPI NTSTATUS NTAPI NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle, KPROCESSOR_MODE WaitMode,
                                                      BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    PVOID signal, wait;
    NTSTATUS status = ObReferenceObjectByHandle(SignalHandle, nullptr, &signal);
    if (!NT_SUCCESS(status))
        return status;
    status = ObReferenceObjectByHandle(WaitHandle, nullptr, &wait);
    if (!NT_SUCCESS(status)) {
        ObfDereferenceObject(signal);
        return status;
    }
    POBJECT_TYPE type = OBJECT_TO_OBJECT_HEADER(signal)->Type;
    // Signal with Wait=TRUE so the wait starts atomically at DISPATCH_LEVEL.
    if (type == &ExEventObjectType) {
        KeSetEvent((PKEVENT)signal, EVENT_INCREMENT, TRUE);
    } else if (type == &ExSemaphoreObjectType) {
        KeReleaseSemaphore((PKSEMAPHORE)signal, SEMAPHORE_INCREMENT, 1, TRUE);
    } else if (type == &ExMutantObjectType && ((PKMUTANT)signal)->OwnerThread == KeGetCurrentThread()) {
        KeReleaseMutant((PKMUTANT)signal, MUTANT_INCREMENT, FALSE, TRUE);
    } else {
        status = STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (NT_SUCCESS(status))
        status = KeWaitForSingleObject(ObpGetWaitableObject(wait), UserRequest, WaitMode, Alertable, Timeout);
    ObfDereferenceObject(wait);
    ObfDereferenceObject(signal);
    return status;
}

NTSTATUS KiYieldExecution(); // ke/sched.cpp

// Any ready thread gets the processor, lower priorities included (spec/stock/07); not a zero-length delay, which only
// yields to equal or higher priorities.
XBAPI NTSTATUS NTAPI NtYieldExecution() { return KiYieldExecution(); }
