// The toasts' queue (spec/oxos_internal/36 §5): oxos_toast_enqueue and oxos_toast_enqueue_wait copy a toast into it, the display (toast_overlay.cpp) takes
// them in order, and the toasts' events wait here for the run loop, which gives each to the extension that queued the toast. Interrupts are off while the
// queue changes, so it may be used at any IRQL.
#include <toasts/toasts.h>
#include <arch/arch.h>
#include <core/core.h>
#include <extensions/extensions.h>

namespace xk::toasts {

// --- The queue and its state

constexpr NTSTATUS STATUS_DEVICE_BUSY_ = (NTSTATUS)0x80000011, STATUS_CANCELLED_ = (NTSTATUS)0xC0000120, STATUS_NOT_SUPPORTED_ = (NTSTATUS)0xC00000BB,
                   STATUS_INVALID_PARAMETER_ = (NTSTATUS)0xC000000D;
constexpr ULONG TOAST_EVENTS_MAX = 16; // events waiting for the run loop; more are lost

// One event for the run loop: Notice for extension Owner.
struct TOAST_EVENT_ENTRY {
    ULONG Owner;
    OXOS_TOAST_NOTICE Notice;
};

XK_PERSIST bool ToastsOff;                         // toasts.h
static QUEUED_TOAST Queue[TOAST_QUEUE_LENGTH]; // the waiting toasts, oldest at QueueFirst
static ULONG QueueFirst, QueueCount;
static TOAST_EVENT_ENTRY Events[TOAST_EVENTS_MAX]; // the events waiting for the run loop, oldest at EventFirst
static ULONG EventFirst, EventCount;
static ULONG NextToastId = 1;      // never 0, the id of a toast that was not queued
static volatile bool QueueOpen;    // a launch's extensions are loaded and its title has not ended
static volatile bool TitleStarted; // the title has been loaded
static volatile ULONG TitleStart;  // KeTickCount when it was
static volatile bool ModeSet;      // the title has set its video mode since
static volatile ULONG ModeSetAt;   // KeTickCount of its last mode set
static KEVENT QueueRoom;           // set when a place in the queue frees, and when the queue closes (the waits then end)

// --- Queueing

// Copies Text into Out: true when it is one or two lines of printable ASCII, each 1 to OXOS_TOAST_LINE_MAX characters.
static bool CopyToastText(const char *Text, char (&Out)[TOAST_TEXT_MAX]) {
    ULONG used = 0, line = 0, lines = 1;
    for (const char *p = Text; *p; p++) {
        if (*p == '\n') {
            if (!line || ++lines > 2)
                return false;
            line = 0;
        } else if (*p < 0x20 || *p > 0x7E || ++line > OXOS_TOAST_LINE_MAX) {
            return false;
        }
        Out[used++] = *p;
    }
    Out[used] = 0;
    return line > 0;
}

// Queues Toast for Owner, or says why not; never waits.
static NTSTATUS QueueToast(OXOS_TOAST &Toast, ULONG Owner) {
    Toast.ToastId = 0;
    if (ToastsOff)
        return STATUS_NOT_SUPPORTED_;
    QUEUED_TOAST entry;
    if (!Toast.Text || !CopyToastText(Toast.Text, entry.Text) || (ULONG)Toast.Place > OXOS_TOAST_BOTTOM_RIGHT || Toast.Seconds > TOAST_SECONDS_MAX)
        return STATUS_INVALID_PARAMETER_;
    entry.Owner = Owner;
    entry.Place = Toast.Place;
    entry.Seconds = Toast.Seconds ? Toast.Seconds : TOAST_SECONDS_DEFAULT;
    ULONG flags = save_flags_cli();
    NTSTATUS status = !QueueOpen ? STATUS_CANCELLED_ : QueueCount == TOAST_QUEUE_LENGTH ? STATUS_DEVICE_BUSY_ : STATUS_SUCCESS;
    if (status == STATUS_SUCCESS) {
        entry.ToastId = NextToastId++;
        if (!NextToastId)
            NextToastId = 1;
        Queue[(QueueFirst + QueueCount++) % TOAST_QUEUE_LENGTH] = entry;
        Toast.ToastId = entry.ToastId;
    }
    restore_flags(flags);
    return status;
}

NTSTATUS EnqueueToast(OXOS_TOAST &Toast) {
    return QueueToast(Toast, extensions::FindExtensionAt(__builtin_return_address(0)));
}

NTSTATUS EnqueueToastWait(OXOS_TOAST &Toast, ULONG TimeoutMs) {
    ULONG owner = extensions::FindExtensionAt(__builtin_return_address(0));
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        Toast.ToastId = 0;
        return STATUS_INVALID_PARAMETER_;
    }
    ULONG start = KeTickCount;
    for (;;) {
        NTSTATUS status = QueueToast(Toast, owner);
        if (status != STATUS_DEVICE_BUSY_)
            return status;
        ULONG waited = KeTickCount - start;
        if (waited >= TimeoutMs)
            return STATUS_TIMEOUT;
        LARGE_INTEGER timeout;
        timeout.QuadPart = -(LONGLONG)(TimeoutMs - waited) * 10000;
        KeWaitForSingleObject(&QueueRoom, Executive, KernelMode, FALSE, &timeout);
        if (!QueueOpen) {
            KeSetEvent(&QueueRoom, 0, FALSE); // the next waiter ends too
            return STATUS_CANCELLED_;
        }
    }
}

// --- A title's toasts

void OpenToasts() {
    ULONG flags = save_flags_cli();
    QueueFirst = QueueCount = EventFirst = EventCount = 0;
    TitleStarted = false;
    ModeSet = false;
    QueueOpen = true;
    restore_flags(flags);
    KeInitializeEvent(&QueueRoom, SynchronizationEvent, FALSE);
}

// A time is kept with a flag of its own, never as KeTickCount | 1 to keep it from 0: that can be a tick ahead of KeTickCount, and the difference read in the
// same tick then wraps to a huge "time since" (in-game shortcuts acted at once on a console, 2026-11-23).
void NoteTitleStarted() {
    TitleStart = KeTickCount;
    ModeSet = false;
    TitleStarted = true;
}

void NoteDisplayModeSet() {
    if (!TitleStarted) // the kernel's own screens, and the title before it was loaded
        return;
    ModeSetAt = KeTickCount;
    ModeSet = true;
}

// When the title's video is ready for the overlay (spec/oxos_internal/36 §3.4): every title sets its mode through AvSetDisplayMode (nxdk's XVideoSetMode
// too), so toasts wait for that, and half a second more; a title that keeps the mode it was started in gets them 2 seconds after its start.
bool TitleSettled() {
    if (!TitleStarted)
        return false;
    ULONG now = KeTickCount;
    return ModeSet ? now - ModeSetAt >= TOAST_MODE_SETTLE_MS : now - TitleStart >= TOAST_TITLE_SETTLE_MS;
}

void CloseToasts() {
    bool open = QueueOpen;
    ULONG flags = save_flags_cli();
    QueueOpen = false;
    QueueCount = EventCount = 0;
    TitleStarted = false;
    ModeSet = false;
    restore_flags(flags);
    HideToast();
    if (open)
        KeSetEvent(&QueueRoom, 0, FALSE);
}

bool TakeNextToast(QUEUED_TOAST &Toast) {
    ULONG flags = save_flags_cli();
    bool taken = QueueCount != 0;
    if (taken) {
        Toast = Queue[QueueFirst];
        QueueFirst = (QueueFirst + 1) % TOAST_QUEUE_LENGTH;
        QueueCount--;
    }
    restore_flags(flags);
    if (taken)
        KeSetEvent(&QueueRoom, 0, FALSE);
    return taken;
}

void ForgetToastOwner(ULONG Owner) {
    ULONG flags = save_flags_cli();
    for (ULONG i = 0; i < QueueCount; i++) {
        QUEUED_TOAST &toast = Queue[(QueueFirst + i) % TOAST_QUEUE_LENGTH];
        if (toast.Owner == Owner)
            toast.Owner = TOAST_NO_OWNER;
    }
    restore_flags(flags);
}

// --- Events

void NoteToastEvent(const QUEUED_TOAST &Toast, OXOS_TOAST_EVENT Event) {
    if (Toast.Owner == TOAST_NO_OWNER)
        return;
    ULONG flags = save_flags_cli();
    if (EventCount < TOAST_EVENTS_MAX)
        Events[(EventFirst + EventCount++) % TOAST_EVENTS_MAX] = {Toast.Owner, {Toast.ToastId, Event}};
    restore_flags(flags);
}

bool TakeToastEvent(ULONG &Owner, OXOS_TOAST_NOTICE &Notice) {
    ULONG flags = save_flags_cli();
    bool taken = EventCount != 0;
    if (taken) {
        Owner = Events[EventFirst].Owner;
        Notice = Events[EventFirst].Notice;
        EventFirst = (EventFirst + 1) % TOAST_EVENTS_MAX;
        EventCount--;
    }
    restore_flags(flags);
    return taken;
}

} // namespace xk::toasts
