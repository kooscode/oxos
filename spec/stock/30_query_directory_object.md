# Spec 30 — NtQueryDirectoryObject (export 208): entry order, short buffers, end of the directory (xboxkrnl 1.0.5838.1; 4627 compared)

Clean-room behavioural spec, from Ghidra 12.1.4 analysis (REA) of the shipped binaries. No code is reproduced. Addresses are 5838 unless marked.
**Status: implemented** (kernel/src/objects/object_manager.cpp, unit test Ob.DirectoryQueriesAnswerAsTheStockKernel).

## Sources
| Binary | Version | sha256 |
|---|---|---|
| `5838-unpacked/xboxkrnl.exe` (primary) | 1.0.5838.1 | 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde |
| `xboxkrnl-4627.exe` (compared) | 1.0.4627.1 | 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f |

Note: `NtQueryDirectoryObject` is export ordinal **208**. Ordinal 207 is `NtQueryDirectoryFile` (`kernel/exports.tsv`).

## Addresses (citations)
| Item | 5838 | 4627 |
|---|---|---|
| NtQueryDirectoryObject (208) | 0x80020A7D | 0x80020823 |
| ObReferenceObjectByHandle (246), used for the handle | 0x8002077E | 0x80020524 |
| ObDirectoryObjectType (240) | 0x80010D30 | 0x80010D38 |
| KeRaiseIrqlToDpcLevel (129) / KfLowerIrql (161), around the walk | 0x80013D04 / 0x80013D24 | 0x80014348 / 0x80014368 |
| ObfDereferenceObject (the reference is dropped before return) | 0x800206F0 | 0x80020491 |
| Name hash (used by insert and lookup) | 0x80020B6C | 0x80020912 |
| Directory lookup of one name element | 0x80020BA6 | 0x8002094C |
| ObInsertObject (241), adds a named object to a directory | 0x800214AE | 0x80021266 |
| Directory removal of a name entry (internal, the only routine that unlinks) | 0x80020856 | 0x800205FC |
| NtClose (187) | 0x800208F0 | 0x80020696 |
| Handle table: take a handle out (internal, used by NtClose) | 0x8002183E | 0x800215F6 |
| ObMakeTemporaryObject (242), by object pointer | 0x800209C5 | 0x8002076B |
| Make temporary by handle (internal, not exported; used by IoDeleteSymbolicLink) | 0x800209FD | 0x800207A3 |
| IoCreateDevice (65) | 0x80015CB4 | 0x80015B24 |
| IoCreateSymbolicLink (67) | 0x80015FBE | 0x80015E2E |
| IoDeleteDevice (68) | 0x80015FFB | 0x80015E6B |
| IoDeleteSymbolicLink (69) | 0x80016037 | 0x80015EA7 |
| NtOpenSymbolicLinkObject (203), used by IoDeleteSymbolicLink | 0x80021003 | 0x80020DA9 |
| `\??` directory object pointer (variable) | 0x8003BBAC | 0x8003ABAC |
| Root directory object pointer (variable) | 0x8003BBB0 | 0x8003ABB0 |
| Directory used for RootDirectory handle −4 (variable) | 0x8003BBA4 | 0x8003ABA4 |
| Drive-letter table (26 slots of 4 bytes, `A`/`a` .. `Z`/`z`) | 0x8003BC00..0x8003BC67 | 0x8003AC00..0x8003AC67 |
| Symbolic link object type | 0x80010D4C | 0x80010D54 |
| Page fault (trap 0x0E) entry | 0x8001B910 | 0x8001B720 |
| Memory manager fault routine (MmAccessFault equivalent, internal) | 0x8001F7CB | 0x8001F58C |
| Exception raise from a trap (common stub) | 0x8001B54C | 0x8001B35C |
| Exception dispatch (KiDispatchException equivalent, internal) | 0x8001CB3F | 0x8001C987 |
| Frame-based handler search (RtlDispatchException equivalent, internal) | 0x80023DBB | 0x80023B4A |
| KeBugCheckEx (96) | 0x800195B2 | 0x80019442 |
| Current IRQL (byte in the processor control region) | 0x800370B0 | 0x80035C00 |
| NtAllocateVirtualMemory (184) | 0x8001F0EB | 0x8001EEAC |

Searched: the whole body of export 208 in both versions, the reference-by-handle routine, the two IRQL helpers, the name hash, the lookup routine and the
directory insert in ObInsertObject (both versions); the 4627 hash and the 4627 export 208 body. For the removal (§10): every caller of the name hash (insert,
lookup, removal: no other code computes a bucket), every caller of the removal routine (NtClose and ObMakeTemporaryObject only), ObfDereferenceObject,
the make-temporary-by-handle routine, IoCreateDevice, IoCreateSymbolicLink, IoDeleteDevice and IoDeleteSymbolicLink, in both versions. For faulting
pointers (§11): every instruction of export 208 (both versions) for an exception registration or a segment-FS access; the page fault entry (found as the
only read of CR2 in each image), the memory manager's fault routine, the trap exception stub, the exception dispatch routine, the bug check calls in
them; NtAllocateVirtualMemory and its free-range search (0x80020614, 5838) for the lowest address a title can allocate.

## 1. Data the function reads
- **Directory:** 11 hash buckets (bucket 0..10), each the head of a singly linked chain of name entries (the directory object's body starts with the 11
  chain heads).
- **Name entry** (the object's name information, placed just before its object header): next-entry link at +0, owning directory at +4, the name as an
  ANSI string (Length at +8, MaximumLength at +10, Buffer at +12); the object header follows at +0x10, and the header's object-type pointer is at entry +0x18.
- **Object type:** the 4-byte pool tag at offset +0x18 of the object type structure. The tags are ASCII bytes in memory order:

| Object type | Tag bytes | ULONG value |
|---|---|---|
| Directory | `Dire` | 0x65726944 |
| Symbolic link | `Symb` | 0x626D7953 |
| Device | `Devi` | 0x69766544 |
| File | `File` | 0x656C6946 |
| Event | `Evev` | 0x76657645 |

(Same tags in 4627.)

## 2. Entry order
1. Entries are numbered by one flat index that runs over bucket 0's chain from its head, then bucket 1's chain, ... up to bucket 10. `*Context` is that
   index: the number of entries before the one returned next.
2. Bucket of a name: start with h = 0; for each byte of the name in order, skip it if it is 0x80 or above, otherwise h = 3·h + (h >> 1) + (byte | 0x20),
   in 32-bit unsigned arithmetic; the bucket is h mod 11. (`| 0x20` makes ASCII letters case-insensitive.) The same in 4627.
3. Insertion (ObInsertObject): a new name entry goes in **at the head** of its bucket's chain, so within a bucket the newest entry is listed first.
4. Lookups do not reorder the chains (the lookup at 0x80020BA6 walks the chain and returns, nothing is moved to the front).
5. `\??` drive letters (`C:` etc., 2-character names whose second character is `:`) are inserted into the hash buckets like any other name; the kernel also
   records them in a separate drive-letter table (a fast lookup path), which NtQueryDirectoryObject does not read. They are therefore listed.
6. RestartScan TRUE: the walk starts at index 0 and `*Context` is **not read**. RestartScan FALSE: the walk starts at index `*Context`.

## 3. Steps
1. Length < 12 (the size of the fixed structure): return STATUS_INVALID_PARAMETER (0xC000000D). The handle is not checked; nothing is written
   (no Buffer, no Context, no ReturnLength).
2. Reference DirectoryHandle as a directory object. A failure returns its status and writes nothing (see §5).
3. Preset: status = STATUS_NO_MORE_ENTRIES (0x8000001A), length to report = 0. Start index = 0 if RestartScan, else `*Context`.
4. Raise IRQL to DISPATCH_LEVEL (the object manager's lock).
5. Walk the entries in the order of §2, counting, until the entry whose index equals the start index. If there is one:
   - needed = 12 + Name.Length of the entry;
   - if Length < needed: status = STATUS_BUFFER_TOO_SMALL (0xC0000023), and only Length − 12 bytes of the name will be copied; else status = STATUS_SUCCESS
     and the whole name is copied;
   - the structure is written (always, also when too small), see §4, then the name bytes after it;
   - `*Context` = start index + 1 (**also when the buffer was too small**);
   - length to report = needed.
6. Lower IRQL back; drop the reference to the directory.
7. If ReturnLength is not NULL: `*ReturnLength` = the length to report (needed, or 0 when no entry was at that index).
8. Return the status.

## 4. What is written into Buffer (an entry was found)
| Offset | Field | Value |
|---|---|---|
| +0 | Name.Length (USHORT) | the entry's Name.Length (the full length, also when too small) |
| +2 | Name.MaximumLength (USHORT) | the same value as Name.Length (not the stored MaximumLength) |
| +4 | Name.Buffer (pointer) | Buffer + 12 |
| +8 | Type (ULONG) | the object type's pool tag (§1 table), e.g. 0x626D7953 `Symb` for a `\??` drive letter |
| +12 | name bytes | Name.Length bytes when it fits; Length − 12 bytes (a cut name) when too small |

- No terminating zero is written; bytes of Buffer after the copied name are left as they were.
- The structure is filled field by field before the name is copied; nothing else in Buffer is touched.

## 5. Cases
| Condition | Status | `*Context` | `*ReturnLength` | Buffer |
|---|---|---|---|---|
| Length < 12 | 0xC000000D STATUS_INVALID_PARAMETER | not read, not written | not written | not written |
| Handle not valid (not in the handle table) | 0xC0000008 STATUS_INVALID_HANDLE | not read, not written | not written | not written |
| Handle valid but not a directory; or NtCurrentThread (−2) | 0xC0000024 STATUS_OBJECT_TYPE_MISMATCH | not read, not written | not written | not written |
| Entry at the index, fits (Length ≥ 12 + Name.Length) | 0x00000000 STATUS_SUCCESS | index + 1 | 12 + Name.Length | structure + whole name |
| Entry at the index, too small (12 ≤ Length < 12 + Name.Length) | 0xC0000023 STATUS_BUFFER_TOO_SMALL | **index + 1 (advances)** | 12 + Name.Length (needed) | structure with the full Name.Length + the first Length − 12 name bytes |
| No entry at the index (past the end, or empty directory) | 0x8000001A STATUS_NO_MORE_ENTRIES | not written | 0 | not written |

Here "index" is 0 with RestartScan TRUE, else the value read from `*Context`.

## 6. Other details
- **NULL ReturnLength:** allowed; it is tested before the write.
- **Context:** never tested for NULL. It is read only when RestartScan is FALSE, but written whenever an entry is returned (success or too small), so it
  must be valid then even with RestartScan TRUE.
- **Locking and IRQL of each access to the caller's memory** (5838 / 4627 addresses of the access):

| Access | When | IRQL | 5838 | 4627 |
|---|---|---|---|---|
| Read `*Context` (RestartScan FALSE only) | before the IRQL is raised, with the directory referenced | caller's | 0x80020AC4 | 0x8002086A |
| Write Buffer +8 (Type), +4, +0, +2, in this order, then the name bytes | entry found | DISPATCH_LEVEL | 0x80020B17..0x80020B41 | 0x800208BD..0x800208E7 |
| Write `*Context` | entry found, right after the name copy | DISPATCH_LEVEL | 0x80020B47 | 0x800208ED |
| Write `*ReturnLength` (if not NULL) | after the IRQL is lowered and the directory reference dropped | caller's | 0x80020B60 | 0x80020906 |

- No exception handler and no probing: §11.
- **Order of checks:** the Length check comes before the handle check (a bad handle with Length < 12 returns STATUS_INVALID_PARAMETER).

## 7. 4627 compared with 5838
The behaviour of export 208 is the same in both versions: the same steps, constants (12, 11 buckets), statuses, writes and order. The name hash is the same.
Only addresses differ (table above). ObInsertObject's insertion at the head of the chain is the same in 4627 (0x80021266: the new entry's link is set to the
old chain head, then the bucket head is set to the new entry), as are the lookup, the removal (§10) and the fault path (§11).

## 8. Consequence for callers (why it matters)
A caller that gets STATUS_BUFFER_TOO_SMALL and calls again with the same small buffer and RestartScan FALSE moves on to the next entry each time and ends with
STATUS_NO_MORE_ENTRIES; it never sees the same entry twice. A caller that wants the entry that did not fit must call again with a bigger buffer and
`*Context` set back to the old index (or `ReturnLength` bytes).

## 9. Unknown
- (Answered by observation, 2026-11-26: a test title under the stock 4627 BIOS in xemu, MCPX 1.0 boot ROM.) **Page 0 is not present while a title runs**:
  MmIsAddressValid gives FALSE and MmQueryAddressProtect 0 for 0x0, 0x1000 and 0xF000 (and for 0x7FFE0000); 0x10000 (the XBE header) gives TRUE and
  PAGE_READWRITE 4. A plain read of address 0 at PASSIVE_LEVEL stopped the title (the screen never changed again), as §11.5 predicts. The 5713 BIOS did not
  boot in xemu, so 5838-era kernels were not observed.
- What the flag at 0x8003BB20 (4627: 0x8003AB20) stands for. It decides whether a page fault taken with interrupts disabled is handled normally or ends
  in bug check 0x0A with IRQL 0xFF (§11.2 step 2); not needed for export 208, whose faults happen with interrupts enabled.
- What the counter at 0x8003B8A0 (4627: 0x8003A8A0), incremented just before the 0x0A bug check, is used for.

## 10. Removal of a name from a directory
### 10.1 Object header fields used (offsets from the object body)
| Offset | Field |
|---|---|
| −0x20 | name entry (§1): +0 next link, +4 owning directory, +8 the name |
| −0x10 | pointer (reference) count |
| −0x0C | handle count |
| −0x08 | object type; the type's procedures: +4 free, +8 close, +0x0C delete |
| −0x04 | flags: 0x1 has a name entry, 0x2 permanent, 0x4 is in a directory |

### 10.2 Insertion (ObInsertObject, for reference)
- Named insert: flag 0x4 is set, the entry's directory = the directory, the entry goes at the **head** of its bucket (§2.3), and **one reference is added
  to the directory and one to the object** (the directory's hold on its entry). Flag 0x2 (permanent) is set when the attributes carry OBJ_PERMANENT
  (0x10). A handle is always created (out of handles: 0xC000009A), so a named non-permanent object starts with a handle count of at least 1.
- `\??` drive letters: when the directory is `\??`, the name is 2 characters and the second is `:`, and the first is `A`..`Z` or `a`..`z`, the letter's slot
  in the drive-letter table (one slot per letter, upper and lower case share it) is set: to the link's **target object** (the symbolic link body's first
  field) when the object is a symbolic link, else to the object itself.
- IoCreateDevice inserts a named device with OBJ_PERMANENT (attributes 0x10, plus 0x20 when exclusive) and closes the handle at once: the device stays in
  its directory with handle count 0. IoCreateSymbolicLink creates the link with attributes 0x50 (OBJ_PERMANENT | OBJ_CASE_INSENSITIVE) and closes the
  handle.

### 10.3 When a name is removed
Only one routine unlinks a name entry (0x80020856; 4627 0x800205FC), and it has exactly two callers:

| Path | Condition for the removal | Effect when the condition is false |
|---|---|---|
| NtClose (187) | after the handle count is decremented: handle count == 0 **and** flag 0x4 (in a directory) **and not** flag 0x2 (permanent) | the IRQL is lowered, nothing is removed |
| ObMakeTemporaryObject (242) | clears flag 0x2 first; then handle count == 0 and flag 0x4 | the name stays; it is removed at the last NtClose |

- **The trigger is the last handle close, not the last reference.** Pointer references held elsewhere (ObReferenceObjectByHandle/ByName, the I/O manager)
  do not keep the name in the directory; they only keep the object's memory alive after the removal.
- ObfDereferenceObject (250) never touches a directory: when the reference count reaches 0 it calls the type's delete procedure (if any), then the type's
  free procedure with the start of the allocation (body − 0x20 when flag 0x1, else body − 0x10). A named object cannot reach 0 references while still in a
  directory, because the directory's own reference (§10.2) is dropped only by the removal.
- NtClose order: raise to DISPATCH_LEVEL, take the handle out of the table (invalid: lower, return 0xC0000008 STATUS_INVALID_HANDLE); decrement the
  handle count; if the type has a close procedure, lower the IRQL, call it with the object and the handle count before the decrement, raise again; then the
  removal check above; then drop the handle's reference (ObfDereferenceObject) at the caller's IRQL; return 0.
- **IoDeleteSymbolicLink (69):** opens the link by name (NtOpenSymbolicLinkObject, attributes 0x40 OBJ_CASE_INSENSITIVE, no root directory), makes it
  temporary through the handle (internal routine: reference by handle with no type check, ObMakeTemporaryObject, dereference), then NtClose on that
  handle; the close is the last handle (the creator's handle was closed by IoCreateSymbolicLink), so the name is removed there. Returns the open's or the
  make-temporary's status; on success 0.
- **IoDeleteDevice (68):** if the device has a name (device flags at device +0x14, bit 0x8), ObMakeTemporaryObject: with no open handle the name is removed
  at once, else at the last NtClose. Then, at DISPATCH_LEVEL, the device's delete-pending byte (+0x1F) is set to 1, and if the device's reference count
  (+0x04) is 0 the device is unloaded/freed (0x80017DF2; 4627 0x80017C73), otherwise the IRQL is just lowered.

### 10.4 What the removal does
Entered at DISPATCH_LEVEL (the caller raised it and passes the old IRQL):
1. If the owning directory is `\??`, the name is 2 characters with `:` second, and the first is `A`..`Z` or `a`..`z`: the letter's drive-letter slot is set
   to 0 (it is cleared without checking what it pointed to).
2. The bucket is computed with the name hash (§2.2). The chain is walked from the bucket head to the entry; the entry is **unlinked**: the predecessor's
   link (or the bucket head, when the entry is first) is set to the entry's own next link. **Nothing else moves**: the other entries keep their relative
   order, and their flat indices (§2.1) after the removed one drop by 1.
3. The entry's next link and its owning-directory field are set to 0. Flag 0x4 is not cleared.
4. The IRQL is lowered to the passed value.
5. The directory's reference and the object's directory reference are dropped (ObfDereferenceObject on the directory, then on the object), at the
   caller's IRQL; this may free the object.

### 10.5 Moves
No routine moves an entry: insertion only puts a new entry at a bucket head, the lookup (0x80020BA6) only reads, the removal only unlinks. Consequence for
enumeration: removing an entry with an index below a caller's saved `*Context` makes the next call skip one entry; adding one in a bucket before it (or at
the head of the same bucket, ahead of the saved position) makes the next call return an entry already seen.

### 10.6 Lookup through the drive-letter table
The lookup (0x80020BA6) in `\??`, when asked to follow links, for a 2-character name with `:` second and a letter first, returns the drive-letter slot's
object directly when the slot is non-zero (the link's target, §10.2); otherwise (slot 0, or not a letter) it hashes and walks the bucket, comparing names
case-insensitively, and returns the found object (a symbolic link's target when following links). NtQueryDirectoryObject never uses this path (§2.5).

## 11. Faulting Buffer, Context or ReturnLength pointers
### 11.1 In export 208
- **No exception handler:** no exception registration record is built and nothing reads or writes through segment FS anywhere in export 208 (5838 and
  4627); the only calls are the reference-by-handle routine, the two IRQL helpers and ObfDereferenceObject. No pointer is probed.
- A fault therefore goes straight to the page fault handler with the IRQL of the access (§6 table).

### 11.2 The page fault handler (trap 0x0E, 5838 0x8001B910; 4627 0x8001B720)
1. Builds the trap frame, reads the faulting address from CR2, enables interrupts.
2. If interrupts were disabled at the fault (EFLAGS IF clear in the trap frame) and the flag at 0x8003BB20 is 0: bug check 0x0A with IRQL 0xFF (step 5).
3. Calls the memory manager's fault routine with: write flag (error code bit 1), faulting address, trap frame.
4. Result ≥ 0: return from the trap (the access is retried). Faulting EIP = 0x80012C06 (4627 0x80012C76; the interlocked singly-linked-list pop): EIP is
   set back to 0x80012BF4 (4627 0x80012C64) to retry the pop, and the trap returns.
5. Result 0xD0000006: bug check **0x0A** (IRQL_NOT_LESS_OR_EQUAL), parameters: (1) faulting address, (2) the current IRQL, (3) write flag (0 read, 1 write),
   (4) faulting EIP. (A counter at 0x8003B8A0 is incremented first.)
6. Result 0xC0000005, 0x80000001 or 0xC00000FD: an exception with that code is raised at the faulting EIP with 2 parameters (write flag, faulting
   address). Any other result: exception 0xC0000006 with 3 parameters (write flag, address, result); the fault routine returns no such value.

### 11.3 The memory manager's fault routine (5838 0x8001F7CB; 4627 0x8001F58C)
There is no demand paging: the routine never makes a page present.

| Condition (in this order) | Result |
|---|---|
| current IRQL ≥ DISPATCH_LEVEL (2) | 0xD0000006 (→ bug check 0x0A) |
| address < 0x10000 or ≥ 0x7FFE0000 | 0xC0000005 STATUS_ACCESS_VIOLATION, nothing looked at |
| else, under the address-space lock (0x80036CC4; 4627 0x800360D4) and at DISPATCH_LEVEL: page-directory entry present, PTE non-zero, and the PTE's protection is a guard page | the guard bit is removed from the PTE, the TLB entry flushed: 0x80000001 STATUS_GUARD_PAGE_VIOLATION |
| anything else (not present, PTE zero, present but read-only, ...) | 0xC0000005 |

- **Present-but-read-only makes no difference:** the write flag is not used to decide; at DISPATCH_LEVEL it is bug check 0x0A, below it an access
  violation, the same as a not-present page.

### 11.4 Exception dispatch and its end (5838 0x8001CB3F; 4627 0x8001C987)
1. First chance: the kernel debugger hook (variable 0x8003B8DC; 4627 0x8003A8DC) if one is set; then the frame-based handlers on the current thread's
   exception registration chain (0x80023DBB). There is no user mode on the console: a title's own handlers are on this chain and are searched too.
2. Second chance: the debugger hook again, if set.
3. Not handled: bug check **0x1E** (KMODE_EXCEPTION_NOT_HANDLED), parameters: (1) exception code (0xC0000005), (2) faulting EIP, (3) write flag,
   (4) faulting address.

### 11.5 Outcomes for export 208
| Bad pointer | IRQL at the access | Outcome |
|---|---|---|
| Buffer not writable (an entry is found, Length ≥ 12) | DISPATCH_LEVEL | bug check 0x0A (address, 2, 1, EIP in 0x80020B17..0x80020B41; 4627 0x800208BD..0x800208E7) — no handler can catch it |
| Context not writable (an entry is found), including **NULL Context with RestartScan TRUE** | DISPATCH_LEVEL | bug check 0x0A (address, 2, 1, 0x80020B47; 4627 0x800208ED); for NULL: (0, 2, 1, EIP), if page 0 is not present (§9) |
| Context not readable, RestartScan FALSE | caller's (PASSIVE for a title) | exception 0xC0000005 at 0x80020AC4 (4627 0x8002086A); a handler of the caller can catch it, else bug check 0x1E (0xC0000005, EIP, 0, address). When caught, the reference to the directory taken in step 2 is never dropped (leaked) |
| ReturnLength not writable | caller's | exception 0xC0000005 at 0x80020B60 (4627 0x80020906), after the entry was copied, `*Context` advanced and the reference dropped; catchable, else 0x1E |
| Buffer or Context fault with nothing found (STATUS_NO_MORE_ENTRIES) | — | no fault: neither is written |

- **Page 0:** NtAllocateVirtualMemory rounds a requested base down to 64 KiB, so a base below 0x10000 becomes 0, which means "anywhere" (a free range is
  searched); a commit-only request below 0x10000 needs an existing reservation there, which none of the searched paths creates. The fault routine treats
  every address below 0x10000 as an access violation without looking at the page tables. Whether the page tables leave page 0 present is not established
  (§9).
