// The error screens (spec/oxos_internal/29_error_screens.md): fatal boot errors (spec/stock/05 §6: LED sequence, EEPROM error history, the error screen, then
// halt), No Dashboard Found and the bug check screen, drawn with the kernel's graphics (graphics/screen.cpp).
#include <user_interface/user_interface.h>
#include <graphics/graphics.h>
#include <core/core.h>
#include <memory/memory.h>
#include <runtime/runtime.h>
#include <hardware/chipset/serial.h>
#include <hardware/chipset/chipset.h>
#include <hardware/video/video.h>
#include <titles/titles.h>
#include <storage/storage.h>
#include <boot_splash/boot_splash.h>
#include <gamepad/gamepad.h>

namespace xk::user_interface {

volatile ULONG FatalErrorCode;        // the last reported code (read by the boot tests)
volatile ULONG FatalErrorFrameBuffer; // the error screen's physical address (read by the boot tests)
// How the error stopped the boot sound: 1 at once (the idle thread), 2 with the splash (read by the boot tests).
volatile ULONG FatalSoundStopped;

// --- The error history in the EEPROM

// EEPROM layout used here (256-byte image, setting 0xFFFF): +0xFA last error code (manufacturing units only), +0xFC 16-bit history, bit (code - 5) per code
// above 4.
static void RecordError(ULONG Code) {
    UCHAR eeprom[256];
    ULONG type, length;
    if (!NT_SUCCESS(ExQueryNonVolatileSetting(0xFFFF, &type, eeprom, sizeof(eeprom), &length)))
        return;
    bool manufacturing = titles::ConsoleGameRegion & 0x80000000;
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

// --- The error screens (spec/oxos_internal/29_error_screens.md): the OXOS version and encoder at the top, then
// (fatal errors) the code in large digits, a title, what to try and the details, or (bug checks) the bug check code and its parameters.

// The details under a fatal error's help: what the kernel knows about the cause.
static ULONG FatalDetails(ULONG Code, char (*lines)[XK_FATAL_LINE + 1]) {
    ULONG n = 0;
    if (Code >= 6 && Code <= 9) {
        hardware::ide::DiskModelLine(lines[n], sizeof(lines[n]));
        if (lines[n][0])
            n++;
        if (Code == 9 && hardware::ide::DiskLengthInSectors) // it answered: too small
            RtlSnprintf(lines[n++], sizeof(lines[0]), "Size: %lu GB (8 GB needed)", (ULONG)(hardware::ide::DiskLengthInSectors / 1953125));
        else
            RtlSnprintf(lines[n++], sizeof(lines[0]), "Disk status: 0x%02X", hardware::ide::DiskLastStatus);
    } else if (Code > 13 && Code < 20) {
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Dashboard launch reason: %lu", Code - 13);
    } else if (!(Code >= 20 && Code <= 21) && Code != 13) {
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Code: %lu (0x%lX)", Code, Code);
    }
    return n;
}

static void DrawFatal(ULONG *fb, ULONG Code) {
    graphics::DrawLargeDigit(fb, 330, 72, Code % 10);
    graphics::DrawLargeDigit(fb, 250, 72, Code / 10 % 10);
    const XK_FATAL_HELP *help = FatalHelp(Code);
    graphics::DrawScreenTitle(fb, 204, help->Title);
    ULONG y = 244;
    for (const char *const *line = help->Lines; *line; line++, y += graphics::SCREEN_LINE)
        graphics::DrawText(fb, graphics::SCREEN_WIDTH, graphics::SCREEN_LEFT, y, *line, 2, graphics::COLOR_TEXT);
    char details[3][XK_FATAL_LINE + 1];
    ULONG count = FatalDetails(Code, details);
    y += 8;
    for (ULONG i = 0; i < count; i++, y += graphics::SCREEN_LINE)
        graphics::DrawText(fb, graphics::SCREEN_WIDTH, graphics::SCREEN_LEFT, y, details[i], 2, graphics::COLOR_GREY);
}

// The fatal error screen (only with an AV pack connected), in its own frame buffer.
static ULONG *ShowCode(ULONG Code) {
    ULONG *fb = graphics::OpenScreen();
    if (!fb)
        return nullptr;
    DrawFatal(fb, Code);
    graphics::ShowScreen(MmGetPhysicalAddress(fb));
    FatalErrorFrameBuffer = MmGetPhysicalAddress(fb);
    return fb;
}

// --- No Dashboard Found on a cold boot (spec/oxos_internal/32 §1): the fatal error's screen until X is pressed.
// Boot-only: the title launcher calls it before the first title starts.

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

void NoDashboardError(ULONG Code) {
    XK_SERIAL("*** fatal error %lu: X opens the File Manager\n", Code);
    FatalErrorCode = Code;
    HalWriteSMBusValue(0x20, 0x08, FALSE, 0xE1); // LED sequence, then LED mode "custom", as ShowFatalError
    HalWriteSMBusValue(0x20, 0x07, FALSE, 1);
    RecordError(Code);
    // The jingle has ended; the boot-only pages stay (the USB and the File Manager run from them).
    boot_splash::StopSplash();
    FatalSoundStopped = 2;
    ULONG *fb = ShowCode(Code);
    gamepad::WaitForXButton();
    FatalErrorCode = 0;
    graphics::CloseScreen(&fb);
    hardware::chipset::SetBootLed(0x0F);
}

#pragma code_seg()
#pragma const_seg()

// The running title's ID from its XBE header at 0x10000 (certificate address at +0x118, title ID at +8); 0 if none.
static ULONG TitleIdText() {
    auto header = (const ULONG *)0x10000;
    if (!MmIsAddressValid((PVOID)header) || header[0] != 0x48454258) // "XBEH"
        return 0;
    auto cert = (const ULONG *)header[0x118 / 4];
    if (!MmIsAddressValid((PVOID)cert) || !MmIsAddressValid((PVOID)(cert + 3)))
        return 0;
    return cert[2];
}

static void DrawBugCheck(ULONG *fb, ULONG Code, const ULONG_PTR *P) {
    graphics::StartScreen(fb);
    graphics::DrawScreenTitle(fb, 96, "Kernel Stopped");
    char lines[8][XK_FATAL_LINE + 1];
    ULONG n = 0;
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Bug check 0x%08lX", Code);
    if (Code == 0x1E) // KMODE_EXCEPTION_NOT_HANDLED: the exception code and the instruction
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Exception 0x%08lX at 0x%08lX", P[0], P[1]);
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Parameters: 0x%08lX 0x%08lX", P[0], P[1]);
    RtlSnprintf(lines[n++], sizeof(lines[0]), "            0x%08lX 0x%08lX", P[2], P[3]);
    if (ULONG title = TitleIdText())
        RtlSnprintf(lines[n++], sizeof(lines[0]), "Title: %08lX", title);
    lines[n++][0] = 0;
    RtlSnprintf(lines[n++], sizeof(lines[0]), "Take a photo of this screen for a bug report,");
    RtlSnprintf(lines[n++], sizeof(lines[0]), "then turn the console off and on again.");
    ULONG y = 160;
    for (ULONG i = 0; i < n; i++, y += graphics::SCREEN_LINE)
        graphics::DrawText(fb, graphics::SCREEN_WIDTH, graphics::SCREEN_LEFT, y, lines[i], 2, i < n - 2 ? graphics::COLOR_TEXT : graphics::COLOR_GREY);
}

// The bug check screen. Interrupts are off and nothing may be allocated, so it takes over the frame buffer on screen (a title's, or the splash's): the GPU
// stops drawing (FIFO, graphics engine, video overlay), the memory tiles are switched off so the CPU's writes appear as written, and the screen is drawn
// through the frame-buffer window at 0xF0000000 (always mapped, write-through) and shown at 640x480 in 32 bits. Titles' frame buffers hold at least 640x480
// pixels; in 16 bits the screen runs past the first one into the title's next buffer, which no longer matters.
void ShowBugCheck(ULONG Code, const ULONG_PTR *Parameters) {
    if (HalBootSMCVideoMode == 7)
        return;
    auto regs = (volatile UCHAR *)0xFD000000;
    auto reg32 = [&](ULONG offset) -> volatile ULONG & { return *(volatile ULONG *)(regs + offset); };
    ULONG phys = reg32(0x600800) & 0x07FFFFFF; // the CRTC's scan-out start
    ULONG size = graphics::SCREEN_WIDTH * graphics::SCREEN_HEIGHT * 4;
    if (!phys || phys + size > (memory::NumberOfPhysicalPages << PAGE_SHIFT))
        return;
    reg32(0x2500) = 0;   // PFIFO: no channel switches
    reg32(0x3220) = 0;   // PFIFO: the push buffer fetch off
    reg32(0x3250) = 0;   // PFIFO: the puller off
    reg32(0x400720) = 0; // PGRAPH: the FIFO off
    reg32(0x8704) = 0x11; // PVIDEO: the overlay stopped
    for (ULONG i = 0; i < 8; i++)
        reg32(0x100240 + i * 0x10) = 0; // PFB: tile regions off
    DrawBugCheck((ULONG *)(0xF0000000 | phys), Code, Parameters);
    graphics::ShowScreen(phys);
}

// --- The fatal error

[[noreturn]] void ShowFatalError(ULONG Code) {
    XK_SERIAL("*** fatal error %lu\n", Code);
    FatalErrorCode = Code;
    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
        KfLowerIrql(PASSIVE_LEVEL);
    HalWriteSMBusValue(0x20, 0x08, FALSE, 0xE1); // LED sequence, then LED mode "custom"
    HalWriteSMBusValue(0x20, 0x07, FALSE, 1);
    RtlZeroMemory(core::CertKey, sizeof(core::CertKey));
    RtlZeroMemory(KiPersistedBootKeys + 16, 16);
    RecordError(Code);
    // The sound must stop: left running, the sound controller replays the jingle's last buffers in a loop. On the boot thread, which is the idle thread during
    // start-up (the disk's errors, a title launch from it), nothing else runs once it halts and it must never wait: the controller is stopped at once. Any
    // other thread lets the splash and the jingle finish, as before a title starts (boot_splash::EndSplash), and then blocks rather than spins.
    bool idle = KeGetCurrentThread() == KiPcr.Prcb->IdleThread;
    if (idle)
        boot_splash::StopBootSound();
    else
        boot_splash::EndSplash();
    FatalSoundStopped = idle ? 1 : 2;
    ShowCode(Code);
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

} // namespace xk::user_interface
