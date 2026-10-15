// Boot self-tests for dismount, Fsc and PHY (milestone 5d-1).
#include <xk/io.h>
#include <xk/rtl.h>

XBAPI NTSTATUS NTAPI IoDismountVolumeByName(POBJECT_STRING DeviceName);
XBAPI ULONG NTAPI FscGetCacheSize();
XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG);
XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN, PVOID);
XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN);
extern volatile ULONG KiTestXcDone;

volatile ULONG KiTestMiscDismountOk, KiTestMiscDiscRemountOk, KiTestMiscFscOk, KiTestMiscPhyStatus,
    KiTestMiscPhyLink, KiTestMiscCmosOk, KiTestMiscDone;
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
    while (!KiTestXcDone) {
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    OBJECT_STRING e, z, cd;
    RtlInitAnsiString(&e, "\\Device\\Harddisk0\\Partition1");
    RtlInitAnsiString(&z, "\\Device\\Harddisk0\\Partition5");
    RtlInitAnsiString(&cd, "\\Device\\CdRom0");
    bool ok = OpenClose("\\Device\\Harddisk0\\Partition1\\TDATA") == 0;
    PDEVICE_OBJECT part = Device("\\Device\\Harddisk0\\Partition1");
    ok &= part && part->MountedOrSelfDevice != nullptr;
    ok &= IoDismountVolumeByName(&e) == 0 && part->MountedOrSelfDevice == nullptr;
    ok &= OpenClose("\\Device\\Harddisk0\\Partition1\\TDATA") == 0 && part->MountedOrSelfDevice != nullptr;
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

    // CMOS "clock set" signature (spec/05 §1): written, detected, a damaged byte detected, restored.
    HalpWriteCmosSignature();
    ok = HalpCmosSignatureValid();
    __asm__ volatile("cli; movb $0x90, %%al; outb %%al, $0x72; xorl %%eax, %%eax; outb %%al, $0x71; sti" ::: "eax");
    ok &= !HalpCmosSignatureValid();
    HalpWriteCmosSignature();
    ok &= HalpCmosSignatureValid();
    KiTestMiscCmosOk = ok;

    KiTestMiscPhyStatus = PhyInitialize(TRUE, nullptr);
    KiTestMiscPhyLink = PhyGetLinkState(TRUE);
    KiTestMiscDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartMiscSelfTests() { KiCreateSystemThread(MiscControllerThread, nullptr, 0, 9); }
