// oxos_igs.xos: in-game shortcuts (spec/oxos_internal/48_in_game_shortcuts.md). Each section of C:\OXOS\extensions\oxos_igs.ini is a shortcut: buttons held
// together on a pad, in any title, power the console off and on, reset it, or start an XBE (or the dashboard) at once, after a toast saying so when the
// section has a toast_message. The pads' state comes from the kernel (oxos_on_pad_changed), for every title however it was built.
#include <oxos_sdk.h>

#include "shortcuts.h"

using namespace oxos::igs;

static BOOLEAN ExtensionStart(void);
static void ExtensionRunLoop(void);
static void ExtensionTerminate(void);

OXOS_DEFINE_EXTENSION(.Name = "oxos_igs", .Version = "1.0.0", .Start = ExtensionStart, .RunLoop = ExtensionRunLoop, .Terminate = ExtensionTerminate);

// --- The shortcuts, read from oxos_igs.ini at each start (each title's launch)

constexpr ULONG POOL_TAG = 0x2073676F; // "ogs "

static SHORTCUT *Shortcuts;    // the valid shortcuts, in the file's order (from the pool); null when there are none
static ULONG ShortcutCount;
// Over all pads, not per OXOS_PAD_STATE::Port: titles whose USB code fills a new buffer for every report move one pad from port to port (spec/oxos_internal/22
// §9.2).
static ULONG Blocked;          // the shortcuts that fired, until their buttons are released

// Reads every section of the file into Shortcuts; a section that is not valid is logged and left out.
static void ReadShortcuts() {
    char **names;
    uint32_t count;
    if (!NT_SUCCESS(oxos_ini_sections(names, count)) || !count) {
        oxos_log("No shortcuts (C:\\OXOS\\extensions\\oxos_igs.ini).");
        return;
    }
    if (count > SHORTCUTS_MAX) {
        oxos_log("%lu shortcuts: only the first %lu are used.", (ULONG)count, SHORTCUTS_MAX);
        count = SHORTCUTS_MAX;
    }
    Shortcuts = static_cast<SHORTCUT *>(ExAllocatePoolWithTag(count * sizeof(SHORTCUT), POOL_TAG));
    for (uint32_t i = 0; Shortcuts && i < count; i++) {
        OXOS_INI_KEY *keys;
        uint32_t keyCount;
        if (!NT_SUCCESS(oxos_ini_section(names[i], keys, keyCount)))
            continue;
        SHORTCUT &shortcut = Shortcuts[ShortcutCount];
        SHORTCUT_TEXT text = {oxos_ini_value(keys, keyCount, "buttons"),       oxos_ini_value(keys, keyCount, "action"),
                              oxos_ini_value(keys, keyCount, "target"),        oxos_ini_value(keys, keyCount, "toast_message"),
                              oxos_ini_value(keys, keyCount, "toast_timeout"), oxos_ini_value(keys, keyCount, "launch_delay")};
        const char *why = ReadShortcut(names[i], text, shortcut);
        if (!why && shortcut.Target[0] && !oxos_file_exists(shortcut.Target))
            why = "target: no such file";
        if (why)
            oxos_log("[%s] left out: %s.", names[i], why);
        else
            ShortcutCount++;
        oxos_ini_free(keys);
    }
    oxos_list_free(names);
}

// --- Firing

static PENDING_SHORTCUT Pending = {-1, 0, 0, false, 0, 0}; // a shortcut waiting for its launch_delay or its toast (spec/oxos_internal/48 §3.4)

// Does what Shortcut says. Returns only when it could not (logged).
static void RunShortcut(const SHORTCUT &Shortcut) {
    char buttons[160];
    DescribeButtons(Shortcut.Buttons, buttons, sizeof(buttons));
    NTSTATUS status;
    if (Shortcut.Action == SHORTCUT_ACTION::DIRECT) {
        oxos_log("[%s] %s: direct %s", Shortcut.Name, buttons, Shortcut.Target[0] ? Shortcut.Target : "(the dashboard)");
        status = oxos_title_launch(Shortcut.Target[0] ? Shortcut.Target : nullptr);
    } else {
        bool cold = Shortcut.Action == SHORTCUT_ACTION::COLD_BOOT;
        oxos_log("[%s] %s: %s", Shortcut.Name, buttons, cold ? "coldboot" : "warmboot");
        status = oxos_console_reset(cold ? OXOS_RESET_COLD : OXOS_RESET_WARM);
    }
    oxos_log("[%s] failed: 0x%08lX.", Shortcut.Name, (ULONG)status);
}

// Shortcut Index fired: the action comes launch_delay seconds after its toast shows (RunPendingShortcut), or after the press without a toast_message;
// launch_delay = 0: at once, without the toast.
static void FireShortcut(int Index) {
    const SHORTCUT &shortcut = Shortcuts[Index];
    const char *title = oxos_title_path();
    if (shortcut.Action == SHORTCUT_ACTION::DIRECT && shortcut.Target[0] && title && strcasecmp(shortcut.Target, title) == 0) {
        oxos_log("[%s] %s is already running.", shortcut.Name, shortcut.Target);
        return;
    }
    ULONG delayMs = shortcut.LaunchDelaySeconds * 1000;
    if (shortcut.ToastMessage[0] && delayMs) {
        OXOS_TOAST toast = {shortcut.ToastMessage, OXOS_TOAST_BOTTOM_CENTRE, shortcut.ToastSeconds, 0};
        NTSTATUS status = oxos_toast_enqueue(toast);
        if (NT_SUCCESS(status)) {
            Pending = {Index, toast.ToastId, KeTickCount, false, 0, delayMs};
            return;
        }
        oxos_log("[%s] toast not queued (0x%08lX): acting at once.", shortcut.Name, (ULONG)status);
        RunShortcut(shortcut);
        return;
    }
    if (delayMs) {
        Pending = {Index, 0, KeTickCount, true, KeTickCount, delayMs};
        return;
    }
    RunShortcut(shortcut);
}

// The waiting shortcut acts when its time has come (IsPendingShortcutDue), or now (Now set: its toast was dropped).
static void RunPendingShortcut(bool Now) {
    if (Pending.Shortcut < 0 || (!Now && !IsPendingShortcutDue(Pending, KeTickCount)))
        return;
    int index = Pending.Shortcut;
    Pending.Shortcut = -1;
    RunShortcut(Shortcuts[index]);
}

// A pad's buttons changed (the run-loop thread, passive level). While a shortcut waits for its toast, no other fires.
static void PadChanged(const OXOS_PAD_STATE &State, void *UserData) {
    int fired = FindFiringShortcut(Shortcuts, ShortcutCount, State.Buttons, Blocked);
    if (fired >= 0 && Pending.Shortcut < 0)
        FireShortcut(fired);
}

// One of the extension's toasts changed (the run-loop thread). Hidden before launch_delay is up: the action still waits for it.
static void ToastChanged(ULONG ToastId, OXOS_TOAST_EVENT Event, void *UserData) {
    if (Pending.Shortcut < 0 || ToastId != Pending.ToastId)
        return;
    if (Event == OXOS_TOAST_SHOWN) {
        Pending.ShownAt = KeTickCount;
        Pending.Shown = true;
    }
    else if (Event == OXOS_TOAST_DROPPED)
        RunPendingShortcut(true);
}

// --- The extension

static void ExtensionRunLoop(void) {
    RunPendingShortcut(false);
}

static void ExtensionTerminate(void) {
    if (Shortcuts)
        ExFreePool(Shortcuts);
    Shortcuts = nullptr;
    ShortcutCount = 0;
}

// Without a valid shortcut it does not stay loaded.
static BOOLEAN ExtensionStart(void) {
    ReadShortcuts();
    if (ShortcutCount && oxos_on_pad_changed(PadChanged, nullptr) && oxos_on_toast(ToastChanged, nullptr))
        return TRUE;
    ExtensionTerminate();
    return FALSE;
}
