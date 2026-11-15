// Boot self-tests for the FATX driver (milestone 3c-2). boot_test.py checks the results against the HDD image and, after the run, re-reads what was written
// here from the disk copy with an independent FATX reader.
#include <kernel_self_tests.h>
#include <storage/storage.h>
#include <io/io.h>
#include <runtime/runtime.h>
#include <core/core.h>
#include <memory/memory.h>
#include <filesystems/filesystems.h>
#include <filesystems/hddfs/hddfs.h>
#include <diagnostics/diagnostics.h>

using namespace xk; // the kernel's subsystems as storage::, memory::, ...

volatile ULONG KiTestFATXReadOk, KiTestFATXReadSum, KiTestFATXReadLength, KiTestFATXInfoOk, KiTestFATXDirOk, KiTestFATXWriteOk, KiTestFATXRenameDeleteOk,
    KiTestFATXErrorsOk, KiTestFATXVolumeOk, KiTestFATXFreeBalanced, KiTestFATXTotalClusters, KiTestFATXSerial, KiTestFATXDone;
volatile char KiTestFATXDirNames[64];
// exFAT disk: a title-style format of X: (Partition3) through its volume handle empties the folder (spec/oxos_internal/05_exfat_storage_spec.md §5); E: refuses
// it.
volatile ULONG KiTestEXFATFormatOk, KiTestEXFATFormatFree, KiTestEXFATFormatTotal, KiTestEXFATRefuseOk, KiTestEXFATFormatSpc;

// exFAT disk: allocations through two folder volumes in turn (EXFATCrossFolderTest).
volatile ULONG KiTestEXFATCrossOk;

// exFAT: a FAT chain is followed only as far as the file's length (E:\XkStale.stale has a stale tail past it), and preallocation does not allocate (an entry
// set cannot hold clusters past its length).
volatile ULONG KiTestEXFATStaleOk, KiTestEXFATPreallocOk;

// exFAT: a file grows past 4 GB, shrinks, and takes writes after it is opened again (EXFATLargeFileTest).
volatile ULONG KiTestEXFATLargeOk;
// exFAT: names of 255 characters, Latin-1 ones among them, created, listed, opened in another case, renamed and deleted (EXFATLongNameTest).
volatile ULONG KiTestEXFATLongNameOk;
// exFAT: X:'s format kept in C:\OXOS\cache\oxos_cache_partitions.cache, and X: mounted with it again after the kernel forgot it, as after a power-off.
volatile ULONG KiTestEXFATKeptFormatOk;

// A path in the title's D: folder reports 2048-byte sectors (spec/oxos_internal/25 §2).
volatile ULONG KiTestFATXTitleFolderOk;

static void Sleep(LONG ms) {
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static NTSTATUS Open(HANDLE *h, PCSZ path, ACCESS_MASK access, ULONG disposition, ULONG options, ULONG_PTR *info = nullptr, HANDLE root = nullptr) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, path);
    OBJECT_ATTRIBUTES a = {root, &s, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    iosb.Information = 0;
    NTSTATUS status = NtCreateFile(h, access | SYNCHRONIZE, &a, &iosb, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_DELETE, disposition,
                                   options | FILE_SYNCHRONOUS_IO_NONALERT);
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

static void EXFATFormatTest(PUCHAR buf);
static void EXFATCrossFolderTest(PUCHAR buf);
static void EXFATLengthTest(PUCHAR buf);
static void EXFATLargeFileTest(PUCHAR buf);
static void EXFATLongNameTest(PUCHAR buf);

static void NTAPI FATXControllerThread(PVOID) {
    while (!KiTestIdexDone)
        Sleep(10);
    IO_STATUS_BLOCK iosb;

    // C:\xboxdash.xbe — whole-file read (cluster runs + partial tail through the cache).
    HANDLE h;
    ULONG_PTR info;
    NTSTATUS status = Open(&h, "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &info);
    ULONG standard[6] = {};
    if (NT_SUCCESS(status))
        NtQueryInformationFile(h, &iosb, standard, sizeof(standard), FileStandardInformation);
    // Large enough for the whole file (the stock dashboard's xboxdash.xbe is bigger than the xemu image's) and for the write tests below (0x40000).
    auto buf = (PUCHAR)ExAllocatePoolWithTag(standard[2] > 0x40000 ? standard[2] : 0x40000, 'tsxF');
    if (NT_SUCCESS(status) && buf) {
        ULONG size = standard[2];
        bool ok = info == 1;
        ULONG got = 0;
        // Odd-sized pieces exercise the unaligned paths.
        while (ok && got < size) {
            ULONG piece = size - got < 30001 ? size - got : 30001;
            ok = NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf + got, piece, nullptr)) && iosb.Information == piece;
            got += piece;
        }
        ok &= NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 1, nullptr) == STATUS_END_OF_FILE;
        ULONG sum = 0;
        for (ULONG i = 0; i < size; i++)
            sum = sum * 31 + buf[i];
        KiTestFATXReadSum = sum;
        KiTestFATXReadLength = size;
        KiTestFATXReadOk = ok && *(PULONG)buf == 0x48454258; // "XBEH"

        ULONG net[14], mode;
        char name[64];
        ok = NtQueryInformationFile(h, &iosb, net, sizeof(net), FileNetworkOpenInformation) == 0 && net[10] == size && net[12] == 0x80 /*NORMAL*/;
        ok &= NtQueryInformationFile(h, &iosb, name, sizeof(name), FileNameInformation) == 0 &&
              *(PULONG)name == 13 && name[4] == '\\' && name[5] == 'x';
        ok &= NtQueryInformationFile(h, &iosb, &mode, 4, FileModeInformation) == 0;
        KiTestFATXInfoOk = ok;
        NtClose(h);
    }

    // E:\ enumeration with the default mask, one entry per call, to the end (an empty E:\ is fine: boot_test.py compares the names with the disk image's).
    HANDLE hd;
    if (NT_SUCCESS(Open(&hd, "\\Device\\Harddisk0\\Partition1\\", FILE_READ_DATA, FILE_OPEN, FILE_DIRECTORY_FILE))) {
        ULONG n = 0, entries = 0;
        UCHAR dir[0x40 + 64];
        bool restart = TRUE;
        NTSTATUS last;
        while ((last = NtQueryDirectoryFile(hd, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation, nullptr, restart)) == 0) {
            restart = FALSE;
            ULONG len = *(PULONG)(dir + 0x3C);
            for (ULONG i = 0; i < len && n < sizeof(KiTestFATXDirNames) - 2; i++)
                KiTestFATXDirNames[n++] = dir[0x40 + i];
            if (n < sizeof(KiTestFATXDirNames) - 2) // at most 62 characters, as boot_test.py compares; stays terminated
                KiTestFATXDirNames[n++] = '|';
            entries++;
        }
        // The end of the listing is STATUS_NO_MORE_FILES; an empty directory answers the first call with STATUS_NO_SUCH_FILE.
        KiTestFATXDirOk = last == (entries ? (NTSTATUS)0x80000006 : (NTSTATUS)0xC000000F);
        NtClose(hd);
    }

    // Write path on E:: directory + 100 KB file, reopen, verify, enumerate with a mask, rename, delete.
    HANDLE hv;
    Open(&hv, "\\Device\\Harddisk0\\Partition1", GENERIC_READ, FILE_OPEN, 0);
    ULONG freeBefore = FreeClusters(hv);
    HANDLE hdir, hf;
    bool ok = Open(&hdir, "\\Device\\Harddisk0\\Partition1\\XkTest", GENERIC_READ, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &info) == 0;
    const ULONG big = 100 * 1024 + 123;
    for (ULONG i = 0; i < big; i++)
        buf[i] = (UCHAR)(i ^ (i >> 8));
    ok &= Open(&hf, "hello.txt", GENERIC_READ | GENERIC_WRITE, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &info, hdir) == 0;
    ok &= NtWriteFile(hf, nullptr, nullptr, nullptr, &iosb, buf, 5000, nullptr) == 0 && iosb.Information == 5000;
    ok &= NtWriteFile(hf, nullptr, nullptr, nullptr, &iosb, buf + 5000, big - 5000, nullptr) == 0;
    NtClose(hf);
    memset(buf, 0, big);
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
    KiTestFATXWriteOk = ok;

    // Masked enumeration, rename, delete.
    UCHAR dir[0x40 + 64];
    OBJECT_STRING mask;
    RtlInitAnsiString(&mask, "*.txt");
    ok = NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation, &mask, TRUE) == 0 &&
         *(PULONG)(dir + 0x3C) == 9 && *(PULONG)(dir + 0x28) == big;
    ok &= NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation, nullptr,
                               FALSE) == (NTSTATUS)0x80000006; // STATUS_NO_MORE_FILES
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
    ok &= NtSetInformationFile(hdir, &iosb, &del, sizeof(del), FileDispositionInformation) == (NTSTATUS)0xC0000101; // STATUS_DIRECTORY_NOT_EMPTY
    ok &= NtSetInformationFile(hsub, &iosb, &del, sizeof(del), FileDispositionInformation) == 0;
    NtClose(hsub);
    KiTestFATXRenameDeleteOk = ok;
    NtClose(hdir);

    // Errors.
    ok = Open(&hx, "\\Device\\Harddisk0\\Partition1\\NoSuchFile", GENERIC_READ, FILE_OPEN, 0) == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\NoDir\\x", GENERIC_READ, FILE_OPEN, 0) == STATUS_OBJECT_PATH_NOT_FOUND;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\keep.bin", GENERIC_READ, FILE_CREATE, 0) == STATUS_OBJECT_NAME_COLLISION;
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest", GENERIC_READ, FILE_OPEN,
          FILE_NON_DIRECTORY_FILE) == (NTSTATUS)0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY (XkTest: created above, so on any disk)
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\keep.bin", GENERIC_READ, FILE_OPEN,
               FILE_DIRECTORY_FILE) == (NTSTATUS)0xC0000103; // STATUS_NOT_A_DIRECTORY
    ok &= Open(&hx, "\\Device\\Harddisk0\\Partition1\\bad*name", GENERIC_READ, FILE_OPEN_IF, 0) == STATUS_OBJECT_NAME_INVALID;
    // A name one character too long: 43 on FATX, 256 on exFAT (EXFATLongNameTest creates the longest that fits).
    char tooLong[29 + 256 + 1];
    ULONG tooLongLength = storage::DiskHasEXFATLayout ? 256 : 43;
    memcpy(tooLong, "\\Device\\Harddisk0\\Partition1\\", 29);
    memset(tooLong + 29, 'a', tooLongLength);
    tooLong[29 + tooLongLength] = 0;
    ok &= Open(&hx, tooLong, GENERIC_READ, FILE_OPEN_IF, 0) == STATUS_OBJECT_NAME_INVALID;
    // One doubled separator is accepted (as the reference); a tripled one is not.
    bool doubled = Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\\\keep.bin", GENERIC_READ, FILE_OPEN, 0) == STATUS_SUCCESS;
    if (doubled)
        NtClose(hx);
    ok &= doubled && Open(&hx, "\\Device\\Harddisk0\\Partition1\\XkTest\\\\\\keep.bin", GENERIC_READ, FILE_OPEN, 0) == STATUS_OBJECT_NAME_INVALID;
    KiTestFATXErrorsOk = ok;

    // Volume information; every cluster used above came back except keep.bin's one.
    ULONG size[6] = {}, vol[8] = {};
    UCHAR attr[16] = {};
    ok = NtQueryVolumeInformationFile(hv, &iosb, size, 24, FileFsSizeInformation) == 0 && size[4] == 32 && size[5] == 512;
    ok &= NtQueryVolumeInformationFile(hv, &iosb, vol, 18, FileFsVolumeInformation) == 0;
    ok &= NtQueryVolumeInformationFile(hv, &iosb, attr, 16, FileFsAttributeInformation) == 0 && attr[12] == 'F' && attr[15] == 'X';
    KiTestFATXTotalClusters = size[0];
    KiTestFATXSerial = vol[2];
    KiTestFATXVolumeOk = ok;
    // XkTest may have existed from a previous run on the same image: it and keep.bin hold 2 clusters.
    KiTestFATXFreeBalanced = FreeClusters(hv) + 2 == freeBefore || FreeClusters(hv) == freeBefore;
    // D: as a hard-disk folder (spec/oxos_internal/25 §2): D:\ and a file in it report 2048-byte sectors with the same cluster size; the rest of the volume
    // keeps its own. The link is removed again before the title starts.
    {
        OBJECT_STRING link, target;
        RtlInitAnsiString(&link, "\\??\\D:");
        RtlInitAnsiString(&target, "\\Device\\Harddisk0\\Partition1\\XkTest");
        ULONG bytes = size[4] * size[5], d[6] = {}, k[6] = {}, e[6] = {};
        IoDeleteSymbolicLink(&link);
        ok = IoCreateSymbolicLink(&link, &target) == 0;
        filesystems::HDDFSRememberTitleFolder();
        HANDLE hd, hk;
        if (ok && Open(&hd, "\\??\\D:\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) == 0) {
            ok &= NtQueryVolumeInformationFile(hd, &iosb, d, 24, FileFsSizeInformation) == 0 && d[5] == 2048 && d[4] * d[5] == bytes && d[0] == size[0];
            NtClose(hd);
        } else {
            ok = false;
        }
        if (ok && Open(&hk, "\\??\\D:\\keep.bin", GENERIC_READ, FILE_OPEN, 0) == 0) {
            ok &= NtQueryVolumeInformationFile(hk, &iosb, k, 24, FileFsSizeInformation) == 0 && k[5] == 2048;
            NtClose(hk);
        } else {
            ok = false;
        }
        ok &= NtQueryVolumeInformationFile(hv, &iosb, e, 24, FileFsSizeInformation) == 0 && e[5] == size[5];
        IoDeleteSymbolicLink(&link);
        filesystems::HDDFSRememberTitleFolder(); // D: gone: nothing is remembered
        ok &= NtQueryVolumeInformationFile(hv, &iosb, e, 24, FileFsSizeInformation) == 0 && e[5] == size[5];
        KiTestFATXTitleFolderOk = ok;
    }
    NtClose(hv);
    // Log rotation (spec/oxos_internal/35): 50 lines of 200 bytes into a log that rotates at 4 KB in self-test builds, 20 lines a part. boot_test.py reads
    // C:\OXOS\log\selftest_2.log (lines 0-19), selftest_1.log (20-39) and selftest.log (40-49) back from the disk.
    for (ULONG i = 0; i < 50; i++)
        diagnostics::AppendLogLine("selftest", "rotation line %02lu %0180lu", i, 0ul);
    if (storage::DiskHasEXFATLayout) {
        EXFATFormatTest(buf);
        EXFATCrossFolderTest(buf);
        EXFATLengthTest(buf);
        EXFATLargeFileTest(buf);
        EXFATLongNameTest(buf);
    }
    ExFreePool(buf);
    KiTestFATXDone = 1;
}

// The format sequence the trace showed (Halo, spec §8): raw open, geometry + partition info, a FATX header at offset 0, the allocation table and root directory
// after it, then a dismount.
static NTSTATUS FormatLikeATitle(PCSZ volume, PUCHAR buf) {
    HANDLE h;
    NTSTATUS status = Open(&h, volume, GENERIC_READ | GENERIC_WRITE, FILE_OPEN, 0);
    if (!NT_SUCCESS(status))
        return status;
    IO_STATUS_BLOCK iosb;
    PARTITION_INFORMATION pi = {};
    status = NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &pi, sizeof(pi));
    // 64 KB clusters, as Halo 2 formats its cache partition.
    ULONG entries = (ULONG)(pi.PartitionLength.QuadPart >> 16) + 1, fat = (entries * 2 + 0xFFF) & ~0xFFFu;
    LARGE_INTEGER at = {};
    memset(buf, 0, 0x1000);
    ((PULONG)buf)[0] = 0x58544146; // "FATX"
    ((PULONG)buf)[1] = 0x12345678;
    ((PULONG)buf)[2] = 128;
    ((PULONG)buf)[3] = 1;
    if (NT_SUCCESS(status))
        status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x1000, &at);
    memset(buf, 0, 0x1000);
    for (ULONG off = 0; NT_SUCCESS(status) && off < fat; off += 0x1000) {
        ((PULONG)buf)[0] = off ? 0 : 0xFFFFFFF8;
        at.QuadPart = 0x1000 + off;
        status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x1000, &at);
    }
    memset(buf, 0xFF, 0x1000);
    for (ULONG off = 0; NT_SUCCESS(status) && off < 0x10000; off += 0x1000) {
        at.QuadPart = 0x1000 + fat + off;
        status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x1000, &at);
    }
    if (NT_SUCCESS(status))
        status = NtFsControlFile(h, nullptr, nullptr, nullptr, &iosb, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
    NtClose(h);
    return status;
}

static void EXFATFormatTest(PUCHAR buf) {
    HANDLE h, hv;
    IO_STATUS_BLOCK iosb;
    // Something in X:, then the format.
    bool ok = Open(&h, "\\Device\\Harddisk0\\Partition3\\XkCache", GENERIC_READ, FILE_OPEN_IF, FILE_DIRECTORY_FILE) == 0;
    if (ok)
        NtClose(h);
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition3\\XkCache\\junk.bin", GENERIC_WRITE, FILE_OVERWRITE_IF, 0) == 0;
    if (ok) {
        memset(buf, 0x5A, 0x10000);
        LARGE_INTEGER zero = {};
        ok = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x10000, &zero) == 0;
        NtClose(h);
    }
    ok &= FormatLikeATitle("\\Device\\Harddisk0\\Partition3", buf) == 0;
    // Afterwards: nothing in X:\, a FATX volume with the header's cluster size and only its root directory in use, the header's serial shown.
    ok &= Open(&hv, "\\Device\\Harddisk0\\Partition3\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) == 0;
    if (ok) {
        ULONG size[6] = {}, vol[8] = {};
        NtQueryVolumeInformationFile(hv, &iosb, size, sizeof(size), FileFsSizeInformation);
        NtQueryVolumeInformationFile(hv, &iosb, vol, 18, FileFsVolumeInformation);
        KiTestEXFATFormatTotal = size[0];
        KiTestEXFATFormatFree = size[2];
        KiTestEXFATFormatSpc = size[4];
        UCHAR dir[0x100];
        ok = NtQueryDirectoryFile(hv, nullptr, nullptr, nullptr, &iosb, dir, sizeof(dir), FileDirectoryInformation, nullptr, TRUE) == (NTSTATUS)0xC000000F &&
                                  // STATUS_NO_SUCH_FILE: empty
             vol[2] == 0x12345678;
        NtClose(hv);
    }
    KiTestEXFATFormatOk = ok;
    // The format on the disk (spec/oxos_internal/05 §3): the file has X:'s serial and cluster size; with them forgotten, as a power-off does, and read back
    // from the file as a cold boot does, X: mounts again with them.
    ULONG saved[16] = {};
    bool kept = Open(&hv, "\\Device\\Harddisk0\\Partition2\\OXOS\\cache\\oxos_cache_partitions.cache", GENERIC_READ, FILE_OPEN,
                     FILE_NON_DIRECTORY_FILE) == 0;
    if (kept) {
        LARGE_INTEGER at = {};
        kept = NT_SUCCESS(NtReadFile(hv, nullptr, nullptr, nullptr, &iosb, saved, sizeof(saved), &at)) && iosb.Information == 64;
        NtClose(hv);
    }
    kept &= saved[0] == 0x50434F58 && saved[1] == 1 && saved[2 + 3] == 0x12345678 && saved[9 + 3] == 128; // "XOCP", version, X: is partition 3
    filesystems::EXFATForgetCachePartitions();
    if (memory::BootPagesPresent())
        filesystems::ReadCachePartitions();
    kept &= Open(&hv, "\\Device\\Harddisk0\\Partition3", GENERIC_READ, FILE_OPEN, 0) == 0;
    if (kept) {
        NtFsControlFile(hv, nullptr, nullptr, nullptr, &iosb, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
        NtClose(hv);
        kept = Open(&hv, "\\Device\\Harddisk0\\Partition3\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) == 0;
    }
    if (kept) {
        ULONG size[6] = {}, vol[8] = {};
        NtQueryVolumeInformationFile(hv, &iosb, size, sizeof(size), FileFsSizeInformation);
        NtQueryVolumeInformationFile(hv, &iosb, vol, 18, FileFsVolumeInformation);
        kept = vol[2] == 0x12345678 && size[4] == 128;
        NtClose(hv);
    }
    KiTestEXFATKeptFormatOk = kept;
    // E: is not a cache partition: the header write is refused and nothing changes.
    KiTestEXFATRefuseOk = FormatLikeATitle("\\Device\\Harddisk0\\Partition1", buf) == STATUS_ACCESS_DENIED;
}

// Allocations through two folder volumes in turn (a directory on E:, a file on Y:, a directory on E:), as a dashboard writing its cache on E: while a
// downloader uses Z:. Every folder volume must see the other's allocation-bitmap changes: with a cache copy each, E:'s second write put back its stale bitmap
// sector and cleared Y:'s cluster, which was then given out again (2026-10-30, a 2 TB disk). The volume check after the run (tests/boot_test.py) finds a
// cluster in use but not marked.
static void EXFATCrossFolderTest(PUCHAR buf) {
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    // From a previous run on the same image: gone first, so each step allocates again.
    static const PCSZ old[] = {"\\Device\\Harddisk0\\Partition1\\XkCross\\d1", "\\Device\\Harddisk0\\Partition1\\XkCross\\d0",
                               "\\Device\\Harddisk0\\Partition1\\XkCross", "\\Device\\Harddisk0\\Partition4\\XkCross.bin"};
    for (PCSZ path : old) {
        if (Open(&h, path, DELETE, FILE_OPEN, FILE_DELETE_ON_CLOSE) == 0)
            NtClose(h);
    }
    bool ok = Open(&h, "\\Device\\Harddisk0\\Partition1\\XkCross", GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE) == 0;
    if (ok)
        NtClose(h);
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition1\\XkCross\\d0", GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE) == 0;
    if (ok)
        NtClose(h);
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition4\\XkCross.bin", GENERIC_WRITE, FILE_CREATE, 0) == 0;
    if (ok) {
        memset(buf, 0xC5, 0x1000);
        LARGE_INTEGER zero = {};
        ok = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x1000, &zero) == 0;
        NtClose(h);
    }
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition1\\XkCross\\d1", GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE) == 0;
    if (ok)
        NtClose(h);
    KiTestEXFATCrossOk = ok;
}

void KiStartFATXSelfTests() {
    core::CreateSystemThread(FATXControllerThread, nullptr, 0, 9);
}

// E:\XkStale.stale: one cluster long, its FAT chain running two clusters further into clusters marked free. Growing it must allocate new clusters (the stale
// ones may be handed to anybody), and E:\XkPre.bin, created with an allocation size and written short, must hold no more clusters than its length. boot_test.py
// reads both back from the disk copy and checks the volume (no cluster used but free, none marked but unowned).
static void EXFATLengthTest(PUCHAR buf) {
    HANDLE h, hv = nullptr;
    IO_STATUS_BLOCK iosb;
    const ULONG cluster = 16 * 1024, grown = 3 * cluster + 100;
    bool ok = Open(&hv, "\\Device\\Harddisk0\\Partition1\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) == 0;
    ULONG before = ok ? FreeClusters(hv) : 0;
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition1\\XkStale.stale", GENERIC_READ | GENERIC_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE) == 0;
    if (ok) {
        for (ULONG i = 0; i < grown; i++)
            buf[i] = (UCHAR)(i * 7 + i / 251);
        LARGE_INTEGER at = {};
        ok = NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, grown, &at)) && iosb.Information == grown;
        // The file now spans 4 clusters: 3 more than before. A driver that follows the stale chain takes only 1.
        ok &= FreeClusters(hv) + 3 == before;
        memset(buf + grown, 0, grown);
        ok &= NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf + grown, grown, &at)) && iosb.Information == grown && memcmp(buf,
              buf + grown, grown) == 0;
        NtClose(h);
    }
    KiTestEXFATStaleOk = ok;

    // Created with a 256 KB allocation size, then 100 bytes written; an allocation-only change asks for 512 KB.
    before = FreeClusters(hv);
    OBJECT_STRING s;
    RtlInitAnsiString(&s, "\\Device\\Harddisk0\\Partition1\\XkPre.bin");
    OBJECT_ATTRIBUTES a = {nullptr, &s, OBJ_CASE_INSENSITIVE};
    LARGE_INTEGER allocation;
    allocation.QuadPart = 256 * 1024;
    ok = NT_SUCCESS(NtCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &a, &iosb, &allocation, 0, 0, FILE_OVERWRITE_IF,
                                 FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT));
    if (ok) {
        ok = FreeClusters(hv) == before; // nothing allocated yet
        memset(buf, 'p', 100);
        ok &= NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, 100, nullptr));
        LARGE_INTEGER more;
        more.QuadPart = 512 * 1024;
        ok &= NT_SUCCESS(NtSetInformationFile(h, &iosb, &more, sizeof(more), FileAllocationInformation));
        ok &= FreeClusters(hv) + 1 == before;
        NtClose(h);
    }
    KiTestEXFATPreallocOk = ok;
    if (hv)
        NtClose(hv);
}

// The size and allocation of an open file (FileStandardInformation).
static bool StandardSizes(HANDLE h, ULONGLONG *Allocation, ULONGLONG *EndOfFile) {
    ULONG standard[6] = {};
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtQueryInformationFile(h, &iosb, standard, sizeof(standard), FileStandardInformation)))
        return false;
    *Allocation = (ULONGLONG)standard[1] << 32 | standard[0];
    *EndOfFile = (ULONGLONG)standard[3] << 32 | standard[2];
    return true;
}

// E:\XkLarge.bin (spec/oxos_internal/07_exfat_driver.md §2): 8 KB written across the 4 GB mark and 4 KB 1 MB past it (size 0x100101000), both read back; then
// shrunk to 0x100000800, still over 4 GB, giving back its last clusters; then opened again for writing and 2 KB written at 4 GB. boot_test.py reads the file
// back from the disk copy.
static void EXFATLargeFileTest(PUCHAR buf) {
    constexpr ULONGLONG cluster = 16 * 1024, across = 0xFFFFF000, past = 0x100100000, shrunk = 0x100000800;
    constexpr ULONG acrossLength = 0x2000, pastLength = 0x1000, againLength = 0x800;
    constexpr ULONGLONG grown = past + pastLength;
    HANDLE h, hv;
    IO_STATUS_BLOCK iosb;
    if (Open(&hv, "\\Device\\Harddisk0\\Partition1\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) != 0)
        return;
    ULONG before = FreeClusters(hv);
    bool ok = Open(&h, "\\Device\\Harddisk0\\Partition1\\XkLarge.bin", GENERIC_READ | GENERIC_WRITE, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE) == 0;
    if (ok) {
        PUCHAR check = buf + 0x10000;
        for (ULONG i = 0; i < acrossLength; i++)
            buf[i] = (UCHAR)(i * 13 + 5);
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)across;
        ok = NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, acrossLength, &at)) && iosb.Information == acrossLength;
        ok &= NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, check, acrossLength, &at)) && memcmp(buf, check,
              acrossLength) == 0;
        memset(buf, 'L', pastLength);
        at.QuadPart = (LONGLONG)past;
        ok &= NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, pastLength, &at));
        ok &= NT_SUCCESS(NtReadFile(h, nullptr, nullptr, nullptr, &iosb, check, pastLength, &at)) && memcmp(buf, check, pastLength) == 0;
        ULONGLONG allocation, size;
        ok &= StandardSizes(h, &allocation, &size) && size == grown && allocation == (grown + cluster - 1) / cluster * cluster;
        ok &= FreeClusters(hv) + (grown + cluster - 1) / cluster == before;
        LARGE_INTEGER end;
        end.QuadPart = (LONGLONG)shrunk;
        ok &= NT_SUCCESS(NtSetInformationFile(h, &iosb, &end, sizeof(end), FileEndOfFileInformation));
        ok &= StandardSizes(h, &allocation, &size) && size == shrunk;
        ok &= FreeClusters(hv) + (shrunk + cluster - 1) / cluster == before;
        NtClose(h);
    }
    ok &= Open(&h, "\\Device\\Harddisk0\\Partition1\\XkLarge.bin", GENERIC_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE) == 0;
    if (ok) {
        for (ULONG i = 0; i < againLength; i++)
            buf[i] = (UCHAR)(i * 3 + 1);
        LARGE_INTEGER at;
        at.QuadPart = 0x100000000;
        ok = NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf, againLength, &at));
        NtClose(h);
    }
    KiTestEXFATLargeOk = ok;
    NtClose(hv);
}

// E:\ (spec/oxos_internal/05 §3: exFAT's own name rules): a file whose name has 255 characters, with an e acute (0xE9) in it, is written, found in a listing
// with its whole name, opened by its name in capitals, renamed to another 255-character name and deleted.
static void EXFATLongNameTest(PUCHAR buf) {
    constexpr ULONG PREFIX = 29, LENGTH = 255; // "\Device\Harddisk0\Partition1\"
    char path[PREFIX + LENGTH + 1], upper[PREFIX + LENGTH + 1], renamed[PREFIX + LENGTH + 1];
    memcpy(path, "\\Device\\Harddisk0\\Partition1\\", PREFIX);
    for (ULONG i = 0; i < LENGTH; i++)
        path[PREFIX + i] = i == 7 ? (char)0xE9 : (char)('a' + i % 26);
    path[PREFIX + LENGTH] = 0;
    memcpy(upper, path, sizeof(path));
    memcpy(renamed, path, sizeof(path));
    for (ULONG i = PREFIX; i < PREFIX + LENGTH; i++)
        upper[i] = (char)toupper((UCHAR)upper[i]);
    renamed[PREFIX + LENGTH - 1] = 'Z';
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    bool ok = Open(&h, path, GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE) == 0;
    if (ok) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        ok = NT_SUCCESS(NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, (PVOID)"long", 4, &at));
        NtClose(h);
    }
    // The root's listing has it, with all 255 characters.
    bool listed = false;
    if (ok && Open(&h, "\\Device\\Harddisk0\\Partition1\\", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE) == 0) {
        for (BOOLEAN first = TRUE; !listed; first = FALSE) {
            if (!NT_SUCCESS(NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x1000, FileDirectoryInformation, nullptr, first)))
                break;
            for (PUCHAR e = buf;; e += *(PULONG)e) {
                listed |= *(PULONG)(e + 0x3C) == LENGTH && memcmp(e + 0x40, path + PREFIX, LENGTH) == 0 && *(PULONG)(e + 0x28) == 4;
                if (!*(PULONG)e)
                    break;
            }
        }
        NtClose(h);
    }
    ok &= listed;
    // Opened in capitals (the ASCII letters; exFAT compares names in any case), renamed, gone under its old name, deleted.
    ok &= Open(&h, upper, GENERIC_READ | DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE) == 0;
    if (ok) {
        FILE_RENAME_INFORMATION rename = {FALSE, nullptr, objects::ObjectName(renamed + PREFIX)};
        ok = NT_SUCCESS(NtSetInformationFile(h, &iosb, &rename, sizeof(rename), FileRenameInformation));
        NtClose(h);
    }
    ok &= Open(&h, path, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE) == STATUS_OBJECT_NAME_NOT_FOUND;
    ok &= NT_SUCCESS(io::DeleteFile(renamed)) && Open(&h, renamed, GENERIC_READ, FILE_OPEN, 0) == STATUS_OBJECT_NAME_NOT_FOUND;
    KiTestEXFATLongNameOk = ok;
}
