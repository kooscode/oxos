// Boot self-tests for the IDE disk driver (milestone 3c-1). boot_test.py compares the data read here with the
// HDD image it attached.
#include <xk/idex.h>
#include <xk/rtl.h>

volatile ULONG KiTestIdexPartitionSig[6][4]; // first 16 bytes of each partition (0 = whole disk sector 0)
volatile ULONG KiTestIdexBigSum, KiTestIdexBigOk;  // 256 KB read at the start of partition 1 (E:)
volatile ULONG KiTestIdexWriteOk, KiTestIdexIoctlOk, KiTestIdexErrorsOk, KiTestIdexNamesOk, KiTestIdexDone;
volatile ULONG KiTestIdexPassThroughOk, KiTestIdexSecurityAfter; // IDENTIFY word 128 after boot
volatile UCHAR KiTestIdexPassword[32]; // the derived drive password (boot_test.py recomputes it)

static PDEVICE_OBJECT IdexTestOpen(PCSZ name) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, name);
    PVOID dev = nullptr;
    return NT_SUCCESS(ObReferenceObjectByName(&s, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &dev)) ? (PDEVICE_OBJECT)dev
                                                                                                  : nullptr;
}

static NTSTATUS IdexTestRead(PDEVICE_OBJECT dev, PVOID buf, ULONG len, LONGLONG offset, UCHAR major = IRP_MJ_READ) {
    LARGE_INTEGER o;
    o.QuadPart = offset;
    return IoSynchronousFsdRequest(major, dev, buf, len, &o);
}

extern volatile ULONG KiTestMmDone, KiTestIoDone;

static void NTAPI IdexControllerThread(PVOID) {
    // Run after the memory and I/O suites: they measure pool/page balances.
    while (!KiTestMmDone || !KiTestIoDone) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    auto buf = (PUCHAR)MmAllocateContiguousMemory(0x40000);
    static const char *names[] = {"\\Device\\Harddisk0\\Partition0", "\\Device\\Harddisk0\\Partition1",
                                  "\\Device\\Harddisk0\\partition2", "\\Device\\Harddisk0\\Partition3",
                                  "\\Device\\Harddisk0\\Partition4", "\\Device\\Harddisk0\\Partition5"};
    PDEVICE_OBJECT parts[6];
    bool ok = true;
    for (ULONG i = 0; i < 6; i++) {
        parts[i] = IdexTestOpen(names[i]);
        ok &= parts[i] != nullptr;
        if (parts[i] && NT_SUCCESS(IdexTestRead(parts[i], buf, 512, 0)))
            for (ULONG w = 0; w < 4; w++)
                KiTestIdexPartitionSig[i][w] = ((PULONG)buf)[w];
    }
    OBJECT_STRING s1, s2;
    RtlInitAnsiString(&s1, "\\Device\\Harddisk0\\Partition6");
    RtlInitAnsiString(&s2, "\\Device\\Harddisk0\\Partition6\\x");
    PVOID gone;
    KiTestIdexNamesOk = ok && IdexTestOpen(names[1]) == parts[1] && // same device on a second lookup
                        ObReferenceObjectByName(&s1, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_NAME_NOT_FOUND &&
                        ObReferenceObjectByName(&s2, 0, nullptr, nullptr, &gone) == STATUS_OBJECT_PATH_NOT_FOUND;
    ObfDereferenceObject(parts[1]);
    if (!parts[0] || !parts[1] || !parts[2]) {
        KiTestIdexDone = 1;
        return;
    }

    // Two 128 KB DMA chunks in one request, buffer from non-contiguous pool pages too.
    RtlFillMemory(buf, 0x40000, 0xCC);
    KiTestIdexBigOk = NT_SUCCESS(IdexTestRead(parts[1], buf, 0x40000, 0));
    ULONG sum = 0;
    for (ULONG i = 0; i < 0x40000 / 4; i++)
        sum += ((PULONG)buf)[i] * (i | 1);
    KiTestIdexBigSum = sum;

    // Write + read back in the unused area before partition 3 (X:), on the test copy of the disk.
    auto pool = (PUCHAR)ExAllocatePoolWithTag(0x2000, 'tsxI'); // crosses a page boundary
    PUCHAR w = pool + 0x200;
    for (ULONG i = 0; i < 0x1000; i++)
        w[i] = (UCHAR)(i * 7 + 3);
    ok = NT_SUCCESS(IdexTestRead(parts[0], w, 0x1000, 0x3F0LL * 512, IRP_MJ_WRITE));
    RtlZeroMemory(buf, 0x1000);
    ok &= NT_SUCCESS(IdexTestRead(parts[0], buf, 0x1000, 0x3F0LL * 512));
    for (ULONG i = 0; ok && i < 0x1000; i++)
        ok = buf[i] == (UCHAR)(i * 7 + 3);
    KiTestIdexWriteOk = ok;
    ExFreePool(pool);

    // IOCTLs.
    PARTITION_INFORMATION pi;
    DISK_GEOMETRY g;
    ULONG ret;
    ok = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, parts[2], nullptr, 0, &pi, sizeof(pi),
                                             &ret, FALSE) == 0 &&
         ret == sizeof(pi) && pi.StartingOffset.QuadPart == 0x465400LL * 512 &&
         pi.PartitionLength.QuadPart == 0x1F400000 && pi.PartitionNumber == 2;
    ok &= IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, parts[0], nullptr, 0, &g, sizeof(g),
                                              &ret, FALSE) == 0 &&
          g.Cylinders.QuadPart == IdexDiskLengthInSectors && g.BytesPerSector == 512;
    KiTestIdexIoctlOk = ok;

    // Parameter errors: unaligned length/offset, past the end of a partition.
    ok = IdexTestRead(parts[2], buf, 100, 0) == STATUS_INVALID_PARAMETER;
    ok &= IdexTestRead(parts[2], buf, 512, 3) == STATUS_INVALID_PARAMETER;
    ok &= IdexTestRead(parts[2], buf, 1024, 0x1F400000 - 512) == STATUS_INVALID_PARAMETER;
    ok &= IdexTestRead(parts[2], buf, 512, 0x1F400000 - 512) == STATUS_SUCCESS;
    KiTestIdexErrorsOk = ok;

    // IDE pass-through: IDENTIFY DEVICE through Partition0 returns the model the driver reported; the buffer rule.
    ok = parts[0] != nullptr;
    if (ok) {
        ATA_PASS_THROUGH p = {}, q = {};
        p.IdeReg.bCommandReg = 0xEC;
        p.DataBufferSize = 512;
        p.DataBuffer = buf;
        RtlZeroMemory(buf, 512);
        ULONG got = 0;
        ok &= IoSynchronousDeviceIoControlRequest(IOCTL_IDE_PASS_THROUGH, parts[0], &p, sizeof(p), &p, sizeof(p),
                                                  &got, FALSE) == STATUS_SUCCESS && got == sizeof(p);
        ok &= !(p.IdeReg.bCommandReg & 0x81) && HalDiskModelNumber.Length >= 2 &&
              buf[54] == (UCHAR)HalDiskModelNumber.Buffer[1] && buf[55] == (UCHAR)HalDiskModelNumber.Buffer[0];
        KiTestIdexSecurityAfter = buf[256] | buf[257] << 8;
        ok &= IoSynchronousDeviceIoControlRequest(IOCTL_IDE_PASS_THROUGH, parts[0], &p, sizeof(p), &q, sizeof(q),
                                                  nullptr, FALSE) == STATUS_INVALID_PARAMETER;
        // A failing command still completes the IRP: status ERR comes back in the registers.
        p = {};
        p.IdeReg.bCommandReg = 0xF2; // SECURITY UNLOCK on an unlocked drive (or one without security) is aborted
        p.IdeReg.bHostSendsData = TRUE;
        p.DataBufferSize = 512;
        p.DataBuffer = buf;
        RtlZeroMemory(buf, 512);
        ok &= IoSynchronousDeviceIoControlRequest(IOCTL_IDE_PASS_THROUGH, parts[0], &p, sizeof(p), &p, sizeof(p),
                                                  nullptr, FALSE) == STATUS_SUCCESS;
        KiTestIdexPassThroughOk = ok;
    }
    UCHAR password[32] = {};
    IdexDiskComputePassword(password);
    for (ULONG i = 0; i < 32; i++)
        KiTestIdexPassword[i] = password[i];

    for (ULONG i = 0; i < 6; i++)
        if (i != 1 && parts[i])
            ObfDereferenceObject(parts[i]);
    MmFreeContiguousMemory(buf);
    KiTestIdexDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartIdexSelfTests() { KiCreateSystemThread(IdexControllerThread, nullptr, 0, 9); }
