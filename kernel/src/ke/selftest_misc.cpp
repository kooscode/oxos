// Boot self-tests for dismount, Fsc and PHY (milestone 5d-1).
#include <xk/arch.h>
#include <xk/io.h>
#include <xk/rtl.h>

XBAPI NTSTATUS NTAPI IoDismountVolumeByName(POBJECT_STRING DeviceName);
XBAPI ULONG NTAPI FscGetCacheSize();
XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG);
XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN, PVOID);
XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN);
extern volatile ULONG KiTestXcDone, KiTestFatxDone;
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID, ULONG, ULONG, ULONG, ULONG, ULONG);
XBAPI void NTAPI AvSendTVEncoderOption(PVOID, ULONG, ULONG, PULONG);
extern volatile UCHAR AvpEncoderAddress;
extern volatile ULONG AvpEncoderProgrammed, AvpEncoderMismatches;
volatile ULONG KiTestAvModesTried, KiTestAvModesOk; // TV encoder modes set / programmed and read back correctly

volatile ULONG KiTestMiscDismountOk, KiTestMiscDiscRemountOk, KiTestMiscFscOk, KiTestMiscPhyStatus,
    KiTestMiscPhyLink, KiTestMiscCmosOk, KiTestMiscSmcOk, KiTestMiscScatterOk, KiTestMiscDone;
volatile ULONG KiTestMiscPmTicksPerSecond, KiTestMiscPerfOk;
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceCounter();
XBAPI LARGE_INTEGER NTAPI KeQueryPerformanceFrequency();
void HalpSmcHandleEvents(ULONG reason);
XBAPI NTSTATUS NTAPI NtReadFileScatter(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT,
                                       ULONG, PLARGE_INTEGER);
XBAPI NTSTATUS NTAPI NtWriteFileGather(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT,
                                       ULONG, PLARGE_INTEGER);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount);
bool HalpCmosSignatureValid();
void HalpWriteCmosSignature();

static NTSTATUS OpenClose(PCSZ path) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, path);
    OBJECT_ATTRIBUTES a = {nullptr, &s, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS status = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &a, &iosb, FILE_SHARE_READ,
                                 FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(status))
        NtClose(h);
    return status;
}

static PDEVICE_OBJECT Device(PCSZ path) {
    OBJECT_STRING s;
    RtlInitAnsiString(&s, path);
    PVOID d = nullptr;
    if (NT_SUCCESS(ObReferenceObjectByName(&s, 0, nullptr, nullptr, &d)))
        ObfDereferenceObject(d);
    return (PDEVICE_OBJECT)d;
}

static void NTAPI MiscControllerThread(PVOID) {
    // After the FATX suite too: the scatter/gather test writes in its E:\XkTest, and E:\ must not change while that
    // suite lists it and balances its free clusters.
    while (!KiTestXcDone || !KiTestFatxDone) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    OBJECT_STRING e, z, cd;
    RtlInitAnsiString(&e, "\\Device\\Harddisk0\\Partition1");
    RtlInitAnsiString(&z, "\\Device\\Harddisk0\\Partition5");
    RtlInitAnsiString(&cd, "\\Device\\CdRom0");
    // Any open mounts the volume; E:'s root exists on every disk.
    bool ok = OpenClose("\\Device\\Harddisk0\\Partition1\\") == 0;
    PDEVICE_OBJECT part = Device("\\Device\\Harddisk0\\Partition1");
    ok &= part && part->MountedOrSelfDevice != nullptr;
    ok &= IoDismountVolumeByName(&e) == 0 && part->MountedOrSelfDevice == nullptr;
    ok &= OpenClose("\\Device\\Harddisk0\\Partition1\\") == 0 && part->MountedOrSelfDevice != nullptr;
    ok &= IoDismountVolumeByName(&z) == (NTSTATUS)0xC000026E; // never mounted
    KiTestMiscDismountOk = ok;

    PDEVICE_OBJECT disc = Device("\\Device\\CdRom0");
    ok = OpenClose("\\Device\\CdRom0\\media\\a.txt") == 0 && IoDismountVolumeByName(&cd) == 0 &&
         disc->MountedOrSelfDevice == nullptr && OpenClose("\\Device\\CdRom0\\media\\a.txt") == 0;
    KiTestMiscDiscRemountOk = ok;

    ULONG before = FscGetCacheSize();
    ok = FscSetCacheSize(0x801) == STATUS_INVALID_PARAMETER && FscSetCacheSize(32) == 0 && FscGetCacheSize() == 32;
    FscSetCacheSize(before);
    KiTestMiscFscOk = ok;

    // CMOS "clock set" signature (spec/stock/05 §1): written, detected, a damaged byte detected, restored.
    HalpWriteCmosSignature();
    ok = HalpCmosSignatureValid();
    __asm__ volatile("cli; movb $0x90, %%al; outb %%al, $0x72; xorl %%eax, %%eax; outb %%al, $0x71; sti" ::: "eax");
    ok &= !HalpCmosSignatureValid();
    HalpWriteCmosSignature();
    ok &= HalpCmosSignatureValid();
    KiTestMiscCmosOk = ok;

    // SMC tray events (spec/stock/12 §4): "tray opening" caches state 0, "tray state changed" drops the cache (the SMC is
    // read again: 0x60 with the test disc); each one counts a change.
    ULONG tray, count0, count1, count2;
    HalReadSMCTrayState(&tray, &count0);
    HalpSmcHandleEvents(0x40);
    ok = NT_SUCCESS(HalReadSMCTrayState(&tray, &count1)) && tray == 0 && count1 == count0 + 1;
    HalpSmcHandleEvents(0x02);
    ok &= NT_SUCCESS(HalReadSMCTrayState(&tray, &count2)) && tray == 0x60 && count2 == count1 + 1;
    KiTestMiscSmcOk = ok;

    // FATX scatter/gather: three pages written with NtWriteFileGather, read back into pages in reverse order with
    // NtReadFileScatter, then the file is deleted.
    {
        auto pages = (PUCHAR)MmAllocateContiguousMemory(6 * PAGE_SIZE);
        ok = pages != nullptr;
        if (ok) {
            FILE_SEGMENT_ELEMENT out[4] = {}, in[4] = {};
            for (ULONG i = 0; i < 3; i++) {
                RtlFillMemory(pages + i * PAGE_SIZE, PAGE_SIZE, (UCHAR)(0xA0 + i));
                out[i].Buffer = pages + i * PAGE_SIZE;
                in[i].Buffer = pages + (5 - i) * PAGE_SIZE;
            }
            OBJECT_STRING name;
            RtlInitAnsiString(&name, "\\Device\\Harddisk0\\Partition1\\XkTest\\sg.bin");
            OBJECT_ATTRIBUTES a = {nullptr, &name, OBJ_CASE_INSENSITIVE};
            IO_STATUS_BLOCK iosb;
            HANDLE h;
            ok = NT_SUCCESS(NtCreateFile(&h, GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE, &a, &iosb, nullptr,
                                         0x80 /*FILE_ATTRIBUTE_NORMAL*/, 0, FILE_OVERWRITE_IF,
                                         FILE_NO_INTERMEDIATE_BUFFERING | FILE_SYNCHRONOUS_IO_NONALERT));
            if (ok) {
                LARGE_INTEGER zero;
                zero.QuadPart = 0;
                ok &= NtWriteFileGather(h, nullptr, nullptr, nullptr, &iosb, out, 3 * PAGE_SIZE, &zero) == 0 &&
                      iosb.Information == 3 * PAGE_SIZE;
                ok &= NtReadFileScatter(h, nullptr, nullptr, nullptr, &iosb, in, 3 * PAGE_SIZE, &zero) == 0 &&
                      iosb.Information == 3 * PAGE_SIZE;
                for (ULONG i = 0; i < 3 && ok; i++)
                    ok = pages[(5 - i) * PAGE_SIZE] == 0xA0 + i && pages[(5 - i) * PAGE_SIZE + PAGE_SIZE - 1] == 0xA0 + i;
                ok &= NtReadFileScatter(h, nullptr, nullptr, nullptr, &iosb, in, 100, &zero) == STATUS_INVALID_PARAMETER;
                FILE_DISPOSITION_INFORMATION del = {TRUE};
                ok &= NtSetInformationFile(h, &iosb, &del, sizeof(del), FileDispositionInformation) == 0;
                NtClose(h);
            }
            MmFreeContiguousMemory(pages);
        }
        KiTestMiscScatterOk = ok;
    }

    // Performance counter: monotonic, and one KeTickCount second ≈ KeQueryPerformanceFrequency (3.375 MHz).
    {
        ULONG t0 = KeTickCount;
        while (KeTickCount == t0) {
        }
        LARGE_INTEGER start = KeQueryPerformanceCounter(), last = start, now;
        ULONG startTick = KeTickCount;
        bool monotonic = true;
        while (KeTickCount - startTick < 1000) {
            now = KeQueryPerformanceCounter();
            monotonic &= now.QuadPart >= last.QuadPart;
            last = now;
        }
        LONGLONG rate = last.QuadPart - start.QuadPart, f = KeQueryPerformanceFrequency().QuadPart;
        KiTestMiscPmTicksPerSecond = (ULONG)rate;
        KiTestMiscPerfOk = monotonic && f == 3375000 && rate > f * 98 / 100 && rate < f * 102 / 100;
    }

    // TV encoder (spec/oxos-internal/tv-encoders.md): every supported mode word programs the generated registers,
    // which read back as written (xemu's encoder models store them); then the previous mode is restored. Conexant
    // (xemu's default model): the 8 SD and 4 HD tests below; Focus (make test-encoders): the 4 HD ones.
    static const ULONG cxModes[] = {0x04010101, 0x04020202, 0x04010103, 0x44030307, 0x44040408, 0x0401010B,
                                    0x2001010B, 0x60030307, 0x88070701, 0x88080801, 0x880B0A02, 0x880E0C03};
    const ULONG *modesBegin = AvpEncoderAddress == 0x8A ? cxModes : AvpEncoderAddress == 0xD4 ? cxModes + 8 : nullptr;
    if (modesBegin) {
        const ULONG *modesEnd = cxModes + sizeof(cxModes) / sizeof(cxModes[0]);
        ULONG previous = 0;
        AvSendTVEncoderOption(nullptr, 0x0D, 0, &previous);
        ULONG fb = *(volatile ULONG *)0xFD600800, ok = 0;
        for (const ULONG *mode = modesBegin; mode < modesEnd; mode++) {
            AvSetDisplayMode((PVOID)0xFD000000, 0, *mode, 0x12, 640 * 4, fb);
            if (AvpEncoderProgrammed == *mode && AvpEncoderMismatches == 0)
                ok++;
        }
        KiTestAvModesTried = (ULONG)(modesEnd - modesBegin);
        KiTestAvModesOk = ok;
        if (previous)
            AvSetDisplayMode((PVOID)0xFD000000, 0, previous, 0x12, 640 * 4, fb);
    }

    KiTestMiscPhyStatus = PhyInitialize(TRUE, nullptr);
    KiTestMiscPhyLink = PhyGetLinkState(TRUE);
    KiTestMiscDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartMiscSelfTests() { KiCreateSystemThread(MiscControllerThread, nullptr, 0, 9); }
