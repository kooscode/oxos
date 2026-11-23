// The toasts' display (spec/oxos_internal/36 §3): each run-loop round, once the title has set its video mode, the next toast is drawn with the OXOS font,
// turned into YUY2 and put on the GPU's video overlay (PVIDEO), which the CRTC blends over the title's own picture; it comes off when its time is up. The
// overlay registers a title uses are put back afterwards, and a title that has the overlay on keeps it (§7).
#include <toasts/toasts.h>
#include <toasts/toast_layout.h>
#include <core/core.h>
#include <diagnostics/diagnostics.h>
#include <graphics/graphics.h>
#include <memory/memory.h>
#include <runtime/runtime.h>

namespace xk::toasts {

// --- The overlay registers (spec/oxos_internal/36 §3.1), at 0xFD000000

constexpr ULONG NV2A_BASE = 0xFD000000;
constexpr ULONG PVIDEO_BUFFER = 0x8700, PVIDEO_STOP = 0x8704;
constexpr ULONG PVIDEO_BASE = 0x8900, PVIDEO_LIMIT = 0x8908, PVIDEO_LUMINANCE = 0x8910, PVIDEO_CHROMINANCE = 0x8918, PVIDEO_OFFSET = 0x8920,
                PVIDEO_SIZE_IN = 0x8928, PVIDEO_POINT_IN = 0x8930, PVIDEO_DS_DX = 0x8938, PVIDEO_DT_DY = 0x8940, PVIDEO_POINT_OUT = 0x8948,
                PVIDEO_SIZE_OUT = 0x8950, PVIDEO_FORMAT = 0x8958;
constexpr ULONG PVIDEO_FORMAT_YUY2 = 1 << 16;           // with the row pitch in bytes in the low bits; bit 20 (colour key) clear
constexpr ULONG PVIDEO_BUFFER_IN_USE = 0x11;            // buffer 0 or 1 shown
constexpr ULONG PVIDEO_SCALE_SHIFT = 20;                // DS_DX / DT_DY: source pixels per screen pixel, 1 << 20 for 1:1
constexpr ULONG PVIDEO_COLOR_NEUTRAL = 0x00001000;      // LUMINANCE / CHROMINANCE: contrast 1.0, brightness 0
constexpr ULONG PVIDEO_LIMIT_ALL = 0x07FFFFFF;          // buffer 0 may be anywhere in the first 128 MB
// The flat-panel timing's vertical and horizontal display end, the mode's active height and width minus one (written at every mode set,
// hardware/video/video.cpp). Not the horizontal CRTC size (0x680828): the encoders' 640-pixel modes have 600 there (xemu, 2026-11-23).
constexpr ULONG DISPLAY_END_HEIGHT = 0x680800, DISPLAY_END_WIDTH = 0x680820;

// The registers a toast writes, in the order they are put back.
constexpr ULONG SAVED_REGISTERS[] = {PVIDEO_BASE,     PVIDEO_LIMIT,  PVIDEO_LUMINANCE, PVIDEO_CHROMINANCE, PVIDEO_OFFSET,    PVIDEO_SIZE_IN, PVIDEO_POINT_IN,
                                     PVIDEO_DS_DX,    PVIDEO_DT_DY,  PVIDEO_POINT_OUT, PVIDEO_SIZE_OUT,    PVIDEO_FORMAT,    PVIDEO_STOP,    PVIDEO_BUFFER};
constexpr ULONG SAVED_COUNT = sizeof(SAVED_REGISTERS) / sizeof(SAVED_REGISTERS[0]);

static volatile ULONG &Register(ULONG Offset) {
    return *(volatile ULONG *)(NV2A_BASE + Offset);
}

// --- The toast on screen

static QUEUED_TOAST Current;           // the toast taken from the queue: waiting for the overlay, or on screen
static bool Taken;                     // Current holds a toast
static bool Showing;                   // Current is on screen
static ULONG TakenAt, ShownAt;         // KeTickCount when it was taken and shown
static PVOID Picture;                  // its picture (contiguous memory), null when none
static ULONG Saved[SAVED_COUNT];       // the overlay's registers before it showed

// Draws Toast into a new picture: the box at 480-line size in X8R8G8B8, turned into YUY2 in place (the first half of the buffer). Null without memory.
static PVOID DrawToast(const QUEUED_TOAST &Toast, TOAST_BOX Box) {
    ULONG pixels = Box.Width * Box.Height;
    auto image = (ULONG *)MmAllocateContiguousMemoryEx(pixels * 4, 0, PVIDEO_LIMIT_ALL, PAGE_SIZE, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!image)
        return nullptr;
    for (ULONG i = 0; i < pixels; i++) {
        ULONG x = i % Box.Width, y = i / Box.Width;
        bool frame = x < TOAST_FRAME || x >= Box.Width - TOAST_FRAME || y < TOAST_FRAME || y >= Box.Height - TOAST_FRAME;
        image[i] = frame ? TOAST_COLOR_FRAME : TOAST_COLOR_BACKGROUND;
    }
    char line[OXOS_TOAST_LINE_MAX + 1];
    ULONG y = TOAST_PADDING_TOP;
    for (const char *p = Toast.Text; *p; y += TOAST_LINE_HEIGHT) {
        ULONG length = 0;
        while (p[length] && p[length] != '\n')
            length++;
        memcpy(line, p, length);
        line[length] = 0;
        graphics::DrawText(image, Box.Width, TOAST_PADDING_X, y, line, TOAST_SCALE, TOAST_COLOR_TEXT);
        p += length + (p[length] == '\n');
    }
    // In place: the dword for pixels i and i + 1 goes at byte 2 * i, never after the pixels still to read.
    for (ULONG i = 0; i < pixels; i += 2)
        ((ULONG *)image)[i / 2] = ConvertToYUY2(image[i], image[i + 1]);
    return image;
}

// Puts Current on the overlay. False without memory for its picture.
static bool ShowToast() {
    TOAST_BOX box = MeasureToast(Current.Text);
    Picture = DrawToast(Current, box);
    if (!Picture)
        return false;
    ULONG modeWidth = Register(DISPLAY_END_WIDTH) + 1, modeHeight = Register(DISPLAY_END_HEIGHT) + 1;
    if (modeWidth < 320 || modeWidth > 1920 || modeHeight < 240 || modeHeight > 1080) { // not set up yet: the boot's 640x480
        modeWidth = 640;
        modeHeight = 480;
    }
    TOAST_PLACEMENT place = PlaceToast(box, Current.Place, modeWidth, modeHeight);
    for (ULONG i = 0; i < SAVED_COUNT; i++)
        Saved[i] = Register(SAVED_REGISTERS[i]);
    Register(PVIDEO_STOP) = 1;
    Register(PVIDEO_BASE) = 0;
    Register(PVIDEO_LIMIT) = PVIDEO_LIMIT_ALL;
    Register(PVIDEO_LUMINANCE) = PVIDEO_COLOR_NEUTRAL;
    Register(PVIDEO_CHROMINANCE) = PVIDEO_COLOR_NEUTRAL;
    Register(PVIDEO_OFFSET) = MmGetPhysicalAddress(Picture);
    Register(PVIDEO_SIZE_IN) = box.Height << 16 | box.Width;
    Register(PVIDEO_POINT_IN) = 0;
    Register(PVIDEO_DS_DX) = (ULONG)(((ULONGLONG)box.Width << PVIDEO_SCALE_SHIFT) / place.Width);
    Register(PVIDEO_DT_DY) = (ULONG)(((ULONGLONG)box.Height << PVIDEO_SCALE_SHIFT) / place.Height);
    Register(PVIDEO_POINT_OUT) = place.Y << 16 | place.X;
    Register(PVIDEO_SIZE_OUT) = place.Height << 16 | place.Width;
    Register(PVIDEO_FORMAT) = box.Width * 2 | PVIDEO_FORMAT_YUY2;
    Register(PVIDEO_STOP) = 0;
    Register(PVIDEO_BUFFER) = 1;
    Showing = true;
    XK_SERIAL("toasts: %lu shown at %lu,%lu (%lux%lu in %lux%lu): %s\n", Current.ToastId, place.X, place.Y, place.Width, place.Height, modeWidth,
              modeHeight, Current.Text);
    return true;
}

void HideToast() {
    if (Showing) {
        Register(PVIDEO_STOP) = 1;
        Register(PVIDEO_BUFFER) = 0;
        for (ULONG i = 0; i < SAVED_COUNT; i++)
            Register(SAVED_REGISTERS[i]) = Saved[i];
        Showing = false;
    }
    Taken = false;
    if (Picture && KeGetCurrentIrql() == PASSIVE_LEVEL) { // at a raised IRQL (a title rebooting) the reboot takes it back
        MmFreeContiguousMemory(Picture);
        Picture = nullptr;
    }
}

void RunToasts() {
    ULONG now = KeTickCount;
    if (Showing) {
        if (now - ShownAt < Current.Seconds * 1000)
            return;
        XK_SERIAL("toasts: %lu hidden\n", Current.ToastId);
        HideToast();
        NoteToastEvent(Current, OXOS_TOAST_HIDDEN);
        return;
    }
    if (!Taken) {
        if (!TitleSettled() || !TakeNextToast(Current))
            return;
        Taken = true;
        TakenAt = now;
    }
    // A title that shows video through the overlay keeps it: the toast waits, up to its own time (§7).
    if ((Register(PVIDEO_BUFFER) & PVIDEO_BUFFER_IN_USE) && !(Register(PVIDEO_STOP) & 1)) {
        if (now - TakenAt < Current.Seconds * 1000)
            return;
        Taken = false;
        XK_SERIAL("toasts: %lu dropped: the title has the overlay\n", Current.ToastId);
        NoteToastEvent(Current, OXOS_TOAST_DROPPED);
        return;
    }
    if (!ShowToast()) {
        Taken = false;
        XK_SERIAL("toasts: %lu dropped: no memory for its picture\n", Current.ToastId);
        NoteToastEvent(Current, OXOS_TOAST_DROPPED);
        return;
    }
    ShownAt = now;
    NoteToastEvent(Current, OXOS_TOAST_SHOWN);
}

} // namespace xk::toasts
