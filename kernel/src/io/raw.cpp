// RAW file system: whole-volume access when no file system recognises the media (spec/11 §4).
#include <xk/io.h>

struct RAW_VOLUME {
    PDEVICE_OBJECT TargetDevice;
    SHARE_ACCESS ShareAccess;
    KEVENT Lock;
};

static NTSTATUS RawComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information = 0) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IofCompleteRequest(Irp, 0);
    return Status;
}

static NTSTATUS NTAPI RawCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    auto v = (RAW_VOLUME *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    if (s->Parameters.Create.RemainingName->Length || s->FileObject->RelatedFileObject)
        return RawComplete(Irp, STATUS_OBJECT_PATH_NOT_FOUND);
    ULONG disposition = s->Parameters.Create.Options >> 24;
    if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF)
        return RawComplete(Irp, STATUS_ACCESS_DENIED);
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = IoCheckShareAccess(s->Parameters.Create.DesiredAccess, s->Parameters.Create.ShareAccess,
                                         s->FileObject, &v->ShareAccess, TRUE);
    KeSetEvent(&v->Lock, 0, FALSE);
    return RawComplete(Irp, status, NT_SUCCESS(status) ? 1 /*FILE_OPENED*/ : 0);
}

static NTSTATUS NTAPI RawCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    auto v = (RAW_VOLUME *)DeviceObject->DeviceExtension;
    KeWaitForSingleObject(&v->Lock, Executive, KernelMode, FALSE, nullptr);
    IoRemoveShareAccess(IoGetCurrentIrpStackLocation(Irp)->FileObject, &v->ShareAccess);
    KeSetEvent(&v->Lock, 0, FALSE);
    return RawComplete(Irp, STATUS_SUCCESS);
}

static NTSTATUS NTAPI RawSuccess(PDEVICE_OBJECT, PIRP Irp) { return RawComplete(Irp, STATUS_SUCCESS); }

static NTSTATUS NTAPI RawPassDown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IofCallDriver(((RAW_VOLUME *)DeviceObject->DeviceExtension)->TargetDevice, Irp);
}

static NTSTATUS NTAPI RawQueryVolume(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    auto out = (PULONG)Irp->UserBuffer;
    if (s->Parameters.QueryVolume.FsInformationClass == FileFsDeviceInformation &&
        s->Parameters.QueryVolume.Length >= 8) {
        out[0] = ((RAW_VOLUME *)DeviceObject->DeviceExtension)->TargetDevice->DeviceType;
        out[1] = 0;
        return RawComplete(Irp, STATUS_SUCCESS, 8);
    }
    return RawComplete(Irp, STATUS_INVALID_PARAMETER);
}

static NTSTATUS NTAPI RawDismountVolume(PDEVICE_OBJECT DeviceObject) {
    auto v = (RAW_VOLUME *)DeviceObject->DeviceExtension;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (v->TargetDevice->MountedOrSelfDevice == DeviceObject)
        v->TargetDevice->MountedOrSelfDevice = nullptr;
    KfLowerIrql(old);
    IoDeleteDevice(DeviceObject);
    return STATUS_SUCCESS;
}

static void NTAPI RawDeleteVolume(PDEVICE_OBJECT DeviceObject) {
    ObfDereferenceObject(((RAW_VOLUME *)DeviceObject->DeviceExtension)->TargetDevice);
}

static DRIVER_OBJECT RawDriverObject = {
    nullptr,
    RawDeleteVolume,
    RawDismountVolume,
    {RawCreate, RawSuccess, RawPassDown, RawPassDown, IoInvalidDeviceRequest, IoInvalidDeviceRequest, RawPassDown,
     RawQueryVolume, IoInvalidDeviceRequest, IoInvalidDeviceRequest, RawPassDown, IoInvalidDeviceRequest,
     IoInvalidDeviceRequest, RawCleanup}};

NTSTATUS RawMountVolume(PDEVICE_OBJECT TargetDevice) {
    PDEVICE_OBJECT dev;
    NTSTATUS status = IoCreateDevice(&RawDriverObject, sizeof(RAW_VOLUME), nullptr, 8, FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->StackSize = (CHAR)(dev->StackSize + TargetDevice->StackSize);
    dev->AlignmentRequirement = TargetDevice->AlignmentRequirement;
    dev->SectorSize = TargetDevice->SectorSize;
    dev->Flags |= TargetDevice->Flags & (DO_DIRECT_IO | 0x40);
    auto v = (RAW_VOLUME *)dev->DeviceExtension;
    v->TargetDevice = TargetDevice;
    KeInitializeEvent(&v->Lock, SynchronizationEvent, TRUE);
    ObfReferenceObject(TargetDevice);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
