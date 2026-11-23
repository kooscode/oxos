// oxos_igs: the shortcuts of oxos_igs.ini (spec/oxos_internal/48_in_game_shortcuts.md §2-3): a section's buttons, action, target and toast message, and the
// rule that decides which shortcut fires. No kernel calls, so the host unit tests run it unchanged (tests/unit/test_in_game_shortcuts.cpp).
#pragma once
#include <oxos_sdk.h>

namespace oxos::igs {

constexpr ULONG SHORTCUTS_MAX = 16;      // shortcuts kept from the file (FindFiringShortcut's Blocked has a bit for each)
constexpr ULONG SECTION_NAME_MAX = 32;           // a section's name kept for the log, with its 0 (a longer one is cut)
constexpr ULONG TARGET_MAX = 360;        // a target's \Device\ path, with its 0: the length the kernel's launch path takes
constexpr ULONG TOAST_MESSAGE_MAX = 43;  // a toast message, with its 0: one line of a toast (OXOS_TOAST_LINE_MAX)
constexpr ULONG LAUNCH_DELAY_DEFAULT = 1, LAUNCH_DELAY_MAX = 5; // launch_delay: seconds from the toast showing (or the press) to the action; 0 at once
constexpr ULONG TOAST_TIMEOUT_DEFAULT = 2, TOAST_TIMEOUT_MAX = 30;          // toast_timeout: seconds the toast stays up

// What a shortcut does (`action =`).
enum class SHORTCUT_ACTION { COLD_BOOT, WARM_BOOT, DIRECT };

// One shortcut: one section of the file.
struct SHORTCUT {
    char Name[SECTION_NAME_MAX];                  // the section's name, for the log
    ULONG Buttons;                        // OXOS_PAD_* bits, all held together to fire
    SHORTCUT_ACTION Action;
    char Target[TARGET_MAX];              // DIRECT: the XBE as a \Device\ path; empty: the dashboard
    char ToastMessage[TOAST_MESSAGE_MAX]; // shown before the action; empty: none
    ULONG ToastSeconds;                   // how long the toast stays up
    ULONG LaunchDelaySeconds;             // from the toast showing (without one: from the press) to the action; 0: at once, no toast
};

// One section's values as the file gives them (null for a key it does not have).
struct SHORTCUT_TEXT {
    const char *Buttons;
    const char *Action;
    const char *Target;
    const char *ToastMessage;
    const char *ToastTimeout;
    const char *LaunchDelay;
};

// The button names of `buttons =`, in OXOS_PAD_* bit order (bit 0 = padup).
constexpr const char *BUTTON_NAMES[16] = {"padup", "paddown", "padleft", "padright", "start", "back",  "lthumb",   "rthumb",
                                          "a",     "b",       "x",       "y",        "black", "white", "ltrigger", "rtrigger"};

// The drives a target may name, and their \Device\ paths: C E X Y Z as spec/stock/04 §4, F and G as spec/oxos_internal/45, D the DVD drive.
struct TARGET_DRIVE {
    char Letter;
    const char *Device;
};
constexpr TARGET_DRIVE TARGET_DRIVES[] = {{'C', "\\Device\\Harddisk0\\Partition2"}, {'D', "\\Device\\CdRom0"},
                                          {'E', "\\Device\\Harddisk0\\Partition1"}, {'F', "\\Device\\Harddisk0\\Partition6"},
                                          {'G', "\\Device\\Harddisk0\\Partition7"}, {'X', "\\Device\\Harddisk0\\Partition3"},
                                          {'Y', "\\Device\\Harddisk0\\Partition4"}, {'Z', "\\Device\\Harddisk0\\Partition5"}};

inline char ToLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool IsBlank(char c) {
    return c == ' ' || c == '\t';
}

// True when Text[0..Length) is Word (lower case), in any case.
inline bool IsWord(const char *Text, ULONG Length, const char *Word) {
    ULONG i = 0;
    for (; i < Length && Word[i]; i++) {
        if (ToLower(Text[i]) != Word[i])
            return false;
    }
    return i == Length && !Word[i];
}

// Copies Text into Out (Size bytes with the 0); false when it does not fit, and Out then holds as much as fits.
inline bool CopyText(char *Out, ULONG Size, const char *Text) {
    ULONG i = 0;
    for (; Text[i] && i + 1 < Size; i++)
        Out[i] = Text[i];
    Out[i] = 0;
    return !Text[i];
}

// How many buttons Buttons holds.
inline ULONG CountButtons(ULONG Buttons) {
    ULONG count = 0;
    for (; Buttons; Buttons &= Buttons - 1)
        count++;
    return count;
}

// The buttons of a `buttons =` value: names joined by '+', blanks around each. False for a name that is not a button, or an empty one.
inline bool ReadButtons(const char *Text, ULONG &Buttons) {
    Buttons = 0;
    for (const char *p = Text;; p++) {
        while (IsBlank(*p))
            p++;
        const char *name = p;
        while (*p && *p != '+' && !IsBlank(*p))
            p++;
        ULONG length = static_cast<ULONG>(p - name);
        while (IsBlank(*p))
            p++;
        ULONG bit = 0;
        for (ULONG i = 0; i < 16 && !bit; i++) {
            if (IsWord(name, length, BUTTON_NAMES[i]))
                bit = 1u << i;
        }
        if (!bit || (*p && *p != '+'))
            return false;
        Buttons |= bit;
        if (!*p)
            return true;
    }
}

// The button names of Buttons joined by " + " in Out (Size bytes), for the log.
inline void DescribeButtons(ULONG Buttons, char *Out, ULONG Size) {
    ULONG used = 0;
    Out[0] = 0;
    for (ULONG i = 0; i < 16; i++) {
        if (!(Buttons & 1u << i))
            continue;
        const char *parts[2] = {used ? " + " : "", BUTTON_NAMES[i]};
        for (const char *part : parts) {
            for (; *part && used + 1 < Size; part++)
                Out[used++] = *part;
        }
        Out[used] = 0;
    }
}

// The \Device\ path of a target in Out (Size bytes): a drive path (C:\..., one of TARGET_DRIVES, any case) with its drive's device, or a \Device\ path as it
// is; '/' is read as '\'. False for another drive, a relative path, or one that does not fit.
inline bool ReadTarget(const char *Target, char *Out, ULONG Size) {
    const char *device = "";
    const char *rest = Target;
    if (Target[0] && Target[1] == ':') {
        for (const TARGET_DRIVE &drive : TARGET_DRIVES) {
            if (ToLower(Target[0]) == ToLower(drive.Letter))
                device = drive.Device;
        }
        if (!*device || (Target[2] != '\\' && Target[2] != '/'))
            return false;
        rest = Target + 2;
    } else if (Target[0] != '\\') {
        return false;
    }
    ULONG used = 0;
    for (const char *p = device; *p; p++) {
        if (used + 1 >= Size)
            return false;
        Out[used++] = *p;
    }
    for (const char *p = rest; *p; p++) {
        if (used + 1 >= Size)
            return false;
        Out[used++] = *p == '/' ? '\\' : *p;
    }
    Out[used] = 0;
    return true;
}

// A whole number of at most 7 digits in Value; false for anything else.
inline bool ReadNumber(const char *Text, ULONG &Value) {
    Value = 0;
    ULONG digits = 0;
    for (; Text[digits]; digits++) {
        if (Text[digits] < '0' || Text[digits] > '9' || digits == 7)
            return false;
        Value = Value * 10 + (ULONG)(Text[digits] - '0');
    }
    return digits > 0;
}

// One section of oxos_igs.ini into Shortcut: its name and its keys' values. Null when the shortcut is valid, else why not, for the log. Whether the target
// exists is the caller's to check.
inline const char *ReadShortcut(const char *Name, const SHORTCUT_TEXT &Text, SHORTCUT &Shortcut) {
    Shortcut = {};
    CopyText(Shortcut.Name, sizeof(Shortcut.Name), Name);
    Shortcut.ToastSeconds = TOAST_TIMEOUT_DEFAULT;
    Shortcut.LaunchDelaySeconds = LAUNCH_DELAY_DEFAULT;
    if (!Text.Buttons)
        return "no buttons = line";
    if (!ReadButtons(Text.Buttons, Shortcut.Buttons))
        return "buttons: a name that is not a button";
    if (CountButtons(Shortcut.Buttons) < 2)
        return "buttons: fewer than two";
    ULONG actionLength = 0;
    while (Text.Action && Text.Action[actionLength])
        actionLength++;
    if (Text.Action && IsWord(Text.Action, actionLength, "coldboot"))
        Shortcut.Action = SHORTCUT_ACTION::COLD_BOOT;
    else if (Text.Action && IsWord(Text.Action, actionLength, "warmboot"))
        Shortcut.Action = SHORTCUT_ACTION::WARM_BOOT;
    else if (Text.Action && IsWord(Text.Action, actionLength, "direct"))
        Shortcut.Action = SHORTCUT_ACTION::DIRECT;
    else
        return "action: not coldboot, warmboot or direct";
    if (Shortcut.Action == SHORTCUT_ACTION::DIRECT && Text.Target && Text.Target[0] &&
        !ReadTarget(Text.Target, Shortcut.Target, sizeof(Shortcut.Target)))
        return "target: not on a drive OXOS knows (C D E F G X Y Z), or too long";
    if (Text.ToastMessage && !CopyText(Shortcut.ToastMessage, sizeof(Shortcut.ToastMessage), Text.ToastMessage))
        return "toast_message: longer than 42 characters";
    if (Text.ToastTimeout && (!ReadNumber(Text.ToastTimeout, Shortcut.ToastSeconds) || !Shortcut.ToastSeconds || Shortcut.ToastSeconds > TOAST_TIMEOUT_MAX))
        return "toast_timeout: not 1 to 30 (seconds)";
    if (Text.LaunchDelay && (!ReadNumber(Text.LaunchDelay, Shortcut.LaunchDelaySeconds) || Shortcut.LaunchDelaySeconds > LAUNCH_DELAY_MAX))
        return "launch_delay: not 0 to 5 (seconds)";
    return nullptr;
}

// The shortcut that fires when a pad's buttons change to Held: the first of the Count whose buttons are exactly the ones held, no more and no fewer (user,
// 2026-11-23: "we only match EXACT buttons"), and that has not fired since they were last released; -1 when none. Blocked (a bit per shortcut, over all
// pads) is kept between calls: a shortcut that fires stays blocked until none of its buttons is held, and until then no shortcut fires (letting go of one
// button of a combination must not fire a shorter one). Buttons still held from the title before are no change: the kernel sends no event for them
// (spec/oxos_internal/22 §9.2).
inline int FindFiringShortcut(const SHORTCUT *Shortcuts, ULONG Count, ULONG Held, ULONG &Blocked) {
    for (ULONG i = 0; i < Count; i++) {
        if (!(Held & Shortcuts[i].Buttons))
            Blocked &= ~(1u << i);
    }
    if (Blocked)
        return -1;
    for (ULONG i = 0; i < Count; i++) {
        if (Held != Shortcuts[i].Buttons)
            continue;
        Blocked |= 1u << i;
        return static_cast<int>(i);
    }
    return -1;
}

// --- A shortcut that waits (spec/oxos_internal/48 §3.4): launch_delay seconds after its toast shows (without a toast_message, after the press), or at once
// when the toast does not show within a second: the action never waits on the toast.

constexpr ULONG SHOW_LIMIT_MS = 1000; // from queueing the toast to the action, when it has not shown

// A shortcut that fired and waits.
struct PENDING_SHORTCUT {
    int Shortcut;   // its index; -1 when none waits
    ULONG ToastId;  // its toast; 0 without one
    ULONG QueuedAt; // KeTickCount when the toast was queued
    bool Shown;     // its toast has shown (without a toast_message: set at the press)
    ULONG ShownAt;  // KeTickCount when it showed, or of the press without a toast
    ULONG DelayMs;  // its launch_delay, in milliseconds
};

// True when Pending's shortcut must act now (Now: KeTickCount).
inline bool IsPendingShortcutDue(const PENDING_SHORTCUT &Pending, ULONG Now) {
    if (Pending.Shortcut < 0)
        return false;
    return Pending.Shown ? Now - Pending.ShownAt >= Pending.DelayMs : Now - Pending.QueuedAt >= SHOW_LIMIT_MS;
}

} // namespace oxos::igs
