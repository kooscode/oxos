// The fatal error screen's text (spec/oxos_internal/29_error_screens.md): per code, a title and what to try. The codes
// and when they are raised follow the reference (spec/stock/05 §6); the words are OXOS's own. Lines are at most
// XK_FATAL_LINE characters (a unit test checks them all).
#include <xk/core.h>

static const char *const XkpHelpUnlock[] = {
    "The hard disk is locked, and this console's",
    "key does not unlock it.",
    "",
    "- A disk locked by another Xbox unlocks only",
    "  in that Xbox. Unlock it there, or on a PC.",
    nullptr,
};

static const char *const XkpHelpTimeout[] = {
    "The hard disk stayed busy for 30 seconds.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- The disk may be failing: try another one.",
    nullptr,
};

static const char *const XkpHelpIdentify[] = {
    "The hard disk did not answer the IDENTIFY",
    "command.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- A SATA disk needs a working IDE adapter.",
    nullptr,
};

static const char *const XkpHelpNoDisk[] = {
    "No hard disk answered on the IDE cable, or",
    "the disk is smaller than 8 GB.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- A SATA disk needs a working IDE adapter.",
    nullptr,
};

// Codes 13-20: the dashboards tried (titles/loader.cpp, in its order), and the way to the File Manager (core/fatal.cpp,
// XkNoDashboardError).
static const char *const XkpHelpNoDashboard[] = {
    "Tried:",
    "  C:\\evoxdash.xbe",
    "  E:\\evoxdash.xbe",
    "  C:\\xboxdash.xbe",
    "  C:\\Dashboard\\default.xbe",
    "",
    "[ Press  X  to launch Web File Manager ]", // the X drawn as the button (XkDrawText)
    nullptr,
};

static const char *const XkpHelpPreviousBoot[] = {
    "A title or the dashboard stopped with a fatal",
    "error and restarted the console.",
    "",
    "- Turn the console off and on again.",
    "- If it happens again, reinstall that title",
    "  or the dashboard.",
    nullptr,
};

static const char *const XkpHelpOther[] = {
    "The kernel could not finish starting up.",
    "",
    "- Turn the console off and on again.",
    "- If it happens again, report the code below.",
    nullptr,
};

// 13 + the launch data's dashboard reason (1-6) gives 14-19: the same screen as 13.
static const XK_FATAL_HELP XkpHelp[] = {
    {6, 6, "Could Not Unlock HDD", XkpHelpUnlock},
    {7, 7, "HDD Timeout", XkpHelpTimeout},
    {8, 8, "HDD IDENTIFY Failed", XkpHelpIdentify},
    {9, 9, "No HDD Detected", XkpHelpNoDisk},
    {13, 19, "No Dashboard Found", XkpHelpNoDashboard},
    {20, 20, "No Dashboard Found (Disc Inserted)", XkpHelpNoDashboard},
    {21, 21, "Previous Boot Failed", XkpHelpPreviousBoot},
};

static const XK_FATAL_HELP XkpHelpFallback = {0, 0xFFFFFFFF, "Fatal Error", XkpHelpOther};

const XK_FATAL_HELP *XkFatalHelp(ULONG Code) {
    for (auto &h : XkpHelp) {
        if (Code >= h.First && Code <= h.Last)
            return &h;
    }
    return &XkpHelpFallback;
}
