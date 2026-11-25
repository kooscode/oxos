// oxos_trainers.xos: Universal Trainers (spec/oxos_internal/37_universal_trainers.md).
//   - A game that has a trainer (its title ID's [section] in C:\OXOS\extensions\oxos_trainers.ini names a file in C:\OXOS\trainers\), started by a title or a
//     dashboard: the title-loading event starts the trainer menu, C:\OXOS\extensions\oxos_trainers.xbe (app/), in its place (oxos_title_redirect). The menu
//     writes the section and starts the game again with the same launch data; that launch goes through.
//   - When a title has been loaded: with trainer_enabled = yes in its section, its trainer is loaded, each cheat's switch set as the "N. name = yes/no" lines
//     say, and the trainer run the way a dashboard does (spec/oxos_internal/23 §1): it patches the title before its first instruction.
// A title started from XBMC4Gamers with its own trainer hook installed is left to XBMC4Gamers.
#include <oxos_sdk.h>

#include "trainer_file.h"

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "oxos_trainers", .Version = "1.1.0", .Start = ExtensionStart);

// --- The trainer file (.etm, trainer_file.h)

// The dword at Offset of the trainer's Size bytes, or 0 past its end.
static ULONG ReadDword(const UCHAR *Trainer, ULONG Size, ULONG Offset) {
    if (Offset > Size || Size - Offset < 4)
        return 0;
    return *(const ULONG *)(Trainer + Offset);
}

// How many cheats the trainer has: its text table's entries after the name and the author. 0 when the table is damaged.
static ULONG CountCheats(const UCHAR *Trainer, ULONG Size) {
    ULONG table = ReadDword(Trainer, Size, ETM_TEXTS);
    ULONG entries = 0;
    while (entries < ETM_MAX_CHEATS + 2 && table + 4 * entries + 4 <= Size && ReadDword(Trainer, Size, table + 4 * entries))
        entries++;
    return entries > 2 ? entries - 2 : 0;
}

// Why the trainer cannot run for TitleId, or null when it can: the checks a dashboard's stub makes, and that every offset is inside the file.
static const char *CheckTrainer(const UCHAR *Trainer, ULONG Size, ULONG TitleId, ULONG Cheats) {
    if (Size < ETM_HEADER_BYTES || *(const USHORT *)(Trainer + ETM_FORMAT) != 1 || ReadDword(Trainer, Size, ETM_SIZE) > Size)
        return "not a trainer file (.etm, format 1)";
    if (ReadDword(Trainer, Size, ETM_HELPER_ENTRY))
        return "it needs a dashboard's helper routines, which OXOS does not have";
    ULONG entry = ReadDword(Trainer, Size, ETM_ENTRY);
    ULONG switches = ReadDword(Trainer, Size, ETM_SWITCHES);
    if (!entry || entry >= Size || switches + Cheats > Size)
        return "its header points outside the file";
    ULONG ids = ReadDword(Trainer, Size, ETM_TITLE_IDS);
    for (ULONG i = 0; i < 3; i++) {
        if (ReadDword(Trainer, Size, ids + 4 * i) == TitleId)
            return nullptr;
    }
    return "it is for other titles";
}

// Runs the trainer at Base: entered at Base + Entry with ebp and edx pointing at Base and the CPU's write protection off (CR0.WP), as a dashboard's stub runs
// it, so it can patch the title's code; every register comes back as it was.
static void RunTrainer(UCHAR *Base, ULONG Entry) {
    UCHAR *entry = Base + Entry;
    __asm__ volatile("movl %0, %%edx\n\t"
                     "movl %1, %%ecx\n\t"
                     "pushal\n\t"
                     "movl %%cr0, %%eax\n\t"
                     "pushl %%eax\n\t"
                     "andl $0xFFFEFFFF, %%eax\n\t"
                     "movl %%eax, %%cr0\n\t"
                     "movl %%edx, %%ebp\n\t"
                     "call *%%ecx\n\t"
                     "popl %%eax\n\t"
                     "movl %%eax, %%cr0\n\t"
                     "popal\n\t"
                     :
                     : "m"(Base), "m"(entry)
                     : "eax", "ecx", "edx", "memory", "cc");
}

// --- XBMC4Gamers' trainer hook (spec/oxos_internal/23 §1): it rewrites the kernel's title-start call "call [0x10128]" to "call [0x8000000C]"

constexpr ULONG KERNEL_IMAGE = 0x80010000;
constexpr ULONG KERNEL_SEARCH_BYTES = 0x2AF90; // the range XBMC4Gamers searches
constexpr UCHAR XBMC_HOOKED_CALL[6] = {0xFF, 0x15, 0x0C, 0x00, 0x00, 0x80};

// XBMC4Gamers' hook is installed: its stub runs its own trainer for this launch.
static BOOLEAN XBMCHookInstalled(void) {
    const UCHAR *kernel = (const UCHAR *)KERNEL_IMAGE;
    for (ULONG i = 0; i + sizeof(XBMC_HOOKED_CALL) <= KERNEL_SEARCH_BYTES; i++) {
        if (kernel[i] == 0xFF && memcmp(kernel + i, XBMC_HOOKED_CALL, sizeof(XBMC_HOOKED_CALL)) == 0)
            return TRUE;
    }
    return FALSE;
}

// --- nxdk programs: OXOS's ISO launcher carries the game's title ID (tools/oxos_isonator), so a dashboard shows the game, but it is not the game

constexpr ULONG XBE_LIBRARY_COUNT = 0x160;    // the image header: how many XDK libraries the title lists, and where
constexpr ULONG XBE_LIBRARY_VERSIONS = 0x164; // each: a name of 8 characters, then its version

// The title was built with nxdk (its one library entry is "CXBE", as nxdk's cxbe writes it): homebrew, or OXOS's ISO launcher with a game's title ID, never
// the game a trainer is for. Retail games list the XDK's libraries.
static BOOLEAN TitleBuiltWithNXDK(const UCHAR *XbeHeader) {
    ULONG count = *(const ULONG *)(XbeHeader + XBE_LIBRARY_COUNT);
    const char *library = *(const char *const *)(XbeHeader + XBE_LIBRARY_VERSIONS);
    return count == 1 && library && memcmp(library, "CXBE", 4) == 0;
}

// --- Title updates (spec/oxos_internal/37 §7.5): a game started from its disc (or an image of it, or its folder) looks for an update in its title data
// folder, E:\TDATA\<title ID>\$u\: when contentmeta.xbx there describes it, the game starts $u\<its own XBE's name> in its place (the XDK's start-up code in
// the game does this, before the game itself runs). So the menu and the trainer wait for that launch: the build that really runs is the update's.

// The XBE at Path, of TitleId, is about to start its title update: it is not in the update folder itself, and the folder holds contentmeta.xbx and an XBE
// of Path's name.
static BOOLEAN StartsItsUpdate(const char *Path, ULONG TitleId) {
    char folder[64], file[160];
    snprintf(folder, sizeof(folder), "\\Device\\Harddisk0\\Partition1\\TDATA\\%08lx\\$u\\", TitleId);
    if (strncasecmp(Path, folder, strlen(folder)) == 0)
        return FALSE;
    const char *name = strrchr(Path, '\\');
    snprintf(file, sizeof(file), "%scontentmeta.xbx", folder);
    if (!oxos_file_exists(file))
        return FALSE;
    snprintf(file, sizeof(file), "%s%s", folder, name ? name + 1 : Path);
    return oxos_file_exists(file);
}

// --- The title's section of oxos_trainers.ini, and the trainer it names

constexpr char TRAINERS_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\trainers\\"; // C:\OXOS\trainers, where the trainers are

// The title's section (its title ID in 8 hex digits) in Keys, and the trainer file it names when that is in C:\OXOS\trainers\: TRUE. FALSE, with nothing to
// free, when the title has no section or no trainer file.
static BOOLEAN ReadSection(ULONG TitleId, OXOS_INI_KEY *&Keys, uint32_t &Count, const char *&File) {
    char section[9];
    snprintf(section, sizeof(section), "%08lX", TitleId);
    if (!NT_SUCCESS(oxos_ini_section(section, Keys, Count))) // no section: a title without a trainer
        return FALSE;
    File = oxos_ini_value(Keys, Count, "trainer");
    char path[160];
    snprintf(path, sizeof(path), "%s%s", TRAINERS_FOLDER, File ? File : "");
    if (File && oxos_file_exists(path))
        return TRUE;
    oxos_ini_free(Keys);
    return FALSE;
}

// The number N of a cheat line "N. name", or 0 when the key is not one.
static ULONG CheatNumber(const char *Key) {
    if (!isdigit(*Key))
        return 0;
    char *end;
    ULONG number = strtoul(Key, &end, 10);
    return *end == '.' ? number : 0;
}

// The loaded trainer: its code runs, so it lives in this extension's image, which the kernel keeps in pages a title's contiguous requests may move and code
// may run from (spec/oxos_internal/34 §8.2). One trainer per title; a new title's load replaces it.
static UCHAR TrainerSpace[ETM_MAX_BYTES];

// Loads the trainer File from C:\OXOS\trainers\ into TrainerSpace; its size in *Size. Null, with the reason logged, when it cannot.
static UCHAR *LoadTrainer(const char *Title, const char *File, ULONG *Size) {
    char path[160];
    snprintf(path, sizeof(path), "%s%s", TRAINERS_FOLDER, File);
    UCHAR probe;
    NTSTATUS status = oxos_file_read(path, &probe, 1, Size);
    if (status != STATUS_BUFFER_TOO_SMALL || *Size > ETM_MAX_BYTES) {
        oxos_log("%s: trainer %s not loaded (%#lx, %lu bytes).", Title, path, status, *Size);
        return nullptr;
    }
    if (!NT_SUCCESS(oxos_file_read(path, TrainerSpace, *Size, Size))) {
        oxos_log("%s: trainer %s not loaded (not read).", Title, path);
        return nullptr;
    }
    return TrainerSpace;
}

// The title-loaded event: its trainer, when its section says so.
static void TitleLoaded(const OXOS_TITLE_LOADED *Title, void *UserData) {
    if (TitleBuiltWithNXDK((const UCHAR *)Title->XbeHeader) || StartsItsUpdate(Title->Path, Title->TitleId))
        return;
    OXOS_INI_KEY *keys;
    uint32_t count;
    const char *file;
    if (!ReadSection(Title->TitleId, keys, count, file))
        return;
    const char *enabled = oxos_ini_value(keys, count, "trainer_enabled");
    if (!enabled || strcasecmp(enabled, "yes") != 0) {
        oxos_ini_free(keys);
        return;
    }
    if (XBMCHookInstalled()) {
        oxos_log("%s: XBMC4Gamers' trainer hook is installed, so its trainer runs, not %s.", Title->TitleName, file);
        oxos_ini_free(keys);
        return;
    }

    ULONG size;
    UCHAR *trainer = LoadTrainer(Title->TitleName, file, &size);
    ULONG cheats = trainer ? CountCheats(trainer, size) : 0;
    const char *refused = trainer ? CheckTrainer(trainer, size, Title->TitleId, cheats) : nullptr;
    if (refused) {
        oxos_log("%s: trainer %s not run: %s.", Title->TitleName, file, refused);
        trainer = nullptr;
    }
    if (!trainer) {
        oxos_ini_free(keys);
        return;
    }

    // Each cheat's switch from its "N. name = yes/no" line; a cheat without a line keeps the trainer's own default.
    UCHAR *switches = trainer + ReadDword(trainer, size, ETM_SWITCHES);
    for (uint32_t i = 0; i < count; i++) {
        ULONG number = CheatNumber(keys[i].Key);
        if (number >= 1 && number <= cheats)
            switches[number - 1] = strcasecmp(keys[i].Value, "yes") == 0 ? 1 : 0;
    }
    ULONG on = 0;
    for (ULONG i = 0; i < cheats; i++)
        on += switches[i] ? 1 : 0;

    RunTrainer(trainer, ReadDword(trainer, size, ETM_ENTRY));
    oxos_log("%s: trainer %s applied, %lu of %lu cheats on.", Title->TitleName, file, on, cheats);
    oxos_ini_free(keys); // after the log: file is in keys
}

// --- The trainer menu (app/): oxos_trainers.xbe, started in place of a game that has a trainer

constexpr char MENU_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\oxos_trainers.xbe";

constexpr ULONG XBE_BASE_ADDRESS = 0x104; // the image header: where the XBE is loaded
constexpr ULONG XBE_TIMESTAMP = 0x114;    // the image header's build timestamp: which build of the title this is (a trainer is made for one)
constexpr ULONG XBE_CERTIFICATE = 0x118;  // the certificate's address; the title ID is at +8 in it
constexpr ULONG XBE_HEADER_BYTES = 0x170; // the header up to the library versions' address

// The state page (oxos_state): the launch that started the menu in place of a title, and that title. The menu starts it at the next launch, which goes
// through; the launches of the same build straight after it get no second menu.
struct MENU_STATE {
    ULONG MenuLaunch;    // oxos_launch_number() of the menu's launch; 0 = none
    char TitlePath[360]; // the title's path, as the title-loading event had it
    ULONG TitleId;       // the title's ID
    ULONG TitleBuild;    // the title's build (its XBE timestamp)
    ULONG TitleLaunch;   // the launch of the title the menu started, or of its latest launch of the same build straight after; 0 = none
};

// The title ID and build (XBE timestamp) of the XBE at Path, read from the file before it is loaded: TRUE. FALSE when it cannot be read, or for an nxdk
// program (TitleBuiltWithNXDK).
static BOOLEAN ReadTitleIdAndBuild(const char *Path, ULONG &TitleId, ULONG &Build) {
    HANDLE file;
    if (!NT_SUCCESS(oxos_file_open(Path, GENERIC_READ, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE, &file)))
        return FALSE;
    UCHAR header[XBE_HEADER_BYTES];
    ULONG certificate[3];
    char library[8];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = 0;
    BOOLEAN read = NT_SUCCESS(NtReadFile(file, nullptr, nullptr, nullptr, &iosb, header, sizeof(header), &at)) && iosb.Information == sizeof(header);
    ULONG base = read ? *(const ULONG *)(header + XBE_BASE_ADDRESS) : 0;
    if (read) {
        at.QuadPart = *(const ULONG *)(header + XBE_CERTIFICATE) - base;
        read = NT_SUCCESS(NtReadFile(file, nullptr, nullptr, nullptr, &iosb, certificate, sizeof(certificate), &at)) && iosb.Information == sizeof(certificate);
    }
    BOOLEAN nxdk = FALSE;
    if (read && *(const ULONG *)(header + XBE_LIBRARY_COUNT) == 1) {
        at.QuadPart = *(const ULONG *)(header + XBE_LIBRARY_VERSIONS) - base;
        nxdk = NT_SUCCESS(NtReadFile(file, nullptr, nullptr, nullptr, &iosb, library, sizeof(library), &at)) && memcmp(library, "CXBE", 4) == 0;
    }
    NtClose(file);
    TitleId = certificate[2];
    Build = *(const ULONG *)(header + XBE_TIMESTAMP);
    return read && !nxdk;
}

// The title-loading event: the menu in place of a game that has a trainer, when this launch can be redirected and the menu is there.
static void TitleLoading(const OXOS_TITLE_LOADING *Title, void *UserData) {
    auto state = (MENU_STATE *)oxos_state();
    ULONG launch = oxos_launch_number();
    if (state->MenuLaunch && launch != state->MenuLaunch) {
        BOOLEAN fromMenu = launch == state->MenuLaunch + 1 && strcasecmp(Title->Path, state->TitlePath) == 0;
        state->MenuLaunch = 0;
        if (fromMenu) {
            state->TitleLaunch = launch;
            return;
        }
    }
    // Only the very next launch can be the title launching itself again.
    BOOLEAN afterTitle = state->TitleLaunch && launch == state->TitleLaunch + 1;
    state->TitleLaunch = 0;
    if (Title->Size < sizeof(OXOS_TITLE_LOADING) || !Title->Target || strlen(Title->Path) >= sizeof(state->TitlePath) || !oxos_file_exists(MENU_PATH))
        return;
    ULONG titleId, build;
    OXOS_INI_KEY *keys;
    uint32_t count;
    const char *file;
    if (!ReadTitleIdAndBuild(Title->Path, titleId, build) || !ReadSection(titleId, keys, count, file))
        return;
    oxos_ini_free(keys);
    // The title the menu started, launching the same build again straight away (it restarts itself at start-up, or goes on to another XBE of that build):
    // no second menu; the title-loaded event applies the chosen trainer again. Another build (a title update, another XBE built apart) gets the menu, which
    // shows the trainers made for that build (spec/oxos_internal/37 §7.5b).
    if (afterTitle && titleId == state->TitleId && build == state->TitleBuild) {
        state->TitleLaunch = launch;
        oxos_log("%08lX: the title launches the same build again (%08lX): no menu (%s).", titleId, build, Title->Path);
        return;
    }
    if (StartsItsUpdate(Title->Path, titleId)) {
        oxos_log("%08lX: the title update in E:\\TDATA\\%08lx\\$u starts next: the trainer menu comes with it (%s).", titleId, titleId, Title->Path);
        return;
    }
    if (XBMCHookInstalled() || !oxos_title_redirect(Title, MENU_PATH))
        return;
    state->MenuLaunch = launch;
    state->TitleId = titleId;
    state->TitleBuild = build;
    strlcpy(state->TitlePath, Title->Path, sizeof(state->TitlePath));
    oxos_log("%08lX: the trainer menu first (%s).", titleId, Title->Path);
}

// --- The extension

static BOOLEAN ExtensionStart(void) {
    return oxos_on_title_loading(TitleLoading, nullptr) && oxos_on_title_loaded(TitleLoaded, nullptr);
}
