# Spec 04b — FATX file system (stock behaviour)

On-disk format (publicly documented; confirmed against the test image with `tests/fatx.py`) plus the
driver behaviour taken from the reference FATX driver. Evidence: batch_decompile `ev_484d7415…` (FatxMountVolume,
RAW mount), `ev_0590f4f8…` (volume device creation, boot-sector processing), `ev_c176be65…` (FatxFsdCreate), and the
FATX driver object at 0x80012470. **Status: implemented (milestone 3c-2).**

## 1. On-disk format
- **Superblock** (partition offset 0, 4 KB reserved): 'FATX', serial number, sectors per cluster, root directory
  cluster (1). Layout and cluster arithmetic: spec/stock/04 §6. Data cluster N (N ≥ 1) sits at data start + (N−1) × cluster size.
- **FAT:**
  - entries are 16-bit (FATX16, fewer than 0xFFF0 clusters) or 32-bit
  - 0 = free; ≥ 0xFFF8 (sign-extended for 16-bit entries) = end of chain; 0xFFF7 = bad
- **Directory entry** (64 bytes):
  - FileNameLength — 0x00 or 0xFF ends the directory; 0xE5 marks a deleted entry
  - Attributes (READONLY 1, HIDDEN 2, SYSTEM 4, DIRECTORY 0x10, ARCHIVE 0x20)
  - Name[42], with unused bytes 0xFF
  - FirstCluster, FileSize (0 for directories)
  - Creation/LastWrite/LastAccess timestamps
- **Directories** are cluster chains of entries, at most 4096 per directory. A new directory gets one cluster filled
  with 0xFF.
- **Timestamps:** high word = date (day, month << 5, (year − 2000) << 9), low word = time (seconds / 2,
  minute << 5, hour << 11).
- **Names:**
  - 1–42 characters from 0x20–0x7E
  - none of `"*/:<>?\|`; "." and ".." are invalid
  - case-insensitive compare, case preserved
  - whole path ≤ 250 characters

## 2. Driver behaviour
- Volume device: FILE_DEVICE_DISK_FILE_SYSTEM (8), extension = volume control block. StackSize = target + 1;
  alignment = max(own, target); sector size from the drive geometry. It holds a reference on the partition device.
  Free clusters are counted at mount.
- **Create:**
  - dismounted volume → STATUS_VOLUME_DISMOUNTED
  - FILE_OPEN_BY_FILE_ID → STATUS_NOT_IMPLEMENTED
  - allocation size above 4 GB → STATUS_INVALID_PARAMETER
- **Opening the volume itself** (empty name, no related file):
  - only FILE_OPEN / FILE_OPEN_IF are allowed (otherwise ACCESS_DENIED)
  - FILE_DIRECTORY_FILE → NOT_A_DIRECTORY; DELETE_ON_CLOSE → CANNOT_DELETE
  - shared volume share-access; Information = FILE_OPENED
- **Path walk:**
  - an invalid element → OBJECT_NAME_INVALID
  - a doubled separator (`dir\\name`) is accepted **once** (reference create 0x8002a37a, name splitter 0x80021709,
    which drops one leading `\`): the walk through already-open directory FCBs stops at the first `\\` or at the
    first directory not open; the slower walk that continues splits the rest again (dropping the extra `\`) and
    from then on rejects a rest that starts with `\` (OBJECT_NAME_INVALID). Brute Force opens
    `z:\splash_screen\\textures-splash_screen.tex`; rejecting it left its menu textures unloaded (garbage).
  - a missing or non-directory intermediate → OBJECT_PATH_NOT_FOUND
  - a missing final element:
    - DELETE_PENDING if the parent is being deleted
    - NAME_NOT_FOUND for OPEN / OVERWRITE
    - otherwise create the entry (a directory if FILE_DIRECTORY_FILE) → FILE_CREATED
  - an existing final element:
    - file + FILE_CREATE → NAME_COLLISION
    - file + trailing `\` or FILE_DIRECTORY_FILE → NOT_A_DIRECTORY
    - directory with anything but OPEN/OPEN_IF → NAME_COLLISION
    - directory + NON_DIRECTORY_FILE → FILE_IS_A_DIRECTORY
- **Access and sharing on open:**
  - access check (4627 0x8002a047; the create path passes whether the file is new): ACCESS_DENIED for the attribute
    bits 0xC8 or access bits 0xFEE0FE00 (after the generic mapping); on a read-only file, write access (0xFEE0FE46:
    write data, append, write EA and the bits above) → ACCESS_DENIED only when the file already exists (a new file
    may be created read-only with write access: UnleashX extracts its read-only skin files to X: this way), and
    DELETE_ON_CLOSE → CANNOT_DELETE whether new or not
  - share access is checked per file; SUPERSEDE / OVERWRITE(_IF) truncate → FILE_OVERWRITTEN (or FILE_SUPERSEDED)
  - DELETE_ON_CLOSE marks a file delete-pending
- **Read / write:**
  - a read at or past EOF → END_OF_FILE; reads stop at EOF
  - writes extend the cluster chain; FILE_WRITE_TO_END_OF_FILE appends; above 4 GB → DISK_FULL
  - unbuffered handles need sector-aligned offsets and lengths
  - volume handles pass reads and writes to the partition
- **Information:**
  - Query: Basic, Standard, Internal, Ea, Position, Name, NetworkOpen, All
  - Set: Basic, Disposition (directories must be empty → DIRECTORY_NOT_EMPTY; read-only → CANNOT_DELETE),
    Position, EndOfFile, Allocation, Rename (ReplaceIfExists; no directory targets)
- **Directory query:**
  - FileDirectoryInformation and FileNamesInformation, ANSI names
  - mask with `*` and `?` (default `*`), RestartScan
  - first empty result → NO_SUCH_FILE; afterwards → NO_MORE_FILES
- **Volume information:** Volume (serial number), Size / FullSize (clusters, sectors per cluster, bytes per sector),
  Device, Attribute ("FATX", 42-character components).
- **Delete:** at the last cleanup of a delete-pending file, the entry is marked 0xE5 and the chain is freed.

## Implementation notes
- Our driver uses a 32 × 4 KB write-through block cache for the FAT and directories. Sector-aligned data runs go
  straight to the disk. One lock per volume.
- Scatter/gather (`NtReadFileScatter` / `NtWriteFileGather`): each FILE_SEGMENT_ELEMENT is one page of the transfer;
  offset and length must be sector multiples. Ours since milestone 6e (volume handles still reject segment arrays).
  The Fsc exports, dismount and RAW are covered by specs 16 and 04c.

## Read/write argument checks (reference read/write dispatch 0x80029a01)
- Read: length 0 → success with 0 bytes; offset at or past the end of the file → STATUS_END_OF_FILE; the length is
  clipped to the end of the file. Only then do non-cached (IRP_NOCACHE) and scatter/gather reads require the offset
  and the requested length to be sector multiples (else STATUS_INVALID_PARAMETER). The stock dashboard reads a
  non-cached file in 64 KB pieces until it gets STATUS_END_OF_FILE, at an unaligned position; rejecting that read
  makes it show error 21.
- Write: the reference grows the cluster chain (allocation) first and checks alignment afterwards; the file size
  itself only changes after the data is written. Ours rejects a misaligned non-cached write up front, without
  extending the file (deviation).

## Asynchronous completion (pending)
Evidence: 4627 read/write dispatch 0x80029a01; asynchronous transfer 0x800298a0, its run issuer 0x8002982f and
completion routine 0x800297a6; synchronous non-cached transfer 0x80029398 (completion routine 0x800292f5) and its
tail 0x8002930a; cached transfers 0x80029516 / 0x800296d4 (choice 0x8001cf5b); volume completion routine 0x8002929f.
NtReadFile 0x80017467, NtReadFileScatter 0x80017633, IopSynchronousServiceTail 0x800181bb. 5838: dispatch
0x80029ea2, asynchronous transfer 0x80029d33, run issuer 0x80029cc2, completion routine 0x80029c39, volume
completion routine 0x80029748: same logic.

**Rule.** The dispatch returns STATUS_PENDING in exactly one case: a file (not the volume) whose file object does not
have FO_SYNCHRONOUS_IO (flag 0x01), an IRP with IRP_NOCACHE, a length > 0, an offset and requested length that are
sector multiples, for a read an offset inside the file, for a write an end at or below the current file size, and the
transfer context allocated and every cluster run mapped. The decision is made before the disk is touched: once the
first run is sent, STATUS_PENDING is returned whatever the disk does, also when the disk finishes at once or fails.
Every other request finishes inside the dispatch, which sets IoStatus.Status and completes the IRP itself
(IofCompleteRequest, no boost) and returns that status. The volume handle is the exception: it returns what the disk
driver returned.

Where the flags come from:
- IRP_NOCACHE (Irp->Flags 0x01) is set by NtReadFile/NtWriteFile when the file object has FO_NO_INTERMEDIATE_BUFFERING
  (0x04), i.e. FILE_FLAG_NO_BUFFERING (4627 0x80017467: Flags |= 0x91 instead of 0x90).
- NtReadFileScatter/NtWriteFileGather require FO_NO_INTERMEDIATE_BUFFERING and always set 0x491 (NOCACHE + scatter/
  gather). So a segment array always takes the non-cached path; the dispatch has no separate scatter/gather branch.
- IRP_SYNCHRONOUS_API, IRP_DEFER_IO_COMPLETION, FO_ALERTABLE_IO and the file's data alignment play no part. The
  file size only matters for writes (see the table).

| Path | Condition | Returned status | How completed |
|---|---|---|---|
| Directory FCB | FCB flag 0x02 | STATUS_INVALID_DEVICE_REQUEST | synchronously, Information untouched |
| Zero length | Length 0 (checked before anything else but the directory test) | STATUS_SUCCESS | synchronously, Information 0 |
| Volume dismounted | volume flag | STATUS_VOLUME_DISMOUNTED | synchronously |
| Cleaned-up file | file object flag 0x10 | STATUS_FILE_CLOSED | synchronously |
| Read at/after end | offset high part ≠ 0 or offset ≥ file size | STATUS_END_OF_FILE | synchronously, Information untouched (0) |
| Non-cached, misaligned | offset or the requested (unclipped) length not a sector multiple | STATUS_INVALID_PARAMETER | synchronously |
| Write overflow | offset + length wraps or offset high part ≠ 0 | STATUS_DISK_FULL | synchronously |
| Write, corrupt sizes | file size > allocation size | STATUS_FILE_CORRUPT_ERROR | synchronously |
| Cached (no IRP_NOCACHE), read or write | any file object | the transfer's status | synchronously: copied through the Fsc cache, or the middle sector-aligned part sent straight to the disk with the synchronous helper and the unaligned head/tail through the cache. Never pends; nothing is posted to a worker thread. |
| Non-cached, FO_SYNCHRONOUS_IO set | aligned | the transfer's status | synchronously: one disk request per contiguous run, each waited for (below) |
| Non-cached write that extends the file | end > file size (also FILE_WRITE_TO_END_OF_FILE, offset −1) | the transfer's status | synchronously, even on an asynchronous file object; then the file size and the directory entry are updated |
| **Non-cached, asynchronous file object** | aligned; read inside the file, or write not past the file size | **STATUS_PENDING** | asynchronously (below) |
| Async setup failure | transfer context allocation fails / a cluster run cannot be mapped | STATUS_INSUFFICIENT_RESOURCES / the mapping error | synchronously |
| Volume handle (FCB flag 0x01) | offset < partition length; length clipped to the partition end | what IofCallDriver to the disk returned (pending for a normal transfer) | passed down on the same IRP (below), on any file object |
| Volume handle, offset ≥ partition length | | STATUS_END_OF_FILE | synchronously |

Asynchronous (pending) path, 4627 0x800298a0:
- The FCB lock (shared for reads, exclusive for writes) and the volume's outstanding-request count stay held when the
  dispatch returns; the dispatch only leaves its critical region.
- A non-paged context (tag 'FtAd') holds: major function, clipped length, sector-rounded length still to do, buffer
  offset, run index, FCB, then one {disk offset, length} pair per contiguous run. All runs are mapped before anything
  is sent; a mapping error frees the context and fails synchronously.
- The disk length is the clipped length rounded up to the sector size (the caller's aligned length leaves room).
  For writes the matching Fsc cache pages are invalidated first.
- It sets SL_PENDING_RETURNED in its own stack location (IoMarkIrpPending), then sends the first run.
- Each run is sent on the same IRP (no associated or new IRPs): the next stack location gets the major function,
  run length, buffer offset (bytes already done) and disk byte offset; completion routine set for success, error
  and cancel (Control 0xE0); IofCallDriver to the partition device. Its return value is ignored.
- The completion routine: if the status is a success and runs remain, it sends the next run and returns
  STATUS_MORE_PROCESSING_REQUIRED. When all are done: for a write, unless the FCB has flag 0x08, record the current
  time as the last-write time and mark the FCB dirty (0x40); set Information to the clipped length. On an error it
  stops, leaving the disk's status and Information. Either way it drops the outstanding count, releases the FCB
  lock, frees the context and returns STATUS_SUCCESS, so normal completion continues (the caller's I/O status
  block, event, APC or completion port).
- The asynchronous path never moves the file object's CurrentByteOffset and never changes the file size.

Synchronous non-cached helper (4627 0x80029398): for each contiguous run it sets the next stack location like the
asynchronous path, with a completion routine that sets a local notification event and returns
STATUS_MORE_PROCESSING_REQUIRED; when IofCallDriver returns STATUS_PENDING it waits (Executive, KernelMode, not
alertable) and takes Irp->IoStatus.Status; it stops at the first error. Then (0x8002930a): a write past the file
size raises the file size and writes the directory entry (on failure the old size is restored and that error is
returned); a write also updates the last-write time and dirty flag as above; when the file object has
FO_SYNCHRONOUS_IO, CurrentByteOffset = offset + clipped length; Information = clipped length.

Volume handle path: the current stack location is copied to the next one, the user buffer is locked, for a write the
Fsc cache range is invalidated, the next Length is set to the clipped length, and completion routine 0x8002929f is
set (Control 0xE0). The volume lock (shared for reads) stays held until that routine runs. The routine propagates the
pending flag (if PendingReturned, mark the stack location pending), on success of a read/write sets the file
object's CurrentByteOffset = ByteOffset + Information when it has FO_SYNCHRONOUS_IO, releases the volume lock and
returns STATUS_SUCCESS. The dispatch returns IofCallDriver's status unchanged.

I/O manager side (IopSynchronousServiceTail, 4627 0x800181bb): the IRP is queued on the thread and sent. If the
driver did not return STATUS_PENDING, the request is completed inline (deferred completion). For a file object with
FO_SYNCHRONOUS_IO and a STATUS_PENDING return, it waits on the file object's Event (+0x38) — Executive, KernelMode,
not alertable, no timeout (0x80018241) — and returns FinalStatus (+0x10); then releases the file object lock. For an
asynchronous file object STATUS_PENDING goes back to the caller, and the result arrives later through the I/O
status block, event, APC or completion port. Titles depend on it (observed 2026-11-07: an overlapped no-buffering
streaming loop advances only when ReadFile reports the read pending; XMV playback requires ERROR_IO_PENDING).

Ours: transfers synchronously, then completes the IRP as a pended one when the status is a success, the transfer
moved bytes, the file object lacks FO_SYNCHRONOUS_IO, and the IRP is non-cached or has a segment array.
Differences from the reference: a non-cached write that extends the file is pended by ours but completed
synchronously by the reference; a disk error on the pending path is reported synchronously by ours, as a pending
request by the reference.
