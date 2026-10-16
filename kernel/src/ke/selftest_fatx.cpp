// Boot self-tests for the FATX driver (milestone 3c-2). boot_test.py checks the results against the HDD image and,
// after the run, re-reads what was written here from the disk copy with an independent FATX reader.
#include <xk/io.h>
#include <xk/rtl.h>

volatile ULONG KiTestFatxReadOk, KiTestFatxReadSum, KiTestFatxReadLength, KiTestFatxInfoOk, KiTestFatxDirOk,
    KiTestFatxWriteOk, KiTestFatxRenameDeleteOk, KiTestFatxErrorsOk, KiTestFatxVolumeOk, KiTestFatxFreeBalanced,
    KiTestFatxTotalClusters, KiTestFatxSerial, KiTestFatxDone;
volatile char KiTestFatxDirNames[64];
extern volatile ULONG KiTestIdexDone;

static void Sleep(LONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static NTSTATUS Open(HANDLE *h, PCSZ path, ACCESS_MASK access, ULONG disposition, ULONG options,
                     ULONG_PTR *info = nullptr, HANDLE root = nullptr) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, path);
    OBJECT_ATTRIBUTES a = {root, &s, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    iosb.Information = 0;
    NTSTATUS status = NtCreateFile(h, access | SYNCHRONIZE, &a, &iosb, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                   disposition, options | FILE_SYNCHRONOUS_IO_NONALERT);
    if (info)
        *info = iosb.Information;
    return status;
}

static ULONG FreeClusters(HANDLE h) {
    ULONG size[6] = {};
    IO_STATUS_BLOCK iosb;
    NtQueryVolumeInformationFile(h, &iosb, size, sizeof(size), FileFsSizeInformation);
    return size[2];
}

static void NTAPI FatxControllerThread(PVOID) {
    while (!KiTestIdexDone)
        Sleep(10);
    IO_STATUS_BLOCK iosb;

    // C:\xboxdash.xbe — whole-file read (cluster runs + partial tail through the cache).
    HANDLE h;
    ULONG_PTR info;
    NTSTATUS status = Open(&h, "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe", GENERIC_READ, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE, &info);
    ULONG standard[6] = {};
    if (NT_SUCCESS(status))
        NtQueryInformationFile(h, &iosb, standard, sizeof(standard), FileStandardInformation);
    // Large enough for the whole file (the stock dashboard's xboxdash.xbe is bigger than the xemu image's) and for
    // the write tests below (0x40000).
    auto buf = (PUCHAR)ExAllocatePoolWithTag(standard[2] > 0x40000 ? standard[2] : 0x40000, 'tsxF');
    if (NT_SUCCESS(status) && buf) {
        ULONG size = standard[2];
        bool ok = info == 1;
        ULONG got = 0;
        // Odd-sized pieces exercise the unaligned paths.
        while (ok && got < size) {
            ULONG piece = size - got < 30001 ? size - got : 30001;
            ok = NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf + got, piece, nullptr)) &&
                 iosb.Information == piece;
            got += piece;
        }
        ok &= NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 1, nullptr) == STATUS_END_OF_FILE;
        ULONG sum = 0;
        for (ULONG i = 0; i < size; i++)
            sum = sum * 31 + buf[i];
        KiTestFatxReadSum = sum;
        KiTestFatxReadLength = size;
        KiTestFatxReadOk = ok && *(PULONG)buf == 0x48454258; // "XBEH"

        ULONG net[14], mode;
        char name[64];
        ok = NtQueryInformationFile(h, &iosb, net, sizeof(net), FileNetworkOpenInformation) == 0 &&
             net[10] == size && net[12] == 0x80 /*NORMAL*/;
        ok &= NtQueryInformationFile(h, &iosb, name, sizeof(name), FileNameInformation) == 0 &&
              *(PULONG)name == 13 && name[4] == '\\' && name[5] == 'x';
        ok &= NtQueryInformationFile(h, &iosb, &mode, 4, FileModeInformation) == 0;
        KiTestFatxInfoOk = ok;
        NtClose(h);
    }

    // E:\ enumeration with the default mask, one entry per call.
    HANDLE hd;
    if (NT_SUCCESS(Open(&hd, "\\Device\\Harddisk0\\Partition1\\", FILE_READ_DATA, FILE_OPEN,
                        FILE_DIRECTORY_FILE))) {
        ULONG n = 0, entries = 0;
        UCHAR dir[0x40 + 64];
        bool restart = TRUE;
        while (NtQueryDirectoryFile(hd, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation,
                                    nullptr, restart) == 0) {
            restart = FALSE;
            ULONG len = *(PULONG)(dir + 0x3C);
            for (ULONG i = 0; i < len && n < sizeof(KiTestFatxDirNames) - 2; i++)
                KiTestFatxDirNames[n++] = dir[0x40 + i];
            KiTestFatxDirNames[n++] = '|';
            entries++;
        }
        KiTestFatxDirOk = entries >= 1;
        NtClose(hd);
    }

    // Write path on E:: directory + 100 KB file, reopen, verify, enumerate with a mask, rename, delete.
    HANDLE hv;
    Open(&hv, "\\Device\\Harddisk0\\Partition1", GENERIC_READ, FILE_OPEN, 0);
    ULONG freeBefore = FreeClusters(hv);
    HANDLE hdir, hf;
    bool ok = Open(&hdir, "\\Device\\Harddisk0\\Partition1\\XkTest", GENERIC_READ, FILE_OPEN_IF,
                   FILE_DIRECTORY_FILE, &info) == 0;
    const ULONG big = 100 * 1024 + 123;
    for (ULONG i = 0; i < big; i++)
        buf[i] = (UCHAR)(i ^ (i >> 8));
    ok &= Open(&hf, "hello.txt", GENERIC_READ | GENERIC_WRITE, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &info,
               hdir) == 0;
    ok &= NtWriteFile(hf, nullptr, nullptr, nullptr, &iosb, buf, 5000, nullptr) == 0 && iosb.Information == 5000;
    ok &= NtWriteFile(hf, nullptr, nullptr, nullptr, &iosb, buf + 5000, big - 5000, nullptr) == 0;
    NtClose(hf);
    RtlZeroMemory(buf, big);
    ok &= Open(&hf, "\\Device\\Harddisk0\\Partition1\\XkTest\\HELLO.TXT", GENERIC_READ, FILE_OPEN, 0) == 0;
    ok &= NtReadFile(hf, nullptr, nullptr, nullptr, &iosb, buf, big + 100, nullptr) == 0 && iosb.Information == big;
    for (ULONG i = 0; ok && i < big; i++)
        ok = buf[i] == (UCHAR)(i ^ (i >> 8));
    NtClose(hf);
    // A second file stays on disk for the host-side check.
    HANDLE hk;
    ok &= Open(&hk, "keep.bin", GENERIC_WRITE, FILE_OVERWRITE_IF, 0, nullptr, hdir) == 0;
    static const char keep[] = "open xbox kernel fatx test";
    ok &= NtWriteFile(hk, nullptr, nullptr, nullptr, &iosb, (PVOID)keep, sizeof(keep) - 1, nullptr) == 0;
    NtClose(hk);
    KiTestFatxWriteOk = ok;

    // Masked enumeration, rename, delete.
    UCHAR dir[0x40 + 64];
    OBJECT_STRING mask;
    RtlInitAnsiString(&mask, "*.txt");
    ok = NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation,
                              &mask, TRUE) == 0 &&
         *(PULONG)(dir + 0x3C) == 9 && *(PULONG)(dir + 0x28) == big;
    ok &= NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation,
                               nullptr, FALSE) == (NTSTATUS)0x80000006; // STATUS_NO_MORE_FILES
    ok &= Open(&hf, "hello.txt", GENERIC_READ | DELETE, FILE_OPEN, 0, nullptr, hdir) == 0;
    struct {
        BOOLEAN ReplaceIfExists;
        HANDLE RootDirectory;
        OBJECT_STRING FileName;
    } rename = {FALSE, nullptr, {}};
    RtlInitAnsiString(&rename.FileName, "renamed.txt");
    ok &= NtSetInformationFile(hf, &iosb, &rename, sizeof(rename), FileRenameInformation) == 0;
    FILE_DISPOSITION_INFORMATION del = {TRUE};
    ok &= NtSetInformationFile(hf, &iosb, &del, sizeof(del), FileDispositionInformation) == 0;
    NtClose(hf);
    HANDLE hx;
    ok &= Open(&hx, "renamed.txt", GENERIC_READ, FILE_OPEN, 0, nullptr, hdir) == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= Open(&hx, "hello.txt", GENERIC_READ, FILE_OPEN, 0, nullptr, hdir) == STATUS_OBJECT_NAME_NOT_FOUND;
    // Directory deletion: refused while not empty, done after the content is gone.
    HANDLE hsub;
    ok &= Open(&hsub, "sub", GENERIC_READ | DELETE, FILE_CREATE, FILE_DIRECTORY_FILE, nullptr, hdir) == 0;
    ok &= NtSetInformationFile(hdir, &iosb, &del, sizeof(del), FileDispositionInformation) ==
          (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    ok &= NtSetInformationFile(hsub, &iosb, &del, sizeof(del), FileDispositionInformation) == 0;
    NtClose(hsub);
    KiTestFatxRenameDeleteOk = ok;
    NtClose(hdir);

    // Errors.
    ok = Open(&hx, "\\Device\\Harddisk0\\Partition1\\NoSuchFile", GENERIC_READ, FILE_OPEN, 0) ==
         STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\NoDir\\x", GENERIC_READ, FILE_OPEN, 0) ==
          STATUS_OBJECT_PATH_NOT_FOUND;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\keep.bin", GENERIC_READ, FILE_CREATE, 0) ==
          STATUS_OBJECT_NAME_COLLISION;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\TDATA", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE) ==
          (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\keep.bin", GENERIC_READ, FILE_OPEN,
               FILE_DIRECTORY_FILE) == (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\bad*name", GENERIC_READ, FILE_OPEN_IF, 0) ==
          STATUS_OBJECT_NAME_INVALID;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\a23456789012345678901234567890123456789012x", GENERIC_READ,
               FILE_OPEN_IF, 0) == STATUS_OBJECT_NAME_INVALID; // 43 characters
    KiTestFatxErrorsOk = ok;

    // Volume information; every cluster used above came back except keep.bin's one.
    ULONG size[6] = {}, vol[8] = {};
    UCHAR attr[16] = {};
    ok = NtQueryVolumeInformationFile(hv, &iosb, size, 24, FileFsSizeInformation) == 0 && size[4] == 32 &&
         size[5] == 512;
    ok &= NtQueryVolumeInformationFile(hv, &iosb, vol, 18, FileFsVolumeInformation) == 0;
    ok &= NtQueryVolumeInformationFile(hv, &iosb, attr, 16, FileFsAttributeInformation) == 0 && attr[12] == 'F' &&
          attr[15] == 'X';
    KiTestFatxTotalClusters = size[0];
    KiTestFatxSerial = vol[2];
    KiTestFatxVolumeOk = ok;
    // XkTest may have existed from a previous run on the same image: it and keep.bin hold 2 clusters.
    KiTestFatxFreeBalanced = FreeClusters(hv) + 2 == freeBefore || FreeClusters(hv) == freeBefore;
    NtClose(hv);
    ExFreePool(buf);
    KiTestFatxDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartFatxSelfTests() { KiCreateSystemThread(FatxControllerThread, nullptr, 0, 9); }
