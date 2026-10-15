// Boot self-tests for the I/O manager (milestone 3b): a RAM-disk test driver behind \Device\XkRam.
#include <xk/io.h>
#include <xk/ps.h>
#include <xk/rtl.h>

volatile ULONG KiTestIoOpenOk, KiTestIoSyncRwOk, KiTestIoQueryOk, KiTestIoAsyncOk, KiTestIoApcOk, KiTestIoPortOk,
    KiTestIoIoctlOk, KiTestIoRelativeOk, KiTestIoErrorsOk, KiTestIoStartIoOk, KiTestIoCloseOk, KiTestIoDeleteOk,
    KiTestIoPoolBalanced, KiTestIoDone;
extern volatile ULONG ExpPoolBlocks;

constexpr ULONG RAM_SIZE = 0x1000, PEND_OFFSET = 0x800; // reads at/after PEND_OFFSET complete from a DPC
constexpr ULONG IOCTL_XK_INCREMENT = 0x00220000 | (0x801 << 2); // METHOD_BUFFERED

static DRIVER_OBJECT RamDriver, QueueDriver;
static PDEVICE_OBJECT RamDevice, QueueDevice;
static char LastRemaining[64];
static PFILE_OBJECT LastRelated;
static volatile ULONG Creates, Cleanups, Closes;

static SHARE_ACCESS RamShare, QueueShare;

static void Sleep(LONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static PUCHAR RamData() { return (PUCHAR)RamDevice->DeviceExtension; }

static void RamTransfer(PIRP irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG offset = s->Parameters.Read.ByteOffset.LowPart, len = s->Parameters.Read.Length;
    if (s->Parameters.Read.ByteOffset.HighPart || offset >= RAM_SIZE) {
        irp->IoStatus.Status = STATUS_END_OF_FILE;
        irp->IoStatus.Information = 0;
        return;
    }
    if (len > RAM_SIZE - offset)
        len = RAM_SIZE - offset;
    if (s->MajorFunction == IRP_MJ_READ)
        RtlCopyMemory(irp->UserBuffer, RamData() + offset, len);
    else
        RtlCopyMemory(RamData() + offset, irp->UserBuffer, len);
    if (s->FileObject->Flags & FO_SYNCHRONOUS_IO)
        s->FileObject->CurrentByteOffset.QuadPart = offset + len;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = len;
}

static KDPC RamDpc;
static PIRP RamPendingIrp;
static void NTAPI RamDpcRoutine(PKDPC, PVOID, PVOID, PVOID) {
    PIRP irp = RamPendingIrp;
    RamPendingIrp = nullptr;
    RamTransfer(irp);
    IofCompleteRequest(irp, 1);
}

static NTSTATUS NTAPI RamDispatch(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    switch (s->MajorFunction) {
    case IRP_MJ_CREATE: {
        status = IoCheckShareAccess(s->Parameters.Create.DesiredAccess, s->Parameters.Create.ShareAccess,
                                    s->FileObject, &RamShare, TRUE);
        if (!NT_SUCCESS(status))
            break;
        POBJECT_STRING n = s->Parameters.Create.RemainingName;
        ULONG len = n->Length < sizeof(LastRemaining) - 1 ? n->Length : sizeof(LastRemaining) - 1;
        RtlCopyMemory(LastRemaining, n->Buffer, len);
        LastRemaining[len] = 0;
        LastRelated = s->FileObject->RelatedFileObject;
        Creates = Creates + 1;
        irp->IoStatus.Information = 1; // FILE_OPENED
        break;
    }
    case IRP_MJ_CLEANUP:
        IoRemoveShareAccess(s->FileObject, &RamShare);
        Cleanups = Cleanups + 1;
        break;
    case IRP_MJ_CLOSE:
        Closes = Closes + 1;
        break;
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
        if (s->Parameters.Read.ByteOffset.QuadPart >= PEND_OFFSET && s->Parameters.Read.ByteOffset.QuadPart < RAM_SIZE) {
            IoMarkIrpPending(irp);
            RamPendingIrp = irp;
            KeInsertQueueDpc(&RamDpc, nullptr, nullptr);
            return STATUS_PENDING;
        }
        RamTransfer(irp);
        status = irp->IoStatus.Status;
        IofCompleteRequest(irp, 0);
        return status;
    case IRP_MJ_DEVICE_CONTROL:
        if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_XK_INCREMENT &&
            s->Parameters.DeviceIoControl.InputBufferLength >= 4 && s->Parameters.DeviceIoControl.OutputBufferLength >= 4) {
            *(PULONG)irp->UserBuffer = *(PULONG)s->Parameters.DeviceIoControl.InputBuffer + 1;
            irp->IoStatus.Information = 4;
        } else {
            status = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    case IRP_MJ_QUERY_INFORMATION:
        if (s->Parameters.QueryFile.FileInformationClass == FileStandardInformation) {
            auto p = (PULONG)irp->UserBuffer; // {AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory}
            RtlZeroMemory(p, 24);
            p[0] = p[2] = RAM_SIZE;
            p[4] = 1;
            irp->IoStatus.Information = 24;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    default:
        return IoInvalidDeviceRequest(dev, irp);
    }
    irp->IoStatus.Status = status;
    IofCompleteRequest(irp, 0);
    return status;
}

// --- StartIo-serialised device: every read is queued; a DPC completes it and starts the next one.
static ULONG StartOrder[4], StartCount;
static KDPC QueueDpc;
static void NTAPI QueueDpcRoutine(PKDPC, PVOID, PVOID dev, PVOID) {
    PDEVICE_OBJECT d = (PDEVICE_OBJECT)dev;
    PIRP irp = d->CurrentIrp;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = IoGetCurrentIrpStackLocation(irp)->Parameters.Read.ByteOffset.LowPart;
    IoStartNextPacket(d);
    IofCompleteRequest(irp, 0);
}
static void NTAPI QueueStartIo(PDEVICE_OBJECT dev, PIRP irp) {
    if (StartCount < 4)
        StartOrder[StartCount++] = IoGetCurrentIrpStackLocation(irp)->Parameters.Read.ByteOffset.LowPart;
    KeInsertQueueDpc(&QueueDpc, dev, nullptr);
}
static NTSTATUS NTAPI QueueDispatch(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (s->MajorFunction == IRP_MJ_READ) {
        IoMarkIrpPending(irp);
        IoStartPacket(dev, irp, nullptr);
        return STATUS_PENDING;
    }
    if (s->MajorFunction == IRP_MJ_CREATE)
        IoSetShareAccess(s->Parameters.Create.DesiredAccess, s->Parameters.Create.ShareAccess, s->FileObject,
                         &QueueShare);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}

static volatile ULONG ApcRan;
static IO_STATUS_BLOCK *ApcIosbSeen;
static void NTAPI TestIoApc(PVOID ctx, PIO_STATUS_BLOCK iosb, ULONG) {
    ApcIosbSeen = iosb;
    ApcRan = (ULONG)ctx;
}

static OBJECT_ATTRIBUTES Attrs(OBJECT_STRING *s, PCSZ text, HANDLE root = nullptr) {
    RtlInitAnsiString(s, text);
    return OBJECT_ATTRIBUTES{root, s, OBJ_CASE_INSENSITIVE};
}

static void NTAPI IoControllerThread(PVOID) {
    extern volatile ULONG KiTestObDone;
    while (!KiTestObDone) // after the object-manager suite: both measure pool balances
        Sleep(10);
    ULONG poolBefore = ExpPoolBlocks;

    // Devices.
    for (auto &f : RamDriver.MajorFunction)
        f = RamDispatch;
    OBJECT_STRING dn, ln, qn;
    RtlInitAnsiString(&dn, "\\Device\\XkRam");
    bool ok = IoCreateDevice(&RamDriver, RAM_SIZE, &dn, FILE_DEVICE_UNKNOWN, FALSE, &RamDevice) == 0;
    RamDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    RtlInitAnsiString(&ln, "\\??\\R:");
    ok &= IoCreateSymbolicLink(&ln, &dn) == 0;
    KeInitializeDpc(&RamDpc, RamDpcRoutine, nullptr);

    for (auto &f : QueueDriver.MajorFunction)
        f = QueueDispatch;
    QueueDriver.DriverStartIo = QueueStartIo;
    RtlInitAnsiString(&qn, "\\Device\\XkQueue");
    ok &= IoCreateDevice(&QueueDriver, 0, &qn, FILE_DEVICE_UNKNOWN, TRUE, &QueueDevice) == 0;
    KeInitializeDpc(&QueueDpc, QueueDpcRoutine, nullptr);

    // Open through the drive-letter link: the driver sees the remaining path.
    OBJECT_STRING n1;
    OBJECT_ATTRIBUTES a1 = Attrs(&n1, "\\??\\R:\\foo\\bar.txt");
    HANDLE hs;
    IO_STATUS_BLOCK iosb;
    ok &= NtOpenFile(&hs, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &a1, &iosb, FILE_SHARE_READ,
                     FILE_SYNCHRONOUS_IO_NONALERT) == 0;
    OBJECT_STRING want, got;
    RtlInitAnsiString(&want, "\\foo\\bar.txt");
    RtlInitAnsiString(&got, LastRemaining);
    ok &= RtlEqualString(&want, &got, FALSE) && iosb.Information == 1 && RamDevice->ReferenceCount == 1;
    KiTestIoOpenOk = ok;

    // Synchronous read/write with the file pointer.
    char out[8] = "hello", in[8] = {};
    ok = NtWriteFile(hs, nullptr, nullptr, nullptr, &iosb, out, 5, nullptr) == 0 && iosb.Information == 5;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ok &= NtReadFile(hs, nullptr, nullptr, nullptr, &iosb, in, 5, &zero) == 0 && iosb.Information == 5;
    ok &= in[0] == 'h' && in[4] == 'o';
    // A pended read on a synchronous file waits inside the service.
    LARGE_INTEGER pend;
    pend.QuadPart = PEND_OFFSET;
    RamData()[PEND_OFFSET] = 0x5A;
    in[0] = 0;
    ok &= NtReadFile(hs, nullptr, nullptr, nullptr, &iosb, in, 1, &pend) == 0 && in[0] == 0x5A &&
          iosb.Information == 1;
    KiTestIoSyncRwOk = ok;

    // Queries: position/mode/alignment in the I/O manager, standard info from the driver.
    FILE_POSITION_INFORMATION pos;
    ULONG mode = 0;
    ULONG standard[6];
    ok = NtQueryInformationFile(hs, &iosb, &pos, sizeof(pos), FilePositionInformation) == 0 &&
         pos.CurrentByteOffset.QuadPart == PEND_OFFSET + 1;
    ok &= NtQueryInformationFile(hs, &iosb, &mode, 4, FileModeInformation) == 0 && mode == FILE_SYNCHRONOUS_IO_NONALERT;
    ok &= NtQueryInformationFile(hs, &iosb, standard, sizeof(standard), FileStandardInformation) == 0 &&
          standard[2] == RAM_SIZE && iosb.Information == 24;
    ok &= NtQueryInformationFile(hs, &iosb, standard, 4, FileStandardInformation) == STATUS_INFO_LENGTH_MISMATCH;
    ok &= NtQueryInformationFile(hs, &iosb, standard, 64, FileAccessInformation) == STATUS_INVALID_INFO_CLASS;
    pos.CurrentByteOffset.QuadPart = 2;
    ok &= NtSetInformationFile(hs, &iosb, &pos, sizeof(pos), FilePositionInformation) == 0;
    ok &= NtReadFile(hs, nullptr, nullptr, nullptr, &iosb, in, 3, nullptr) == 0 && in[0] == 'l' && in[2] == 'o';
    KiTestIoQueryOk = ok;

    // Asynchronous file: explicit offsets, event signalled on completion.
    OBJECT_STRING n2;
    OBJECT_ATTRIBUTES a2 = Attrs(&n2, "\\Device\\XkRam\\async");
    HANDLE ha, he, hx0;
    ok = NtOpenFile(&ha, GENERIC_READ, &a2, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, 0) == 0;
    ok &= NtOpenFile(&hx0, GENERIC_WRITE, &a2, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, 0) ==
          STATUS_SHARING_VIOLATION; // the first opener does not share writing
    NtCreateEvent(&he, nullptr, NotificationEvent, FALSE);
    ok &= NtReadFile(ha, nullptr, nullptr, nullptr, &iosb, in, 1, nullptr) == STATUS_INVALID_PARAMETER;
    IO_STATUS_BLOCK aiosb = {{(NTSTATUS)0x12345}, 0};
    in[0] = 0;
    ok &= NtReadFile(ha, he, nullptr, nullptr, &aiosb, in, 1, &pend) == STATUS_PENDING;
    ok &= NtWaitForSingleObject(he, FALSE, nullptr) == 0 && aiosb.Status == 0 && aiosb.Information == 1 &&
          in[0] == 0x5A;
    KiTestIoAsyncOk = ok;

    // Kernel-mode APC completion routine with its context and the caller's IO_STATUS_BLOCK.
    IO_STATUS_BLOCK apcIosb;
    ok = NtReadFile(ha, nullptr, TestIoApc, (PVOID)7, &apcIosb, in, 1, &pend) == STATUS_PENDING;
    Sleep(20);
    KiTestIoApcOk = ok && ApcRan == 7 && ApcIosbSeen == &apcIosb && apcIosb.Information == 1;

    // Completion port: an IRP and a mini packet.
    HANDLE hp;
    ok = NtCreateIoCompletion(&hp, 0, nullptr, 0) == 0;
    FILE_COMPLETION_INFORMATION ci = {hp, (PVOID)0x77};
    ok &= NtSetInformationFile(ha, &iosb, &ci, sizeof(ci), FileCompletionInformation) == 0;
    ok &= NtSetInformationFile(ha, &iosb, &ci, sizeof(ci), FileCompletionInformation) == STATUS_INVALID_PARAMETER;
    ok &= NtReadFile(ha, nullptr, TestIoApc, (PVOID)1, &iosb, in, 1, &pend) == STATUS_INVALID_PARAMETER;
    IO_STATUS_BLOCK piosb;
    ok &= NtReadFile(ha, nullptr, nullptr, (PVOID)0x55, &piosb, in, 1, &pend) == STATUS_PENDING;
    PVOID key = nullptr, ctx = nullptr;
    IO_STATUS_BLOCK riosb;
    ok &= NtRemoveIoCompletion(hp, &key, &ctx, &riosb, nullptr) == 0 && key == (PVOID)0x77 && ctx == (PVOID)0x55 &&
          riosb.Status == 0 && riosb.Information == 1;
    ok &= NtSetIoCompletion(hp, (PVOID)1, (PVOID)2, 3, 4) == 0;
    ok &= NtRemoveIoCompletion(hp, &key, &ctx, &riosb, &zero) == 0 && key == (PVOID)1 && ctx == (PVOID)2 &&
          riosb.Status == 3 && riosb.Information == 4;
    ok &= NtRemoveIoCompletion(hp, &key, &ctx, &riosb, &zero) == STATUS_TIMEOUT;
    NtSetIoCompletion(hp, nullptr, nullptr, 0, 0); // left queued: freed when the port is deleted
    KiTestIoPortOk = ok;

    // IOCTLs through a handle and directly to the device.
    ULONG inV = 41, outV = 0, ret = 0;
    ok = NtDeviceIoControlFile(hs, nullptr, nullptr, nullptr, &iosb, IOCTL_XK_INCREMENT, &inV, 4, &outV, 4) == 0 &&
         outV == 42 && iosb.Information == 4;
    inV = 99;
    ok &= IoSynchronousDeviceIoControlRequest(IOCTL_XK_INCREMENT, RamDevice, &inV, 4, &outV, 4, &ret, FALSE) == 0 &&
          outV == 100 && ret == 4;
    ok &= NtDeviceIoControlFile(hs, nullptr, nullptr, nullptr, &iosb, 0x1234, nullptr, 0, nullptr, 0) ==
          STATUS_INVALID_DEVICE_REQUEST;
    KiTestIoIoctlOk = ok;

    // Relative open: the root is a file handle; the driver gets the bare name and the related file.
    OBJECT_STRING n3;
    OBJECT_ATTRIBUTES a3 = Attrs(&n3, "child", hs);
    HANDLE hr;
    PFILE_OBJECT fs;
    ObReferenceObjectByHandle(hs, &IoFileObjectType, (PVOID *)&fs);
    ok = NtOpenFile(&hr, GENERIC_READ, &a3, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    FILE_SYNCHRONOUS_IO_NONALERT) == 0;
    RtlInitAnsiString(&want, "child");
    RtlInitAnsiString(&got, LastRemaining);
    ok &= RtlEqualString(&want, &got, FALSE) && LastRelated == fs;
    ObfDereferenceObject(fs);
    NtClose(hr);
    KiTestIoRelativeOk = ok;

    // Errors: synchronous failures leave the IO_STATUS_BLOCK untouched; bad names; exclusive device; access.
    LARGE_INTEGER past;
    past.QuadPart = 0x2000;
    IO_STATUS_BLOCK eiosb = {{(NTSTATUS)0x5555}, 0x6666};
    ok = NtReadFile(hs, nullptr, nullptr, nullptr, &eiosb, in, 1, &past) == STATUS_END_OF_FILE &&
         eiosb.Status == 0x5555 && eiosb.Information == 0x6666;
    OBJECT_STRING n4, n5, n6;
    OBJECT_ATTRIBUTES a4 = Attrs(&n4, "\\Device\\XkMissing"), a5 = Attrs(&n5, "\\Device\\XkQueue"),
                      a6 = Attrs(&n6, "\\Win32NamedObjects");
    HANDLE hx, hq, hq2;
    ok &= NtOpenFile(&hx, GENERIC_READ, &a4, &iosb, 0, 0) == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= NtOpenFile(&hx, GENERIC_READ, &a6, &iosb, 0, 0) == STATUS_OBJECT_TYPE_MISMATCH;
    ok &= NtOpenFile(&hq, GENERIC_READ, &a5, &iosb, 0, 0) == STATUS_NO_SUCH_DEVICE; // still initializing
    QueueDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    ok &= NtOpenFile(&hq, GENERIC_READ, &a5, &iosb, 0, 0) == 0;
    ok &= NtOpenFile(&hq2, GENERIC_READ, &a5, &iosb, 0, 0) == STATUS_ACCESS_DENIED; // exclusive
    ok &= NtWriteFile(ha, nullptr, nullptr, nullptr, &iosb, in, 1, &zero) == STATUS_ACCESS_DENIED;
    KiTestIoErrorsOk = ok;

    // StartIo: three queued reads run one at a time, in order.
    HANDLE ev[3];
    IO_STATUS_BLOCK qiosb[3];
    ok = true;
    for (ULONG i = 0; i < 3; i++) {
        NtCreateEvent(&ev[i], nullptr, NotificationEvent, FALSE);
        LARGE_INTEGER o;
        o.QuadPart = 100 + i;
        ok &= NtReadFile(hq, ev[i], nullptr, nullptr, &qiosb[i], in, 1, &o) == STATUS_PENDING;
    }
    ok &= NtWaitForMultipleObjectsEx(3, ev, WaitAll, KernelMode, FALSE, nullptr) == 0;
    for (ULONG i = 0; i < 3; i++) {
        ok &= StartOrder[i] == 100 + i && qiosb[i].Information == 100 + i;
        NtClose(ev[i]);
    }
    KiTestIoStartIoOk = ok && StartCount == 3 && QueueDevice->CurrentIrp == nullptr;

    // Closing: one CLEANUP + CLOSE per file object; the device references go away.
    ULONG opens = Creates;
    NtClose(hs);
    NtClose(ha);
    NtClose(he);
    NtClose(hp);
    NtClose(hq);
    KiTestIoCloseOk = Cleanups == opens && Closes == opens && RamDevice->ReferenceCount == 0 &&
                      QueueDevice->ReferenceCount == 0;

    // Deleting: names disappear, opens fail.
    IoDeleteSymbolicLink(&ln);
    IoDeleteDevice(QueueDevice);
    PVOID gone;
    ok = ObReferenceObjectByName(&qn, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= NtOpenFile(&hx, GENERIC_READ, &a1, &iosb, 0, 0) == STATUS_OBJECT_PATH_NOT_FOUND;
    IoDeleteDevice(RamDevice);
    ok &= NtOpenFile(&hx, GENERIC_READ, &a2, &iosb, 0, 0) == STATUS_OBJECT_PATH_NOT_FOUND;
    KiTestIoDeleteOk = ok;

    Sleep(5);
    KiTestIoPoolBalanced = ExpPoolBlocks <= poolBefore + 2;
    KiTestIoDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartIoSelfTests() { KiCreateSystemThread(IoControllerThread, nullptr, 0, 9); }
