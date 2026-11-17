// The kernel extension loader (spec/oxos_internal/34 §3-§8): before each title, reads C:\OXOS\oxos_extensions.ini, loads each listed .xos from
// C:\OXOS\extensions\ with the settings of its own NAME.ini there, gives it its state page and calls its Load, then links the hook chains and starts the run
// loop thread. Not part of the resident kernel: linked on its own at XK_EXTENSION_LOADER_BASE against the kernel's addresses, and run there for each launch by
// extensions/extension_runtime_loader.cpp (§8.1). What runs while the title runs is in extensions/extension_runtime.cpp.
#include <extensions/extension_state.h>
#include <extensions/extension_loader.h>
#include <core/core.h>
#include <ini_file/extensions_ini.h>
#include <io/io.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <extensions/extension_parser.h>
#include <runtime/runtime.h>
#include <diagnostics/diagnostics.h>
#include <titles/titles.h>

namespace xk::extensions {

namespace {

constexpr char LIST_PATH[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\oxos_extensions.ini";
constexpr char FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\";
constexpr ULONG LIST_MAX_BYTES = 0x10000;      // the list file
constexpr ULONG FILE_MAX_BYTES = 0x40000;      // one extension's file
constexpr ULONG OWN_INI_MAX_BYTES = 0x100000;  // its own NAME.ini: settings at the top, data sections after them (read by the SDK)
constexpr ULONG IMAGES_MAX_BYTES = 0x80000;    // all loaded images together
constexpr ULONG LOOP_STACK_BYTES = 0x4000;     // the run loop thread's kernel stack
constexpr ULONG POOL_TAG = 'txEX';
constexpr ULONG IMPORT_SLOTS_MAX = 1024;       // kernel imports of all extensions together
constexpr NTSTATUS STATUS_NOT_SUPPORTED = (NTSTATUS)0xC00000BB;
constexpr ULONG EXPORT_ORDINAL_MAX = 512;      // above the kernel's highest export (kernel/exports.tsv)

} // namespace

// Whether the descriptor's context pointer lies inside the extension's own image (the kernel writes there).
static bool InImage(OXOS_CONTEXT **Slot, PUCHAR Image, ULONG ImageSize) {
    auto at = (PUCHAR)Slot;
    return at >= Image && at + sizeof(*Slot) <= Image + ImageSize;
}

// --- State pages (§6): one persisted page per descriptor Name, found again through a persisted index page; zeroed again when the Version changes

namespace {

constexpr ULONG STATE_INDEX_MAGIC = 0x58444958; // "XIDX"
constexpr ULONG STATE_NAME_MAX = 31;
constexpr ULONG STATE_VERSION_MAX = 15;
constexpr ULONG STATE_INDEX_ENTRIES = 64;

struct STATE_INDEX_ENTRY {
    char Name[STATE_NAME_MAX + 1];
    char Version[STATE_VERSION_MAX + 1]; // the descriptor's Version that wrote the page
    PVOID Page;
    BOOLEAN Used; // taken at this launch
};

struct STATE_INDEX {
    ULONG Magic;
    ULONG Checksum; // of everything after it
    ULONG Count;
    STATE_INDEX_ENTRY Entries[STATE_INDEX_ENTRIES];
};
static_assert(sizeof(STATE_INDEX) <= PAGE_SIZE);

ULONG IndexChecksum(const STATE_INDEX *Index) {
    auto words = (const ULONG *)&Index->Count;
    ULONG sum = 0x6B657821;
    for (ULONG i = 0; i < (sizeof(STATE_INDEX) - 8) / 4; i++)
        sum = (sum << 5 | sum >> 27) ^ words[i];
    return sum;
}

// The persisted index (the kernel keeps its address in StateIndexPage, extensions/extension_runtime.cpp).
STATE_INDEX *&StateIndex() {
    return *reinterpret_cast<STATE_INDEX **>(&StateIndexPage);
}

} // namespace

// Checks the state index at the start of a launch: none after a cold boot, a new one when its contents are not intact (the pages it named stay lost until the
// next cold boot).
static void CheckStateIndex() {
    STATE_INDEX *index = StateIndex();
    if (!KiQuickRebootFlag || !index) {
        StateIndex() = nullptr;
        return;
    }
    if (index->Magic != STATE_INDEX_MAGIC || index->Count > STATE_INDEX_ENTRIES || index->Checksum != IndexChecksum(index)) {
        XK_SERIAL("extensions: state index damaged: every state page starts new\n");
        StateIndex() = nullptr;
        return;
    }
    for (ULONG i = 0; i < index->Count; i++)
        index->Entries[i].Used = FALSE;
}

// The state page of Name, taken for this launch: its page from the last launch, zeroed again when Version is not the one that wrote it (a new build may lay
// it out differently), or a new zeroed one; null when memory is short.
static PVOID StatePageFor(const char *Name, const char *Version) {
    STATE_INDEX *index = StateIndex();
    for (ULONG i = 0; index && i < index->Count; i++) {
        STATE_INDEX_ENTRY &e = index->Entries[i];
        if (strncmp(e.Name, Name, STATE_NAME_MAX) == 0 && !e.Used) {
            if (strncmp(e.Version, Version, STATE_VERSION_MAX) != 0) {
                memset(e.Page, 0, PAGE_SIZE);
                strlcpy(e.Version, Version, sizeof(e.Version));
            }
            e.Used = TRUE;
            return e.Page;
        }
    }
    if (!index) {
        index = (STATE_INDEX *)MmAllocateContiguousMemory(PAGE_SIZE);
        if (!index)
            return nullptr;
        memset(index, 0, PAGE_SIZE);
        index->Magic = STATE_INDEX_MAGIC;
        MmPersistContiguousMemory(index, PAGE_SIZE, TRUE);
        StateIndex() = index;
    }
    if (index->Count == STATE_INDEX_ENTRIES)
        return nullptr;
    PVOID page = MmAllocateContiguousMemory(PAGE_SIZE);
    if (!page)
        return nullptr;
    memset(page, 0, PAGE_SIZE);
    MmPersistContiguousMemory(page, PAGE_SIZE, TRUE);
    STATE_INDEX_ENTRY &e = index->Entries[index->Count++];
    strlcpy(e.Name, Name, sizeof(e.Name));
    strlcpy(e.Version, Version, sizeof(e.Version));
    e.Page = page;
    e.Used = TRUE;
    return page;
}

// Gives back the state page of an extension whose Load failed: freed with the pages no extension took.
static void ReleaseStatePage(PVOID Page) {
    STATE_INDEX *index = StateIndex();
    for (ULONG i = 0; index && Page && i < index->Count; i++) {
        if (index->Entries[i].Page == Page)
            index->Entries[i].Used = FALSE;
    }
}

// After the loads: frees the pages no extension took at this launch, and the index when it holds none.
static void TidyStateIndex() {
    STATE_INDEX *index = StateIndex();
    if (!index)
        return;
    ULONG kept = 0;
    for (ULONG i = 0; i < index->Count; i++) {
        STATE_INDEX_ENTRY e = index->Entries[i];
        if (!e.Used) {
            MmFreeContiguousMemory(e.Page);
            continue;
        }
        index->Entries[kept++] = e;
    }
    index->Count = kept;
    if (!kept) {
        MmFreeContiguousMemory(index);
        StateIndex() = nullptr;
        return;
    }
    index->Checksum = IndexChecksum(index);
}

// --- The loader's functions for Load (§5.1)

namespace {

// Kernel functions an extension may not hook (§7.1): called at every IRQL, or the reboot.
constexpr ULONG NOT_HOOKABLE[] = {
    49,  // HalReturnToFirmware
    103, // KeGetCurrentIrql
    129, // KeRaiseIrqlToDpcLevel
    160, // KfRaiseIrql
    161, // KfLowerIrql
};
// OXOS's runtime exports for extensions (the C library, the compiler's 64-bit helpers, the log line): titles never import them.
constexpr ULONG RUNTIME_ORDINALS_FIRST = 402, RUNTIME_ORDINALS_LAST = 428;

// The kernel image's code section: a data export lies outside it.
bool IsKernelCode(ULONG Address) {
    auto image = (PUCHAR)0x80010000;
    PUCHAR nt = image + *(ULONG *)(image + 0x3C);
    PUCHAR section = nt + 24 + *(USHORT *)(nt + 20);
    ULONG start = (ULONG)image + *(ULONG *)(section + 12);
    return Address >= start && Address < start + *(ULONG *)(section + 8);
}

// The index of the extension Context belongs to.
ULONG ExtensionIndex(OXOS_CONTEXT *Context) {
    return (ULONG)((XK_EXTENSION *)Context - LoadedExtensions->Extensions);
}

// The ordinal of the kernel export at Address (an extension's import of it, before the chains are linked), or 0.
static ULONG ExportOrdinalAt(PVOID Address) {
    for (ULONG ordinal = 1; ordinal <= EXPORT_ORDINAL_MAX; ordinal++) {
        if (titles::KernelExportAddress(ordinal) == (ULONG)Address)
            return ordinal;
    }
    return 0;
}

NTSTATUS NTAPI HookFunction(OXOS_CONTEXT *Context, PVOID Function, PVOID Hook) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    ULONG hooked = ExportOrdinalAt(Function); // its ordinal
    ULONG target = (ULONG)Function;
    if (!hooked || !Hook)
        return STATUS_INVALID_PARAMETER;
    for (ULONG ordinal : NOT_HOOKABLE) {
        if (ordinal == hooked)
            return STATUS_NOT_SUPPORTED;
    }
    if (!IsKernelCode(target) || (hooked >= RUNTIME_ORDINALS_FIRST && hooked <= RUNTIME_ORDINALS_LAST))
        return STATUS_NOT_SUPPORTED;
    ULONG extension = ExtensionIndex(Context);
    for (ULONG i = 0; i < state->HookCount; i++) {
        if (state->Hooks[i].Ordinal == hooked && state->Hooks[i].Extension == extension)
            return STATUS_OBJECT_NAME_COLLISION;
    }
    if (state->HookCount == XK_EXTENSION_HOOKS_MAX)
        return STATUS_INSUFFICIENT_RESOURCES;
    state->Hooks[state->HookCount++] = {hooked, extension, Hook, nullptr};
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RegisterEvent(OXOS_CONTEXT *Context, ULONG Event, PVOID Callback, PVOID UserData) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    if (!Event || Event >= OXOS_EVENT_COUNT || !Callback)
        return STATUS_INVALID_PARAMETER;
    if (state->EventCount == XK_EXTENSION_EVENTS_MAX)
        return STATUS_INSUFFICIENT_RESOURCES;
    state->Events[state->EventCount++] = {Event, ExtensionIndex(Context), (OXOS_EVENT_CALLBACK)Callback, UserData};
    return STATUS_SUCCESS;
}

const OXOS_API LOADER_API = {sizeof(OXOS_API), HookFunction, RegisterEvent};

// Removes what extension Index registered (its Load failed).
void DropRegistrations(ULONG Index) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    ULONG kept = 0;
    for (ULONG i = 0; i < state->HookCount; i++) {
        if (state->Hooks[i].Extension != Index)
            state->Hooks[kept++] = state->Hooks[i];
    }
    state->HookCount = kept;
    kept = 0;
    for (ULONG i = 0; i < state->EventCount; i++) {
        if (state->Events[i].Extension != Index)
            state->Events[kept++] = state->Events[i];
    }
    state->EventCount = kept;
}

} // namespace

// --- Loading one extension

namespace {

// An extension's kernel import: linked into the chains once every Load has returned (§7.1).
struct IMPORT_SLOT {
    ULONG Extension;
    ULONG Ordinal;
    ULONG *Slot;
};

// The kernel imports of this launch's extensions.
struct IMPORT_SLOTS {
    ULONG Count;
    ULONG Extension; // the extension being bound
    IMPORT_SLOT Slots[IMPORT_SLOTS_MAX];
};

bool ResolveImport(PVOID Context, ULONG Ordinal, ULONG *Slot) {
    auto slots = (IMPORT_SLOTS *)Context;
    ULONG target = titles::KernelExportAddress(Ordinal);
    if (!target || slots->Count == IMPORT_SLOTS_MAX)
        return false;
    *Slot = target;
    slots->Slots[slots->Count++] = {slots->Extension, Ordinal, Slot};
    return true;
}

// The text as a zero-terminated copy in pool memory (null when there is none).
char *PoolText(const XK_TEXT &Text) {
    auto copy = (char *)ExAllocatePoolWithTag(Text.Length + 1, POOL_TAG);
    if (!copy)
        return nullptr;
    memcpy(copy, Text.Text, Text.Length);
    copy[Text.Length] = 0;
    return copy;
}

// The extension's settings as OXOS_SETTINGs, keys and values copied: those of its own ini (OwnKeys, OwnValues), then those of its section in
// oxos_extensions.ini that its own ini does not set (the older place), at most XK_EXTENSION_SETTINGS_MAX. Null when it has none or memory is short.
OXOS_SETTING *PoolSettings(const XK_EXTENSION_ENTRY &Entry, const XK_TEXT *OwnKeys, const XK_TEXT *OwnValues, ULONG OwnCount, ULONG *Count) {
    *Count = 0;
    if (!OwnCount && !Entry.SettingCount)
        return nullptr;
    auto settings = (OXOS_SETTING *)ExAllocatePoolWithTag(XK_EXTENSION_SETTINGS_MAX * sizeof(OXOS_SETTING), POOL_TAG);
    if (!settings)
        return nullptr;
    for (ULONG i = 0; i < OwnCount; i++)
        settings[(*Count)++] = {PoolText(OwnKeys[i]), PoolText(OwnValues[i])};
    for (ULONG i = 0; i < Entry.SettingCount && *Count < XK_EXTENSION_SETTINGS_MAX; i++) {
        bool own = false;
        for (ULONG j = 0; j < OwnCount && !own; j++)
            own = ini_file::SameText(Entry.Keys[i], OwnKeys[j]);
        if (!own)
            settings[(*Count)++] = {PoolText(Entry.Keys[i]), PoolText(Entry.Values[i])};
    }
    return settings;
}

// C:\OXOS\extensions\NAME.ini for the file NAME.xos (FileName: the value of extension =), in Path.
void BuildOwnINIPath(const XK_TEXT &FileName, char *Path) {
    ULONG stem = FileName.Length;
    if (stem > 4 && strncasecmp(FileName.Text + stem - 4, ".xos", 4) == 0)
        stem -= 4;
    memcpy(Path, FOLDER, sizeof(FOLDER) - 1);
    memcpy(Path + sizeof(FOLDER) - 1, FileName.Text, stem);
    memcpy(Path + sizeof(FOLDER) - 1 + stem, ".ini", 5); // with its terminating 0
}

// The extension's settings and its own NAME.ini's path, into Context (§3.1): the settings at the top of NAME.ini, then those of its section in
// oxos_extensions.ini that NAME.ini does not set. Name is its section's name, for the log.
void LoadSettings(const XK_EXTENSION_ENTRY &Entry, const char *Name, OXOS_CONTEXT &Context) {
    char iniPath[sizeof(FOLDER) + XK_EXTENSION_FILE_NAME_MAX + 4];
    BuildOwnINIPath(Entry.File, iniPath);
    ULONG iniBytes = 0;
    NTSTATUS status;
    char *ini = titles::ReadOXOSFile(iniPath, OWN_INI_MAX_BYTES, POOL_TAG, &iniBytes, &status);
    if (!ini && status != STATUS_OBJECT_NAME_NOT_FOUND)
        XK_SERIAL("extensions: [%s] %s not read (%#lx)\n", Name, iniPath, status);
    XK_TEXT ownKeys[XK_EXTENSION_SETTINGS_MAX];
    XK_TEXT ownValues[XK_EXTENSION_SETTINGS_MAX];
    ULONG ownCount = 0;
    ULONG errors = 0;
    if (ini)
        ownCount = ini_file::ParseOwnSettings(ini, iniBytes, ownKeys, ownValues, XK_EXTENSION_SETTINGS_MAX, &errors);
    if (errors)
        XK_SERIAL("extensions: [%s] %s: %lu settings lines not understood\n", Name, iniPath, errors);
    Context.Settings = PoolSettings(Entry, ownKeys, ownValues, ownCount, &Context.SettingCount);
    if (ini)
        ExFreePool(ini);
    Context.INIPath = PoolText({iniPath, (ULONG)strlen(iniPath)});
}

// The extension's oxos_log lines may go anywhere: its settings say logging_enabled = yes (§3.1). Without it the log target starts empty, and the SDK's
// oxos_log writes nothing afterwards either, whatever oxos_set_log_target asks.
bool IsLoggingEnabled(const OXOS_CONTEXT &Context) {
    for (ULONG i = 0; i < Context.SettingCount; i++) {
        const OXOS_SETTING &setting = Context.Settings[i];
        if (setting.Key && setting.Value && strcasecmp(setting.Key, "logging_enabled") == 0)
            return strcasecmp(setting.Value, "yes") == 0;
    }
    return false;
}

} // namespace

// Loads the extension Entry names as LoadedExtensions->Extensions[Count] and calls its Load; true when it stays. *ImagesBytes counts the loaded images' sizes
// against IMAGES_MAX_BYTES.
static bool LoadExtension(const XK_EXTENSION_ENTRY &Entry, IMPORT_SLOTS *Slots, ULONG *ImagesBytes) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    char *section = PoolText(Entry.Section);
    const char *name = section ? section : "?";
    char path[sizeof(FOLDER) + XK_EXTENSION_FILE_NAME_MAX];
    memcpy(path, FOLDER, sizeof(FOLDER) - 1);
    memcpy(path + sizeof(FOLDER) - 1, Entry.File.Text, Entry.File.Length);
    path[sizeof(FOLDER) - 1 + Entry.File.Length] = 0;

    ULONG size;
    NTSTATUS status;
    auto file = (PUCHAR)titles::ReadOXOSFile(path, FILE_MAX_BYTES, POOL_TAG, &size, &status);
    if (!file) {
        XK_SERIAL("extensions: [%s] %s not read (%#lx)\n", name, path, status);
        return false;
    }
    ULONG imageSize = 0;
    XK_PE_ERROR error = CheckImage(file, size, &imageSize);
    if (error == XK_PE_ERROR::NONE && *ImagesBytes + imageSize > IMAGES_MAX_BYTES) {
        XK_SERIAL("extensions: [%s] skipped: the loaded images would pass %lu KB\n", name, IMAGES_MAX_BYTES / 1024);
        ExFreePool(file);
        return false;
    }
    auto image = error == XK_PE_ERROR::NONE ? (PUCHAR)MmAllocateContiguousMemory(imageSize) : nullptr;
    if (image)
        error = MapImage(file, size, image, imageSize, (ULONG)image);
    ExFreePool(file);
    Slots->Extension = state->Count;
    ULONG firstSlot = Slots->Count;
    if (image && error == XK_PE_ERROR::NONE)
        error = BindImports(image, imageSize, ResolveImport, Slots);
    bool bound = image && error == XK_PE_ERROR::NONE;
    ULONG descriptorRva = bound ? FindExport(image, imageSize, OXOS_EXTENSION_EXPORT_NAME) : 0;
    auto descriptor = descriptorRva ? (const OXOS_EXTENSION *)(image + descriptorRva) : nullptr;
    const char *refused = error != XK_PE_ERROR::NONE ? "not a valid .xos image"
                          : !image                  ? "no memory for its image"
                          : !descriptor             ? "no oxos_extension descriptor"
                          : descriptor->Magic != OXOS_EXTENSION_MAGIC || !descriptor->Start || !InImage(descriptor->Context, image, imageSize)
                              ? "a damaged descriptor"
                          : (descriptor->ApiVersion >> 16) != (OXOS_API_VERSION >> 16) ||
                                  (descriptor->ApiVersion & 0xFFFF) > (OXOS_API_VERSION & 0xFFFF) ? "built for another kernel API version" : nullptr;
    if (refused) {
        XK_SERIAL("extensions: [%s] %s refused: %s (%lu)\n", name, path, refused, (ULONG)error);
        Slots->Count = firstSlot;
        if (image)
            MmFreeContiguousMemory(image);
        return false;
    }
    // Every extension has its state page, so oxos_state never returns null.
    PVOID statePage = StatePageFor(descriptor->Name ? descriptor->Name : name, descriptor->Version ? descriptor->Version : "");
    if (!statePage) {
        XK_SERIAL("extensions: [%s] skipped: no memory for its state page\n", name);
        Slots->Count = firstSlot;
        MmFreeContiguousMemory(image);
        return false;
    }

    XK_EXTENSION &extension = state->Extensions[state->Count];
    extension.Descriptor = descriptor;
    OXOS_CONTEXT &context = extension.Context;
    context.Size = sizeof(OXOS_CONTEXT);
    context.ApiVersion = OXOS_API_VERSION;
    context.KernelVersion = KernelVersionText;
    context.Generation = KiQuickRebootCount + 1;
    context.BootFlags = KiQuickRebootFlag ? OXOS_BOOT_QUICK : OXOS_BOOT_COLD;
    context.SectionName = name;
    LoadSettings(Entry, name, context);
    context.ImageBase = image;
    context.ImageSize = imageSize;
    context.State = statePage;
    context.Api = &LOADER_API;
    context.Kernel = &state->Kernel;
    context.LogTarget = IsLoggingEnabled(context) ? OXOS_LOG_SERIAL | OXOS_LOG_FILE : 0;

    *descriptor->Context = &context; // the SDK's oxos_context: its functions read this launch's context from there
    if (!descriptor->Start()) {
        XK_SERIAL("extensions: [%s] Start returned FALSE: unloaded\n", name);
        DropRegistrations(state->Count);
        ReleaseStatePage(context.State);
        Slots->Count = firstSlot;
        MmFreeContiguousMemory(image);
        return false;
    }
    *ImagesBytes += imageSize;
    state->Count++;
    XK_SERIAL("extensions: [%s] %s %s at %p, %lu KB\n", name, descriptor->Name ? descriptor->Name : "?", descriptor->Version ? descriptor->Version : "?", image,
              imageSize / 1024);
    return true;
}

// --- After every Load (§4.3 step 3)

// Links the hook chains: each hook's Next is the next extension's hook of its ordinal, else the kernel's function; each extension's own import of a hooked
// ordinal goes to the first hook of an extension loaded after it.
static void LinkChains(const IMPORT_SLOTS *Slots) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    for (ULONG i = 0; i < state->HookCount; i++) {
        XK_EXTENSION_HOOK &hook = state->Hooks[i];
        ULONG next = titles::KernelExportAddress(hook.Ordinal);
        for (ULONG j = i + 1; j < state->HookCount; j++) {
            if (state->Hooks[j].Ordinal == hook.Ordinal) {
                next = (ULONG)state->Hooks[j].Replacement;
                break;
            }
        }
        if (hook.Next)
            *hook.Next = (PVOID)next;
    }
    for (ULONG s = 0; s < Slots->Count; s++) {
        const IMPORT_SLOT &slot = Slots->Slots[s];
        for (ULONG i = 0; i < state->HookCount; i++) {
            const XK_EXTENSION_HOOK &hook = state->Hooks[i];
            if (hook.Ordinal == slot.Ordinal && hook.Extension > slot.Extension) {
                *slot.Slot = (ULONG)hook.Replacement;
                break;
            }
        }
    }
}

// --- Loading the list

extern "C" void XkExtensionLoaderEntry() {
    ULONG got;
    NTSTATUS status;
    char *text = titles::ReadOXOSFile(LIST_PATH, LIST_MAX_BYTES, POOL_TAG, &got, &status);
    if (!text) {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
            XK_SERIAL("extensions: oxos_extensions.ini not read (%#lx)\n", status);
        // No extension at this launch: the state pages of the last one are freed (§6).
        CheckStateIndex();
        TidyStateIndex();
        return;
    }
    ULONG entriesBytes = XK_EXTENSIONS_MAX * sizeof(XK_EXTENSION_ENTRY);
    auto entries = (XK_EXTENSION_ENTRY *)ExAllocatePoolWithTag(entriesBytes, POOL_TAG);
    auto slots = (IMPORT_SLOTS *)ExAllocatePoolWithTag(sizeof(IMPORT_SLOTS), POOL_TAG);
    LoadedExtensions = (XK_EXTENSIONS_STATE *)ExAllocatePoolWithTag(sizeof(XK_EXTENSIONS_STATE), POOL_TAG);
    if (entries && slots && LoadedExtensions) {
        memset(LoadedExtensions, 0, sizeof(XK_EXTENSIONS_STATE));
        KeInitializeEvent(&LoadedExtensions->LoopStop, NotificationEvent, FALSE);
        LoadedExtensions->LoopExtension = XK_NO_EXTENSION;
        LoadedExtensions->Kernel.Size = sizeof(OXOS_KERNEL_INFO);
        LoadedExtensions->Kernel.MemoryPages = memory::HighestPhysicalPage + 1;
        slots->Count = 0;
        ULONG errors;
        ULONG count = ini_file::ParseExtensions(text, got, entries, XK_EXTENSIONS_MAX, &errors);
        if (errors)
            XK_SERIAL("extensions: oxos_extensions.ini: %lu lines not understood\n", errors);
        CheckStateIndex();
        ULONG imagesBytes = 0;
        for (ULONG i = 0; i < count; i++) {
            if (entries[i].Enabled)
                LoadExtension(entries[i], slots, &imagesBytes);
        }
        TidyStateIndex();
        LinkChains(slots);
    }
    ExFreePool(text);
    if (entries)
        ExFreePool(entries);
    if (slots)
        ExFreePool(slots);
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    if (state && !state->Count) {
        ExFreePool(state);
        LoadedExtensions = nullptr;
        return;
    }
    if (!state)
        return;
    // The loader's functions end with it; then the run loop starts, when an extension has a RunLoop: one thread for all of them (§4.3).
    bool loop = false;
    for (ULONG i = 0; i < state->Count; i++) {
        state->Extensions[i].Context.Api = nullptr;
        loop |= state->Extensions[i].Descriptor->RunLoop != nullptr;
    }
    if (loop && !NT_SUCCESS(PsCreateSystemThreadEx(&state->LoopThread, 0, LOOP_STACK_BYTES, 0, nullptr, nullptr, nullptr, FALSE, FALSE, RunLoopThread))) {
        state->LoopThread = nullptr;
        XK_SERIAL("extensions: the run loop did not start\n");
    }
}

} // namespace xk::extensions
