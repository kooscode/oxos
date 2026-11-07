# Spec 07 — Threads, scheduler, waits (xboxkrnl 1.0.4627.1) — milestone 2b input

Clean-room behavioural spec. Evidence: batch_decompile `ev_17a6d736…` (KeInitializeThread,
PsCreateSystemThreadEx, KiSwapContext, KiUnlockDispatcherDatabase, KiDispatchInterrupt),
`ev_796a4236…` (context init, KiReadyThread, KeReadyThread, quantum end), `ev_bf895671…`
(KeWaitForSingleObject, KeDelayExecutionThread, PsTerminateSystemThread), and objdump of the clock ISR
(0x80030ea0), the #NM trap (0x8001b518) and the thread trampoline (0x8001cc78).
**Status: implemented (milestones 2b and 2b+; APCs, alerts, queues and boosts are specified in spec 08).** Implementation choices that differ internally but not in ABI:
- the switch frame also saves ebp/ebx/esi/edi
- IRQ dispatch levels: a connected KINTERRUPT's IRQL replaces the default level for its IRQ when the PIC mask is computed
- kernel stacks and thread objects come from a boot-time bump allocator and are not yet reclaimed

## Corrections to spec 06
- **Clock IRQ0 runs at IRQL 0x1C (28)**, not 26 − 0. Decoding the reference per-IRQL PIC mask table
  (0x80014224, low 16 bits) gives the IRQL at which each IRQ is first masked: IRQ0 → 28, IRQ8 → 26,
  IRQ12 → 27, IRQ2 never; every other IRQ n → 26 − n. `HalGetInterruptVector` still returns 26 − n for every IRQ.
- The reference clock bucketizes timers into 32 lists indexed by `tick & 31` (0x8003A900); our sorted
  list is equivalent.

## Clock tick (every 1 ms)
- Advance KeInterruptTime and KeSystemTime by 10000; KeTickCount++. Queue the timer-expiry DPC if the
  current bucket's head is due.
- Time accounting by the interrupted IRQL: < 2 → `thread.KernelTime`; == 2 → `Prcb.DpcTime` if a DPC is
  running, else `KernelTime`; > 2 → `Prcb.InterruptTime`.
- **Quantum**: `thread.Quantum (+0x6C) -= 3`. If ≤ 0 and the thread is not idle: `Prcb.QuantumEnd (+0x38) =
  nonzero`, request a DISPATCH software interrupt. Process quantum 0x3C → 20 ms; idle process 0x7F.

## Layouts (nxdk order with MSVC 8-byte alignment; confirmed by field writes)
KTHREAD (0x110): Header 0x00 (Type 6, Size 0x44) · MutantListHead 0x10 · KernelTime 0x18 · StackBase 0x1C ·
StackLimit 0x20 · KernelStack 0x24 · **TlsData 0x28** · State 0x2C · Alerted[2] 0x2D · Alertable 0x2F ·
NpxState 0x30 · Saturation 0x31 · Priority 0x32 · ApcState 0x34 (lists 0x34/0x3C, Process 0x44,
KernelApcInProgress 0x48, KernelApcPending 0x49, UserApcPending 0x4A, ApcQueueable 0x4B) ·
ContextSwitches 0x4C · WaitStatus 0x50 · WaitIrql 0x54 · WaitMode 0x55 · WaitNext 0x56 · WaitReason 0x57 ·
WaitBlockList 0x58 · WaitListEntry 0x5C (also the ready-list link) · WaitTime 0x64 · KernelApcDisable 0x68 ·
Quantum 0x6C · BasePriority 0x70 · DecrementCount 0x71 · PriorityDecrement 0x72 · DisableBoost 0x73 ·
NpxIrql 0x74 · SuspendCount 0x75 · Preempted 0x76 · HasTerminated 0x77 · Queue 0x78 · QueueListEntry 0x7C ·
**Timer 0x88** (8-aligned) · TimerWaitBlock 0xB0 · SuspendApc 0xC8 · SuspendSemaphore 0xF0 · ThreadListEntry 0x104.
ETHREAD (0x140 + extension): Tcb · CreateTime 0x110 · ExitTime 0x118 · ExitStatus 0x120 ·
ReaperListEntry 0x124 · **UniqueThread 0x12C (= handle)** · StartAddress 0x130 · IrpList 0x134 · DebugData 0x13C.
KPROCESS: ReadyListHead 0 · ThreadListHead 8 · StackCount 0x10 · ThreadQuantum 0x14 · BasePriority 0x18 ·
DisableBoost 0x19 · DisableQuantum 0x1A. The system process is at 0x8003A840 (base priority 8, quantum 0x3C).
Thread states: Initialized 0, Ready 1, Running 2, Standby 3, Terminated 4, Waiting 5.

## Thread creation
`PsCreateSystemThreadEx(Handle, ExtSize, KernelStackSize, TlsDataSize, *ThreadId, Ctx1, Ctx2, CreateSuspended,
DebuggerThread, SystemRoutine)`:
1. `ObCreateObject(PsThreadObjectType, 0x140 + ExtSize)` and zero it. Initialize the IrpList and Reaper lists.
2. Stack = round_up(KernelStackSize, 4 KB), minimum 0x3000, from `MmCreateKernelStack`.
   Failure → STATUS_INSUFFICIENT_RESOURCES.
3. StartAddress = Ctx1. Call `KeInitializeThread(thr, stackTop, size, TlsDataSize, SystemRoutine, Ctx1, Ctx2,
   &SystemProcess)`. Enable APC queuing. If CreateSuspended → `KeSuspendThread`.
4. Take 2 references. `ObInsertObject` puts the handle in UniqueThread. On failure → HasTerminated = 1 (resume if suspended).
   Otherwise call the create-notify routines (≤ 8, at 0x8003ACA0) with (thr, UniqueThread, TRUE), then
   `ObOpenObjectByPointer` → *Handle. *ThreadId = UniqueThread.
5. CreateTime = system time. `KeReadyThread`. Dereference.
`KeInitializeThread`: initialize the lists. Type 6, Size 0x44. Process = given; ApcQueueable = 1.
SuspendApc (kernel nop routine, normal routine = suspend). SuspendSemaphore (0, limit 2).
Timer = notification timer. TimerWaitBlock: Thread = self, Object = &Timer, WaitKey 0x102, WaitType 1,
linked to the timer's wait list. StackBase = top, StackLimit = top − size.
**Context init**:
- NPX area 0x210 at StackBase − 0x210, zeroed, FCW 0x027F, MXCSR (+0x18) 0x1F80; NpxState = 0x0A (not loaded).
- **TLS = round_up(TlsDataSize, 4) bytes directly below the NPX area**, zeroed; TlsData = its address, or NULL if the size is 0.
- Switch frame below that: [ExceptionList = −1][EFLAGS = 0x200][ret = KiThreadStartup][SystemRoutine]
  [StartRoutine (Ctx1)][StartContext (Ctx2)]. KernelStack points at the first entry.
- Priority = BasePriority = process base; Quantum = process quantum; DisableBoost from the process.
- Link into the process thread list; process StackCount++.
**KiThreadStartup**: clear ebx/esi/edi/ebp; `KfLowerIrql(PASSIVE)`. If HasTerminated →
`PsTerminateSystemThread(STATUS_NO_MEMORY)`. Else pop and call `SystemRoutine(StartRoutine, StartContext)`
(stdcall). If it returns → `KeBugCheck(0x0E)`.

## Scheduling
- 32 ready lists (0x8003AA20) + a ready summary bitmask (0x8003AA00), linked through WaitListEntry.
- **KiReadyThread(t)**:
  - clear Preempted (remember it); WaitTime = KeTickCount
  - if no NextThread and t.Priority > Current.Priority → Current.Preempted = 1, NextThread = t (Standby)
  - if a NextThread exists and t.Priority > Next.Priority → t replaces it (Standby), and the displaced thread is
    re-readied as preempted
  - otherwise state Ready, inserted at the **head** if it was preempted, else at the tail; set its summary bit
  - (first-thread flag 0x8003AA04: the first ready thread becomes NextThread unconditionally)
- **KiUnlockDispatcherDatabase(oldIrql)**: if NextThread:
  - oldIrql < 2 → switch now (Current.WaitIrql = oldIrql; ready the old thread if it's still running; KiSwapContext;
    deliver a kernel APC if one is pending), then lower to the new thread's WaitIrql
  - else, if no DPC is active → request DISPATCH
  Finally lower to oldIrql.
- **KiDispatchInterrupt**: drain DPCs **on Prcb.DpcStack** (0x3000 bytes), setting ExceptionList = −1 meanwhile.
  Then: QuantumEnd → clear it, KiQuantumEnd; else take NextThread. If there is a new thread: swap.
- **KiQuantumEnd**: if Quantum ≤ 0: if process.DisableQuantum and priority ≥ 16 → Quantum = 0x7FFFFFFF; else
  refill from the process. For priority < 16: new = prio − PriorityDecrement − 1, clamped to BasePriority;
  PriorityDecrement = 0. If unchanged and no NextThread → pick a ready thread with priority ≥ the current one (Standby).
  If changed → set priority.
- **NtYieldExecution** (0x8001aef3): if the ready summary is 0 → `STATUS_NO_YIELD_PERFORMED` (0x40000024). Else
  raise to DISPATCH (Current.WaitIrql = the old IRQL); if there is no NextThread, take the highest ready thread with
  priority **≥ 1** (any priority, lower ones included) as NextThread; none → lower IRQL, `STATUS_NO_YIELD_PERFORMED`.
  Otherwise the current thread: Quantum = process quantum, state Ready, and below 16 its priority decays as at a
  quantum end (− 1 − PriorityDecrement, clamped to BasePriority; PriorityDecrement = 0); inserted at the **tail** of
  its ready list, summary bit set; switch (KiSwapThread); return 0.
- **KiSwapContext**:
  - new.State = Running
  - push EFLAGS and PCR.ExceptionList
  - DPC active → `KeBugCheck(0xB8)`
  - old.KernelStack = esp
  - PCR.StackBase = new.StackBase − 0x210; PCR.StackLimit = new.StackLimit
  - esp = new.KernelStack
  - new.ContextSwitches++, Prcb.KeContextSwitches++
  - pop ExceptionList and EFLAGS
  - if new.KernelApcPending → deliver now or request an APC interrupt
- **Lazy FPU (#NM, vector 7)**: if current.NpxState ≠ 0: clear CR0 TS/EM/MP; if Prcb.NpxThread (+0x0C) is set →
  fxsave into its area and set its NpxState = 0x0A; fxrstor the current area; NpxState = 0; NpxThread = current.
  Then OR in the area's CR0 dword (+0x200). Context switches arm TS for threads that don't own the FPU.

## Waits (NT semantics)
`KeWaitForSingleObject(obj, reason, mode, alertable, timeout)`:
1. Raise to DISPATCH unless WaitNext is set. Loop while a kernel APC is pending at WaitIrql 0. WaitStatus = 0.
2. Mutant (type 2):
   - if signalled or owned by self: SignalState == 0x80000000 → `ExRaiseStatus` (limit exceeded)
   - otherwise decrement; on reaching 0 take ownership: Owner (+0x18) = self, insert into the thread's
     MutantListHead, and if Abandoned (+0x1C) → clear it and return STATUS_ABANDONED (0x80)
3. Other object with SignalState > 0: synchronization types (type & 7 == 1) reset to 0; semaphore (5) −1;
   return 0.
4. Otherwise:
   - alertable: an alert pending for the mode → STATUS_ALERTED (0x101); a user-mode wait with pending user APCs →
     STATUS_USER_APC (0xC0)
   - timeout 0 → STATUS_TIMEOUT (0x102)
   - else link the wait block (WaitKey 0, WaitType 1) into the object; with a timeout, arm the thread timer and
     chain TimerWaitBlock
   - if the thread belongs to a queue → wake it
   - set Alertable, WaitMode, WaitReason, WaitTime; State = Waiting; put it on the global wait list
     (0x8003A8A8); KiSwapThread
   - a kernel APC wake (0x100) → recompute the remaining timeout and re-wait
5. Unlock and return WaitStatus.
`KeDelayExecutionThread(mode, alertable, interval)`: same alert/APC checks; WaitReason 4; arm the thread timer.
- An already-expired interval with priority < 16 → reset to BasePriority.
- If a ready thread with priority ≥ the current one exists → yield (refill quantum, ready self).
- Otherwise return 0 at once. STATUS_TIMEOUT maps to 0.

## Termination (`PsTerminateSystemThread(status)`)
1. HasTerminated = 1; lower to PASSIVE.
2. Call the notify routines (thr, UniqueThread, FALSE). Priority < 16 → raise it to 16.
3. Cancel the thread's IRPs; misc rundown. ExitStatus = status; ExitTime = now.
4. NtClose(UniqueThread); UniqueThread = 0.
5. Disable APCs; run the mutant/APC rundown, flushing kernel APCs.
6. Owned mutants remaining → `KeBugCheckEx(0x20, …)`; else KeTerminateThread (signal the thread object, never return).

**KeTerminateThread** (0x800192cb), at DISPATCH_LEVEL:
1. If the thread is associated with a queue (KTHREAD.Queue): unlink QueueListEntry and **KiActivateWaiterQueue** the
   queue (its concurrency slot is released and a queued entry may go to another waiter).
2. SignalState = 1; waiters → KiWaitTest.
3. Unlink ThreadListEntry from the process; State = Terminated (4); process StackCount − 1; if it owns the FPU
   (Prcb.NpxThread) → NpxThread = 0.
4. Append it to the reaper list, queue the reaper, switch away (never returns).

## Plan for 2b implementation
1. Early memory: map-free bump allocator for kernel stacks until milestone 2c (check the 2BL's mapping of
   0x80000000+ with `info mem` first).
2. KTHREAD/KPROCESS/ETHREAD layouts with offset static_asserts; system and idle processes.
3. KiSwapContext (asm), KiThreadStartup, ready lists, KiReadyThread / KiUnlockDispatcherDatabase / KiQuantumEnd.
4. Clock quantum, DPC stack, lazy FPU (#NM).
5. Waits: KeWaitForSingle/MultipleObjects, KiWaitTest (wire up the existing event/timer hooks),
   KeDelayExecutionThread, semaphores, mutants.
6. Internal thread creation (`Ps*` exports need the object manager → milestone 3).
7. Self-tests: two threads ping-pong via synchronization events, a sleep accuracy test, an FPU state check across a switch.
