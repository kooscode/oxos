# Spec 06 — IRQL, interrupts, DPCs, timers, events (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile `ev_9703dd24…` (HalGetInterruptVector,
Ke{Initialize,Connect,Disconnect}Interrupt), `ev_cde1281d…` (DPC, timer, event primitives), and
objdump of 0x80014128–0x800145a4 and 0x80014fb8–0x80015044.
Structure layouts follow nxdk `xboxkrnl.h` (CC0). They match the field writes observed in the kernel.

## 1. IRQL model
- Levels: PASSIVE 0, APC 1, DISPATCH 2, device levels, SYNCH 0x1C, HIGH 0x1F. The current IRQL is
  the byte at **KPCR+0x24**.
- **Device IRQ n ↔ vector 0x30+n ↔ IRQL 26 − n** (`HalGetInterruptVector`: accepts bus levels
  0–26, returns vector 0x30+level and IRQL 0x4A − vector; anything else → vector 0). IDE (IRQ14) is
  at 12, NV2A (IRQ3) at 23, USB0 (IRQ1) at 25, USB1 (IRQ9) at 17. **Correction (spec/stock/07):** the
  reference per-IRQL PIC mask table masks IRQ0 (clock) only from **IRQL 28**, IRQ8 from 26 and
  IRQ12 from 27. The clock interrupt runs at 0x1C.
- The reference uses the NT "lazy IRQL" PIC scheme:
  - `KfRaiseIrql` only stores the level
  - a hardware interrupt that arrives at or below the current IRQL is recorded as pending and masked
  - `KfLowerIrql` dispatches every pending software or hardware interrupt above the new level, highest first
  - the pending word is at 0x80035A1C (bit 1 = APC, bit 2 = DISPATCH); software-interrupt lookup table
    `{0,0,1,1,2,2,2,2}`
  - **Implementation freedom**: any scheme where no interrupt runs at or below its IRQL, and pending
    work runs when IRQL drops, is equivalent for titles. Ours masks the PIC eagerly once the IRQL
    reaches device levels, so it never has to replay a deferred interrupt.
- `KeRaiseIrqlToDpcLevel` / `KeRaiseIrqlToSynchLevel` store 2 / 0x1C and return the old level.
- `KeGetCurrentIrql` returns the PCR byte.

## 2. Software interrupts
- `HalRequestSoftwareInterrupt(FASTCALL KIRQL level)`: with interrupts disabled, mark `level` pending.
  If the highest pending software level is above the current IRQL, run its handler now.
- `HalClearSoftwareInterrupt(FASTCALL KIRQL level)`: clear the pending bit.
- **DISPATCH handler**: raise to DISPATCH, clear the pending bit, enable interrupts, run
  `KiDispatchInterrupt` (drain DPCs, then switch to `Prcb.NextThread` if one is set), then return to
  the previous level.
- **APC handler**: same at APC_LEVEL → `KiDeliverApc`.

## 3. Interrupt objects (`KINTERRUPT`, 0x70 bytes)
| off | field |
|---|---|
| 0x00 | ServiceRoutine `BOOLEAN (NTAPI*)(PKINTERRUPT, PVOID)` |
| 0x04 | ServiceContext |
| 0x08 | BusInterruptLevel (= vector − 0x30) |
| 0x0C | Irql |
| 0x10 | Connected (BOOLEAN) |
| 0x11 | ShareVector |
| 0x12 | Mode (0 = LevelSensitive, 1 = Latched) |
| 0x14 | ServiceCount |
| 0x18 | DispatchCode[22] |
- `KeInitializeInterrupt(obj, routine, ctx, vector, irql, mode, shareVector)`: fills the fields, copies a
  22-dword dispatch thunk into DispatchCode, patches in the object address and a jump to the
  level/latched dispatcher, and sets Connected = 0.
- `KeConnectInterrupt(obj)`: at DISPATCH. If it is not connected and the IDT vector still points at
  its default stub: point the IDT at `obj->DispatchCode`, call `HalEnableSystemInterrupt(level, mode)`,
  set Connected = 1, and return TRUE. Otherwise return FALSE. **Implementation freedom**: the IDT may
  instead keep a fixed stub that finds the connected object. Nothing outside the kernel runs
  DispatchCode.
- `KeDisconnectInterrupt(obj)`: if connected → `HalDisableSystemInterrupt(level)`, restore the default
  IDT stub, set Connected = 0. Returns the previous Connected value.
- `HalEnableSystemInterrupt(level, mode)`: maintain the **ELCR** bitmap (I/O 0x4D0/0x4D1; bit set =
  level-sensitive), clear the IRQ in the disabled mask, and write the PIC masks.
  `HalDisableSystemInterrupt(level)`: set the IRQ in the disabled mask and write the PIC masks.
- Hardware dispatch: raise to the object's IRQL, send EOI, call the ISR with interrupts enabled, lower.

## 4. DPCs (`KDPC`, 0x1C bytes)
`+0 CSHORT Type (=0x13)`, `+2 Inserted`, `+4 LIST_ENTRY DpcListEntry`, `+0xC DeferredRoutine`,
`+0x10 DeferredContext`, `+0x14 SystemArgument1`, `+0x18 SystemArgument2`.
- `KeInitializeDpc(dpc, routine, ctx)`: Type = 0x13, Inserted = 0, routine and context set.
- `KeInsertQueueDpc(dpc, a1, a2)`: at HIGH_LEVEL. If not inserted: store the args, set Inserted, **append to
  `Prcb.DpcListHead` (PRCB+0x28)**. If neither `Prcb.DpcRoutineActive` (+0x30) nor
  `Prcb.DpcInterruptRequested` (+0x24) is set, set the latter and request a DISPATCH software interrupt.
  Returns TRUE if it queued the DPC.
- `KeRemoveQueueDpc(dpc)`: if inserted, unlink and clear Inserted. The reference does this **without
  raising IRQL**.
- Retiring: the DISPATCH handler pops DPCs in FIFO order. For each one: clear Inserted, then call
  `routine(dpc, ctx, a1, a2)` at DISPATCH_LEVEL with interrupts enabled. `DpcRoutineActive` is set
  while the list drains. The reference runs DPCs on the PRCB DPC stack (+0x34; 0x3000 bytes).
- `KeIsExecutingDpc()` returns `Prcb.DpcRoutineActive`.

## 5. Timers (`KTIMER`, 0x28 bytes)
`+0 DISPATCHER_HEADER` (Type 8 = notification, 9 = synchronization; Size 10), `+0x10 DueTime (u64)`,
`+0x18 TimerListEntry`, `+0x20 Dpc`, `+0x24 Period (ms)`.
- `KeInitializeTimerEx(t, type)`: Type = 8 + type, Size = 10, Inserted = 0, SignalState = 0, wait list
  empty, DueTime = 0, Period = 0.
- `KeSetTimerEx(t, DueTime, Period, Dpc)` (at DISPATCH; returns the previous Inserted value):
  1. if inserted, remove it; SignalState = 0; store Dpc and Period
  2. **DueTime < 0** → relative to interrupt time; **≥ 0** → absolute system time (Absolute flag set,
     converted to interrupt time)
  3. if it is already due: satisfy the waiters (wait test), queue `Dpc` with
     **(SystemArgument1, SystemArgument2) = KeSystemTime (low, high)**, and if periodic re-insert at
     `Period × 10000` (100 ns) relative.
- `KeSetTimer(t, DueTime, Dpc)` = `KeSetTimerEx(t, DueTime, 0, Dpc)`.
- `KeCancelTimer(t)`: if inserted, remove it. Returns the previous Inserted value.
- **Expiry** (clock-driven DPC): for each expired timer: remove it, SignalState = 1, satisfy waiters, queue
  its Dpc with the system time as arguments, and re-insert periodic timers at due + Period.

## 6. Events (`KEVENT` = DISPATCHER_HEADER, 0x10 bytes)
- `KeInitializeEvent(e, type, state)`: Type = type (0 notification, 1 synchronization), **Size = 4**,
  SignalState = state, wait list empty. **Bytes 1 (Absolute) and 3 (Inserted) are left untouched.**
- Set/Reset/Pulse semantics follow NT (completed in milestone 2b together with waits).

## Dispatcher object type codes
EventNotification 0, EventSynchronization 1, Mutant 2, Process 3, Queue 4, Semaphore 5, Thread 6,
TimerNotification 8, TimerSynchronization 9, Apc 0x12, Dpc 0x13, DeviceQueue 0x14 (nxdk/NT; Dpc and
Timer confirmed in the reference).

## 7. Performance counter and stalls (0x80015314, 0x80015350, 0x80015358, 0x800153a0; milestone 6d)
- `KeQueryPerformanceFrequency` = **3,375,000** (the MCPX ACPI PM timer runs at 27 MHz / 8; not ACPI's 3.579545 MHz).
- The PM timer (I/O 0x8008) is read as **32 bits**. The SCI handler, on PM1 status bit 0 (timer bit 31 toggled),
  advances a 64-bit base by 2^31 (plus another 2^31 if bit 31 already toggled again); the base's high part is
  written high2/low/high so readers can retry on a torn read. `KeQueryPerformanceCounter` = base high part : (timer
  bits 0–30 | base bit 31), plus 2^31 if timer bit 31 differs from the base's (a toggle not handled yet).
- `KeStallExecutionProcessor(us)`: `cpuid`, then a busy loop of us × 367 iterations (`sub eax,1; jnz`; 32-bit
  product). 0x8003AFF0 is a constant (0x16F, for the 733 MHz CPU), never written: no calibration. Ours does the
  same. (Ours used to poll the PM timer for an exact duration; in xemu, which runs the loop much faster than the
  console, that made stalls far longer than the reference's: Halo 2's PhyInitialize with no link waited 3.5 s
  where the reference took a fraction of a second, delaying its intro.)
- Measured in xemu: 3.378 M counts per second of KeTickCount; the timer goes past 24 bits (32-bit).
