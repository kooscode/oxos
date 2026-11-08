// Fatal boot errors (spec/stock/05 §6): LED sequence, EEPROM error history, the error screen, then halt; and the bug
// check screen (spec/oxos_internal/29_error_screens.md).
#include <xk/core.h>
#include <xk/memory.h>
#include <xk/runtime.h>
#include <xk/serial.h>
#include <xk/hardware.h>
#include <xk/titles.h>
#include <xk/storage.h>
#include <xk/bootscreen.h>

volatile ULONG IdexFatalErrorCode;      // the last reported code (read by the boot tests)
volatile ULONG XkFatalErrorFrameBuffer; // the error screen's physical address (read by the boot tests)

// --- The error history in the EEPROM

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

// --- The error screens (spec/oxos_internal/29_error_screens.md)

// 640x480 X8R8G8B8: the OXOS version and encoder at the top, then (fatal errors) the code in large digits, a title,
// what to try and the details, or (bug checks) the bug check code and its parameters. Left edge and line spacing:
constexpr ULONG XKP_W = 640, XKP_H = 480, XKP_LEFT = 48, XKP_LINE = 20;
constexpr ULONG XKP_WHITE = 0xFFFFFF, XKP_TEXT = 0xC8C8C8, XKP_GREY = 0x808080;

// Seven-segment digits, our own layout: bit 0 top, 1 upper right, 2 lower right, 3 bottom, 4 lower left,
// 5 upper left, 6 middle.
static const UCHAR XkpSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void XkpFill(ULONG *fb, ULONG x, ULONG y, ULONG w, ULONG h) {
    for (ULONG j = y; j < y + h; j++) {
        for (ULONG i = x; i < x + w; i++)
            fb[j * XKP_W + i] = 0xFFFFFFFF;
    }
}

static void XkpDrawDigit(ULONG *fb, ULONG x, ULONG y, ULONG digit) {
    const ULONG w = 60, h = 110, t = 12, half = (h - t) / 2;
    UCHAR s = XkpSegments[digit % 10];
    if (s & 0x01)
        XkpFill(fb, x, y, w, t);
    if (s & 0x02)
        XkpFill(fb, x + w - t, y, t, half + t);
    if (s & 0x04)
        XkpFill(fb, x + w - t, y + half, t, half + t);
    if (s & 0x08)
        XkpFill(fb, x, y + h - t, w, t);
    if (s & 0x10)
        XkpFill(fb, x, y + half, t, half + t);
    if (s & 0x20)
        XkpFill(fb, x, y, t, half + t);
    if (s & 0x40)
        XkpFill(fb, x, y + half, w, t);
}

static void XkpCentred(ULONG *fb, ULONG y, const char *text, ULONG scale, ULONG color) {
    ULONG width = XkTextWidth(text, scale);
    XkDrawText(fb, XKP_W, width < XKP_W ? (XKP_W - width) / 2 : 0, y, text, scale, color);
}

// A black screen with "OXOS v0.3.16/CONEXANT" at the top.
static void XkpStartScreen(ULONG *fb) {
    RtlZeroMemory(fb, XKP_W * XKP_H * 4);
    char header[48] = "OXOS ";
    XkVersionLabel(header + 5, sizeof(header) - 5);
    XkpCentred(fb, 36, header, 2, XKP_GREY);
}

// The title, centred: large when it fits between the margins.
static void XkpTitle(ULONG *fb, ULONG y, const char *title) {
    bool large = XkTextWidth(title, 3) <= XKP_W - 2 * XKP_LEFT;
    XkpCentred(fb, large ? y : y + 4, title, large ? 3 : 2, XKP_WHITE);
}

// The disk's model from IDENTIFY ("Disk: WDC WD80EB-28CGH2"), when the disk answered.
static void XkpDiskLine(char *line, ULONG size) {
    ULONG n = HalDiskModelNumber.Length;
    while (n && HalDiskModelNumber.Buffer[n - 1] == ' ')
        n--;
    if (n)
        RtlSnprintf(line, size, "Disk: %.*s", (int)(n > 39 ? 39 : n), HalDiskModelNumber.Buffer);
    else
        line[0] = 0;
}

// The details under a fatal error's help: what the kernel knows about the cause.
static ULONG XkpFatalDetails(ULONG Code, char (*lines)[XK_FATAL_LINE + 1]) {
    ULONG n = 0;
    if (Code >= 6 && Code <= 9) {
        XkpDiskLine(lines[n], sizeof(lines[n]));
        if (lines[n][0])
            n++;
        if (Code == 9 && IdexDiskLengthInSectors) // it answered: too small
            RtlSnprintf(lines[n++], sizeof(lines[0]), "Size: %lu GB (8 GB needed)",
                        (ULONG)(IdexDiskLengthInSectors / 1953125));
        else
            RtlSnprintf(lines[n++], sizeof(lines[0]), "Disk status: 0x%02X", IdexDiskLastStatus);
    } else if (Code > 13 && Code < 20) {
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Dashboard launch reason: %lu", Code - 13);
    } else if (!(Code >= 20 && Code <= 21) && Code != 13) {
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Code: %lu (0x%lX)", Code, Code);
    }
    return n;
}

static void XkpDrawFatal(ULONG *fb, ULONG Code) {
    XkpStartScreen(fb);
    XkpDrawDigit(fb, 330, 72, Code % 10);
    XkpDrawDigit(fb, 250, 72, Code / 10 % 10);
    const XK_FATAL_HELP *help = XkFatalHelp(Code);
    XkpTitle(fb, 204, help->Title);
    ULONG y = 244;
    for (const char *const *line = help->Lines; *line; line++, y += XKP_LINE)
        XkDrawText(fb, XKP_W, XKP_LEFT, y, *line, 2, XKP_TEXT);
    char details[3][XK_FATAL_LINE + 1];
    ULONG count = XkpFatalDetails(Code, details);
    y += 8;
    for (ULONG i = 0; i < count; i++, y += XKP_LINE)
        XkDrawText(fb, XKP_W, XKP_LEFT, y, details[i], 2, XKP_GREY);
}

// The fatal error screen (only with an AV pack connected), in its own frame buffer.
static void XkpShowCode(ULONG Code) {
    if (HalBootSMCVideoMode == 7)
        return;
    const ULONG pitch = XKP_W * 4;
    auto fb = (ULONG *)MmAllocateContiguousMemoryEx(pitch * XKP_H, 0, 0x3FFFFFF, 0x1000,
                                                    PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!fb)
        return;
    XkpDrawFatal(fb, Code);
    ULONG phys = MmGetPhysicalAddress(fb);
    AvSetDisplayMode((PVOID)0xFD000000, 0, AvBootModeWord(), 0x1E, pitch, phys);
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 0, nullptr); // the screen on (spec/stock/22 §2.3)
    XkFatalErrorFrameBuffer = phys;
}

// The running title's ID from its XBE header at 0x10000 (certificate address at +0x118, title ID at +8); 0 if none.
static ULONG XkpTitleId() {
    auto header = (const ULONG *)0x10000;
    if (!MmIsAddressValid((PVOID)header) || header[0] != 0x48454258) // "XBEH"
        return 0;
    auto cert = (const ULONG *)header[0x118 / 4];
    if (!MmIsAddressValid((PVOID)cert) || !MmIsAddressValid((PVOID)(cert + 3)))
        return 0;
    return cert[2];
}

static void XkpDrawBugCheck(ULONG *fb, ULONG Code, const ULONG_PTR *P) {
    XkpStartScreen(fb);
    XkpTitle(fb, 96, "Kernel Stopped");
    char lines[8][XK_FATAL_LINE + 1];
    ULONG n = 0;
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Bug check 0x%08lX", Code);
    if (Code == 0x1E) // KMODE_EXCEPTION_NOT_HANDLED: the exception code and the instruction
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Exception 0x%08lX at 0x%08lX", P[0], P[1]);
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Parameters: 0x%08lX 0x%08lX", P[0], P[1]);
    RtlSnprintf(lines[n++], sizeof(lines[0]), "            0x%08lX 0x%08lX", P[2], P[3]);
    if (ULONG title = XkpTitleId())
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Title: %08lX", title);
    lines[n++][0] = 0;
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Take a photo of this screen for a bug report,");
    RtlSnprintf(lines[n++], sizeof(lines[0]), "then turn the console off and on again.");
    ULONG y = 160;
    for (ULONG i = 0; i < n; i++, y += XKP_LINE)
        XkDrawText(fb, XKP_W, XKP_LEFT, y, lines[i], 2, i < n - 2 ? XKP_TEXT : XKP_GREY);
}

// The bug check screen. Interrupts are off and nothing may be allocated, so it takes over the frame buffer on screen
// (a title's, or the splash's): the GPU stops drawing (FIFO, graphics engine, video overlay), the memory tiles are
// switched off so the CPU's writes appear as written, and the screen is drawn through the frame-buffer window at
// 0xF0000000 (always mapped, write-through) and shown at 640x480 in 32 bits. Titles' frame buffers hold at least
// 640x480 pixels; in 16 bits the screen runs past the first one into the title's next buffer, which no longer matters.
void XkShowBugCheck(ULONG Code, const ULONG_PTR *Parameters) {
    if (HalBootSMCVideoMode == 7)
        return;
    auto regs = (volatile UCHAR *)0xFD000000;
    auto reg32 = [&](ULONG offset) -> volatile ULONG & { return *(volatile ULONG *)(regs + offset); };
    ULONG phys = reg32(0x600800) & 0x07FFFFFF; // the CRTC's scan-out start
    if (!phys || phys + XKP_W * XKP_H * 4 > (MmNumberOfPhysicalPages << PAGE_SHIFT))
        return;
    reg32(0x2500) = 0;   // PFIFO: no channel switches
    reg32(0x3220) = 0;   // PFIFO: the push buffer fetch off
    reg32(0x3250) = 0;   // PFIFO: the puller off
    reg32(0x400720) = 0; // PGRAPH: the FIFO off
    reg32(0x8704) = 0x11; // PVIDEO: the overlay stopped
    for (ULONG i = 0; i < 8; i++)
        reg32(0x100240 + i * 0x10) = 0; // PFB: tile regions off
    XkpDrawBugCheck((ULONG *)(0xF0000000 | phys), Code, Parameters);
    AvSetDisplayMode((PVOID)regs, 0, AvBootModeWord(), 0x1E, XKP_W * 4, phys);
    AvSendTVEncoderOption((PVOID)regs, 9, 0, nullptr);
}

// --- The fatal error

[[noreturn]] void IdexFatalError(ULONG Code) {
    XK_SERIAL("*** fatal error %lu\n", Code);
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
