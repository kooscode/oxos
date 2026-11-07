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
- **RtlNtStatusToDosError(status)** (4627 0x80023653; 5838 0x800238b3, same code and byte-identical tables). Checks
  in this order:
  1. customer bit (0x20000000) set → return the status unchanged;
  2. high word 0x8007 (HRESULT_FROM_WIN32 warning form) → return the low word. Only 0x8007xxxx: 0xC007xxxx is not
     special (it goes through the table and ends at step 5);
  3. top nibble 0xD (error with the facility-NT bit 0x10000000) → clear bits 0x30000000, so it is looked up as 0xC…;
  4. table lookup (below); a hit returns the table value;
  5. a miss: high word 0xC001 → the low word; otherwise ERROR_MR_MID_NOT_FOUND (317, 0x13D).
  - Table: a run table of 8-byte entries {first status (DWORD), run length (WORD), entry size in words (WORD: 1 = WORD
    values, 2 = DWORD values)} (4627 0x80011F88, 5838 0x80011F80), sorted ascending, 101 runs and a sentinel run
    starting at 0xFFFFFFFF; the scan is bounded at 0x67 entries. The values are one array of 16-bit words (4627
    0x80011988, 5838 0x80011980; 764 words), the runs' values stored back to back. The lookup walks runs while the
    status is not below the next run's first status, adding each skipped run's length × entry size to the word
    offset; at the first run whose successor starts above the status it hits if status − first < run length
    (word offset += entry size × that difference) and misses otherwise. DWORD entries exist (runs at 0x80000001,
    0xC000000D, 0xC000002A, 0xC0000039, 0xC000022A); 0x80000001 maps to itself.
  - Key results: STATUS_SUCCESS → 0; STATUS_PENDING 0x103 → ERROR_IO_PENDING 997; STATUS_TIMEOUT 0x102 → 317 (not in
    the table); other small success codes such as 1, 0x101, 0x104 → 317; 0x105 → 234; 0x10C → 1022;
    0x80000005 → 234; 0x80000006 → 18; 0x8007001F → 31; 0xC007001F → 317; 0xC0010005 → 5; 0xD0000034 → 2 (as
    0xC0000034); STATUS_END_OF_FILE → 38; STATUS_INVALID_PARAMETER → 87; STATUS_INSUFFICIENT_RESOURCES → 1450;
    STATUS_INVALID_DEVICE_REQUEST → 1; STATUS_DISK_FULL → 112; STATUS_FILE_CLOSED → 6; STATUS_VOLUME_DISMOUNTED → 21;
    STATUS_FILE_CORRUPT_ERROR → 1392; STATUS_CANCELLED → 995.
