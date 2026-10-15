// Boot self-tests for SMBus/SMC/EEPROM services (milestone 4b-1).
#include <xk/hal.h>
#include <xk/ke.h>

XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG, PULONG, PVOID, ULONG, PULONG);
XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG, ULONG, PVOID, ULONG);
XBAPI NTSTATUS NTAPI HalReadSMCTrayState(PULONG, PULONG);
extern volatile ULONG ExpEepromSecurityValid;

volatile ULONG KiTestHalEepromOk, KiTestHalSettingsOk, KiTestHalSaveOk, KiTestHalTrayState, KiTestHalRegion,
    KiTestHalDone;

extern volatile ULONG KiTestMmDone, KiTestIoDone, KiTestObDone;

static void NTAPI HalControllerThread(PVOID) {
    while (!KiTestMmDone || !KiTestIoDone || !KiTestObDone) { // those suites measure pool/page balances
        LARGE_INTEGER t;
        t.QuadPart = -10 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    ULONG type = 0, len = 0, v = 0;
    UCHAR all[256];
    bool ok = ExQueryNonVolatileSetting(0xFFFF, &type, all, sizeof(all), &len) == 0 && type == 3 && len == 256;
    ok &= ExQueryNonVolatileSetting(0xFFFF, &type, all, 16, &len) == (NTSTATUS)0xC0000023;
    KiTestHalEepromOk = ok && ExpEepromSecurityValid;

    ok = ExQueryNonVolatileSetting(0x104, &type, &v, 4, &len) == 0 && type == 4;
    KiTestHalRegion = v;
    ok &= ExQueryNonVolatileSetting(0x103, &type, &v, 4, &len) == 0 && type == 4 && len == 4; // video standard
    ok &= ExQueryNonVolatileSetting(0x77, &type, &v, 4, &len) == (NTSTATUS)0xC0000034;
    KiTestHalSettingsOk = ok;

    // Save a user setting (index 3, a dword) and read it back; the section checksum must stay valid.
    ULONG old = 0, now = 0;
    ExQueryNonVolatileSetting(3, &type, &old, 4, &len);
    ULONG value = old ^ 0x5A5A;
    ok = ExSaveNonVolatileSetting(3, 4, &value, 4) == 0;
    ok &= ExQueryNonVolatileSetting(3, &type, &now, 4, &len) == 0 && now == value;
    ok &= ExQueryNonVolatileSetting(0, &type, &v, 4, &len) == 0; // checksum still valid for the section
    ok &= ExSaveNonVolatileSetting(3, 4, &old, 4) == 0;
    KiTestHalSaveOk = ok;

    ULONG tray = 0;
    HalReadSMCTrayState(&tray, nullptr);
    KiTestHalTrayState = tray;
    KiTestHalDone = 1;
}

PKTHREAD KiCreateSystemThread(void(NTAPI *StartRoutine)(PVOID), PVOID Context, ULONG StackSize, LONG Priority);
void KiStartHalSelfTests() { KiCreateSystemThread(HalControllerThread, nullptr, 0, 9); }
