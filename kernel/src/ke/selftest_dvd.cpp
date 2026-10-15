// Boot self-tests for the DVD drive, XDVDFS and the RAW file system (milestone 3c-3). boot_test.py attaches an
// XISO built by tests/mkxiso.py and compares.
#include <xk/idex.h>
#include <xk/rtl.h>

volatile ULONG KiTestDvdDeviceOk, KiTestDvdBigSum, KiTestDvdBigLength, KiTestDvdReadOk, KiTestDvdDirOk,
    KiTestDvdErrorsOk, KiTestDvdRawOk, KiTestDvdDone;
volatile char KiTestDvdDirNames[64];
volatile NTSTATUS KiTestDvdStatus[8];
extern volatile ULONG KiTestFatxDone;

static NTSTATUS DvdOpen(HANDLE *h, PCSZ path, ACCESS_MASK access = GENERIC_READ, ULONG disposition = FILE_OPEN,
                        ULONG options = 0) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, path);
    OBJECT_ATTRIBUTES a = {nullptr, &s, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, access | SYNCHRONIZE, &a, &iosb, nullptr, 0, FILE_SHARE_READ, disposition,
                        options | FILE_SYNCHRONOUS_IO_NONALERT);
}

// UDF disc (milestone 8, boot_test.py --udf with tests/mkudf.py): fragmented, embedded and 16-bit-name files.
volatile ULONG KiTestUdfVolumeOk, KiTestUdfVobSum, KiTestUdfVobLength, KiTestUdfIfoSum, KiTestUdfSmallOk,
    KiTestUdfWideOk, KiTestUdfDirOk, KiTestUdfErrorsOk;
volatile char KiTestUdfDirNames[64];

static ULONG DvdReadAll(PCSZ path, PUCHAR buf, ULONG size, ULONG *got) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    *got = 0;
    if (!NT_SUCCESS(DvdOpen(&h, path)))
        return 0;
    ULONG sum = 0;
    if (NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, size, nullptr) == 0) {
        *got = iosb.Information;
        for (ULONG i = 0; i < *got; i++)
            sum = sum * 31 + buf[i];
    }
    NtClose(h);
    return sum;
}

static void UdfSuite(PUCHAR buf) {
    ULONG got;
    KiTestUdfVobSum = DvdReadAll("\\Device\\CdRom0\\VIDEO_TS\\VTS_01_1.VOB", buf, 0x50000, &got);
    KiTestUdfVobLength = got;
    KiTestUdfIfoSum = DvdReadAll("\\Device\\CdRom0\\video_ts\\video_ts.ifo", buf, 0x50000, &got);
    DvdReadAll("\\??\\CdRom0:\\VIDEO_TS\\small.txt", buf, 100, &got);
    KiTestUdfSmallOk = got == 9 && RtlCompareMemory(buf, "hello udf", 9) == 9;
    DvdReadAll("\\Device\\CdRom0\\wide", buf, 100, &got);
    KiTestUdfWideOk = got == 16 && RtlCompareMemory(buf, "sixteen-bit name", 16) == 16;
    // Volume attributes and a read at an offset.
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    bool ok = DvdOpen(&h, "\\Device\\CdRom0\\readme.txt") == 0;
    if (ok) {
        UCHAR attr[32];
        ok &= NtQueryVolumeInformationFile(h, &iosb, attr, sizeof(attr), FileFsAttributeInformation) == 0 &&
              *(PULONG)(attr + 8) == 3 && RtlCompareMemory(attr + 12, "UDF", 3) == 3;
        LARGE_INTEGER off;
        off.QuadPart = 4;
        ok &= NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 100, &off) == 0 && iosb.Information == 9 &&
              buf[0] == 'r' && buf[8] == 'e';
        NtClose(h);
    }
    KiTestUdfVolumeOk = ok;
    // Root enumeration in on-disc order.
    HANDLE hd;
    if (DvdOpen(&hd, "\\Device\\CdRom0\\", FILE_READ_DATA, FILE_OPEN, FILE_DIRECTORY_FILE) == 0) {
        UCHAR e[0x40 + 64];
        ULONG n = 0;
        BOOLEAN restart = TRUE;
        while (NtQueryDirectoryFile(hd, nullptr, nullptr, nullptr, &iosb, e, sizeof(e), FileDirectoryInformation,
                                    nullptr, restart) == 0) {
            restart = FALSE;
            for (ULONG i = 0; i < *(PULONG)(e + 0x3C) && n < 62; i++)
                KiTestUdfDirNames[n++] = e[0x40 + i];
            KiTestUdfDirNames[n++] = '|';
        }
        KiTestUdfDirOk = n > 0;
        NtClose(hd);
    }
    HANDLE hx;
    ok = DvdOpen(&hx, "\\Device\\CdRom0\\nofile") == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\nodir\\x") == STATUS_OBJECT_PATH_NOT_FOUND;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\readme.txt", GENERIC_WRITE) == (NTSTATUS)0xC00000A2;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\VIDEO_TS", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE) ==
          (NTSTATUS)0xC00000BA;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\readme.txt\\", GENERIC_READ) == (NTSTATUS)0xC0000103;
    KiTestUdfErrorsOk = ok;
}

static void NTAPI DvdControllerThread(PVOID) {
    while (!KiTestFatxDone) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    IO_STATUS_BLOCK iosb;
    OBJECT_STRING name;
    RtlInitAnsiString(&name, "\\Device\\CdRom0");
    PVOID dev = nullptr;
    bool ok = ObReferenceObjectByName(&name, 0, nullptr, nullptr, &dev) == 0;
    ok &= ok && IoSynchronousDeviceIoControlRequest(IOCTL_CDROM_CHECK_VERIFY, (PDEVICE_OBJECT)dev, nullptr, 0,
                                                    nullptr, 0, nullptr, FALSE) == 0;
    if (dev)
        ObfDereferenceObject(dev);
    KiTestDvdDeviceOk = ok;

    // A 300 KB file: three DMA chunks of 128 KB and a partial last sector.
    auto buf = (PUCHAR)ExAllocatePoolWithTag(0x50000, 'tsvD');
    {
        HANDLE hr;
        UCHAR attr[32];
        if (buf && DvdOpen(&hr, "\\Device\\CdRom0\\", FILE_READ_DATA, FILE_OPEN, FILE_DIRECTORY_FILE) == 0) {
            bool udf = NtQueryVolumeInformationFile(hr, &iosb, attr, sizeof(attr), FileFsAttributeInformation) == 0 &&
                       RtlCompareMemory(attr + 12, "UDF", 3) == 3;
            NtClose(hr);
            if (udf) {
                UdfSuite(buf);
                ExFreePool(buf);
                KiTestDvdDone = 1;
                return;
            }
        }
    }
    HANDLE h;
    KiTestDvdStatus[0] = DvdOpen(&h, "\\??\\CdRom0:\\media\\big.bin");
    if (buf && NT_SUCCESS(KiTestDvdStatus[0])) {
        ULONG std[6];
        NtQueryInformationFile(h, &iosb, std, sizeof(std), FileStandardInformation);
        KiTestDvdStatus[1] = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x50000, nullptr);
        ok = KiTestDvdStatus[1] == 0;
        ULONG n = iosb.Information, sum = 0;
        for (ULONG i = 0; i < n; i++)
            sum = sum * 31 + buf[i];
        KiTestDvdBigSum = sum;
        KiTestDvdBigLength = n;
        ok &= n == std[2] && NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 1, nullptr) == STATUS_END_OF_FILE;
        NtClose(h);
        // Small file through a relative-to-root path and an odd offset.
        HANDLE h2;
        ok &= DvdOpen(&h2, "\\Device\\CdRom0\\MEDIA\\a.txt") == 0;
        LARGE_INTEGER off;
        off.QuadPart = 6;
        ok &= NtReadFile(h2, nullptr, nullptr, nullptr, &iosb, buf, 100, &off) == 0 && iosb.Information == 3 &&
              buf[0] == 'd' && buf[2] == 'd';
        NtClose(h2);
        ok &= DvdOpen(&h2, "\\Device\\CdRom0\\media\\Sub\\deep.txt") == 0;
        NtClose(h2);
        KiTestDvdReadOk = ok;
    }

    // Root enumeration in tree (sorted) order.
    HANDLE hd;
    KiTestDvdStatus[2] = DvdOpen(&hd, "\\Device\\CdRom0\\", FILE_READ_DATA, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (NT_SUCCESS(KiTestDvdStatus[2])) {
        UCHAR e[0x40 + 64];
        ULONG n = 0;
        BOOLEAN restart = TRUE;
        for (;;) {
            NTSTATUS qs = NtQueryDirectoryFile(hd, nullptr, nullptr, nullptr, &iosb, e, sizeof(e),
                                               FileDirectoryInformation, nullptr, restart);
            KiTestDvdStatus[4 + (restart ? 0 : 1)] = qs;
            if (qs)
                break;
            restart = FALSE;
            for (ULONG i = 0; i < *(PULONG)(e + 0x3C) && n < 62; i++)
                KiTestDvdDirNames[n++] = e[0x40 + i];
            KiTestDvdDirNames[n++] = '|';
        }
        KiTestDvdDirOk = n > 0;
        NtClose(hd);
    }

    // Read-only media and lookup errors.
    HANDLE hx;
    constexpr NTSTATUS WRITE_PROTECTED = (NTSTATUS)0xC00000A2;
    ok = DvdOpen(&hx, "\\Device\\CdRom0\\nofile") == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\nodir\\x") == STATUS_OBJECT_PATH_NOT_FOUND;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\new.txt", GENERIC_READ, FILE_CREATE) == WRITE_PROTECTED;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\AAA.txt", GENERIC_WRITE) == WRITE_PROTECTED;
    ok &= DvdOpen(&hx, "\\Device\\CdRom0\\media", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE) ==
          (NTSTATUS)0xC00000BA;
    KiTestDvdErrorsOk = ok;

    // RAW: the whole disk (Partition0) never carries a file system; read the sector the IDE suite wrote.
    HANDLE hr = nullptr;
    KiTestDvdStatus[3] = DvdOpen(&hr, "\\Device\\Harddisk0\\Partition0", GENERIC_READ | GENERIC_WRITE);
    ok = KiTestDvdStatus[3] == 0;
    LARGE_INTEGER off;
    off.QuadPart = 0x3F0LL * 512;
    ok &= ok && NtReadFile(hr, nullptr, nullptr, nullptr, &iosb, buf, 512, &off) == 0 && buf[1] == 10 && buf[2] == 17;
    ok &= ok && NtReadFile(hr, nullptr, nullptr, nullptr, &iosb, buf, 100, &off) == STATUS_INVALID_PARAMETER;
    if (hr)
        NtClose(hr);
    ok &= DvdOpen(&hx, "\\Device\\Harddisk0\\Partition0\\file") == STATUS_OBJECT_PATH_NOT_FOUND;
    KiTestDvdRawOk = ok;
    if (buf)
        ExFreePool(buf);
    KiTestDvdDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartDvdSelfTests() { KiCreateSystemThread(DvdControllerThread, nullptr, 0, 9); }
