# Spec 13 — Runtime exports: Rtl, Interlocked, Ex locks, debugger stubs, HAL extras

Evidence: batch_decompile `ev_77d5fe3c…` (ERWLOCK and RTL_CRITICAL_SECTION routines), `ev_9587c92e…`
(HalRegisterShutdownNotification, HalIsResetOrShutdownPending, HalInitiateShutdown, HalWriteSMCScratchRegister,
HalEnableSecureTrayEject, ExReadWriteRefurbInfo, KeSetEventBoostPriority, KeSaveFloatingPointState, NtSetSystemTime,
RtlRip), `ev_14f9dcdb…` (RTC read/write). Generic NT runtime functions follow their documented semantics.
**Status: implemented (milestone 5a).**

## Layouts (Xbox ABI)
- **RTL_CRITICAL_SECTION** (0x1C):
  - an inline synchronization-event header (Type 1, Size 4); LockCount (−1 free); RecursionCount; OwningThread
  - Enter: increment LockCount; if it isn't now 0, recurse when owned, else wait on the event. Leave: if
    RecursionCount drops to 0, clear the owner and signal the event when LockCount stays ≥ 0
  - TryEnter claims a free lock (−1 → 0) or recurses
- **ERWLOCK** (0x34): LockCount (−1 free), WritersWaitingCount, ReadersWaitingCount, ReadersEntryCount, WriterEvent
  (synchronization), ReaderSemaphore (limit 0x7FFFFFFF).
  - Shared acquire waits only when the lock is held and (no readers inside or a writer is waiting).
  - Release by the last holder resets ReadersEntryCount. A writer leaving admits all waiting readers at once; the
    last reader leaving (or a writer with no readers waiting) hands the lock to one writer.
- **HAL_SHUTDOWN_REGISTRATION**:
  - {NotificationRoutine, Priority, ListEntry}, kept in priority order (highest first; equal priorities
    first-come)
  - HalReturnToFirmware (not quick reboot) removes each one and calls it before resetting
- **KFLOATING_SAVE** (0x18): flags, IRQL, …, +8 save buffer (or saved control word / MXCSR), +0x14 thread. Ours keeps
  an FXSAVE copy in pool and gives the caller a fresh FPU state (FNINIT, MXCSR 0x1F80).

## Behaviour
- KeSetEventBoostPriority(event, &thread): no waiter → set the event; otherwise the first waiter gets a new
  quantum and is released with increment 1 (the event stays non-signalled). The woken thread is returned.
- ExReadWriteRefurbInfo(info, 16, write): sector 3 (offset 0x600) of `\Device\Harddisk0\partition0`:
  {'RBRF' 0x52465242, first-boot time, power-cycle count}. A missing signature reads as zeros; other lengths →
  INVALID_PARAMETER.
- **HAL extras:**
  - HalWriteSMCScratchRegister: SMC 0x1B
  - HalEnableSecureTrayEject: if reset-on-eject is enabled, SMC 0x19 ← 0, retried until it succeeds
  - HalIsResetOrShutdownPending: the SMC reset/shutdown state
  - HalInitiateShutdown: start the power-off sequence
- **RTC:** BCD registers 0/2/4/6/7/8/9 plus century 0x7F, read when register A shows no update in progress. Years
  1900–1919 read as 2000–2019. System time is initialised from the RTC; NtSetSystemTime (high part 0…0x20000000)
  sets both.
- **DbgPrint / RtlAssert / RtlRip:** no debugger → the text goes to a kernel log (`XkDebugLog`, ours).
  DbgBreakPoint is a no-op unless KdDebuggerEnabled. DbgPrompt returns an empty response.
- **Strings:** ANSI for STRING, UTF-16 for UNICODE_STRING. Multibyte conversions are Latin-1; characters above 0xFF
  become '?'.
- **RtlSprintf family:** MS C runtime format syntax (flags, width, precision, h/l/ll/I64/w sizes, %d %i %u %x %X
  %o %c %C %s %S %Z %p %n). The *nprintf functions return −1 without a terminator when truncated.
