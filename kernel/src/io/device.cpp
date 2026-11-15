// I/O manager: device objects, symbolic links, StartIo queueing, share access (spec/stock/11 §2).
#include <io/io.h>
#include <runtime/runtime.h>
#include <filesystems/filesystems.h>

namespace xk::io {

static KEVENT DeviceDefaultObject; // devices are not waitable: waits complete at once

// The I/O manager's object types (their routines are in io/file.cpp and below).
XBAPI OBJECT_TYPE IoDeviceObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr, ParseDevice, &DeviceDefaultObject, 'iveD'};
XBAPI OBJECT_TYPE IoFileObjectType = {ExAllocatePoolWithTag, ExFreePool, CloseFileObject, DeleteFileObject, ParseFile, (PVOID)__builtin_offsetof(FILE_OBJECT,
                                      Event), 'eliF'};
XBAPI OBJECT_TYPE IoCompletionObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, DeleteIoCompletion, nullptr, (PVOID)0, 'pmoC'};

IO_FILE_SYSTEM RegisteredFileSystems; // the mount routines, filled in by the file systems (filesystems/mount.cpp)

// --- Devices and symbolic links

void InitIoSystem() {
    KeInitializeEvent(&DeviceDefaultObject, NotificationEvent, TRUE);
    filesystems::InitCache();
    filesystems::HDDFSInitialize();
}

static bool IsMountableDeviceType(ULONG type) {
    return type == FILE_DEVICE_DISK || type == FILE_DEVICE_CD_ROM || type == FILE_DEVICE_MEMORY_UNIT || type == FILE_DEVICE_MEDIA_BOARD;
}

XBAPI NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, POBJECT_STRING DeviceName, ULONG DeviceType, BOOLEAN Exclusive,
                                    PDEVICE_OBJECT *DeviceObject) {
    *DeviceObject = nullptr;
    OBJECT_ATTRIBUTES attrs = {nullptr, DeviceName, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE};
    ULONG size = sizeof(DEVICE_OBJECT) + ((DeviceExtensionSize + 3) & ~3u);
    PDEVICE_OBJECT dev;
    NTSTATUS status = ObCreateObject(&IoDeviceObjectType, DeviceName ? &attrs : nullptr, size, (PVOID *)&dev);
    if (!NT_SUCCESS(status))
        return status;
    memset(dev, 0, size);
    dev->Type = 3;
    dev->Size = (USHORT)size;
    dev->DriverObject = DriverObject;
    dev->Flags = DO_DEVICE_INITIALIZING | (Exclusive ? DO_EXCLUSIVE : 0) | (DeviceName ? DO_DEVICE_HAS_NAME : 0);
    dev->DeviceExtension = DeviceExtensionSize ? dev + 1 : nullptr;
    dev->DeviceType = (UCHAR)DeviceType;
    dev->StackSize = 1;
    KeInitializeDeviceQueue(&dev->DeviceQueue);
    if (IsMountableDeviceType(DeviceType))
        KeInitializeEvent(&dev->DeviceLock, SynchronizationEvent, TRUE);
    else
        dev->MountedOrSelfDevice = dev;
    HANDLE h;
    status = ObInsertObject(dev, DeviceName ? &attrs : nullptr, 1, &h);
    if (!NT_SUCCESS(status))
        return status;
    NtClose(h); // the bias reference keeps the device until IoDeleteDevice
    *DeviceObject = dev;
    return STATUS_SUCCESS;
}

// Last reference to a deleted device: let the driver tear down, then drop the creation reference.
static void CompleteUnload(PDEVICE_OBJECT DeviceObject) {
    if (DeviceObject->DriverObject && DeviceObject->DriverObject->DriverDeleteDevice)
        DeviceObject->DriverObject->DriverDeleteDevice(DeviceObject);
    ObfDereferenceObject(DeviceObject);
}

void DereferenceDevice(PDEVICE_OBJECT DeviceObject) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    bool unload = --DeviceObject->ReferenceCount == 0 && DeviceObject->DeletePending;
    KfLowerIrql(old);
    if (unload)
        CompleteUnload(DeviceObject);
}

XBAPI void NTAPI IoDeleteDevice(PDEVICE_OBJECT DeviceObject) {
    if (DeviceObject->Flags & DO_DEVICE_HAS_NAME)
        ObMakeTemporaryObject(DeviceObject);
    KIRQL old = KeRaiseIrqlToDpcLevel();
    DeviceObject->DeletePending = TRUE;
    bool unload = DeviceObject->ReferenceCount == 0;
    KfLowerIrql(old);
    if (unload)
        CompleteUnload(DeviceObject);
}

XBAPI NTSTATUS NTAPI IoCreateSymbolicLink(POBJECT_STRING SymbolicLinkName, POBJECT_STRING DeviceName) {
    return objects::CreateSymbolicLink(SymbolicLinkName, DeviceName);
}

XBAPI NTSTATUS NTAPI IoDeleteSymbolicLink(POBJECT_STRING SymbolicLinkName) {
    OBJECT_ATTRIBUTES attrs = {nullptr, SymbolicLinkName, OBJ_CASE_INSENSITIVE};
    HANDLE h;
    NTSTATUS status = NtOpenSymbolicLinkObject(&h, &attrs);
    if (!NT_SUCCESS(status))
        return status;
    PVOID link;
    ObReferenceObjectByHandle(h, &ObSymbolicLinkObjectType, &link);
    ObMakeTemporaryObject(link);
    ObfDereferenceObject(link);
    NtClose(h);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI IoInvalidDeviceRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IofCompleteRequest(Irp, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

// --- StartIo serialisation (all at DISPATCH_LEVEL)

static void StartIo(PDEVICE_OBJECT dev, PIRP irp) {
    dev->CurrentIrp = irp;
    dev->StartIoFlags |= DO_STARTIO_BUSY;
    dev->DriverObject->DriverStartIo(dev, irp);
    dev->StartIoFlags &= ~DO_STARTIO_BUSY;
}

static void StartNextPacket(PDEVICE_OBJECT dev, bool byKey, ULONG key) {
    for (;;) {
        dev->CurrentIrp = nullptr;
        PKDEVICE_QUEUE_ENTRY e = byKey ? KeRemoveByKeyDeviceQueue(&dev->DeviceQueue, key) : KeRemoveDeviceQueue(&dev->DeviceQueue);
        if (!e)
            return;
        StartIo(dev, CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry));
        // A StartNextPacket requested from inside StartIo runs here instead of recursing.
        if (!(dev->StartIoFlags & DO_STARTIO_REQUESTED))
            return;
        dev->StartIoFlags &= ~DO_STARTIO_REQUESTED;
        byKey = dev->StartIoFlags & DO_STARTIO_REQUESTED_BYKEY;
        key = dev->StartIoKey;
    }
}

XBAPI void NTAPI IoStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp, PULONG Key) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN queued = Key ? KeInsertByKeyDeviceQueue(&DeviceObject->DeviceQueue, &Irp->Tail.Overlay.DeviceQueueEntry,
                                                    *Key) : KeInsertDeviceQueue(&DeviceObject->DeviceQueue, &Irp->Tail.Overlay.DeviceQueueEntry);
    if (!queued) {
        StartIo(DeviceObject, Irp);
        if (DeviceObject->StartIoFlags & DO_STARTIO_REQUESTED) {
            DeviceObject->StartIoFlags &= ~DO_STARTIO_REQUESTED;
            StartNextPacket(DeviceObject, DeviceObject->StartIoFlags & DO_STARTIO_REQUESTED_BYKEY, DeviceObject->StartIoKey);
        }
    }
    KfLowerIrql(old);
}

static void RequestStartNextPacket(PDEVICE_OBJECT dev, bool byKey, ULONG key) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (dev->StartIoFlags & DO_STARTIO_BUSY) {
        dev->StartIoFlags = (UCHAR)((dev->StartIoFlags & ~DO_STARTIO_REQUESTED_BYKEY) | DO_STARTIO_REQUESTED | (byKey ? DO_STARTIO_REQUESTED_BYKEY : 0));
        dev->StartIoKey = key;
    } else {
        StartNextPacket(dev, byKey, key);
    }
    KfLowerIrql(old);
}

XBAPI void NTAPI IoStartNextPacket(PDEVICE_OBJECT DeviceObject) {
    RequestStartNextPacket(DeviceObject, false, 0);
}

XBAPI void NTAPI IoStartNextPacketByKey(PDEVICE_OBJECT DeviceObject, ULONG Key) {
    RequestStartNextPacket(DeviceObject, true, Key);
}

// --- Share access (byte counters)

static void ComputeAccess(ACCESS_MASK access, ULONG share, PFILE_OBJECT f) {
    f->ReadAccess = (access & (FILE_READ_DATA | FILE_EXECUTE)) != 0;
    f->WriteAccess = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    f->DeleteAccess = (access & DELETE) != 0;
    bool any = f->ReadAccess || f->WriteAccess || f->DeleteAccess;
    f->SharedRead = any && (share & FILE_SHARE_READ);
    f->SharedWrite = any && (share & FILE_SHARE_WRITE);
    f->SharedDelete = any && (share & FILE_SHARE_DELETE);
}

static void AddShare(PFILE_OBJECT f, PSHARE_ACCESS s) {
    s->OpenCount++;
    s->Readers += f->ReadAccess;
    s->Writers += f->WriteAccess;
    s->Deleters += f->DeleteAccess;
    s->SharedRead += f->SharedRead;
    s->SharedWrite += f->SharedWrite;
    s->SharedDelete += f->SharedDelete;
}

XBAPI NTSTATUS NTAPI IoCheckShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess,
                                        BOOLEAN Update) {
    ComputeAccess(DesiredAccess, DesiredShareAccess, FileObject);
    PFILE_OBJECT f = FileObject;
    if (!(f->ReadAccess || f->WriteAccess || f->DeleteAccess))
        return STATUS_SUCCESS;
    PSHARE_ACCESS s = ShareAccess;
    UCHAR n = s->OpenCount;
    if (n == 0xFF || (f->ReadAccess && s->SharedRead < n) || (f->WriteAccess && s->SharedWrite < n) || (f->DeleteAccess && s->SharedDelete < n) ||
        (s->Readers && !(DesiredShareAccess & FILE_SHARE_READ)) || (s->Writers && !(DesiredShareAccess & FILE_SHARE_WRITE)) || (s->Deleters &&
        !(DesiredShareAccess & FILE_SHARE_DELETE)))
        return STATUS_SHARING_VIOLATION;
    if (Update)
        AddShare(f, s);
    return STATUS_SUCCESS;
}

XBAPI void NTAPI IoSetShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess) {
    ComputeAccess(DesiredAccess, DesiredShareAccess, FileObject);
    PFILE_OBJECT f = FileObject;
    if (f->ReadAccess || f->WriteAccess || f->DeleteAccess) {
        memset(ShareAccess, 0, sizeof(*ShareAccess));
        AddShare(f, ShareAccess);
    } else {
        memset(ShareAccess, 0, sizeof(*ShareAccess));
    }
}

XBAPI void NTAPI IoRemoveShareAccess(PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess) {
    PFILE_OBJECT f = FileObject;
    if (!(f->ReadAccess || f->WriteAccess || f->DeleteAccess))
        return;
    PSHARE_ACCESS s = ShareAccess;
    s->OpenCount--;
    s->Readers -= f->ReadAccess;
    s->Writers -= f->WriteAccess;
    s->Deleters -= f->DeleteAccess;
    s->SharedRead -= f->SharedRead;
    s->SharedWrite -= f->SharedWrite;
    s->SharedDelete -= f->SharedDelete;
}

// --- Dismounting (spec/stock/11 §2)

// Dismount the file system mounted on DeviceObject (spec/stock/11 §2): the volume driver detaches it; the volume device is released when its last reference
// goes.
XBAPI NTSTATUS NTAPI IoDismountVolume(PDEVICE_OBJECT DeviceObject) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PDEVICE_OBJECT vol = DeviceObject->MountedOrSelfDevice;
    NTSTATUS status = STATUS_SUCCESS;
    if (!vol) {
        status = (NTSTATUS)0xC000026E; // STATUS_VOLUME_DISMOUNTED
    } else if (!vol->DriverObject->DriverDismountVolume) {
        status = STATUS_INVALID_DEVICE_REQUEST;
        vol = nullptr;
    } else {
        vol->ReferenceCount++;
    }
    KfLowerIrql(old);
    if (vol) {
        status = vol->DriverObject->DriverDismountVolume(vol);
        DereferenceDevice(vol);
    }
    return status;
}

// FILE_SYSTEM_CONTROL for file systems whose only control is FSCTL_DISMOUNT_VOLUME (RAW, XDVDFS, UDF; the reference's 0x8002aef5): the driver's dismount
// routine, anything else is an invalid request.
NTSTATUS NTAPI DismountOnlyFsControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    if (IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.FsControlCode == FSCTL_DISMOUNT_VOLUME)
        status = DeviceObject->DriverObject->DriverDismountVolume(DeviceObject);
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IofCompleteRequest(Irp, 0);
    return status;
}

XBAPI NTSTATUS NTAPI IoDismountVolumeByName(POBJECT_STRING DeviceName) {
    PVOID dev;
    NTSTATUS status = ObReferenceObjectByName(DeviceName, 0, &IoDeviceObjectType, nullptr, &dev);
    if (NT_SUCCESS(status)) {
        status = IoDismountVolume((PDEVICE_OBJECT)dev);
        ObfDereferenceObject(dev);
    }
    return status;
}

// --- OXOS's own devices and links, by name (io.h)

NTSTATUS CreateLink(const char *Link, const char *Target) {
    OBJECT_STRING link = objects::ObjectName(Link), target = objects::ObjectName(Target);
    return objects::CreateSymbolicLink(&link, &target);
}

NTSTATUS DeleteLink(const char *Link) {
    OBJECT_STRING link = objects::ObjectName(Link);
    return IoDeleteSymbolicLink(&link);
}

NTSTATUS CreateDevice(PDRIVER_OBJECT Driver, ULONG ExtensionSize, const char *Name, ULONG Type, PDEVICE_OBJECT *Device) {
    OBJECT_STRING name = objects::ObjectName(Name);
    return IoCreateDevice(Driver, ExtensionSize, &name, Type, FALSE, Device);
}

} // namespace xk::io
