// The fatal error screen's text (spec/oxos_internal/29_error_screens.md): per code, a title and what to try. The codes and when they are raised follow the
// reference (spec/stock/05 §6); the words are OXOS's own. Lines are at most XK_FATAL_LINE characters (a unit test checks them all).
#include <user_interface/user_interface.h>

namespace xk::user_interface {

static const char *const HelpUnlock[] = {
    "The hard disk is locked, and this console's",
    "key does not unlock it.",
    "",
    "- A disk locked by another Xbox unlocks only",
    "  in that Xbox. Unlock it there, or on a PC.",
    nullptr,
};

static const char *const HelpTimeout[] = {
    "The hard disk stayed busy for 30 seconds.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- The disk may be failing: try another one.",
    nullptr,
};

static const char *const HelpIdentify[] = {
    "The hard disk did not answer the IDENTIFY",
    "command.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- A SATA disk needs a working IDE adapter.",
    nullptr,
};

static const char *const HelpNoDisk[] = {
    "No hard disk answered on the IDE cable, or",
    "the disk is smaller than 8 GB.",
    "",
    "- Check the IDE cable and the disk's power.",
    "- A SATA disk needs a working IDE adapter.",
    nullptr,
};

// Codes 13-20 (the title says it: No Dashboard Found): where the kernel looked, oxos_settings.ini's dashboards and then the stock one (titles/title_loader.cpp,
// spec/oxos_internal/19), and the way to the File Manager (error_screens.cpp, NoDashboardError).
static const char *const HelpNoDashboard[] = {
    "No dashboard could be loaded.",
    "Check C:\\OXOS\\oxos_settings.ini",
    "or add the default C:\\xboxdash.xbe.",
    "",
    "[ Press  X  to launch Web File Manager ]", // the X drawn as the button (graphics::DrawText)
    nullptr,
};

static const char *const HelpPreviousBoot[] = {
    "A title or the dashboard stopped with a fatal",
    "error and restarted the console.",
    "",
    "- Turn the console off and on again.",
    "- If it happens again, reinstall that title",
    "  or the dashboard.",
    nullptr,
};

static const char *const HelpOther[] = {
    "The kernel could not finish starting up.",
    "",
    "- Turn the console off and on again.",
    "- If it happens again, report the code below.",
    nullptr,
};

// 13 + the launch data's dashboard reason (1-6) gives 14-19: the same screen as 13.
static const XK_FATAL_HELP Help[] = {
    {6, 6, "Could Not Unlock HDD", HelpUnlock},
    {7, 7, "HDD Timeout", HelpTimeout},
    {8, 8, "HDD IDENTIFY Failed", HelpIdentify},
    {9, 9, "No HDD Detected", HelpNoDisk},
    {13, 19, "No Dashboard Found", HelpNoDashboard},
    {20, 20, "No Dashboard Found (Disc Inserted)", HelpNoDashboard},
    {21, 21, "Previous Boot Failed", HelpPreviousBoot},
};

static const XK_FATAL_HELP HelpFallback = {0, 0xFFFFFFFF, "Fatal Error", HelpOther};

const XK_FATAL_HELP *FatalHelp(ULONG Code) {
    for (auto &h : Help) {
        if (Code >= h.First && Code <= h.Last)
            return &h;
    }
    return &HelpFallback;
}

} // namespace xk::user_interface
