// Process structure: system threads as objects (spec/stock/07 "Thread creation", spec/stock/10 §5).
#include <core/core.h>
#include <core/scheduler.h>
#include <runtime/runtime.h>
#include <diagnostics/diagnostics.h>
#include <objects/objects.h>

namespace xk::core {

static void NTAPI DeleteThread(PVOID Object) {
    auto t = (ETHREAD *)Object;
    if (t->Tcb.StackBase) // never ran / not reaped
        MmDeleteKernelStack(t->Tcb.StackBase, t->Tcb.StackLimit);
}

XBAPI OBJECT_TYPE PsThreadObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, DeleteThread, nullptr, (PVOID)0, 'erhT'};

static PCREATE_THREAD_NOTIFY_ROUTINE CreateThreadNotifyRoutines[8]; // registered by titles, at most 8
static volatile LONG SystemThreadCount;                             // system threads created and not yet deleted

XBAPI NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine(PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine) {
    for (auto &slot : CreateThreadNotifyRoutines) {
        if (!slot) {
            slot = NotifyRoutine;
            return STATUS_SUCCESS;
        }
    }
    return (NTSTATUS)0xC000009A;
}

static void NotifyThreadCreation(ETHREAD *t, BOOLEAN create) {
    for (auto routine : CreateThreadNotifyRoutines) {
        if (routine)
            routine(t, t->UniqueThread, create);
    }
}

XBAPI NTSTATUS NTAPI PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtensionSize, ULONG KernelStackSize, ULONG TlsDataSize, PHANDLE ThreadId,
                                            PVOID StartContext1, PVOID StartContext2, BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
                                            PKSYSTEM_ROUTINE SystemRoutine) {
    ETHREAD *t;
    ULONG size = sizeof(ETHREAD) + ThreadExtensionSize;
    NTSTATUS status = ObCreateObject(&PsThreadObjectType, nullptr, size, (PVOID *)&t);
    if (!NT_SUCCESS(status))
        return status;
    memset(t, 0, size);
    InitializeListHead(&t->ReaperListEntry);
    InitializeListHead(&t->IrpList);
    ULONG stackSize = (KernelStackSize + 0xFFF) & ~0xFFFu;
    if (stackSize < 0x3000)
        stackSize = 0x3000;
    PVOID stack = MmCreateKernelStack(stackSize, DebuggerThread);
    if (!stack) {
        ObfDereferenceObject(t);
        return (NTSTATUS)0xC000009A;
    }
    t->StartAddress = StartContext1;
    InitializeThread(&t->Tcb, stack, stackSize, TlsDataSize, SystemRoutine, StartContext1, StartContext2, &SystemProcess);
    if (CreateSuspended)
        KeSuspendThread(&t->Tcb);

    // References: one for UniqueThread's handle, one for the caller's handle, one released when the thread's stack is reaped after termination.
    ObfReferenceObject(t);
    ObfReferenceObject(t);
    status = ObInsertObject(t, nullptr, 0, &t->UniqueThread);
    if (NT_SUCCESS(status)) {
        NotifyThreadCreation(t, TRUE);
        status = ObOpenObjectByPointer(t, &PsThreadObjectType, ThreadHandle);
    }
    if (!NT_SUCCESS(status)) {
        t->Tcb.HasTerminated = TRUE;
        if (CreateSuspended)
            KeResumeThread(&t->Tcb);
    } else if (ThreadId) {
        *ThreadId = t->UniqueThread;
    }
    KeQuerySystemTime(&t->CreateTime);
    __atomic_add_fetch(&SystemThreadCount, 1, __ATOMIC_SEQ_CST);
    KIRQL old = KeRaiseIrqlToDpcLevel();
    ReadyThread(&t->Tcb);
    KiUnlockDispatcherDatabase(old);
    ObfDereferenceObject(t);
    return status;
}

XBAPI void NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus);

// Calls StartRoutine(StartContext), then terminates the thread.
static void NTAPI SystemThreadStartup(PVOID StartRoutine, PVOID StartContext) {
    ((void(NTAPI *)(PVOID))StartRoutine)(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

XBAPI NTSTATUS NTAPI PsCreateSystemThread(PHANDLE ThreadHandle, PHANDLE ThreadId, PVOID StartRoutine, PVOID StartContext, BOOLEAN DebuggerThread) {
    return PsCreateSystemThreadEx(ThreadHandle, 0, 0x3000, 0, ThreadId, StartRoutine, StartContext, FALSE, DebuggerThread, SystemThreadStartup);
}

XBAPI void NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus) {
    auto t = (ETHREAD *)KeGetCurrentThread();
    t->Tcb.HasTerminated = TRUE;
    KfLowerIrql(PASSIVE_LEVEL);
    if (t->UniqueThread)
        NotifyThreadCreation(t, FALSE);
    if (t->Tcb.Priority < 16)
        KeSetPriorityThread(&t->Tcb, 16);
    t->ExitStatus = ExitStatus;
    KeQuerySystemTime(&t->ExitTime);
    if (t->UniqueThread) {
        HANDLE h = t->UniqueThread;
        t->UniqueThread = nullptr;
        NtClose(h);
    }
    __atomic_sub_fetch(&SystemThreadCount, 1, __ATOMIC_SEQ_CST);
    TerminateThread(); // the reaper drops the last "running" reference
}

XBAPI NTSTATUS NTAPI PsQueryStatistics(PS_STATISTICS *ProcessStatistics) {
    if (ProcessStatistics->Length != sizeof(PS_STATISTICS))
        return (NTSTATUS)0xC000000D;
    ProcessStatistics->ThreadCount = SystemThreadCount;
    ProcessStatistics->HandleCount = ObpObjectHandleTable.HandleCount;
    return STATUS_SUCCESS;
}

// --- Thread control through handles

// PPS_APC_ROUTINE: see objects.h

static void NTAPI FreeApc(PKAPC Apc, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {
    ExFreePool(Apc);
}

XBAPI NTSTATUS NTAPI NtQueueApcThread(HANDLE ThreadHandle, PPS_APC_ROUTINE ApcRoutine, PVOID ApcArgument1, PVOID ApcArgument2, PVOID ApcArgument3) {
    PKTHREAD t;
    NTSTATUS status = ObReferenceObjectByHandle(ThreadHandle, &PsThreadObjectType, (PVOID *)&t);
    if (!NT_SUCCESS(status))
        return status;
    auto apc = (PKAPC)ExAllocatePoolWithTag(sizeof(KAPC), 'pasP');
    if (!apc) {
        status = (NTSTATUS)0xC000009A;
    } else {
        KeInitializeApc(apc, t, FreeApc, nullptr, (PKNORMAL_ROUTINE)ApcRoutine, UserMode, ApcArgument1);
        if (!KeInsertQueueApc(apc, ApcArgument2, ApcArgument3, 0)) {
            ExFreePool(apc);
            status = (NTSTATUS)0xC0000001; // STATUS_UNSUCCESSFUL
        }
    }
    ObfDereferenceObject(t);
    return status;
}

XBAPI NTSTATUS NTAPI NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount) {
    PKTHREAD t;
    NTSTATUS status = ObReferenceObjectByHandle(ThreadHandle, &PsThreadObjectType, (PVOID *)&t);
    if (!NT_SUCCESS(status))
        return status;
    ULONG prev = KeSuspendThread(t);
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;
    ObfDereferenceObject(t);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount) {
    PKTHREAD t;
    NTSTATUS status = ObReferenceObjectByHandle(ThreadHandle, &PsThreadObjectType, (PVOID *)&t);
    if (!NT_SUCCESS(status))
        return status;
    ULONG prev = KeResumeThread(t);
    if (PreviousSuspendCount)
        *PreviousSuspendCount = prev;
    ObfDereferenceObject(t);
    return STATUS_SUCCESS;
}

// Kernel-internal helper used by boot self-tests: start a system thread at a priority and keep a reference to it (the pointer stays valid after the thread
// exits).
PKTHREAD CreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority) {
    HANDLE h;
    if (!NT_SUCCESS(PsCreateSystemThreadEx(&h, 0, StackSize, 0, nullptr, (PVOID)StartRoutine, Context, TRUE, FALSE, SystemThreadStartup)))
        return nullptr;
    PKTHREAD t;
    ObReferenceObjectByHandle(h, &PsThreadObjectType, (PVOID *)&t);
    NtClose(h);
    if (Priority >= 0) {
        t->BasePriority = (SCHAR)Priority;
        KeSetPriorityThread(t, Priority);
    }
    KeResumeThread(t);
    return t;
}

} // namespace xk::core
