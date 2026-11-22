// I/O manager: IRP allocation, dispatch, completion and request builders (spec/stock/11 §3).
#include <io/io.h>
#include <arch/arch.h>
#include <core/core.h>
#include <runtime/runtime.h>

namespace xk::io {

XBAPI void NTAPI IoInitializeIrp(PIRP Irp, USHORT PacketSize, CCHAR StackSize) {
    memset(Irp, 0, PacketSize);
    Irp->Type = 6;
    Irp->Size = PacketSize;
    Irp->StackCount = StackSize;
    Irp->CurrentLocation = (CHAR)(StackSize + 1);
    InitializeListHead(&Irp->ThreadListEntry);
    Irp->Tail.Overlay.CurrentStackLocation = (PIO_STACK_LOCATION)(Irp + 1) + StackSize;
}

XBAPI PIRP NTAPI IoAllocateIrp(CCHAR StackSize) {
    USHORT size = (USHORT)(sizeof(IRP) + StackSize * sizeof(IO_STACK_LOCATION));
    auto irp = (PIRP)ExAllocatePoolWithTag(size, ' prI');
    if (irp)
        IoInitializeIrp(irp, size, StackSize);
    return irp;
}

XBAPI void NTAPI IoFreeIrp(PIRP Irp) {
    ExFreePool(Irp);
}

// Link into / unlink from the issuing thread's IRP list (APC_LEVEL keeps completion APCs out).
void QueueThreadIrp(PIRP Irp) {
    KIRQL old = KfRaiseIrql(APC_LEVEL);
    InsertHeadList(&((PETHREAD)Irp->Tail.Overlay.Thread)->IrpList, &Irp->ThreadListEntry);
    KfLowerIrql(old);
}

static void DequeueThreadIrp(PIRP Irp) {
    RemoveEntryList(&Irp->ThreadListEntry);
    InitializeListHead(&Irp->ThreadListEntry);
}

XBAPI void NTAPI IoQueueThreadIrp(PIRP Irp) {
    QueueThreadIrp(Irp);
}

XBAPI void NTAPI IoMarkIrpMustComplete(PIRP Irp) {
    // Accepted, nothing to do: the Xbox never cancels a thread's IRPs (spec/stock/11).
}

void LockUserBuffer(PIRP Irp, ULONG Length) {
    MmLockUnlockBufferPages(Irp->UserBuffer, Length, FALSE);
    Irp->LockedBufferLength = Length;
    Irp->Flags |= IRP_UNLOCK_USER_BUFFER;
}

XBAPI NTSTATUS FASTCALL IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (--Irp->CurrentLocation <= 0)
        KeBugCheckEx(0x35 /*NO_MORE_IRP_STACK_LOCATIONS*/, (ULONG_PTR)Irp, 0, 0, 0);
    PIO_STACK_LOCATION stack = --Irp->Tail.Overlay.CurrentStackLocation;
    stack->DeviceObject = DeviceObject;
    return CallTitleRoutine<NTSTATUS>((PVOID)DeviceObject->DriverObject->MajorFunction[stack->MajorFunction], DeviceObject, Irp);
}

// --- Completion

static void NTAPI UserApcKernelRoutine(PKAPC Apc, PKNORMAL_ROUTINE *, PVOID *, PVOID *, PVOID *) {
    IoFreeIrp(CONTAINING_RECORD(Apc, IRP, Tail.Apc));
}

static void NTAPI UserApcRundown(PKAPC Apc) {
    IoFreeIrp(CONTAINING_RECORD(Apc, IRP, Tail.Apc));
}

static void QueueToPort(PIRP Irp, PIO_COMPLETION_CONTEXT Context) {
    Irp->Tail.CompletionKey = Context->Key;
    Irp->Tail.Overlay.PacketType = 0;
    KeInsertQueue((PKQUEUE)Context->Port, &Irp->Tail.Overlay.ListEntry);
}

// Final stage of I/O completion, in the context of the thread that issued the request (APC_LEVEL). Runs as the IRP's kernel APC, or inline for deferred
// completions.
void NTAPI CompleteRequest(PKAPC Apc, PKNORMAL_ROUTINE *, PVOID *, PVOID *SystemArgument1, PVOID *) {
    PIRP irp = CONTAINING_RECORD(Apc, IRP, Tail.Apc);
    auto fo = (PFILE_OBJECT)*SystemArgument1;
    NTSTATUS status = irp->IoStatus.Status;
    bool syncApi = irp->Flags & IRP_SYNCHRONOUS_API;

    if (((ULONG)status & 0xC0000000) == 0xC0000000 &&
        (!irp->PendingReturned || syncApi || (fo && (fo->Flags & FO_SYNCHRONOUS_IO)))) {
        // Errors returned synchronously are reported by the service's return value only. A pended error for a synchronous caller still wakes the waiter.
        if (irp->PendingReturned && fo) {
            if (syncApi) {
                *irp->UserIosb = irp->IoStatus;
                KeSetEvent(irp->UserEvent ? irp->UserEvent : &fo->Event, 0, FALSE);
            } else {
                fo->FinalStatus = status;
                KeSetEvent(&fo->Event, 0, FALSE);
            }
        }
        if (fo && !(irp->Flags & IRP_CREATE_OPERATION))
            ObfDereferenceObject(fo);
        if (irp->UserEvent && fo && !syncApi)
            ObfDereferenceObject(irp->UserEvent);
        DequeueThreadIrp(irp);
        IoFreeIrp(irp);
        return;
    }

    PIO_COMPLETION_CONTEXT port = fo ? fo->CompletionContext : nullptr;
    bool keepFileReference = false;
    if (irp->UserIosb)
        *irp->UserIosb = irp->IoStatus;
    if (irp->UserEvent) {
        KeSetEvent(irp->UserEvent, 0, FALSE);
        if (fo) {
            if (!syncApi)
                ObfDereferenceObject(irp->UserEvent);
            if (fo->Flags & FO_SYNCHRONOUS_IO) {
                fo->FinalStatus = status;
                KeSetEvent(&fo->Event, 0, FALSE);
            }
        }
    } else if (fo) {
        fo->FinalStatus = status;
        KeSetEvent(&fo->Event, 0, FALSE);
    }
    if (fo && (irp->Flags & IRP_CREATE_OPERATION)) {
        keepFileReference = true; // the create path owns the file object
        irp->Overlay.AsynchronousParameters.UserApcRoutine = nullptr;
    }
    DequeueThreadIrp(irp);

    PIO_APC_ROUTINE apcRoutine = irp->Overlay.AsynchronousParameters.UserApcRoutine;
    PVOID apcContext = irp->Overlay.AsynchronousParameters.UserApcContext;
    if (apcRoutine) {
        // The APC calls it as its normal routine: three 32-bit arguments, stdcall, the same call. Cast through void (*)(), the generic function pointer,
        // which clang 19+ accepts (-Wcast-function-type-mismatch).
        KeInitializeApc(&irp->Tail.Apc, KeGetCurrentThread(), UserApcKernelRoutine, UserApcRundown, (PKNORMAL_ROUTINE)(void (*)())apcRoutine,
                        apcRoutine == NtUserIoApcDispatcher ? UserMode : KernelMode, apcContext);
        KeInsertQueueApc(&irp->Tail.Apc, irp->UserIosb, nullptr, 2);
    } else if (port && apcContext) {
        QueueToPort(irp, port);
    } else {
        IoFreeIrp(irp);
    }
    if (fo && !keepFileReference)
        ObfDereferenceObject(fo);
}

// Deferred completion: the dispatch routine returned without pending, finish in the caller (spec/stock/11 §5).
void CompleteRequestInline(PIRP Irp, PFILE_OBJECT FileObject) {
    KIRQL old = KfRaiseIrql(APC_LEVEL);
    PVOID arg = FileObject;
    CompleteRequest(&Irp->Tail.Apc, nullptr, nullptr, &arg, nullptr);
    KfLowerIrql(old);
}

XBAPI void FASTCALL IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost) {
    if (Irp->CurrentLocation > Irp->StackCount + 1)
        KeBugCheckEx(0x44 /*MULTIPLE_IRP_COMPLETE_REQUESTS*/, (ULONG_PTR)Irp, 0, 0, 0);

    for (PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp); Irp->CurrentLocation <= Irp->StackCount;
         stack++) {
        Irp->CurrentLocation++;
        Irp->Tail.Overlay.CurrentStackLocation++;
        Irp->PendingReturned = stack->Control & SL_PENDING_RETURNED;
        bool ok = NT_SUCCESS(Irp->IoStatus.Status);
        if ((ok && (stack->Control & SL_INVOKE_ON_SUCCESS)) || (!ok && (stack->Control & SL_INVOKE_ON_ERROR)) || (Irp->Cancel &&
            (stack->Control & SL_INVOKE_ON_CANCEL))) {
            PDEVICE_OBJECT upper = Irp->CurrentLocation == Irp->StackCount + 1 ? nullptr : (stack + 1)->DeviceObject;
            PIO_COMPLETION_ROUTINE routine = stack->CompletionRoutine;
            PVOID context = stack->Context;
            memset(stack, 0, sizeof(*stack));
            if (CallTitleRoutine<NTSTATUS>((PVOID)routine, upper, Irp, context) == STATUS_MORE_PROCESSING_REQUIRED)
                return;
        } else {
            if (Irp->PendingReturned && Irp->CurrentLocation <= Irp->StackCount)
                IoMarkIrpPending(Irp);
            memset(stack, 0, sizeof(*stack));
        }
    }

    if (Irp->Flags & (IRP_CLOSE_OPERATION | IRP_PAGING_IO)) {
        if (Irp->UserIosb)
            *Irp->UserIosb = Irp->IoStatus;
        if (Irp->UserEvent)
            KeSetEvent(Irp->UserEvent, PriorityBoost, FALSE);
        if (Irp->Flags & IRP_PAGING_IO)
            IoFreeIrp(Irp);
        return;
    }
    if (Irp->Flags & IRP_UNLOCK_USER_BUFFER) {
        MmLockUnlockBufferPages(Irp->UserBuffer, Irp->LockedBufferLength, TRUE);
        Irp->Flags &= ~IRP_UNLOCK_USER_BUFFER;
    }
    if ((Irp->Flags & IRP_DEFER_IO_COMPLETION) && !Irp->PendingReturned)
        return;

    PKTHREAD thread = Irp->Tail.Overlay.Thread;
    PFILE_OBJECT fo = Irp->Tail.Overlay.OriginalFileObject;
    KeInitializeApc(&Irp->Tail.Apc, thread, CompleteRequest, nullptr, nullptr, KernelMode, nullptr);
    KeInsertQueueApc(&Irp->Tail.Apc, fo, nullptr, PriorityBoost);
}

// --- Request builders

XBAPI PIRP NTAPI IoBuildAsynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
                                               PIO_STATUS_BLOCK IoStatusBlock) {
    PIRP irp = IoAllocateIrp(DeviceObject->StackSize);
    if (!irp)
        return nullptr;
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = (UCHAR)MajorFunction;
    if (MajorFunction != IRP_MJ_FLUSH_BUFFERS && MajorFunction != IRP_MJ_SHUTDOWN) {
        irp->UserBuffer = Buffer;
        if ((DeviceObject->Flags & DO_DIRECT_IO) && Length)
            LockUserBuffer(irp, Length);
        stack->Parameters.Read.Length = Length;
        stack->Parameters.Read.ByteOffset = *StartingOffset;
    }
    irp->UserIosb = IoStatusBlock;
    return irp;
}

XBAPI PIRP NTAPI IoBuildSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
                                              PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock) {
    PIRP irp = IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset, IoStatusBlock);
    if (irp) {
        irp->UserEvent = Event;
        QueueThreadIrp(irp);
    }
    return irp;
}

XBAPI PIRP NTAPI IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer,
                                               ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl, PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock) {
    PIRP irp = IoAllocateIrp(DeviceObject->StackSize);
    if (!irp)
        return nullptr;
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = InternalDeviceIoControl ? IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    stack->Parameters.DeviceIoControl.InputBuffer = InputBuffer;
    stack->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    stack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    irp->UserBuffer = OutputBuffer;
    ULONG method = IoControlCode & 3;
    if ((method == 1 || method == 2) && OutputBuffer && OutputBufferLength)
        LockUserBuffer(irp, OutputBufferLength); // METHOD_IN/OUT_DIRECT
    irp->UserIosb = IoStatusBlock;
    irp->UserEvent = Event;
    QueueThreadIrp(irp);
    return irp;
}

XBAPI NTSTATUS NTAPI IoSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset) {
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb;
    PIRP irp = IoBuildSynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset, &event, &iosb);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = IofCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = iosb.Status;
    }
    return status;
}

XBAPI NTSTATUS NTAPI IoSynchronousDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject, PVOID InputBuffer, ULONG InputBufferLength,
                                                         PVOID OutputBuffer, ULONG OutputBufferLength, PULONG ReturnedOutputBufferLength,
                                                         BOOLEAN InternalDeviceIoControl) {
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb;
    iosb.Information = 0;
    PIRP irp = IoBuildDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength,
                                             InternalDeviceIoControl, &event, &iosb);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = IofCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = iosb.Status;
    }
    if (ReturnedOutputBufferLength)
        *ReturnedOutputBufferLength = iosb.Information;
    return status;
}

} // namespace xk::io
