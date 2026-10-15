// Fatal boot errors (spec/05 §6): LED sequence, EEPROM error history, the code drawn on screen, then halt.
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>

XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG, PULONG, PVOID, ULONG, PULONG);
XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG, ULONG, PVOID, ULONG);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
XBAPI ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                                   ULONG FrameBuffer);
extern UCHAR KiCertKey[16];
extern "C" UCHAR KiPersistedBootKeys[32];
extern ULONG XeConsoleGameRegion;
XBAPI UCHAR HalBootSMCVideoMode;

volatile ULONG IdexFatalErrorCode; // the last reported code (read by the boot tests)
volatile ULONG XkFatalErrorFrameBuffer;

// EEPROM layout used here (256-byte image, setting 0xFFFF): +0xFA last error code (manufacturing units only),
// +0xFC 16-bit history, bit (code - 5) per code above 4.
static void XkpRecordError(ULONG Code) {
    UCHAR eeprom[256];
    ULONG type, length;
    if (!NT_SUCCESS(ExQueryNonVolatileSetting(0xFFFF, &type, eeprom, sizeof(eeprom), &length)))
        return;
    bool manufacturing = XeConsoleGameRegion & 0x80000000;
    if (!eeprom[0xFA]) {
        if (manufacturing)
            eeprom[0xFA] = (UCHAR)Code;
        if (Code > 4 && Code - 5 < 16)
            *(USHORT *)(eeprom + 0xFC) |= (USHORT)(1 << (Code - 5));
    } else {
        eeprom[0xFA] = 0;
        manufacturing = false;
    }
    ExSaveNonVolatileSetting(0xFFFF, type, eeprom, length);
    if (manufacturing)
        HalWriteSMBusValue(0x20, 0x02, FALSE, 0x40); // power cycle
}

// Seven-segment digits, our own layout: bit 0 top, 1 upper right, 2 lower right, 3 bottom, 4 lower left,
// 5 upper left, 6 middle.
static const UCHAR XkpSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void XkpFill(ULONG *fb, ULONG x, ULONG y, ULONG w, ULONG h) {
    for (ULONG j = y; j < y + h; j++)
        for (ULONG i = x; i < x + w; i++)
            fb[j * 640 + i] = 0xFFFFFFFF;
}

static void XkpDrawDigit(ULONG *fb, ULONG x, ULONG y, ULONG digit) {
    const ULONG w = 60, h = 110, t = 12, half = (h - t) / 2;
    UCHAR s = XkpSegments[digit % 10];
    if (s & 0x01) XkpFill(fb, x, y, w, t);
    if (s & 0x02) XkpFill(fb, x + w - t, y, t, half + t);
    if (s & 0x04) XkpFill(fb, x + w - t, y + half, t, half + t);
    if (s & 0x08) XkpFill(fb, x, y + h - t, w, t);
    if (s & 0x10) XkpFill(fb, x, y + half, t, half + t);
    if (s & 0x20) XkpFill(fb, x, y, t, half + t);
    if (s & 0x40) XkpFill(fb, x, y + half, w, t);
}

// Two digits, white on black, 640x480 X8R8G8B8 (only with an AV pack connected).
static void XkpShowCode(ULONG Code) {
    if (HalBootSMCVideoMode == 7)
        return;
    const ULONG pitch = 640 * 4;
    auto fb = (ULONG *)MmAllocateContiguousMemoryEx(pitch * 480, 0, 0x3FFFFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!fb)
        return;
    RtlZeroMemory(fb, pitch * 480);
    XkpDrawDigit(fb, 330, 185, Code % 10);
    XkpDrawDigit(fb, 250, 185, Code / 10 % 10);
    ULONG phys = MmGetPhysicalAddress(fb);
    AvSetDisplayMode((PVOID)0xFD000000, 0, 0, 0x1E, pitch, phys);
    XkFatalErrorFrameBuffer = phys;
}

[[noreturn]] void IdexFatalError(ULONG Code) {
    IdexFatalErrorCode = Code;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
        KfLowerIrql(PASSIVE_LEVEL);
    HalWriteSMBusValue(0x20, 0x08, FALSE, 0xE1); // LED sequence, then LED mode "custom"
    HalWriteSMBusValue(0x20, 0x07, FALSE, 1);
    RtlZeroMemory(KiCertKey, sizeof(KiCertKey));
    RtlZeroMemory(KiPersistedBootKeys + 16, 16);
    XkpRecordError(Code);
    XkpShowCode(Code);
    for (;;) // possibly on the boot thread: halt, but keep the clock and DPCs running
        __asm__ volatile("sti; hlt");
}
