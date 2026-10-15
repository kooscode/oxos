// Thread and process objects (spec/07 "Thread creation").
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/rtl.h>

#define Prcb (KiPcr.PrcbData)

void KiInitializeNpxArea(PKTHREAD t);
void KiNpxRelease(PKTHREAD t);
extern "C" void KiThreadStartup();

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

    Thread->SuspendApc.Type = ApcObject;
    Thread->SuspendApc.Thread = Thread;
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

// --- Internal system threads (PsCreateSystemThreadEx wraps this once handles exist).

static void NTAPI KiSystemThreadRoutine(PVOID startRoutine, PVOID startContext) {
    ((void(NTAPI *)(PVOID))startRoutine)(startContext);
    KeTerminateThread();
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority) {
    StackSize = (StackSize + 0xFFF) & ~0xFFFu;
    if (StackSize < 0x3000)
        StackSize = 0x3000;
    auto thread = (PKTHREAD)MmEarlyAllocate(0x140, 16); // ETHREAD-sized
    auto stack = (UCHAR *)MmEarlyAllocate(StackSize, 16);
    if (!thread || !stack)
        return nullptr;
    RtlZeroMemory(thread, 0x140);
    KeInitializeThread(thread, stack + StackSize, StackSize, 0, KiSystemThreadRoutine, (PVOID)StartRoutine, Context,
                       &KiSystemProcess);
    if (Priority >= 0)
        thread->Priority = thread->BasePriority = (SCHAR)Priority;

    KIRQL old = KeRaiseIrqlToDpcLevel();
    KiReadyThread(thread);
    KiUnlockDispatcherDatabase(old);
    return thread;
}

// Terminate the current thread: signal its object, wake joiners, switch away for good.
// The stack and object are not reclaimed yet (needs the memory manager).
void KeTerminateThread() {
    KeRaiseIrqlToDpcLevel();
    PKTHREAD t = Prcb.CurrentThread;
    t->HasTerminated = TRUE;
    t->State = Terminated;
    t->Header.SignalState = 1;
    if (!IsListEmpty(&t->Header.WaitListHead))
        KiWaitTest(&t->Header, 0);
    RemoveEntryList(&t->ThreadListEntry);
    t->ApcState.Process->StackCount--;
    KiNpxRelease(t);
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
