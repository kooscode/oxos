// Test title for the XBE loader (milestone 4a). Built freestanding with clang, converted by tools/mkxbe.py, and
// started from the test disc. Results land in XkTitleResults; boot_test.py reads them through the title's map.
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

// Kernel imports, resolved by the loader (0x80000000 | ordinal).
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

static volatile ULONG ThreadRan;
static void NTAPI ThreadBody(PVOID ctx) { ThreadRan = (ULONG)ctx; }

static void Init(STRING *s, const char *t) {
    USHORT n = 0;
    while (t[n])
        n++;
    s->Length = n;
    s->MaximumLength = n + 1;
    s->Buffer = (char *)t;
}

static bool Equal(const char *a, const char *b, ULONG n) {
    for (ULONG i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

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

static void Sleep(LONG ms) {
    LARGE_INTEGER t = {(ULONG)(-10000 * ms), -1};
    K(KeDelayExecutionThread, NTSTATUS(NTAPI *)(char, BOOLEAN, LARGE_INTEGER *))(0, 0, &t);
}

struct LAUNCH_DATA_PAGE {
    ULONG LaunchDataType, TitleId;
    char LaunchPath[520];
    ULONG Flags;
    UCHAR Pad[492];
    UCHAR LaunchData[3072];
};
static_assert(sizeof(LAUNCH_DATA_PAGE) == 4096);
static const ULONG LaunchMarker = 0x324B5858; // "XXK2"
static const ULONG FirstRunBytes = 9 * 4 + 64;

static void Copy(volatile void *to, const volatile void *from, ULONG n) {
    for (ULONG i = 0; i < n; i++)
        ((volatile char *)to)[i] = ((const volatile char *)from)[i];
}

static void SecondRun(volatile RESULTS &r, LAUNCH_DATA_PAGE *ldp) {
    Copy(&r, ldp->LaunchData + 4, FirstRunBytes);
    r.Relaunched = 1;
    auto name = (STRING *)XkThunks[I_XeImageFileName];
    for (ULONG i = 0; i < name->Length && i < sizeof(r.SecondName) - 1; i++)
        r.SecondName[i] = name->Buffer[i];
    r.LaunchPageOk = ldp->LaunchDataType == 1 && ldp->TitleId == 0x584B0001 && (ldp->Flags & 1) &&
                     *(ULONG *)ldp->LaunchData == LaunchMarker;
    char buf[64];
    ULONG got = 0;
    r.DriveLinkOk = ReadFile("\\??\\D:\\a.txt", buf, sizeof(buf), &got) == 0 && got == 9 && Equal(buf, "hello dvd", 9);
    *(PVOID *)XkThunks[I_LaunchDataPage] = nullptr;
    K(MmFreeContiguousMemory, void(NTAPI *)(PVOID))(ldp);
    r.Magic = 0x54544B58;
    for (;;)
        Sleep(1000);
}

// XLaunchNewImage, the way the XAPI does it: a persisted page describes the next title, then a quick reboot.
static void Relaunch(volatile RESULTS &r) {
    auto ldp = (LAUNCH_DATA_PAGE *)K(MmAllocateContiguousMemory, PVOID(NTAPI *)(ULONG))(4096);
    if (!ldp)
        return;
    for (ULONG i = 0; i < 4096; i++)
        ((volatile char *)ldp)[i] = 0;
    K(MmPersistContiguousMemory, void(NTAPI *)(PVOID, ULONG, BOOLEAN))(ldp, 4096, 1);
    ldp->LaunchDataType = 1;
    ldp->TitleId = 0x584B0001;
    const char path[] = "\\Device\\CdRom0\\media;second.xbe";
    Copy(ldp->LaunchPath, path, sizeof(path));
    *(ULONG *)ldp->LaunchData = LaunchMarker;
    Copy(ldp->LaunchData + 4, &r, FirstRunBytes);
    *(PVOID *)XkThunks[I_LaunchDataPage] = ldp;
    K(HalReturnToFirmware, void(NTAPI *)(ULONG))(2);
}

extern "C" void __cdecl XkTitleEntry() {
    volatile RESULTS &r = XkTitleResults;
    auto ldp = *(LAUNCH_DATA_PAGE **)XkThunks[I_LaunchDataPage];
    if (ldp)
        SecondRun(r, ldp);
    r.Started = 1;

    auto name = (STRING *)XkThunks[I_XeImageFileName];
    for (ULONG i = 0; i < name->Length && i < sizeof(r.ImageName) - 1; i++)
        r.ImageName[i] = name->Buffer[i];

    volatile ULONG *tick = (volatile ULONG *)XkThunks[I_KeTickCount];
    ULONG t0 = *tick;
    Sleep(30);
    r.TickAdvanced = *tick - t0 >= 25;

    auto alloc = K(ExAllocatePoolWithTag, PVOID(NTAPI *)(ULONG, ULONG));
    char *p = (char *)alloc(100, 'tseT');
    r.PoolOk = p != nullptr;
    if (p)
        K(ExFreePool, void(NTAPI *)(PVOID))(p);

    char buf[64];
    ULONG got = 0;
    r.DiscReadOk = ReadFile("\\Device\\CdRom0\\media\\a.txt", buf, sizeof(buf), &got) == 0 && got == 9 &&
                   Equal(buf, "hello dvd", 9);

    HANDLE th;
    auto create = K(PsCreateSystemThread, NTSTATUS(NTAPI *)(HANDLE *, HANDLE *, PVOID, PVOID, BOOLEAN));
    if (create(&th, nullptr, (PVOID)ThreadBody, (PVOID)42, 0) == 0) {
        K(NtWaitForSingleObject, NTSTATUS(NTAPI *)(HANDLE, BOOLEAN, LARGE_INTEGER *))(th, 0, nullptr);
        K(NtClose, NTSTATUS(NTAPI *)(HANDLE))(th);
        r.ThreadOk = ThreadRan == 42;
    }

    STRING link, target;
    Init(&link, "\\??\\T:");
    Init(&target, "\\Device\\Harddisk0\\Partition1\\XkTest");
    if (K(IoCreateSymbolicLink, NTSTATUS(NTAPI *)(STRING *, STRING *))(&link, &target) == 0)
        r.PathLinkOk = ReadFile("\\??\\T:\\keep.bin", buf, sizeof(buf), &got) == 0 && got == 26 &&
                       Equal(buf, "open xbox kernel fatx test", 26);

    // Section headers follow the image header; the non-preload one is ".extra".
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

    UCHAR ctx[116], digest[20];
    K(XcSHAInit, void(NTAPI *)(UCHAR *))(ctx);
    K(XcSHAUpdate, void(NTAPI *)(UCHAR *, const UCHAR *, ULONG))(ctx, (const UCHAR *)"abc", 3);
    K(XcSHAFinal, void(NTAPI *)(UCHAR *, UCHAR *))(ctx, digest);
    r.ShaOk = digest[0] == 0xA9 && digest[1] == 0x99 && digest[19] == 0x9D;

    if (ReadFile("\\Device\\CdRom0\\relaunch.txt", buf, sizeof(buf), &got) == 0) // the quick-reboot test disc
        Relaunch(r);
    r.Magic = 0x54544B58;
    for (;;)
        Sleep(1000);
}
