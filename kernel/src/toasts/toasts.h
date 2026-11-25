// Toasts (spec/oxos_internal/36): short messages over a running title, on the GPU's video overlay. The queue, its exports and the toasts' events
// (toast_queue.cpp), and the display (toast_overlay.cpp), which the extensions' run loop drives. Resident: they work while titles run.
#pragma once
#include <runtime/types.h>
#define OXOS_SDK_IN_KERNEL // oxos_sdk.h without oxos_kernel.h: the kernel has its own types
#include <oxos_sdk.h>
#include <oxos_extension_context.h>

namespace xk::toasts {

constexpr ULONG TOAST_QUEUE_LENGTH = 8;                          // toasts waiting at once
constexpr ULONG TOAST_TEXT_MAX = 2 * OXOS_TOAST_LINE_MAX + 2;    // a toast's text with its '\n' and its 0
constexpr ULONG TOAST_SECONDS_DEFAULT = 5, TOAST_SECONDS_MAX = 30;
constexpr ULONG TOAST_MODE_SETTLE_MS = 500;                      // after the title's mode set (AvSetDisplayMode), until toasts show: titles may set it twice
constexpr ULONG TOAST_TITLE_SETTLE_MS = 2000;                    // a title that sets no mode: toasts show this long after it started
constexpr ULONG TOAST_NO_OWNER = 0xFFFFFFFF;                     // a toast no extension queued
constexpr ULONG VIDEO_MODE_TOAST_SECONDS = 2;                    // the video mode's toast
constexpr ULONG VIDEO_MODE_TOAST_DELAY_MS = 2000;                // after the title's last mode set, until its mode is named: a title starting up after its
                                                                 // mode set had not drawn yet, and xemu showed no overlay over it (2026-11-23)

// A queued toast: the caller's, copied.
struct QUEUED_TOAST {
    ULONG ToastId;
    ULONG Owner; // the extension that queued it (its index in extensions::LoadedExtensions), or TOAST_NO_OWNER
    OXOS_TOAST_PLACE Place;
    ULONG Seconds;
    char Text[TOAST_TEXT_MAX];
};

// --- The queue (toast_queue.cpp)

extern XK_PERSIST bool ToastsOff;         // [toasts] enabled = no in oxos_settings.ini, read at the cold boot (spec/oxos_internal/36 §2)
extern XK_PERSIST bool VideoModeToastOff; // [toasts] show_video_mode = no

// Exported as oxos_toast_enqueue / oxos_toast_enqueue_wait (spec/oxos_internal/36 §5): the toast's owner is the extension the call comes from.
NTSTATUS EnqueueToast(OXOS_TOAST &Toast);
NTSTATUS EnqueueToastWait(OXOS_TOAST &Toast, ULONG TimeoutMs);
// The kernel's own toast (spec/oxos_internal/36 §2), at the bottom centre: queued like an extension's, with no owner to tell.
NTSTATUS QueueKernelToast(const char *Text, ULONG Seconds);

void OpenToasts();            // the extensions of a launch are loaded: an empty queue, open for their toasts
void NoteTitleStarted();      // the title has been loaded
void NoteDisplayModeSet(ULONG Mode); // AvSetDisplayMode (hardware/video/video.cpp): a mode set, the title's or the kernel's own screen's
void QueueVideoModeToast();   // the run loop, once the title has settled: its first mode named in a toast, and each later one with another name
bool TitleSettled();          // toasts may show: TOAST_MODE_SETTLE_MS after the title's mode set, or TOAST_TITLE_SETTLE_MS after its start without one
void CloseToasts();           // the title ends: the overlay hidden, the queue emptied and closed, waits end with STATUS_CANCELLED
bool TakeNextToast(QUEUED_TOAST &Toast); // the display takes the next toast; false when none waits
void ForgetToastOwner(ULONG Owner);      // Owner's Start failed: its queued toasts get no events (its index goes to the next extension)
void NoteToastEvent(const QUEUED_TOAST &Toast, OXOS_TOAST_EVENT Event); // for the run loop to deliver to the toast's owner
bool TakeToastEvent(ULONG &Owner, OXOS_TOAST_NOTICE &Notice);          // the next event to deliver; false when none

// --- The display (toast_overlay.cpp)

void RunToasts();  // each run-loop round: shows the next toast once the title has settled, hides it when its time is up
void HideToast();  // at once (the title ends); the picture is freed at passive level only
void ReserveToastPicture(); // each launch, before the title loads: the pages a one-line toast's picture takes (spec/oxos_internal/36 §6); a quick reboot
                            // frees them
// Text on the overlay at once, between titles, bypassing the queue and the title's 5 seconds: the kernel's notice before it powers the console off and on
// (the sound recovery, spec/oxos_internal/46 §3). It stays until the power goes. False when it could not be drawn.
bool ShowNotice(const char *Text);

} // namespace xk::toasts
