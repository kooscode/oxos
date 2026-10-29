# Spec 11 — I/O manager (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile `ev_6c1985b0…` (IoCreateFile, IopParseDevice, NtReadFile),
`ev_96a78746…` (IopCompleteRequest, file Close/Delete/Parse, IopSynchronousServiceTail, IopMountDevice),
`ev_be4317fc…` (IoCheckShareAccess, IoDeleteDevice, IoStartPacket/IoStartNextPacket, IoSetIoCompletion,
NtRemoveIoCompletion, IoSynchronousDeviceIoControlRequest, IoBuildAsynchronousFsdRequest), `ev_1d6a3bf8…`
(NtUserIoApcDispatcher, NtQueryInformationFile, NtSetInformationFile, NtDeviceIoControlFile), plus the M3b-prep
decompiles of IofCallDriver, IofCompleteRequest, IoAllocateIrp, IoBuildDeviceIoControlRequest and IoCreateDevice.
Structure layouts follow nxdk where they agree with the offsets used by the code. **Status: implemented (milestone 3b).**

## 1. Structures
- **DRIVER_OBJECT** (0x44): DriverStartIo, DriverDeleteDevice, DriverDismountVolume, MajorFunction[14].
  Xbox major numbers: CREATE 0, CLOSE 1, READ 2, WRITE 3, QUERY_INFORMATION 4, SET_INFORMATION 5, FLUSH_BUFFERS 6,
  QUERY_VOLUME_INFORMATION 7, DIRECTORY_CONTROL 8, FILE_SYSTEM_CONTROL 9, DEVICE_CONTROL 0xA,
  INTERNAL_DEVICE_CONTROL 0xB, SHUTDOWN 0xC, CLEANUP 0xD.
- **DEVICE_OBJECT** (0x48): Type 3, Size, ReferenceCount +4, DriverObject +8, **MountedOrSelfDevice +0xC**,
  CurrentIrp +0x10, Flags +0x14, DeviceExtension +0x18, DeviceType +0x1C, StartIoFlags +0x1D, StackSize +0x1E,
  DeletePending +0x1F, SectorSize +0x20, AlignmentRequirement +0x24, DeviceQueue +0x28 (KDEVICE_QUEUE, 0xC),
  DeviceLock +0x34 (synchronization KEVENT), StartIoKey +0x44.
  - Flags: 1 RAW_MOUNT_ONLY (mount skips file systems), 2 EXCLUSIVE, 4 DIRECT_IO (lock caller buffers), 8 HAS_NAME, 0x10 DEVICE_INITIALIZING.
  - StartIoFlags: 1 inside StartIo, 2 a StartNextPacket was deferred, 4 deferred one is by key.
- **FILE_OBJECT** (0x48): Type 5, access/share bits byte +2 (1 DeletePending, 2 Read, 4 Write, 8 Delete,
  0x10/0x20/0x40 SharedRead/Write/Delete), Flags +3, DeviceObject +4, FsContext +8, FsContext2 +0xC,
  FinalStatus +0x10, CurrentByteOffset +0x14, RelatedFileObject +0x1C, CompletionContext +0x20 ({Port, Key}, 8 bytes,
  tag 'IoCc'), LockCount +0x24 (−1 = free), Lock +0x28 (synchronization event), Event +0x38 (notification event,
  the object's wait target: IoFileObjectType.DefaultObject = 0x38).
  - Flags: 1 SYNCHRONOUS_IO (create option 0x10 or 0x20), 2 ALERTABLE_IO (0x10), 4 NO_INTERMEDIATE_BUFFERING
    (option 8), 8 SEQUENTIAL_ONLY (option 4), 0x20 cleanup sent ("handle created"), 0x40 RANDOM_ACCESS (option 0x800).
- **IO_STACK_LOCATION** (0x24): MajorFunction, MinorFunction, Flags (1 RESTART_SCAN, 2 RETURN_SINGLE_ENTRY,
  4 INDEX_SPECIFIED, 0x80 CASE_SENSITIVE), Control (1 PENDING_RETURNED; 0x20/0x40/0x80 invoke on
  cancel/success/error), Parameters (16 bytes), DeviceObject +0x14, FileObject +0x18, CompletionRoutine +0x1C,
  Context +0x20. Parameter overlays:
  - Create {DesiredAccess, Options = Disposition << 24 | CreateOptions, FileAttributes (16), ShareAccess (16),
    RemainingName}
  - Read/Write {Length, BufferOffset, ByteOffset (8)}
  - QueryFile/SetFile {Length, FileInformationClass, (set) FileObject}; QueryVolume {Length, FsInformationClass}
  - QueryDirectory {Length, FileName, FileInformationClass}
  - DeviceIoControl / FileSystemControl {OutputBufferLength, InputBuffer, InputBufferLength, IoControlCode}
- **IRP** (0x68 + StackSize × 0x24, tag 'Irp '): Type 6, Size, Flags +4, ThreadListEntry +8, IoStatus +0x10,
  StackCount +0x18, CurrentLocation +0x19 (starts at StackCount + 1), PendingReturned +0x1A, Cancel +0x1B,
  UserIosb +0x1C, UserEvent +0x20, Overlay +0x28 ({UserApcRoutine, UserApcContext} or AllocationSize),
  UserBuffer +0x30, SegmentArray +0x34, LockedBufferLength +0x38, Tail +0x3C. Tail overlays:
  {DeviceQueueEntry / DriverContext[5] +0x3C, Thread +0x50, ListEntry +0x54, CurrentStackLocation / PacketType +0x5C,
  OriginalFileObject +0x60}, or a KAPC at +0x3C, or CompletionKey at +0x3C.
  - Flags: 1 NOCACHE, 2 PAGING_IO, 4 SYNCHRONOUS_API, 8 CREATE_OPERATION, 0x10 READ_OPERATION,
    0x20 WRITE_OPERATION, 0x40 CLOSE_OPERATION, 0x80 DEFER_IO_COMPLETION, 0x200 UNLOCK_USER_BUFFER.

## 2. Devices
- `IoCreateDevice(driver, extSize, name, type, exclusive, &dev)`: ObCreateObject(IoDeviceObjectType, size 0x48 + ext,
  OBJ_PERMANENT if named); zeroed; extension follows the object; StackSize 1; flags 0x10 | (exclusive ? 2) | (name ? 8).
  Mountable types (DISK 7, CD_ROM 2, MEDIA_BOARD 0x3B?, MEMORY_UNIT 0x3A) get a DeviceLock and MountedOrSelfDevice =
  NULL (mounted on first open); everything else gets MountedOrSelfDevice = self. Inserted with bias 1, no handle kept.
  The driver clears DEVICE_INITIALIZING (opens fail with STATUS_NO_SUCH_DEVICE 0xC000000E until it does).
- `IoDeleteDevice`: a named device becomes temporary (ObMakeTemporaryObject); DeletePending = 1; if ReferenceCount
  is 0 the device is unloaded now (driver's DriverDeleteDevice, then the object reference is dropped), otherwise when
  the last file object on it is deleted.
- `IoCreateSymbolicLink(link, target)` / `IoDeleteSymbolicLink(link)`: object-manager symlinks (permanent).
- `IoStartPacket(dev, irp, key*)`: at DISPATCH insert the IRP into the device queue (by key if given). If the queue
  was idle: CurrentIrp = irp and call StartIo with StartIoFlags bit 1 set; afterwards run a deferred StartNextPacket.
  `IoStartNextPacket[ByKey]` from inside StartIo only records the request (bits 2/4); otherwise it removes the next
  IRP (CurrentIrp = NULL if the queue empties) and calls StartIo.

## 3. IRPs
- `IoAllocateIrp(stackSize)`, `IoInitializeIrp(irp, size, stackSize)`, `IoFreeIrp(irp)`.
- `IofCallDriver(dev, irp)` (fastcall): CurrentLocation−−; ≤ 0 → bugcheck NO_MORE_IRP_STACK_LOCATIONS (0x35);
  the new stack location gets DeviceObject = dev; return `dev->DriverObject->MajorFunction[major](dev, irp)`.
- `IofCompleteRequest(irp, boost)` (fastcall): CurrentLocation > StackCount + 1 → bugcheck 0x44. Walk the stack
  upwards: PendingReturned = Control & 1; invoke the completion routine when its Control bit matches
  (success/error/cancel) with the next upper DeviceObject (NULL at the top); STATUS_MORE_PROCESSING_REQUIRED
  (0xC0000016) stops completion. Otherwise propagate the pending bit to the next location. Then:
  - CLOSE_OPERATION or PAGING_IO: copy IoStatus to *UserIosb, set UserEvent (boost), free the IRP if PAGING_IO;
    done.
  - UNLOCK_USER_BUFFER: unlock UserBuffer / LockedBufferLength.
  - DEFER_IO_COMPLETION and not PendingReturned: return — the caller finishes the IRP (see §5).
  - Otherwise queue the IRP's Tail.Apc to Tail.Thread (kernel APC IopCompleteRequest, SystemArgument1 =
    OriginalFileObject, boost).
- **IopCompleteRequest** (runs in the issuing thread, APC_LEVEL):
  - error status (severity 3) and not PendingReturned: nothing is reported — the service returned the status
    synchronously. Drop the file-object reference (not for CREATE) and the caller's event reference (unless
    SYNCHRONOUS_API); unlink from the thread's IRP list; free.
  - otherwise copy IoStatus to *UserIosb; set UserEvent (dropping its reference unless SYNCHRONOUS_API; also set the
    file's Event + FinalStatus for synchronous file objects); with no UserEvent set the file object's Event and
    FinalStatus. Unlink. If UserApcRoutine: reuse the IRP's APC (kernel routine frees the IRP, normal routine =
    UserApcRoutine, context = UserApcContext, SystemArgument1 = UserIosb; UserMode only when the routine is
    `NtUserIoApcDispatcher`, else KernelMode; boost 2). Else if the file has a completion port and UserApcContext ≠ 0
    the IRP itself is queued to the port (CompletionKey = Key, PacketType 0). Else free. Drop the file reference.
- `IoBuildAsynchronousFsdRequest(major, dev, buf, len, offset*, iosb)`: IRP with Thread = current, UserIosb; for
  anything but FLUSH/SHUTDOWN: UserBuffer = buf, Read {len, offset}, locking the buffer for DIRECT_IO devices.
  `IoBuildSynchronousFsdRequest(…, event, iosb)` additionally sets UserEvent and links the IRP into the thread's
  IRP list (no SYNCHRONOUS_API: a pended error must still wake the event); `IoBuildDeviceIoControlRequest(code, dev, in, inLen, out, outLen, internal, event,
  iosb)` likewise (major 0xA / 0xB, UserBuffer = out).
- `IoSynchronousFsdRequest(major, dev, buf, len, offset*)` and `IoSynchronousDeviceIoControlRequest(code, dev, in,
  inLen, out, outLen, returned*, internal)` build, call, wait on a local event if pending, and return the status
  (and Information).
- `IoQueueThreadIrp(irp)` links into the current thread's IRP list; `IoMarkIrpMustComplete` is accepted (no-op —
  there is no cancellation of thread IRPs on the Xbox).

## 4. Opening files
- `IoCreateFile(h*, access, attrs, iosb, allocSize*, fileAttributes, share, disposition, createOptions, options)`:
  with option 0x200 (check parameters) validate as NT does; build an OPEN_PACKET on the stack and
  `ObOpenObjectByName(attrs, NULL, &packet)`. Success (packet consumed by IopParseDevice) → set the file's
  "handle created" flag, return the handle, IoStatus from the packet. `NtCreateFile` / `NtOpenFile` are thin
  wrappers (NtOpenFile = disposition FILE_OPEN, no allocation size).
- **IopParseFile** (FILE type parse): a file handle as RootDirectory → RelatedFileObject; parse its device.
- **IopParseDevice(dev, …, remaining, packet)**: no packet → just reference the device (opening by name returns the
  device object). Otherwise:
  - DeletePending or DEVICE_INITIALIZING → STATUS_NO_SUCH_DEVICE; exclusive device already referenced and not a
    relative open → STATUS_ACCESS_DENIED.
  - Mount: while MountedOrSelfDevice is NULL, IopMountDevice under DeviceLock (CD-ROM: XDVDFS then UDF; disk: FATX;
    on failure, and only when opening the device itself, the RAW file system). The mounted volume device's
    ReferenceCount++.
  - Build a create IRP for the volume device (Flags 0x8C, UserIosb = local, AllocationSize in Overlay,
    Create parameters, Flags |= CASE_SENSITIVE unless OBJ_CASE_INSENSITIVE).
  - Create the FILE_OBJECT: Type 5, RelatedFileObject, flags from the options (§1); synchronous files get the Lock
    event and LockCount −1; DeviceObject = volume device; the Event is a notification event.
  - Call the driver; if pending, wait on the file's Event; else complete inline.
  - Failure: the file object is destroyed without a CLOSE IRP, the device reference is dropped, the status returned.
  - Success: the file object is returned (with STATUS_SUCCESS); IoStatus.Information is passed back.
- File object Close procedure (last handle): set "cleanup sent", send IRP_MJ_CLEANUP synchronously
  (Flags 0x44, acquiring the file lock for synchronous files).
- File object Delete procedure: (cleanup if not sent) send IRP_MJ_CLOSE; free the completion context and its port
  reference; dereference the device (unload when DeletePending and unreferenced).
- `NtDeleteFile` / `NtQueryFullAttributesFile` open the file (DELETE / FILE_READ_ATTRIBUTES), set the disposition or
  query FileNetworkOpenInformation, and close it.

## 5. Read/write/control/query services
Common shape: reference the handle as a file object; check access (read for Read, write for Write); for synchronous
file objects acquire the file lock (alertable per ALERTABLE_IO) and use CurrentByteOffset when no offset or
{−2,−1} (FILE_USE_FILE_POINTER_POSITION) is passed; asynchronous files require an explicit offset (else
STATUS_INVALID_PARAMETER); negative offsets are invalid. Reset the file Event; allocate an IRP for the device's
StackSize (failure → STATUS_INSUFFICIENT_RESOURCES); OriginalFileObject, Thread, UserEvent (referenced event handle,
reset), UserIosb, ApcRoutine/Context, UserBuffer; DIRECT_IO devices lock the buffer. Flags: DEFER_IO_COMPLETION
(and READ/WRITE_OPERATION, NOCACHE for no-buffering files). Then **IopSynchronousServiceTail**: link to the thread's
IRP list, call the driver; if not pending, complete inline (IopCompleteRequest at APC_LEVEL). For synchronous
files: if pending, wait on the file's Event (alertable per file) and take FinalStatus; release the file lock.
- A completion port on the file and an ApcRoutine at the same time → STATUS_INVALID_PARAMETER.
- `NtReadFile`/`NtWriteFile(h, event, apc, ctx, iosb, buf, len, offset*)`; writes at {−1, FILE_WRITE_TO_END_OF_FILE}
  are passed through. `NtReadFileScatter`/`NtWriteFileGather` take a segment array (SegmentArray, length).
- `NtDeviceIoControlFile` / `NtFsControlFile(h, event, apc, ctx, iosb, code, in, inLen, out, outLen)`: major 0xA / 9.
- `NtQueryInformationFile(h, iosb, info, len, class)` / `NtSetInformationFile`: class > 0x24 or unsupported →
  STATUS_INVALID_INFO_CLASS (0xC0000003); short buffer → STATUS_INFO_LENGTH_MISMATCH (0xC0000004). Handled in the
  I/O manager without the driver: FilePositionInformation (14) for synchronous files; FileModeInformation (16)
  (get/set the SYNCHRONOUS/ALERTABLE/SEQUENTIAL flags); FileAlignmentInformation (17); FileCompletionInformation
  (30, once only: {Port handle, Key}). The rest become IRPs; synchronous-API calls on asynchronous files wait on a
  local event.
- FileRenameInformation (10, {BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING FileName}): when the name
  starts with `\` or RootDirectory is set, the I/O manager first opens the target's directory with
  `IoCreateFile(&h, SYNCHRONIZE | FILE_WRITE_DATA (0x100002), {RootDirectory, &FileName, OBJ_CASE_INSENSITIVE}, iosb,
  NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_OPEN_FOR_BACKUP_INTENT (0x4000), 0x105)` (options:
  IO_NO_PARAMETER_CHECKING | IO_OPEN_TARGET_DIRECTORY | IO_FORCE_ACCESS_CHECK, so the create stack location has
  SL_OPEN_TARGET_DIRECTORY and the file system opens the parent and reports whether the final name exists). The
  target file object must be on the same device as the file, else the handle is closed and the call fails with
  STATUS_NOT_SAME_DEVICE (0xC00000D4). The file object goes to the driver in `Parameters.SetFile.FileObject`; the
  handle is closed when the request is done. A plain name (no `\`, no RootDirectory) goes to the driver with no
  target object and renames in place (4627 NtSetInformationFile 0x80016F61, target open 0x80017D42).
- `NtQueryVolumeInformationFile`, `NtQueryDirectoryFile(h, event, apc, ctx, iosb, buf, len, class, mask, restart)`,
  `NtFlushBuffersFile(h, iosb)` follow the same pattern (majors 7, 8, 6).
- `IoQueryFileInformation` / `IoQueryVolumeInformation(fileObject, class, len, buf, &returned)`: kernel callers with a
  file object.

## 6. Completion ports
- `NtCreateIoCompletion(h*, access, attrs, count)`: object type IoCompletionObjectType (KQUEUE body, 'Comp');
  Delete runs the queue down and frees the entries (IRPs or mini packets).
- `IoSetIoCompletion(port, key, apcCtx, status, info)` / `NtSetIoCompletion(h, …)`: queue a 0x1C mini packet
  (tag 'Icp ', PacketType 1: {ListEntry, PacketType, Key, ApcContext, Status, Information}).
- `NtRemoveIoCompletion(h, &key, &apcCtx, iosb, timeout*)`: KeRemoveQueue (kernel mode); STATUS_TIMEOUT /
  STATUS_USER_APC are returned as is; an IRP entry reports {CompletionKey, UserApcContext, IoStatus} and is freed.
- `NtQueryIoCompletion(h, info)` → {Depth = current entry count}.

## 7. Implementation notes
- Self-tests (`kernel/src/ke/selftest_io.cpp`) drive a RAM-disk test driver and a StartIo-queued device: open
  through `\??\R:`, sync/async/pended I/O, APC routine, completion port, IOCTLs, relative open, share access,
  exclusive devices, cleanup/close accounting and device deletion.
- Open question for M4: drive-letter links whose target is a path *inside* a device (`D:` → a title directory)
  need reparse with the link's remaining target; today links resolve to target objects only.
- Our kernel registers file systems through an internal mount table (FATX/XDVDFS/RAW land in milestone 3c); with
  none, mounting fails with STATUS_UNRECOGNIZED_VOLUME (0xC000014F).
