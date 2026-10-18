# Spec 08 — APCs, alerts, suspend/resume, queues, wake-up boost (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile `ev_37559137…` (KeInitializeApc, KeInsertQueueApc,
KiDeliverApc, KeEnter/LeaveCriticalRegion), `ev_3b0c3ce5…` (KiInsertQueueApc, KeTestAlertThread,
KeAlertThread, NtWaitForSingleObjectEx), `ev_eb6d3258…` (user-APC runner, KiUnwaitThread, KeSuspendThread,
KeResumeThread, KeAlertResumeThread). Queue and device-queue objects follow NT semantics (not separately
decompiled). **Status: implemented (milestone 2b+).**

## KAPC (0x28) and initialisation
`+0 Type (0x12)`, `+2 ApcMode`, `+3 Inserted`, `+4 Thread`, `+8 ApcListEntry`, `+0x10 KernelRoutine`,
`+0x14 RundownRoutine`, `+0x18 NormalRoutine`, `+0x1C NormalContext`, `+0x20/+0x24 SystemArgument1/2`.
`KeInitializeApc(apc, thread, kernel, rundown, normal, mode, ctx)` (7 args, stdcall 28). With no normal routine
(a "special" APC), ApcMode = KernelMode and NormalContext = NULL.

## Queuing (`KeInsertQueueApc(apc, a1, a2, increment)`)
At DISPATCH_LEVEL, and only if `thread.ApcState.ApcQueueable`:
1. Store the args.
2. Insert into `ApcListHead[mode]`. **Special APCs go after any existing special APCs** (before the first
   normal one); normal APCs go at the tail. Set Inserted. An already-inserted APC returns FALSE.
3. Kernel mode:
   - set `KernelApcPending`
   - target Running → request an APC_LEVEL software interrupt
   - target Waiting with `WaitIrql == PASSIVE` and (special APC, or `KernelApcDisable == 0` and no kernel
     APC in progress) → unwait it with **STATUS_KERNEL_APC (0x100)**. The wait then delivers the APC and
     **re-waits** for the remaining time.
4. User mode: target Waiting with `WaitMode == UserMode` and Alertable → `UserApcPending = 1`, unwait with
   **STATUS_USER_APC (0xC0)**.
There is no `KeRemoveQueueApc` export on the Xbox.

## Delivery
- `KiDeliverApc` (APC_LEVEL software interrupt) handles **kernel-mode APCs only**. At SYNCH_LEVEL it clears
  KernelApcPending, then loops over ApcListHead[KernelMode]:
  - special APC: dequeue, drop to APC_LEVEL, call `KernelRoutine(apc, &normal, &ctx, &a1, &a2)`
  - normal APC: stop if `KernelApcInProgress` or `KernelApcDisable`. Otherwise dequeue, run KernelRoutine at
    APC_LEVEL; if a normal routine remains, set KernelApcInProgress and run it **at PASSIVE_LEVEL**, then raise back.
- **User APCs** (the Xbox has no user mode): when an alertable wait or delay returns STATUS_USER_APC, the
  wait routine itself (reference 0x8001bc2b) clears UserApcPending and runs every user APC at PASSIVE_LEVEL
  (KernelRoutine, then NormalRoutine) **before returning STATUS_USER_APC to the caller**.
- The context switch requests an APC interrupt when the incoming thread has KernelApcPending.
- `KeEnterCriticalRegion`: `KernelApcDisable--`. `KeLeaveCriticalRegion`: `++`; if it reaches 0 and kernel
  APCs are queued → KernelApcPending, request an APC interrupt.

## Alerts
- `KeAlertThread(t, mode)`: if not already alerted for `mode`: a thread in an alertable wait with
  `mode <= WaitMode` is unwaited with **STATUS_ALERTED (0x101), increment 2**; otherwise set `Alerted[mode]`.
  Returns the previous flag.
- `KeTestAlertThread(mode)`: return and clear `Alerted[mode]`. If it was clear and the mode is UserMode with user
  APCs queued, set UserApcPending.
- Checks before blocking, for alertable waits:
  - `Alerted[mode]` → clear it, return STATUS_ALERTED
  - else, user-mode wait with user APCs queued → UserApcPending, STATUS_USER_APC
  - else `Alerted[KernelMode]` → clear it, STATUS_ALERTED
  For non-alertable user-mode waits, a pending UserApcPending returns STATUS_USER_APC.

## Suspend / resume
- KeInitializeThread sets up: **SuspendApc** (kernel routine = no-op, normal routine = wait on SuspendSemaphore
  with WaitReason Suspended) and **SuspendSemaphore** (count 0, limit 2).
- `KeSuspendThread(t)`: SuspendCount 0x7F → raise STATUS_SUSPEND_COUNT_EXCEEDED. If ApcQueueable: SuspendCount++.
  On the first suspend, insert SuspendApc; if it was still inserted, decrement the semaphore instead. Returns the previous count.
- `KeResumeThread(t)`: if SuspendCount reaches 0 → semaphore +1 and wait test. Returns the previous count.
- `KeAlertResumeThread(t)`: kernel-mode alert (as above), then resume. Returns the previous suspend count.

## Wake-up (KiUnwaitThread)
1. `WaitStatus |= status`; unlink every wait block; leave the global wait list; cancel the thread timer;
   if the thread belongs to a queue → `queue.CurrentCount++`.
2. **Boost** (priority < 16): if PriorityDecrement == 0 and boosts are enabled → priority = max(priority,
   min(base + increment, 15)). If base < 14: Quantum −= 10; at ≤ 0 refill, priority −= 1 + PriorityDecrement
   (clamped to base), decrement = 0. Otherwise (base ≥ 14 or real-time) refill the quantum.
3. KiReadyThread.

## Queues (KQUEUE 0x28: Header, EntryListHead +0x10, CurrentCount +0x18, MaximumCount +0x1C, ThreadListHead +0x20)
- `KeInitializeQueue(q, count)`: count 0 → 1.
- Insert (head/tail): if a waiter exists (served **LIFO**), CurrentCount < Maximum, and the inserter isn't itself a
  queue waiter of this queue → hand the entry to that waiter (its WaitStatus = entry pointer). Otherwise queue
  the entry and bump SignalState. Returns the previous SignalState.
- `KeRemoveQueue(q, mode, timeout)`: on first call, associate the thread with the queue (moving from an old
  queue releases a slot there); otherwise give back its slot. Take an entry if one is available and below the
  limit. Else wait (WaitReason WrQueue). Returns the entry, or STATUS_TIMEOUT / STATUS_USER_APC cast to a pointer.
- A queue thread blocking on anything else releases its slot (KiActivateWaiterQueue).
- `KeRundownQueue`: detach the entries (returned as a ring) and disassociate the threads.

## Device queues (KDEVICE_QUEUE 0x0C, entry 0x10)
- Insert returns FALSE and marks the queue Busy when it was idle; otherwise it appends (or inserts by SortKey).
- Remove: when empty, mark the queue idle and return NULL. RemoveByKey takes the first entry with key ≥ the given key,
  wrapping to the head if there isn't one.
- `KeSynchronizeExecution(int, routine, ctx)`: run routine at the interrupt's IRQL.

## Priority APIs
- `KeSetBasePriorityThread(t, increment)`: base = process base + increment, clamped to the thread's class.
  ±8 saturates. The current priority is shifted by the same delta.
- `KeSetDisableBoostThread`, `KeBoostPriorityThread(t, inc)`: as NT.
- `KeSetPriorityProcess`: shift every thread's base by the same delta.
