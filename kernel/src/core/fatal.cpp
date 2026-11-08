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
#include <xk/gamepad.h>

volatile ULONG IdexFatalErrorCode;      // the last reported code (read by the boot tests)
volatile ULONG XkFatalErrorFrameBuffer; // the error screen's physical address (read by the boot tests)
// How the error stopped the boot sound: 1 at once (the idle thread), 2 with the splash (read by the boot tests).
volatile ULONG XkFatalSoundStopped;

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
constexpr ULONG XKP_DIGIT = 0xFFFFFFFF; // the large digits' white (the boot tests look for this value)

// Seven-segment digits, our own layout: bit 0 top, 1 upper right, 2 lower right, 3 bottom, 4 lower left,
// 5 upper left, 6 middle.
static const UCHAR XkpSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void XkpFill(ULONG *fb, ULONG x, ULONG y, ULONG w, ULONG h, ULONG color) {
    for (ULONG j = y; j < y + h; j++) {
        for (ULONG i = x; i < x + w; i++)
            fb[j * XKP_W + i] = color;
    }
}

static void XkpDrawDigit(ULONG *fb, ULONG x, ULONG y, ULONG digit) {
    const ULONG w = 60, h = 110, t = 12, half = (h - t) / 2;
    UCHAR s = XkpSegments[digit % 10];
    if (s & 0x01)
        XkpFill(fb, x, y, w, t, XKP_DIGIT);
    if (s & 0x02)
        XkpFill(fb, x + w - t, y, t, half + t, XKP_DIGIT);
    if (s & 0x04)
        XkpFill(fb, x + w - t, y + half, t, half + t, XKP_DIGIT);
    if (s & 0x08)
        XkpFill(fb, x, y + h - t, w, t, XKP_DIGIT);
    if (s & 0x10)
        XkpFill(fb, x, y + half, t, half + t, XKP_DIGIT);
    if (s & 0x20)
        XkpFill(fb, x, y, t, half + t, XKP_DIGIT);
    if (s & 0x40)
        XkpFill(fb, x, y + half, w, t, XKP_DIGIT);
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

// --- Opening and closing a screen: the fatal error's, and those that stay up while the kernel works (the format
// countdown, the File Manager: drawn once, then only the parts that change; taken down before the boot goes on).

static void XkpClear(ULONG *fb, ULONG x, ULONG y, ULONG w, ULONG h) {
    for (ULONG j = y; j < y + h; j++)
        RtlZeroMemory(fb + j * XKP_W + x, w * 4);
}

// A new black screen with the header, or null without an AV pack or memory; XkpShowScreen puts it on the TV.
static ULONG *XkpOpenScreen() {
    if (HalBootSMCVideoMode == 7)
        return nullptr;
    auto fb = (ULONG *)MmAllocateContiguousMemoryEx(XKP_W * XKP_H * 4, 0, 0x3FFFFFF, 0x1000,
                                                    PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (fb)
        XkpStartScreen(fb);
    return fb;
}

static void XkpShowScreen(ULONG *fb) {
    AvSetDisplayMode((PVOID)0xFD000000, 0, AvBootModeWord(), 0x1E, XKP_W * 4, MmGetPhysicalAddress(fb));
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 0, nullptr);
}

// Screen off (the boot goes on), memory back.
static void XkpCloseScreen(ULONG **fb) {
    if (!*fb)
        return;
    AvSendTVEncoderOption((PVOID)0xFD000000, 9, 1, nullptr);
    MmFreeContiguousMemory(*fb);
    *fb = nullptr;
}

// The fatal error screen (only with an AV pack connected), in its own frame buffer.
static ULONG *XkpShowCode(ULONG Code) {
    ULONG *fb = XkpOpenScreen();
    if (!fb)
        return nullptr;
    XkpDrawFatal(fb, Code);
    XkpShowScreen(fb);
    XkFatalErrorFrameBuffer = MmGetPhysicalAddress(fb);
    return fb;
}

// --- The countdown before the kernel formats a blank or foreign disk (spec/oxos_internal/30 §4)

static ULONG *XkpCountdownScreen; // the countdown's frame buffer while it is shown

// The line with the seconds (line 5 of the text) and the seconds in large digits.
constexpr ULONG XKP_COUNTDOWN_LINE = 244 + 4 * XKP_LINE;

static void XkpDrawSeconds(ULONG *fb, ULONG Seconds) {
    XkpClear(fb, 250, 72, 140, 110);
    XkpDrawDigit(fb, 330, 72, Seconds % 10);
    XkpDrawDigit(fb, 250, 72, Seconds / 10 % 10);
    XkpClear(fb, XKP_LEFT, XKP_COUNTDOWN_LINE, XKP_W - 2 * XKP_LEFT, 16);
    char line[XK_FATAL_LINE + 1];
    RtlSnprintf(line, sizeof(line), "In %lu s OXOS erases it and formats it as", Seconds);
    XkDrawText(fb, XKP_W, XKP_LEFT, XKP_COUNTDOWN_LINE, line, 2, XKP_WHITE);
}

void XkShowFormatCountdown(const char *Title, const char *What, ULONG Seconds) {
    if (!Title)
        return XkpCloseScreen(&XkpCountdownScreen);
    if (XkpCountdownScreen)
        return XkpDrawSeconds(XkpCountdownScreen, Seconds);
    ULONG *fb = XkpOpenScreen();
    if (!fb)
        return;
    XkpTitle(fb, 204, Title);
    char lines[8][XK_FATAL_LINE + 1] = {};
    XkpDiskLine(lines[0], sizeof(lines[0]));
    RtlSnprintf(lines[1], sizeof(lines[1]), "Size: %lu GB", (ULONG)(IdexDiskLengthInSectors / 1953125));
    RtlSnprintf(lines[2], sizeof(lines[2]), "Holds: %s", What);
    RtlSnprintf(lines[5], sizeof(lines[5]), "exFAT, with all required folders.");
    RtlSnprintf(lines[7], sizeof(lines[7]), "  !! TURN OFF POWER TO SKIP FORMATTING !!");
    for (ULONG i = 0; i < 8; i++)
        XkDrawText(fb, XKP_W, XKP_LEFT, 244 + i * XKP_LINE, lines[i], 2, i < 3 ? XKP_TEXT : XKP_WHITE);
    XkpDrawSeconds(fb, Seconds);
    XkpShowScreen(fb);
    XkpCountdownScreen = fb;
}

// --- The OXOS File Manager's screen (spec/oxos_internal/32 §5): a folder, the title and the server's address.
// Boot-only: the server (uploader/server.cpp) draws it before the first title starts.

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

static ULONG *XkpFileManagerScreen; // its frame buffer while it is shown

constexpr ULONG XKP_GREEN = 0x88FF66, XKP_DARK_GREEN = 0x55BB33;
// The lines that change: the address (large) and the status.
constexpr ULONG XKP_ADDRESS_LINE = 272, XKP_STATUS_LINE = 380;

// A folder in the place of an error's digits, with an arrow up on it: uploads.
static void XkpDrawFolder(ULONG *fb) {
    XkpFill(fb, 250, 76, 60, 16, XKP_DARK_GREEN);  // the tab
    XkpFill(fb, 250, 90, 140, 92, XKP_DARK_GREEN); // the back
    XkpFill(fb, 250, 104, 140, 78, XKP_GREEN);     // the front
    XkpFill(fb, 314, 128, 12, 40, 0);              // the arrow's shaft
    for (ULONG row = 0; row < 18; row++)           // its head
        XkpFill(fb, 320 - row - 1, 112 + row, 2 * row + 2, 1, 0);
}

static void XkpDrawFileManagerState(ULONG *fb, const char *Address, const char *Status) {
    char line[XK_FATAL_LINE + 1];
    XkpClear(fb, 0, XKP_ADDRESS_LINE, XKP_W, 24);
    if (Address && Address[0])
        RtlSnprintf(line, sizeof(line), "http://%s/", Address);
    else
        RtlSnprintf(line, sizeof(line), "(no network address yet)");
    XkpCentred(fb, XKP_ADDRESS_LINE, line, 3, XKP_GREEN);
    XkpClear(fb, 0, XKP_STATUS_LINE, XKP_W, 16);
    XkpCentred(fb, XKP_STATUS_LINE, Status ? Status : "", 2, XKP_GREY);
}

void XkShowFileManagerScreen(const char *Address, const char *Status) {
    if (XkpFileManagerScreen)
        return XkpDrawFileManagerState(XkpFileManagerScreen, Address, Status);
    ULONG *fb = XkpOpenScreen();
    if (!fb)
        return;
    XkpDrawFolder(fb);
    XkpCentred(fb, 204, "OXOS File Manager", 3, XKP_GREEN);
    XkpCentred(fb, 244, "Open this address in a browser:", 2, XKP_TEXT);
    XkpCentred(fb, 316, "[ Upload files, manage folders, launch XBEs ]", 2, XKP_TEXT);
    XkpCentred(fb, 430, "Launch an XBE, or turn the console off.", 2, XKP_GREY);
    XkpDrawFileManagerState(fb, Address, Status);
    XkpShowScreen(fb);
    XkpFileManagerScreen = fb;
}

void XkCloseFileManagerScreen() {
    XkpCloseScreen(&XkpFileManagerScreen);
}

// --- No Dashboard Found on a cold boot (spec/oxos_internal/32 §1): the fatal error's screen until X is pressed

void XkNoDashboardError(ULONG Code) {
    XK_SERIAL("*** fatal error %lu: X opens the File Manager\n", Code);
    IdexFatalErrorCode = Code;
    HalWriteSMBusValue(0x20, 0x08, FALSE, 0xE1); // LED sequence, then LED mode "custom", as IdexFatalError
    HalWriteSMBusValue(0x20, 0x07, FALSE, 1);
    XkpRecordError(Code);
    HalpStopSplash(); // the jingle has ended; the boot-only pages stay (the USB and the File Manager run from them)
    XkFatalSoundStopped = 2;
    ULONG *fb = XkpShowCode(Code);
    GamepadWaitForX();
    IdexFatalErrorCode = 0;
    XkpCloseScreen(&fb);
    HalpSetBootLed(0x0F);
}

#pragma code_seg()
#pragma const_seg()

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
    // The sound must stop: left running, the sound controller replays the jingle's last buffers in a loop. On the boot
    // thread, which is the idle thread during start-up (the disk's errors, a title launch from it), nothing else runs
    // once it halts and it must never wait: the controller is stopped at once. Any other thread lets the splash and the
    // jingle finish, as before a title starts (HalpEndSplash), and then blocks rather than spins.
    bool idle = KeGetCurrentThread() == KiPcr.Prcb->IdleThread;
    if (idle)
        HalpStopBootSound();
    else
        HalpEndSplash();
    XkFatalSoundStopped = idle ? 1 : 2;
    XkpShowCode(Code);
    for (;;) {
        if (idle) { // halt, but keep the clock and DPCs running
            __asm__ volatile("sti; hlt");
        } else {
            LARGE_INTEGER forever;
            forever.QuadPart = -36000000000LL; // an hour at a time
            KeDelayExecutionThread(KernelMode, FALSE, &forever);
        }
    }
}
