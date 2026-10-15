# Spec 10 — Object manager, executive objects, Ps threads (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile `ev_83e69db5…` (ObCreateObject, ObInsertObject,
ObReferenceObjectByHandle, ObfDereferenceObject, NtClose), `ev_d2f9059c…` (handle create/lookup/remove, name hash,
directory lookup, ObReferenceObjectByName), `ev_1b1bec38…` (handle-table growth, ObpReferenceObjectByName,
NtCreateEvent, PsCreateSystemThread), plus `ev_17a6d736…` (PsCreateSystemThreadEx) and `ev_bf895671…`
(PsTerminateSystemThread). OBJECT_TYPE / OBJECT_ATTRIBUTES layouts follow nxdk. **Status: implemented (milestone 3a).**

## 1. Objects
- **Names are ANSI** counted strings (`OBJECT_STRING` = `STRING`). OBJECT_ATTRIBUTES = {RootDirectory, ObjectName,
  Attributes}. Attributes include OBJ_PERMANENT 0x10, OBJ_CASE_INSENSITIVE 0x40, OBJ_OPENIF 0x80.
- `OBJECT_TYPE` (0x1C): Allocate(size, tag), Free(p), Close(obj, handleCount), Delete(obj), Parse(...),
  **DefaultObject** (≥ 0: offset of the waitable dispatcher header inside the body; < 0: absolute object to wait on),
  PoolTag. Exported type objects: ExEvent/ExMutant/ExSemaphore/ExTimer (DefaultObject 0), IoCompletion, IoDevice,
  IoFile, ObDirectory, ObSymbolicLink, PsThread. Titles pass these to `ObReferenceObjectByHandle`.
- **OBJECT_HEADER** (0x10, just before the body): PointerCount, HandleCount, Type, Flags (1 named, 2 permanent,
  4 attached to a directory).
- **Named objects** have a 0x10 name header before that: {ChainLink, Directory, Name}. The final path element is stored after
  the (4-aligned) body. One allocation: name header + header + body + name.
- `ObCreateObject(type, attrs, bodySize, &obj)`: PointerCount 1. A name without a final element →
  STATUS_OBJECT_NAME_INVALID.
- `ObfDereferenceObject` (fastcall): at 0 → Delete, then Free (from the name header if named).

## 2. Handles (`ObpObjectHandleTable`, ordinal 245)
`{HandleCount, FirstFreeTableEntry, NextHandleNeedingPool, RootTable, BuiltinRootTable[8]}`.
- Entry for handle h: `RootTable[h >> 8][(h & 0xFC) / 4]`.
- Subtables of 64 entries (tag 'ObHt') are added on demand; the root array grows by 8 slots (tag 'ObHr').
- Free entries hold `next | 1` (end = −1). **Handles start at 4, step 4; handle 0 is never used**.
- Freed handles are reused **LIFO**.
- `NtCurrentThread()` = (HANDLE)−2 (valid for PsThreadObjectType or untyped references).
  Invalid handle → STATUS_INVALID_HANDLE (0xC0000008). Wrong type → STATUS_OBJECT_TYPE_MISMATCH (0xC0000024).

## 3. Namespace
- Root `\`, plus permanent `\??`, `\Device`, `\Win32NamedObjects`. Directories have **11 hash buckets**;
  hash = Σ over ASCII chars of `h = h*3 + (c | 0x20) + (h >> 1)`, mod 11. Comparison is case-insensitive.
- RootDirectory: NULL (the name must start with `\`) / (HANDLE)−3 = `\??` / (HANDLE)−4 = `\Win32NamedObjects` /
  a directory handle. A relative name with a NULL root, or an absolute name with a root → STATUS_OBJECT_NAME_INVALID (0xC0000033).
- Lookup:
  - follows symbolic links to their **target object**, except for a final element when the requested type is
    SymbolicLink
  - `\??\X:` has a 26-entry drive-letter cache
  - reaching a non-directory object with a ParseProcedure hands it the remaining path (including the leading
    `\`) — the route into the I/O manager
  - missing final element → STATUS_OBJECT_NAME_NOT_FOUND (0xC0000034); missing or non-directory intermediate →
    STATUS_OBJECT_PATH_NOT_FOUND (0xC000003A)
- `ObInsertObject(obj, attrs, bias, &handle)` (at DISPATCH):
  - unnamed objects get a handle (PointerCount += bias + 1)
  - named objects: walk to the parent directory and insert. The directory and the object reference each other.
    The name already existing → OBJ_OPENIF with the same type opens the existing object
    (**STATUS_OBJECT_NAME_EXISTS 0x40000000**); otherwise STATUS_OBJECT_NAME_COLLISION (0xC0000035)
  - OBJ_PERMANENT sets flag 2
  - always drops the creator's reference at the end (a failed insert destroys the new object)
- `NtClose`: remove the handle, HandleCount−−, call Close(obj, previousCount) at PASSIVE. If it was the last handle of a
  named, non-permanent object, unlink it from the directory. Then dereference.
- `ObMakeTemporaryObject` clears permanent and unlinks if there are no handles. `NtDuplicateObject(src, &dst, opts)`:
  option 1 closes the source.
- Symbolic links: {LinkTargetObject (referenced at creation), LinkTarget string}. `NtQuerySymbolicLinkObject`
  returns the string (STATUS_BUFFER_TOO_SMALL if it doesn't fit).
- `NtQueryDirectoryObject` returns OBJECT_DIRECTORY_INFORMATION {Name, Type}, one entry per call; the context is a
  running index; STATUS_NO_MORE_ENTRIES at the end.

## 4. Executive objects (Nt*)
- Event: type must be Notification or Synchronization (else STATUS_INVALID_PARAMETER). Create = ObCreateObject(16) +
  KeInitializeEvent + ObInsertObject. Set/Pulse use increment 1. Clear zeroes the state. Query → {Type, State}.
- Mutant: NtReleaseMutant by a thread that doesn't own it → STATUS_MUTANT_NOT_OWNED. Deleting an owned mutant abandons it.
- Semaphore: requires 0 ≤ initial ≤ max and max > 0. Releasing past the limit → STATUS_SEMAPHORE_LIMIT_EXCEEDED.
- Timer: KTIMER + DPC + APC. NtSetTimerEx with an APC routine queues `routine(context, low, high)` (system time)
  to the thread that set the timer, in the given mode.
- Waits: `NtWaitForSingleObject(h, alertable, timeout)` = Ex(h, **UserMode**, …); WaitReason UserRequest;
  the wait target comes from DefaultObject. `NtWaitForMultipleObjectsEx` accepts up to 64 handles.
  `NtSignalAndWaitForSingleObjectEx` signals with Wait=TRUE so the signal and the wait are atomic.

## 5. Ps threads
- `PsCreateSystemThreadEx` (spec 07) with ETHREAD 0x140 + extension. **UniqueThread = a handle to the thread**
  (the thread ID). References are held by UniqueThread, the caller's handle, and the running thread (released by
  the reaper after termination).
- `PsCreateSystemThread(h, id, start, ctx, dbg)` = Ex(…, stack 0x3000, no TLS, system routine = call
  start(ctx), then PsTerminateSystemThread(0)).
- `PsTerminateSystemThread(status)`:
  - set HasTerminated, drop to PASSIVE
  - call the notify routines (≤ 8, via PsSetCreateThreadNotifyRoutine) with Create = FALSE
  - priority below 16 → raise it to 16
  - record ExitStatus/ExitTime, close UniqueThread, terminate
- `NtQueueApcThread(h, routine, a1, a2, a3)`: a user-mode APC whose kernel routine frees it and whose normal
  routine is `routine(a1, a2, a3)`.
- `NtSuspendThread`/`NtResumeThread` return the previous count. `PsQueryStatistics` → {Length = 12, ThreadCount,
  HandleCount}.

## Implementation notes
- A new thread with kernel APCs queued (e.g. created suspended) gets the APC interrupt when it is switched in. The
  reference does this in KiSwapContext.
