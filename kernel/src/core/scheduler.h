// The dispatcher's internals, shared by the files of src/core/ only (spec/stock/06, 07): thread scheduling, waits, APC and DPC delivery, timers, interrupt
// delivery, the FPU's lazy switching and thread creation. The dispatcher "lock" is DISPATCH_LEVEL on this uniprocessor. What other folders use is in core.h.
#pragma once
#include <core/core.h>

namespace xk::core {

// --- Interrupts and the clock (interrupts.cpp, clock.cpp, deferred_calls.cpp)

void ClockInterrupt(KIRQL interruptedIrql);
void CheckTimers();
#if XK_DEBUG_SERIAL
void DumpTimers(); // the hang dump (interrupts.cpp): the timer list and the expiry DPC
#if XK_DEBUG_SERIAL
void StartHangDumps(); // serial builds: starts the hang dump's thread (interrupts.cpp), before the first title
#endif
#endif
void DispatchInterrupt();
void RetireDpcList();
void DeliverSoftwareInterrupts();          // interrupts disabled on entry and exit
void UpdateRunTime(KIRQL interruptedIrql); // run-time accounting (scheduler.cpp)

// --- Scheduling (scheduler.cpp, thread.cpp)

void ReadyThread(PKTHREAD Thread);
void SetPriorityThread(PKTHREAD Thread, SCHAR Priority);
bool SelectYieldThread(PKTHREAD Current);
LONG SwapThread(); // caller at DISPATCH_LEVEL with its wait state set up
void InitializeProcesses();
void InitializeThread(PKTHREAD Thread, PVOID KernelStack, ULONG KernelStackSize, ULONG TlsDataSize, PKSYSTEM_ROUTINE SystemRoutine, PVOID StartRoutine,
                      PVOID StartContext, PKPROCESS Process);
// Terminated threads whose kernel stacks are freed after the switch away from them.
void ReapDeadThreads();

// --- Waits, APCs, queues and timers (wait.cpp, async_calls.cpp, queue.cpp, timer.cpp)

void WaitTest(DISPATCHER_HEADER *Object, LONG Increment);
void UnwaitThread(PKTHREAD Thread, LONG WaitStatus, LONG Increment);
extern LIST_ENTRY WaitInListHead;
bool InsertQueueApc(PKAPC Apc, LONG Increment);
void DeliverApc();
void ExecuteUserApcs();
void NTAPI SuspendNop(PKAPC, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *); // the suspension APC (async_calls.cpp)
void NTAPI SuspendThread(PVOID, PVOID, PVOID);
void ActivateWaiterQueue(PKQUEUE Queue);
LONG InsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry, bool Head);
bool InsertTimerRelative(PKTIMER Timer, LONGLONG DueTime);
void RemoveTimerIfInserted(PKTIMER Timer);

// --- The FPU (float_state.cpp): lazy switching and each thread's save area

void InitializeNpx();
void InitializeNpxArea(PKTHREAD t);
void NpxPrepareSwitch(PKTHREAD next);
void NpxRelease(PKTHREAD t);

} // namespace xk::core
