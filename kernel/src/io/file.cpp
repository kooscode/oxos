// I/O manager: opening files, file objects, Nt file services, completion ports (spec/11 §4-6).
#include <xk/io.h>
#include <xk/ps.h>
#include <xk/rtl.h>

void IopQueueThreadIrp(PIRP Irp);
void IopLockUserBuffer(PIRP Irp, ULONG Length);
void IopCompleteRequestInline(PIRP Irp, PFILE_OBJECT FileObject);
void IopDereferenceDevice(PDEVICE_OBJECT DeviceObject);

// Carried through the object manager from IoCreateFile to IopParseDevice.
struct OPEN_PACKET {
    CSHORT Type;
    CSHORT Size;
    PFILE_OBJECT FileObject;
    NTSTATUS FinalStatus;
    ULONG_PTR Information;
    ULONG ParseCheck;
    PFILE_OBJECT RelatedFileObject;
    LARGE_INTEGER AllocationSize;
    ULONG CreateOptions;
    USHORT FileAttributes;
    USHORT ShareAccess;
    ULONG Options;
    ULONG Disposition;
    ACCESS_MASK DesiredAccess;
};
constexpr ULONG OPEN_PACKET_PATTERN = 0xBEAA0251;

// --- File object locking (synchronous file objects serialise their I/O)

static void IopAcquireFileObjectLock(PFILE_OBJECT f) {
    if (__atomic_add_fetch(&f->LockCount, 1, __ATOMIC_SEQ_CST) != 0)
        KeWaitForSingleObject(&f->Lock, Executive, KernelMode, FALSE, nullptr);
}

static void IopReleaseFileObjectLock(PFILE_OBJECT f) {
    if (__atomic_sub_fetch(&f->LockCount, 1, __ATOMIC_SEQ_CST) >= 0)
        KeSetEvent(&f->Lock, 0, FALSE);
}

static PIRP IopAllocateIrpMustSucceed(CCHAR StackSize) {
    for (;;) {
        PIRP irp = IoAllocateIrp(StackSize);
        if (irp)
            return irp;
        LARGE_INTEGER wait;
        wait.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
}

#if XK_DRIVE_LOG
void IdexCdRomRecordOpen(PDEVICE_OBJECT dev, POBJECT_STRING name, bool related, ULONG value);
#endif

// --- Mounting

static NTSTATUS IopMountDevice(PDEVICE_OBJECT dev, bool allowRaw) {
    KeWaitForSingleObject(&dev->DeviceLock, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = STATUS_SUCCESS;
    if (!dev->MountedOrSelfDevice) {
        IO_MOUNT_ROUTINE fs = dev->DeviceType == FILE_DEVICE_CD_ROM ? IopFileSystems.CdRom : IopFileSystems.Disk;
        status = (dev->Flags & DO_RAW_MOUNT_ONLY) || !fs ? STATUS_UNRECOGNIZED_VOLUME : fs(dev);
        if (!NT_SUCCESS(status) && allowRaw && IopFileSystems.Raw)
            status = IopFileSystems.Raw(dev);
    }
    KeSetEvent(&dev->DeviceLock, 0, FALSE);
    return status;
}

static ACCESS_MASK IopMapGenericAccess(ACCESS_MASK a) {
    if (a & GENERIC_READ)
        a |= 0x120089; // FILE_GENERIC_READ
    if (a & GENERIC_WRITE)
        a |= 0x120116; // FILE_GENERIC_WRITE
    if (a & 0x20000000)
        a |= 0x1200A0; // GENERIC_EXECUTE -> FILE_GENERIC_EXECUTE
    if (a & 0x10000000)
        a |= 0x1F01FF; // GENERIC_ALL -> FILE_ALL_ACCESS
    return a & 0x0FFFFFFF;
}

// --- Parse procedures

NTSTATUS NTAPI IopParseDevice(PVOID ParseObject, POBJECT_TYPE ObjectType, ULONG Attributes,
                              POBJECT_STRING CompleteName, POBJECT_STRING RemainingName, PVOID Context,
                              PVOID *Object) {
    *Object = nullptr;
    auto op = (OPEN_PACKET *)Context;
    auto dev = (PDEVICE_OBJECT)ParseObject;
    if (!op) {
        // Referencing a device by name.
        if (RemainingName->Length)
            return STATUS_OBJECT_TYPE_MISMATCH;
        ObfReferenceObject(dev);
        *Object = dev;
        return STATUS_SUCCESS;
    }
    if (op->Type != 8 || op->Size != sizeof(OPEN_PACKET))
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (op->RelatedFileObject)
        dev = op->RelatedFileObject->DeviceObject;

    NTSTATUS status;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (dev->DeletePending || (dev->Flags & DO_DEVICE_INITIALIZING))
        status = STATUS_NO_SUCH_DEVICE;
    else if ((dev->Flags & DO_EXCLUSIVE) && dev->ReferenceCount && !op->RelatedFileObject)
        status = STATUS_ACCESS_DENIED;
    else
        status = STATUS_SUCCESS;
    if (!NT_SUCCESS(status)) {
        KfLowerIrql(old);
        op->FinalStatus = status;
        return status;
    }
    PDEVICE_OBJECT vol;
#if XK_DRIVE_LOG
    IdexCdRomRecordOpen(dev, RemainingName, op->RelatedFileObject != nullptr, (ULONG)(ULONG_PTR)dev->MountedOrSelfDevice);
#endif
    while (!(vol = dev->MountedOrSelfDevice)) {
        KfLowerIrql(old);
        status = IopMountDevice(dev, !RemainingName->Length && !op->RelatedFileObject);
#if XK_DRIVE_LOG
        IdexCdRomRecordOpen(dev, nullptr, 0, (ULONG)status);
#endif
        if (!NT_SUCCESS(status)) {
            op->FinalStatus = status;
            return status;
        }
        old = KeRaiseIrqlToDpcLevel();
    }
    vol->ReferenceCount++;
    KfLowerIrql(old);

    op->DesiredAccess = IopMapGenericAccess(op->DesiredAccess);
    PIRP irp = IoAllocateIrp(vol->StackSize);
    if (!irp) {
        IopDereferenceDevice(vol);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    irp->Flags = IRP_CREATE_OPERATION | IRP_SYNCHRONOUS_API | IRP_DEFER_IO_COMPLETION;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_CREATE;
    stack->Flags = (UCHAR)op->Options;
    if (!(Attributes & OBJ_CASE_INSENSITIVE))
        stack->Flags |= SL_CASE_SENSITIVE;
    irp->Overlay.AllocationSize = op->AllocationSize;
    stack->Parameters.Create.DesiredAccess = op->DesiredAccess;
    stack->Parameters.Create.Options = (op->Disposition << 24) | (op->CreateOptions & 0xFFFFFF);
    stack->Parameters.Create.FileAttributes = op->FileAttributes;
    stack->Parameters.Create.ShareAccess = op->ShareAccess;
    stack->Parameters.Create.RemainingName = RemainingName;
    IO_STATUS_BLOCK iosb;
    irp->UserIosb = &iosb;

    PFILE_OBJECT fo;
    OBJECT_ATTRIBUTES fattrs = {nullptr, nullptr, Attributes};
    status = ObCreateObject(&IoFileObjectType, &fattrs, sizeof(FILE_OBJECT), (PVOID *)&fo);
    if (!NT_SUCCESS(status)) {
        IoFreeIrp(irp);
        IopDereferenceDevice(vol);
        op->FinalStatus = status;
        return status;
    }
    RtlZeroMemory(fo, sizeof(*fo));
    fo->Type = 5;
    fo->RelatedFileObject = op->RelatedFileObject;
    if (op->CreateOptions & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) {
        fo->Flags = FO_SYNCHRONOUS_IO | ((op->CreateOptions & FILE_SYNCHRONOUS_IO_ALERT) ? FO_ALERTABLE_IO : 0);
        fo->LockCount = -1;
        KeInitializeEvent(&fo->Lock, SynchronizationEvent, FALSE);
    }
    if (op->CreateOptions & FILE_NO_INTERMEDIATE_BUFFERING)
        fo->Flags |= FO_NO_INTERMEDIATE_BUFFERING;
    if (op->CreateOptions & FILE_SEQUENTIAL_ONLY)
        fo->Flags |= FO_SEQUENTIAL_ONLY;
    if (op->CreateOptions & FILE_RANDOM_ACCESS)
        fo->Flags |= FO_RANDOM_ACCESS;
    fo->DeviceObject = vol;
    KeInitializeEvent(&fo->Event, NotificationEvent, FALSE);
    irp->Tail.Overlay.OriginalFileObject = fo;
    stack->FileObject = fo;
    op->FileObject = fo;

    IopQueueThreadIrp(irp);
    status = IofCallDriver(vol, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&fo->Event, Executive, KernelMode, FALSE, nullptr);
        status = iosb.Status;
    } else {
        // Deferred completion of a create: nothing is reported through APCs.
        KIRQL o = KfRaiseIrql(APC_LEVEL);
        iosb = irp->IoStatus;
        status = iosb.Status;
        fo->Event.Header.SignalState = 1;
        RemoveEntryList(&irp->ThreadListEntry);
        IoFreeIrp(irp);
        KfLowerIrql(o);
    }
    op->Information = iosb.Information;
    op->FinalStatus = status;
    if (!NT_SUCCESS(status)) {
        fo->DeviceObject = nullptr; // the driver never saw a successful create: no CLOSE
        ObfDereferenceObject(fo);
        op->FileObject = nullptr;
        IopDereferenceDevice(vol);
        return status;
    }
    op->ParseCheck = OPEN_PACKET_PATTERN;
    ObfReferenceObject(fo); // the packet's reference, dropped by IoCreateFile
    *Object = fo;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI IopParseFile(PVOID ParseObject, POBJECT_TYPE ObjectType, ULONG Attributes, POBJECT_STRING CompleteName,
                            POBJECT_STRING RemainingName, PVOID Context, PVOID *Object) {
    auto op = (OPEN_PACKET *)Context;
    *Object = nullptr;
    if (!op || op->Type != 8 || op->Size != sizeof(OPEN_PACKET))
        return STATUS_OBJECT_TYPE_MISMATCH;
    auto fo = (PFILE_OBJECT)ParseObject;
    op->RelatedFileObject = fo;
    return IopParseDevice(fo->DeviceObject, ObjectType, Attributes, CompleteName, RemainingName, Context, Object);
}

// --- Close / delete

static void IopSendCloseIrp(PFILE_OBJECT fo, UCHAR major) {
    PDEVICE_OBJECT vol = fo->DeviceObject;
    fo->Event.Header.SignalState = 0;
    PIRP irp = IopAllocateIrpMustSucceed(vol->StackSize);
    KEVENT event;
    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
    irp->Tail.Overlay.OriginalFileObject = fo;
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    irp->UserIosb = &irp->IoStatus;
    irp->UserEvent = &event;
    irp->Flags = IRP_CLOSE_OPERATION | IRP_SYNCHRONOUS_API;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = major;
    stack->FileObject = fo;
    IopQueueThreadIrp(irp);
    if (IofCallDriver(vol, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
    KIRQL old = KfRaiseIrql(APC_LEVEL);
    RemoveEntryList(&irp->ThreadListEntry);
    KfLowerIrql(old);
    IoFreeIrp(irp);
}

void NTAPI IopCloseFileObject(PVOID Object, ULONG SystemHandleCount) {
    auto fo = (PFILE_OBJECT)Object;
    if (SystemHandleCount != 1 || !fo->DeviceObject)
        return;
    fo->Flags |= FO_HANDLE_CREATED;
    bool sync = fo->Flags & FO_SYNCHRONOUS_IO;
    if (sync)
        IopAcquireFileObjectLock(fo);
    IopSendCloseIrp(fo, IRP_MJ_CLEANUP);
    if (sync)
        IopReleaseFileObjectLock(fo);
}

void NTAPI IopDeleteFileObject(PVOID Object) {
    auto fo = (PFILE_OBJECT)Object;
    PDEVICE_OBJECT vol = fo->DeviceObject;
    if (!vol)
        return;
    if (!(fo->Flags & FO_HANDLE_CREATED))
        IopCloseFileObject(fo, 1);
    IopSendCloseIrp(fo, IRP_MJ_CLOSE);
    if (fo->CompletionContext) {
        ObfDereferenceObject(fo->CompletionContext->Port);
        ExFreePool(fo->CompletionContext);
    }
    IopDereferenceDevice(vol);
}

// --- Opening

XBAPI NTSTATUS NTAPI IoCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                  PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                  ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, ULONG Options) {
    if (Options & IO_CHECK_CREATE_PARAMETERS) {
        bool bad = (FileAttributes & 0xFFFF8048) || (ShareAccess & ~7u) || Disposition > FILE_OVERWRITE_IF ||
                   (CreateOptions & 0xFF000000) ||
                   ((CreateOptions & 0x30) && !(DesiredAccess & SYNCHRONIZE)) ||
                   ((CreateOptions & FILE_DELETE_ON_CLOSE) && !(DesiredAccess & DELETE)) ||
                   (CreateOptions & 0x30) == 0x30 ||
                   ((CreateOptions & FILE_DIRECTORY_FILE) && !(CreateOptions & FILE_NON_DIRECTORY_FILE) &&
                    ((CreateOptions & 0xFF5F8ECC) ||
                     (Disposition != FILE_CREATE && Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF))) ||
                   ((CreateOptions & 0x100) && (CreateOptions & 0x100000)) ||
                   ((CreateOptions & FILE_NO_INTERMEDIATE_BUFFERING) && (DesiredAccess & FILE_APPEND_DATA));
        if (bad)
            return STATUS_INVALID_PARAMETER;
    }
    OPEN_PACKET op = {};
    op.Type = 8;
    op.Size = sizeof(OPEN_PACKET);
    if (AllocationSize)
        op.AllocationSize = *AllocationSize;
    op.CreateOptions = CreateOptions;
    op.FileAttributes = (USHORT)FileAttributes;
    op.ShareAccess = (USHORT)ShareAccess;
    op.Options = Options;
    op.Disposition = Disposition;
    op.DesiredAccess = DesiredAccess;

    HANDLE handle;
    NTSTATUS status = ObOpenObjectByName(ObjectAttributes, nullptr, &op, &handle);
    bool parsed = op.ParseCheck == OPEN_PACKET_PATTERN;
    if (NT_SUCCESS(status) && !parsed) {
        NtClose(handle); // the name did not lead to a device
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (!NT_SUCCESS(status)) {
        if (!NT_SUCCESS(op.FinalStatus)) {
            if (((ULONG)op.FinalStatus & 0xC0000000) == 0x80000000) { // warnings are still reported
                IoStatusBlock->Status = op.FinalStatus;
                IoStatusBlock->Information = op.Information;
            }
        } else if (op.FileObject && parsed) {
            ObfDereferenceObject(op.FileObject); // opened, but no handle could be created
        }
        return status;
    }
    op.FileObject->Flags |= FO_HANDLE_CREATED;
    *FileHandle = handle;
    IoStatusBlock->Status = op.FinalStatus;
    IoStatusBlock->Information = op.Information;
    ObfDereferenceObject(op.FileObject);
    return op.FinalStatus;
}

XBAPI NTSTATUS NTAPI NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                  PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                  ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions) {
    return IoCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes,
                        ShareAccess, CreateDisposition, CreateOptions, 0);
}

XBAPI NTSTATUS NTAPI NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions) {
    return IoCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, nullptr, 0, ShareAccess,
                        FILE_OPEN, OpenOptions, 0);
}

// --- Common service plumbing (spec/11 §5)

struct IOP_REQUEST {
    PFILE_OBJECT File;
    PDEVICE_OBJECT Device;
    PKEVENT Event; // referenced user event
    bool Synchronous;
};

static NTSTATUS IopBeginRequest(HANDLE FileHandle, HANDLE EventHandle, IOP_REQUEST *r) {
    r->Event = nullptr;
    NTSTATUS status = ObReferenceObjectByHandle(FileHandle, &IoFileObjectType, (PVOID *)&r->File);
    if (!NT_SUCCESS(status))
        return status;
    r->Device = r->File->DeviceObject;
    if (EventHandle) {
        status = ObReferenceObjectByHandle(EventHandle, &ExEventObjectType, (PVOID *)&r->Event);
        if (!NT_SUCCESS(status)) {
            ObfDereferenceObject(r->File);
            return status;
        }
        KeResetEvent(r->Event);
    }
    r->Synchronous = r->File->Flags & FO_SYNCHRONOUS_IO;
    if (r->Synchronous)
        IopAcquireFileObjectLock(r->File);
    return STATUS_SUCCESS;
}

static NTSTATUS IopAbortRequest(IOP_REQUEST *r, NTSTATUS status) {
    if (r->Event)
        ObfDereferenceObject(r->Event);
    if (r->Synchronous)
        IopReleaseFileObjectLock(r->File);
    ObfDereferenceObject(r->File);
    return status;
}

static PIRP IopAllocateRequestIrp(IOP_REQUEST *r, PIO_STATUS_BLOCK iosb, PIO_APC_ROUTINE apc, PVOID apcContext,
                                  UCHAR major) {
    r->File->Event.Header.SignalState = 0;
    PIRP irp = IoAllocateIrp(r->Device->StackSize);
    if (!irp)
        return nullptr;
    irp->Tail.Overlay.OriginalFileObject = r->File;
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    irp->UserEvent = r->Event;
    irp->UserIosb = iosb;
    irp->Overlay.AsynchronousParameters.UserApcRoutine = apc;
    irp->Overlay.AsynchronousParameters.UserApcContext = apcContext;
    irp->Flags = IRP_DEFER_IO_COMPLETION;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = major;
    stack->FileObject = r->File;
    return irp;
}

static NTSTATUS IopSynchronousServiceTail(IOP_REQUEST *r, PIRP irp) {
    IopQueueThreadIrp(irp);
    NTSTATUS status = IofCallDriver(r->Device, irp);
    if (status != STATUS_PENDING)
        IopCompleteRequestInline(irp, r->File);
    if (r->Synchronous) {
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&r->File->Event, Executive, KernelMode, (r->File->Flags & FO_ALERTABLE_IO) != 0,
                                  nullptr);
            status = r->File->FinalStatus;
        }
        IopReleaseFileObjectLock(r->File);
    }
    return status;
}

static NTSTATUS IopReadWrite(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                             PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
                             PFILE_SEGMENT_ELEMENT Segments, UCHAR major) {
    PFILE_OBJECT f;
    NTSTATUS status = ObReferenceObjectByHandle(FileHandle, &IoFileObjectType, (PVOID *)&f);
    if (!NT_SUCCESS(status))
        return status;
    bool write = major == IRP_MJ_WRITE;
    if (!(write ? f->WriteAccess : f->ReadAccess)) {
        ObfDereferenceObject(f);
        return STATUS_ACCESS_DENIED;
    }
    if (f->CompletionContext && ApcRoutine) {
        ObfDereferenceObject(f);
        return STATUS_INVALID_PARAMETER;
    }
    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    bool haveOffset = ByteOffset != nullptr;
    if (haveOffset)
        offset = *ByteOffset;
    if (!(f->Flags & FO_SYNCHRONOUS_IO) && !haveOffset) {
        ObfDereferenceObject(f);
        return STATUS_INVALID_PARAMETER;
    }
    ObfDereferenceObject(f); // re-referenced below with the event
    IOP_REQUEST r;
    status = IopBeginRequest(FileHandle, Event, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (r.Synchronous && (!haveOffset || (offset.HighPart == -1 && offset.LowPart == FILE_USE_FILE_POINTER_POSITION_LOW)))
        offset = r.File->CurrentByteOffset;
    bool writeToEnd = write && offset.HighPart == -1 && offset.LowPart == 0xFFFFFFFF;
    if (offset.HighPart < 0 && !writeToEnd)
        return IopAbortRequest(&r, STATUS_INVALID_PARAMETER);
    PIRP irp = IopAllocateRequestIrp(&r, IoStatusBlock, ApcRoutine, ApcContext, major);
    if (!irp)
        return IopAbortRequest(&r, STATUS_INSUFFICIENT_RESOURCES);
    irp->UserBuffer = Buffer;
    irp->SegmentArray = Segments;
    if (!Segments && (r.Device->Flags & DO_DIRECT_IO) && Length)
        IopLockUserBuffer(irp, Length);
    irp->Flags |= (write ? IRP_WRITE_OPERATION : IRP_READ_OPERATION) |
                  ((r.File->Flags & FO_NO_INTERMEDIATE_BUFFERING) ? IRP_NOCACHE : 0);
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.Read.Length = Length;
    stack->Parameters.Read.BufferOffset = 0;
    stack->Parameters.Read.ByteOffset = offset;
    return IopSynchronousServiceTail(&r, irp);
}

XBAPI NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                                PLARGE_INTEGER ByteOffset) {
    return IopReadWrite(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, nullptr,
                        IRP_MJ_READ);
}

XBAPI NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                 PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                                 PLARGE_INTEGER ByteOffset) {
    return IopReadWrite(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, nullptr,
                        IRP_MJ_WRITE);
}

XBAPI NTSTATUS NTAPI NtReadFileScatter(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                       PIO_STATUS_BLOCK IoStatusBlock, PFILE_SEGMENT_ELEMENT SegmentArray,
                                       ULONG Length, PLARGE_INTEGER ByteOffset) {
    return IopReadWrite(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, nullptr, Length, ByteOffset,
                        SegmentArray, IRP_MJ_READ);
}

XBAPI NTSTATUS NTAPI NtWriteFileGather(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                       PIO_STATUS_BLOCK IoStatusBlock, PFILE_SEGMENT_ELEMENT SegmentArray,
                                       ULONG Length, PLARGE_INTEGER ByteOffset) {
    return IopReadWrite(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, nullptr, Length, ByteOffset,
                        SegmentArray, IRP_MJ_WRITE);
}

static NTSTATUS IopControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                               PIO_STATUS_BLOCK IoStatusBlock, ULONG Code, PVOID InputBuffer, ULONG InputBufferLength,
                               PVOID OutputBuffer, ULONG OutputBufferLength, UCHAR major) {
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, Event, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (r.File->CompletionContext && ApcRoutine)
        return IopAbortRequest(&r, STATUS_INVALID_PARAMETER);
    PIRP irp = IopAllocateRequestIrp(&r, IoStatusBlock, ApcRoutine, ApcContext, major);
    if (!irp)
        return IopAbortRequest(&r, STATUS_INSUFFICIENT_RESOURCES);
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    stack->Parameters.DeviceIoControl.InputBuffer = InputBuffer;
    stack->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    stack->Parameters.DeviceIoControl.IoControlCode = Code;
    irp->UserBuffer = OutputBuffer;
    ULONG method = Code & 3;
    if ((method == 1 || method == 2) && OutputBuffer && OutputBufferLength)
        IopLockUserBuffer(irp, OutputBufferLength);
    return IopSynchronousServiceTail(&r, irp);
}

XBAPI NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                                           PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
                                           PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer,
                                           ULONG OutputBufferLength) {
    return IopControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer,
                          InputBufferLength, OutputBuffer, OutputBufferLength, IRP_MJ_DEVICE_CONTROL);
}

XBAPI NTSTATUS NTAPI NtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                     PIO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
                                     ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength) {
    return IopControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FsControlCode, InputBuffer,
                          InputBufferLength, OutputBuffer, OutputBufferLength, IRP_MJ_FILE_SYSTEM_CONTROL);
}

XBAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                                          PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                          ULONG Length, FILE_INFORMATION_CLASS FileInformationClass,
                                          POBJECT_STRING FileName, BOOLEAN RestartScan) {
    if (FileInformationClass != FileDirectoryInformation && FileInformationClass != FileFullDirectoryInformation &&
        FileInformationClass != FileBothDirectoryInformation && FileInformationClass != FileNamesInformation)
        return STATUS_INVALID_INFO_CLASS;
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, Event, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (r.File->CompletionContext && ApcRoutine)
        return IopAbortRequest(&r, STATUS_INVALID_PARAMETER);
    PIRP irp = IopAllocateRequestIrp(&r, IoStatusBlock, ApcRoutine, ApcContext, IRP_MJ_DIRECTORY_CONTROL);
    if (!irp)
        return IopAbortRequest(&r, STATUS_INSUFFICIENT_RESOURCES);
    irp->UserBuffer = FileInformation;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MinorFunction = 1; // IRP_MN_QUERY_DIRECTORY
    stack->Flags = (UCHAR)((RestartScan ? SL_RESTART_SCAN : 0) | SL_RETURN_SINGLE_ENTRY);
    stack->Parameters.QueryDirectory.Length = Length;
    stack->Parameters.QueryDirectory.FileName = FileName;
    stack->Parameters.QueryDirectory.FileInformationClass = FileInformationClass;
    return IopSynchronousServiceTail(&r, irp);
}

// --- Information classes (minimum lengths from the reference tables; 0 = not supported)

static const UCHAR IopQueryOperationLength[FileMaximumInformation] = {
    0, 0, 0, 0, 40, 24, 8, 4, 0, 8, 0, 0, 0, 0, 8, 0, 4, 4, 104, 0, 0, 8, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 56, 8, 0};
static const UCHAR IopSetOperationLength[FileMaximumInformation] = {
    0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 16, 16, 0, 1, 8, 0, 4, 0, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 16, 0, 0, 0, 0, 0};
static bool IopSetNeedsWriteAccess(ULONG c) {
    return c == FileAllocationInformation || c == FileEndOfFileInformation || c == FileMoveClusterInformation;
}

static ULONG IopFileMode(PFILE_OBJECT f) {
    ULONG mode = 0;
    if (f->Flags & FO_SEQUENTIAL_ONLY)
        mode |= FILE_SEQUENTIAL_ONLY;
    if (f->Flags & FO_NO_INTERMEDIATE_BUFFERING)
        mode |= FILE_NO_INTERMEDIATE_BUFFERING;
    if (f->Flags & FO_SYNCHRONOUS_IO)
        mode |= (f->Flags & FO_ALERTABLE_IO) ? FILE_SYNCHRONOUS_IO_ALERT : FILE_SYNCHRONOUS_IO_NONALERT;
    return mode;
}

// Query/set through an IRP. Asynchronous file objects still behave synchronously: wait on a local event.
static NTSTATUS IopInformationIrp(IOP_REQUEST *r, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                                  ULONG Class, UCHAR major) {
    KEVENT event;
    IO_STATUS_BLOCK localIosb;
    PIRP irp = IopAllocateRequestIrp(r, IoStatusBlock, nullptr, nullptr, major);
    if (!irp)
        return IopAbortRequest(r, STATUS_INSUFFICIENT_RESOURCES);
    if (!r->Synchronous) {
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp->UserEvent = &event;
        irp->UserIosb = &localIosb;
        irp->Flags |= IRP_SYNCHRONOUS_API;
    }
    irp->UserBuffer = Buffer;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->Parameters.QueryFile.Length = Length;
    stack->Parameters.QueryFile.FileInformationClass = (FILE_INFORMATION_CLASS)Class;
    IopQueueThreadIrp(irp);
    NTSTATUS status = IofCallDriver(r->Device, irp);
    if (status == STATUS_PENDING) {
        if (!r->Synchronous) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
            *IoStatusBlock = localIosb;
            return localIosb.Status;
        }
        KeWaitForSingleObject(&r->File->Event, Executive, KernelMode, FALSE, nullptr);
        status = r->File->FinalStatus;
    } else {
        if (!r->Synchronous)
            irp->UserEvent = nullptr; // nothing to wake: completing inline
        irp->UserIosb = IoStatusBlock;
        IopCompleteRequestInline(irp, r->File);
    }
    if (r->Synchronous)
        IopReleaseFileObjectLock(r->File);
    return status;
}

XBAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                            ULONG Length, FILE_INFORMATION_CLASS FileInformationClass) {
    ULONG c = FileInformationClass;
    if (c >= FileMaximumInformation || !IopQueryOperationLength[c])
        return STATUS_INVALID_INFO_CLASS;
    if (Length < IopQueryOperationLength[c])
        return STATUS_INFO_LENGTH_MISMATCH;
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, nullptr, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (c == FilePositionInformation && r.Synchronous) {
        ((FILE_POSITION_INFORMATION *)FileInformation)->CurrentByteOffset = r.File->CurrentByteOffset;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
        return IopAbortRequest(&r, STATUS_SUCCESS);
    }
    if (c == FileModeInformation || c == FileAlignmentInformation) {
        *(PULONG)FileInformation = c == FileModeInformation ? IopFileMode(r.File) : r.Device->AlignmentRequirement;
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = sizeof(ULONG);
        return IopAbortRequest(&r, STATUS_SUCCESS);
    }
    return IopInformationIrp(&r, IoStatusBlock, FileInformation, Length, c, IRP_MJ_QUERY_INFORMATION);
}

XBAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                          ULONG Length, FILE_INFORMATION_CLASS FileInformationClass) {
    ULONG c = FileInformationClass;
    if (c >= FileMaximumInformation || !IopSetOperationLength[c])
        return STATUS_INVALID_INFO_CLASS;
    if (Length < IopSetOperationLength[c])
        return STATUS_INFO_LENGTH_MISMATCH;
    if ((c == FilePositionInformation || c == FileAllocationInformation || c == FileEndOfFileInformation) &&
        ((PLARGE_INTEGER)FileInformation)->HighPart < 0)
        return STATUS_INVALID_PARAMETER;
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, nullptr, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (IopSetNeedsWriteAccess(c) && !r.File->WriteAccess)
        return IopAbortRequest(&r, STATUS_ACCESS_DENIED);
    PFILE_OBJECT f = r.File;
    auto done = [&](NTSTATUS s) {
        if (NT_SUCCESS(s)) {
            IoStatusBlock->Status = s;
            IoStatusBlock->Information = 0;
        }
        return IopAbortRequest(&r, s);
    };
    if (c == FilePositionInformation && r.Synchronous) {
        LARGE_INTEGER pos = ((FILE_POSITION_INFORMATION *)FileInformation)->CurrentByteOffset;
        // Unbuffered files may only seek to sector-aligned positions.
        if ((f->Flags & FO_NO_INTERMEDIATE_BUFFERING) && r.Device->SectorSize &&
            (pos.LowPart & (r.Device->SectorSize - 1)))
            return done(STATUS_INVALID_PARAMETER);
        f->CurrentByteOffset = pos;
        return done(STATUS_SUCCESS);
    }
    if (c == FileModeInformation) {
        ULONG mode = *(PULONG)FileInformation;
        bool syncMode = mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT);
        if ((mode & ~(FILE_WRITE_THROUGH | FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_ALERT |
                      FILE_SYNCHRONOUS_IO_NONALERT)) ||
            syncMode != r.Synchronous ||
            (mode & 0x30) == 0x30)
            return done(STATUS_INVALID_PARAMETER);
        f->Flags = (UCHAR)((f->Flags & ~FO_SEQUENTIAL_ONLY) | ((mode & FILE_SEQUENTIAL_ONLY) ? FO_SEQUENTIAL_ONLY : 0));
        if (r.Synchronous)
            f->Flags = (UCHAR)((f->Flags & ~FO_ALERTABLE_IO) | ((mode & FILE_SYNCHRONOUS_IO_ALERT) ? FO_ALERTABLE_IO : 0));
        return done(STATUS_SUCCESS);
    }
    if (c == FileCompletionInformation) {
        if (f->CompletionContext || r.Synchronous)
            return done(STATUS_INVALID_PARAMETER);
        auto info = (FILE_COMPLETION_INFORMATION *)FileInformation;
        PVOID port;
        status = ObReferenceObjectByHandle(info->Port, &IoCompletionObjectType, &port);
        if (!NT_SUCCESS(status))
            return done(status);
        auto ctx = (PIO_COMPLETION_CONTEXT)ExAllocatePoolWithTag(sizeof(IO_COMPLETION_CONTEXT), 'cCoI');
        if (!ctx) {
            ObfDereferenceObject(port);
            return done(STATUS_INSUFFICIENT_RESOURCES);
        }
        ctx->Port = port;
        ctx->Key = info->Key;
        PIO_COMPLETION_CONTEXT expected = nullptr;
        if (__atomic_compare_exchange_n(&f->CompletionContext, &expected, ctx, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            return done(STATUS_SUCCESS);
        ExFreePool(ctx);
        ObfDereferenceObject(port);
        return done(STATUS_INVALID_PARAMETER);
    }
    // Rename/link targets are passed to the file system as given (relative names resolve in the FSD).
    return IopInformationIrp(&r, IoStatusBlock, FileInformation, Length, c, IRP_MJ_SET_INFORMATION);
}

XBAPI NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                                                  PVOID FsInformation, ULONG Length,
                                                  FS_INFORMATION_CLASS FsInformationClass) {
    if (FsInformationClass <= 0 || FsInformationClass >= FileFsMaximumInformation)
        return STATUS_INVALID_INFO_CLASS;
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, nullptr, &r);
    if (!NT_SUCCESS(status))
        return status;
    return IopInformationIrp(&r, IoStatusBlock, FsInformation, Length, FsInformationClass,
                             IRP_MJ_QUERY_VOLUME_INFORMATION);
}

XBAPI NTSTATUS NTAPI NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock) {
    IOP_REQUEST r;
    NTSTATUS status = IopBeginRequest(FileHandle, nullptr, &r);
    if (!NT_SUCCESS(status))
        return status;
    if (!r.File->WriteAccess)
        return IopAbortRequest(&r, STATUS_ACCESS_DENIED);
    return IopInformationIrp(&r, IoStatusBlock, nullptr, 0, 0, IRP_MJ_FLUSH_BUFFERS);
}

// Kernel-mode queries on a referenced file object.
static NTSTATUS IopQueryObject(PFILE_OBJECT FileObject, ULONG Class, ULONG Length, PVOID Buffer, PULONG Returned,
                               UCHAR major) {
    ObfReferenceObject(FileObject);
    IOP_REQUEST r = {FileObject, FileObject->DeviceObject, nullptr, (FileObject->Flags & FO_SYNCHRONOUS_IO) != 0};
    if (r.Synchronous)
        IopAcquireFileObjectLock(FileObject);
    IO_STATUS_BLOCK iosb;
    iosb.Information = 0;
    NTSTATUS status = IopInformationIrp(&r, &iosb, Buffer, Length, Class, major);
    if (Returned)
        *Returned = iosb.Information;
    return status;
}

XBAPI NTSTATUS NTAPI IoQueryFileInformation(PFILE_OBJECT FileObject, FILE_INFORMATION_CLASS FileInformationClass,
                                            ULONG Length, PVOID FileInformation, PULONG ReturnedLength) {
    return IopQueryObject(FileObject, FileInformationClass, Length, FileInformation, ReturnedLength,
                          IRP_MJ_QUERY_INFORMATION);
}

XBAPI NTSTATUS NTAPI IoQueryVolumeInformation(PFILE_OBJECT FileObject, FS_INFORMATION_CLASS FsInformationClass,
                                              ULONG Length, PVOID FsInformation, PULONG ReturnedLength) {
    return IopQueryObject(FileObject, FsInformationClass, Length, FsInformation, ReturnedLength,
                          IRP_MJ_QUERY_VOLUME_INFORMATION);
}

XBAPI NTSTATUS NTAPI NtDeleteFile(POBJECT_ATTRIBUTES ObjectAttributes) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = IoCreateFile(&h, DELETE, ObjectAttributes, &iosb, nullptr, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT, 0);
    if (!NT_SUCCESS(status))
        return status;
    FILE_DISPOSITION_INFORMATION d = {TRUE};
    status = NtSetInformationFile(h, &iosb, &d, sizeof(d), FileDispositionInformation);
    NtClose(h);
    return status;
}

XBAPI NTSTATUS NTAPI NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes,
                                               PFILE_NETWORK_OPEN_INFORMATION FileInformation) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = IoCreateFile(&h, FILE_READ_ATTRIBUTES, ObjectAttributes, &iosb, nullptr, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT, 0);
    if (!NT_SUCCESS(status))
        return status;
    status = NtQueryInformationFile(h, &iosb, FileInformation, sizeof(*FileInformation), FileNetworkOpenInformation);
    NtClose(h);
    return status;
}

// --- Completion ports (spec/11 §6)

struct IO_MINI_COMPLETION_PACKET {
    LIST_ENTRY ListEntry;
    ULONG PacketType; // 1 (an IRP has 0 at the same offset)
    PVOID KeyContext;
    PVOID ApcContext;
    NTSTATUS IoStatus;
    ULONG_PTR IoStatusInformation;
};
static_assert(sizeof(IO_MINI_COMPLETION_PACKET) == 0x1C);

static void IopFreeCompletionEntry(PLIST_ENTRY e) {
    auto mini = CONTAINING_RECORD(e, IO_MINI_COMPLETION_PACKET, ListEntry);
    if (mini->PacketType)
        ExFreePool(mini);
    else
        IoFreeIrp(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry));
}

void NTAPI IopDeleteIoCompletion(PVOID Object) {
    PLIST_ENTRY head = KeRundownQueue((PKQUEUE)Object);
    if (!head)
        return;
    PLIST_ENTRY e = head;
    do {
        PLIST_ENTRY next = e->Flink;
        IopFreeCompletionEntry(e);
        e = next;
    } while (e != head);
}

XBAPI NTSTATUS NTAPI NtCreateIoCompletion(PHANDLE IoCompletionHandle, ACCESS_MASK DesiredAccess,
                                          POBJECT_ATTRIBUTES ObjectAttributes, ULONG Count) {
    PKQUEUE q;
    NTSTATUS status = ObCreateObject(&IoCompletionObjectType, ObjectAttributes, sizeof(KQUEUE), (PVOID *)&q);
    if (!NT_SUCCESS(status))
        return status;
    KeInitializeQueue(q, Count);
    return ObInsertObject(q, ObjectAttributes, 0, IoCompletionHandle);
}

XBAPI NTSTATUS NTAPI IoSetIoCompletion(PVOID IoCompletion, PVOID KeyContext, PVOID ApcContext, NTSTATUS IoStatus,
                                       ULONG_PTR IoStatusInformation) {
    auto p = (IO_MINI_COMPLETION_PACKET *)ExAllocatePoolWithTag(sizeof(IO_MINI_COMPLETION_PACKET), ' pcI');
    if (!p)
        return STATUS_INSUFFICIENT_RESOURCES;
    p->PacketType = 1;
    p->KeyContext = KeyContext;
    p->ApcContext = ApcContext;
    p->IoStatus = IoStatus;
    p->IoStatusInformation = IoStatusInformation;
    KeInsertQueue((PKQUEUE)IoCompletion, &p->ListEntry);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI NtSetIoCompletion(HANDLE IoCompletionHandle, PVOID KeyContext, PVOID ApcContext,
                                       NTSTATUS IoStatus, ULONG_PTR IoStatusInformation) {
    PVOID q;
    NTSTATUS status = ObReferenceObjectByHandle(IoCompletionHandle, &IoCompletionObjectType, &q);
    if (!NT_SUCCESS(status))
        return status;
    status = IoSetIoCompletion(q, KeyContext, ApcContext, IoStatus, IoStatusInformation);
    ObfDereferenceObject(q);
    return status;
}

XBAPI NTSTATUS NTAPI NtRemoveIoCompletion(HANDLE IoCompletionHandle, PVOID *KeyContext, PVOID *ApcContext,
                                          PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER Timeout) {
    PVOID q;
    NTSTATUS status = ObReferenceObjectByHandle(IoCompletionHandle, &IoCompletionObjectType, &q);
    if (!NT_SUCCESS(status))
        return status;
    PLIST_ENTRY e = KeRemoveQueue((PKQUEUE)q, KernelMode, Timeout);
    if ((ULONG)e == (ULONG)STATUS_TIMEOUT || (ULONG)e == (ULONG)STATUS_USER_APC) {
        status = (NTSTATUS)(ULONG)e;
    } else {
        auto mini = CONTAINING_RECORD(e, IO_MINI_COMPLETION_PACKET, ListEntry);
        if (mini->PacketType) {
            *KeyContext = mini->KeyContext;
            *ApcContext = mini->ApcContext;
            IoStatusBlock->Status = mini->IoStatus;
            IoStatusBlock->Information = mini->IoStatusInformation;
            ExFreePool(mini);
        } else {
            PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
            *KeyContext = irp->Tail.CompletionKey;
            *ApcContext = irp->Overlay.AsynchronousParameters.UserApcContext;
            *IoStatusBlock = irp->IoStatus;
            IoFreeIrp(irp);
        }
        status = STATUS_SUCCESS;
    }
    ObfDereferenceObject(q);
    return status;
}

struct IO_COMPLETION_BASIC_INFORMATION {
    LONG Depth;
};

XBAPI NTSTATUS NTAPI NtQueryIoCompletion(HANDLE IoCompletionHandle, IO_COMPLETION_BASIC_INFORMATION *Information) {
    PKQUEUE q;
    NTSTATUS status = ObReferenceObjectByHandle(IoCompletionHandle, &IoCompletionObjectType, (PVOID *)&q);
    if (!NT_SUCCESS(status))
        return status;
    Information->Depth = q->Header.SignalState;
    ObfDereferenceObject(q);
    return STATUS_SUCCESS;
}

// Converts the I/O status to a Win32 error for XAPI completion routines (ordinal 232).
XBAPI ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);
XBAPI void NTAPI NtUserIoApcDispatcher(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved) {
    ULONG error = 0, bytes = 0;
    if (((ULONG)IoStatusBlock->Status & 0xC0000000) == 0xC0000000)
        error = RtlNtStatusToDosError(IoStatusBlock->Status);
    else
        bytes = IoStatusBlock->Information;
    ((void(NTAPI *)(ULONG, ULONG, PIO_STATUS_BLOCK))ApcContext)(error, bytes, IoStatusBlock);
}
