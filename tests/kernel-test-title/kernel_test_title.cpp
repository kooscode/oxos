// OXOS kernel test title: a minimal Xbox "game" that the automated boot tests run from a disc, to check that the
// kernel loads and starts titles and that the kernel functions every title relies on work.
//
// How it runs (make test, make test-relaunch):
//   1. The Makefile compiles this file freestanding (no SDK, no C library) and tools/mkxbe.py turns it into an XBE
//      ("OXOS kernel test title", title ID 0x584B0001).
//   2. tests/mkxiso.py writes it to a generated test disc as default.xbe, next to the files it reads (media\a.txt,
//      a copy of itself as media\second.xbe, and relaunch.txt for the relaunch test).
//   3. The kernel boots the disc like a game. XkTitleEntry() runs the tests below and records each result in
//      XkTitleResults: 1 = passed, 0 = failed (or not reached).
//   4. tests/boot_test.py finds XkTitleResults through the title's map file, reads it from xemu's memory over QMP
//      and reports each field as a check (the "M4a" checks of make test).
//
// Tests, in order (one RESULTS field each):
//   - the loader started the title, and XeImageFileName holds its path
//   - KeTickCount advances (timer interrupt and sleep)
//   - pool allocation (ExAllocatePoolWithTag / ExFreePool)
//   - reading a file from the disc (NtOpenFile / NtReadFile on \Device\CdRom0)
//   - creating a thread and waiting for it (PsCreateSystemThread / NtWaitForSingleObject)
//   - a drive-letter link to a hard-disk folder, then a read through it (IoCreateSymbolicLink, FATX)
//   - loading and unloading a section on demand (XeLoadSection / XeUnloadSection)
//   - SHA-1 (XcSHAInit / XcSHAUpdate / XcSHAFinal, known answer for "abc")
// Relaunch test (only when the disc has relaunch.txt): the title restarts itself as media\second.xbe the way games
// do with XLaunchNewImage (a persisted LaunchDataPage, then a quick reboot); the second run checks that the launch
// data and the first run's results survived, then asks for a missing XBE so the kernel falls back to the dashboard.
typedef unsigned long ULONG, *PULONG;
typedef long NTSTATUS, LONG;
typedef unsigned char UCHAR, BOOLEAN;
typedef unsigned short USHORT;
typedef void *PVOID, *HANDLE;
#define NTAPI __stdcall

struct STRING {
    USHORT Length, MaximumLength;
    char *Buffer;
};
struct OBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    STRING *ObjectName;
    ULONG Attributes;
};
struct IO_STATUS_BLOCK {
    NTSTATUS Status;
    ULONG Information;
};
struct LARGE_INTEGER {
    ULONG Low;
    LONG High;
};

// Kernel imports: the loader replaces each entry (0x80000000 | export ordinal) with the function's or variable's
// address before the title starts, as for any XBE. K(name, type) calls one through its entry.
enum : ULONG {
    I_KeTickCount, I_XeImageFileName, I_ExAllocatePoolWithTag, I_ExFreePool, I_NtOpenFile, I_NtReadFile, I_NtClose,
    I_PsCreateSystemThread, I_NtWaitForSingleObject, I_KeDelayExecutionThread, I_IoCreateSymbolicLink,
    I_XeLoadSection, I_XeUnloadSection, I_XcSHAInit, I_XcSHAUpdate, I_XcSHAFinal, I_MmAllocateContiguousMemory,
    I_MmPersistContiguousMemory, I_MmFreeContiguousMemory, I_LaunchDataPage, I_HalReturnToFirmware, I_COUNT
};
extern "C" ULONG XkThunks[I_COUNT + 1] = {
    0x80000000 | 156, 0x80000000 | 326, 0x80000000 | 15,  0x80000000 | 17,  0x80000000 | 202, 0x80000000 | 219,
    0x80000000 | 187, 0x80000000 | 254, 0x80000000 | 233, 0x80000000 | 99,  0x80000000 | 67,  0x80000000 | 327,
    0x80000000 | 328, 0x80000000 | 335, 0x80000000 | 336, 0x80000000 | 337,
    0x80000000 | 165, 0x80000000 | 178, 0x80000000 | 171, 0x80000000 | 164, 0x80000000 | 49,  0};
#define K(name, type) ((type)XkThunks[I_##name])

// The results boot_test.py reads; each test sets its field to 1 when it passes.
struct RESULTS {
    ULONG Magic;          // 'XKTT' once finished
    ULONG Started;        // 1 when the entry point ran
    ULONG TickAdvanced;   // KeTickCount moved while sleeping 30 ms
    ULONG PoolOk;
    ULONG DiscReadOk;     // \Device\CdRom0\media\a.txt
    ULONG ThreadOk;       // PsCreateSystemThread + wait
    ULONG PathLinkOk;     // \??\T: -> \Device\Harddisk0\Partition1\XkTest, read keep.bin
    ULONG SectionOk;      // XeLoadSection of the non-preload section, content check, unload
    ULONG ShaOk;          // XcSHA* known answer ("abc")
    char ImageName[64];   // XeImageFileName
    // Milestone 5d-2: the first run relaunches itself as media\second.xbe through a persisted LaunchDataPage and a
    // quick reboot; the second run restores the fields above from LaunchData and checks the launch.
    ULONG Relaunched;     // 1 in the second run, with the first run's results intact
    ULONG DriveLinkOk;    // \??\D: -> \Device\CdRom0\media
    ULONG LaunchPageOk;   // LaunchDataPage persisted with type, title id, flags and data
    char SecondName[64];  // XeImageFileName of the second run
};
extern "C" volatile RESULTS XkTitleResults = {};
__attribute__((section(".extra"))) extern "C" const char XkExtraData[] = "data from a section loaded on demand";

// --- Helpers (the title has no C library)

// Test 5's thread: stores the argument it was started with.
static volatile ULONG ThreadRan;
static void NTAPI ThreadBody(PVOID ctx) { ThreadRan = (ULONG)ctx; }

// A counted string for the kernel's object names.
static void Init(STRING *s, const char *t) {
    USHORT n = 0;
    while (t[n])
        n++;
    s->Length = n;
    s->MaximumLength = n + 1;
    s->Buffer = (char *)t;
}

// Compares n bytes.
static bool Equal(const char *a, const char *b, ULONG n) {
    for (ULONG i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

// Opens a file by its kernel path, reads up to len bytes from its start and closes it; *got = bytes read.
static NTSTATUS ReadFile(const char *path, char *buf, ULONG len, ULONG *got) {
    STRING s;
    Init(&s, path);
    OBJECT_ATTRIBUTES a = {nullptr, &s, 0x40};
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    auto open = K(NtOpenFile, NTSTATUS(NTAPI *)(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, ULONG, ULONG));
    NTSTATUS st = open(&h, 0x80000000 | 0x100000, &a, &iosb, 1, 0x20 | 0x40);
    if (st < 0)
        return st;
    auto read = K(NtReadFile, NTSTATUS(NTAPI *)(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG,
                                               LARGE_INTEGER *));
    st = read(h, nullptr, nullptr, nullptr, &iosb, buf, len, nullptr);
    *got = iosb.Information;
    K(NtClose, NTSTATUS(NTAPI *)(HANDLE))(h);
    return st;
}

// Waits ms milliseconds (a negative due time is relative, in 100 ns units).
static void Sleep(LONG ms) {
    LARGE_INTEGER t = {(ULONG)(-10000 * ms), -1};
    K(KeDelayExecutionThread, NTSTATUS(NTAPI *)(char, BOOLEAN, LARGE_INTEGER *))(0, 0, &t);
}

// --- Relaunch test

// The page a title leaves for the next one (XLaunchNewImage): which image to start, and up to 3 KB of data for it.
struct LAUNCH_DATA_PAGE {
    ULONG LaunchDataType, TitleId;
    char LaunchPath[520];
    ULONG Flags;
    UCHAR Pad[492];
    UCHAR LaunchData[3072];
};
static_assert(sizeof(LAUNCH_DATA_PAGE) == 4096);
static const ULONG LaunchMarker = 0x324B5858; // "XXK2": first dword of our launch data
static const ULONG FirstRunBytes = 9 * 4 + 64; // the first run's results (RESULTS up to ImageName), carried over

// Copies n bytes.
static void Copy(volatile void *to, const volatile void *from, ULONG n) {
    for (ULONG i = 0; i < n; i++)
        ((volatile char *)to)[i] = ((const volatile char *)from)[i];
}

static void Relaunch(volatile RESULTS &r, const char *path);

// Relaunch test, second run (media\second.xbe, started by Relaunch() below): restore the first run's results from
// the launch data, then check what the kernel handed over.
static void SecondRun(volatile RESULTS &r, LAUNCH_DATA_PAGE *ldp) {
    Copy(&r, ldp->LaunchData + 4, FirstRunBytes);
    r.Relaunched = 1;
    // The kernel started the image the launch page named.
    auto name = (STRING *)XkThunks[I_XeImageFileName];
    for (ULONG i = 0; i < name->Length && i < sizeof(r.SecondName) - 1; i++)
        r.SecondName[i] = name->Buffer[i];
    // The launch page survived the quick reboot intact, and the kernel moved it below itself (out of the title's way).
    r.LaunchPageOk =ldp->LaunchDataType == 1 && ldp->TitleId == 0x584B0001 && (ldp->Flags & 1) &&
                     *(ULONG *)ldp->LaunchData == LaunchMarker && (ULONG)ldp < 0x80010000; // moved below the kernel
    // The kernel maps D: to the folder the title was started from (here \Device\CdRom0\media).
    char buf[64];
    ULONG got = 0;
    r.DriveLinkOk = ReadFile("\\??\\D:\\a.txt", buf, sizeof(buf), &got) == 0 && got == 9 && Equal(buf, "hello dvd", 9);
    // Done with the launch page, as a game would be.
    *(PVOID *)XkThunks[I_LaunchDataPage] = nullptr;
    K(MmFreeContiguousMemory, void(NTAPI *)(PVOID))(ldp);
    r.Magic = 0x54544B58;
    // Third launch (milestone 5e): an image that does not exist. The kernel falls back to the dashboard with
    // LaunchDataPage = {type 1, reason 1, code 1}; the host has 8 s to collect the results above first.
    Sleep(8000);
    Relaunch(r, "\\Device\\CdRom0\\media;missing.xbe");
    for (;;)
        Sleep(1000);
}

// XLaunchNewImage, the way the XAPI does it: a persisted page describes the next title, then a quick reboot.
static void Relaunch(volatile RESULTS &r, const char *path) {
    auto ldp = (LAUNCH_DATA_PAGE *)K(MmAllocateContiguousMemory, PVOID(NTAPI *)(ULONG))(4096);
    if (!ldp)
        return;
    for (ULONG i = 0; i < 4096; i++)
        ((volatile char *)ldp)[i] = 0;
    K(MmPersistContiguousMemory, void(NTAPI *)(PVOID, ULONG, BOOLEAN))(ldp, 4096, 1);
    ldp->LaunchDataType = 1;
    ldp->TitleId = 0x584B0001;
    ULONG n = 0;
    while (path[n])
        n++;
    Copy(ldp->LaunchPath, path, n + 1);
    *(ULONG *)ldp->LaunchData = LaunchMarker;
    Copy(ldp->LaunchData + 4, &r, FirstRunBytes);
    *(PVOID *)XkThunks[I_LaunchDataPage] = ldp;
    K(HalReturnToFirmware, void(NTAPI *)(ULONG))(2);
}

// The title's entry point: runs each test in turn and records the result (see the list at the top of the file).
extern "C" void __cdecl XkTitleEntry() {
    volatile RESULTS &r = XkTitleResults;
    // A launch page with this title's marker means this is the relaunch test's second run (a page from another
    // launcher, such as tools/iso-launcher, is not).
    auto ldp = *(LAUNCH_DATA_PAGE **)XkThunks[I_LaunchDataPage];
    if (ldp && *(ULONG *)ldp->LaunchData == LaunchMarker)
        SecondRun(r, ldp);

    // Test 1: the loader started the title, and XeImageFileName holds the path it was loaded from.
    r.Started = 1;
    auto name = (STRING *)XkThunks[I_XeImageFileName];
    for (ULONG i = 0; i < name->Length && i < sizeof(r.ImageName) - 1; i++)
        r.ImageName[i] = name->Buffer[i];

    // Test 2: the clock ticks. KeTickCount (milliseconds) must advance by about 30 during a 30 ms sleep, which needs
    // the timer interrupt and KeDelayExecutionThread to work.
    volatile ULONG *tick = (volatile ULONG *)XkThunks[I_KeTickCount];
    ULONG t0 = *tick;
    Sleep(30);
    r.TickAdvanced = *tick - t0 >= 25;

    // Test 3: the kernel's memory pool hands out and takes back a small block.
    auto alloc = K(ExAllocatePoolWithTag, PVOID(NTAPI *)(ULONG, ULONG));
    char *p = (char *)alloc(100, 'tseT');
    r.PoolOk = p != nullptr;
    if (p)
        K(ExFreePool, void(NTAPI *)(PVOID))(p);

    // Test 4: read a file from the disc the title runs from (DVD driver and XDVDFS), and check its contents.
    char buf[64];
    ULONG got = 0;
    r.DiscReadOk = ReadFile("\\Device\\CdRom0\\media\\a.txt", buf, sizeof(buf), &got) == 0 && got == 9 &&
                   Equal(buf, "hello dvd", 9);

    // Test 5: start a second thread, wait for it to finish, and check that it ran with the argument it was given.
    HANDLE th;
    auto create =K(PsCreateSystemThread, NTSTATUS(NTAPI *)(HANDLE *, HANDLE *, PVOID, PVOID, BOOLEAN));
    if (create(&th, nullptr, (PVOID)ThreadBody, (PVOID)42, 0) == 0) {
        K(NtWaitForSingleObject, NTSTATUS(NTAPI *)(HANDLE, BOOLEAN, LARGE_INTEGER *))(th, 0, nullptr);
        K(NtClose, NTSTATUS(NTAPI *)(HANDLE))(th);
        r.ThreadOk = ThreadRan == 42;
    }

    // Test 6: map drive letter T: to a folder on the hard disk (as games do for their save and cache drives), then
    // read a file the test prepared there through the new drive letter (object manager links and FATX).
    STRING link, target;
    Init(&link, "\\??\\T:");
    Init(&target, "\\Device\\Harddisk0\\Partition1\\XkTest");
    if (K(IoCreateSymbolicLink, NTSTATUS(NTAPI *)(STRING *, STRING *))(&link, &target) == 0)
        r.PathLinkOk = ReadFile("\\??\\T:\\keep.bin", buf, sizeof(buf), &got) == 0 && got == 26 &&
                       Equal(buf, "open xbox kernel fatx test", 26);

    // Test 7: load a section on demand. The ".extra" section (XkExtraData) is marked not to be loaded at start, as
    // games do with level data; XeLoadSection must bring it in with the right contents, and XeUnloadSection release
    // it. Section headers follow the XBE image header at 0x10000.
    ULONG count = *(ULONG *)0x1011C;
    ULONG *sections = (ULONG *)*(ULONG *)0x10120;
    for (ULONG i = 0; i < count; i++) {
        ULONG *s = sections + i * 14;
        if (Equal((const char *)s[5], ".extra", 7)) {
            NTSTATUS a = K(XeLoadSection, NTSTATUS(NTAPI *)(PVOID))(s);
            bool ok = a == 0 && Equal((const char *)XkExtraData, "data from a section loaded on demand", 36);
            ok &= K(XeUnloadSection, NTSTATUS(NTAPI *)(PVOID))(s) == 0;
            r.SectionOk = ok;
        }
    }

    // Test 8: the kernel's SHA-1 gives the standard answer for "abc" (a9993e36...9cd0d89d).
    UCHAR ctx[116], digest[20];
    K(XcSHAInit, void(NTAPI *)(UCHAR *))(ctx);
    K(XcSHAUpdate, void(NTAPI *)(UCHAR *, const UCHAR *, ULONG))(ctx, (const UCHAR *)"abc", 3);
    K(XcSHAFinal, void(NTAPI *)(UCHAR *, UCHAR *))(ctx, digest);
    r.ShaOk = digest[0] == 0xA9 && digest[1] == 0x99 && digest[19] == 0x9D;

    // Relaunch test, first run: only the relaunch test's disc has relaunch.txt. The title then restarts itself as
    // media\second.xbe (the run continues in SecondRun); otherwise it marks the results complete and idles.
    if (ReadFile("\\Device\\CdRom0\\relaunch.txt", buf, sizeof(buf), &got) == 0)
        Relaunch(r, "\\Device\\CdRom0\\media;second.xbe");
    r.Magic = 0x54544B58;
    for (;;)
        Sleep(1000);
}
