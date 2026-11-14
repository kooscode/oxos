// The kernel extension loader (spec/oxos_internal/34 §3-§8): before each title, reads C:\OXOS\oxos_extensions.ini, loads each listed .xos from
// C:\OXOS\extensions\, gives it its state page and calls its Load, then links the hook chains and starts the run loop thread. Not part of the resident kernel:
// linked on its own at XK_EXTENSION_LOADER_BASE against the kernel's addresses, and run there for each launch by extensions/extension_runtime_loader.cpp
// (§8.1). What runs while the title runs is in extensions/extension_runtime.cpp.
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
constexpr ULONG LIST_MAX_BYTES = 0x10000;    // the list file
constexpr ULONG FILE_MAX_BYTES = 0x40000;    // one extension's file
constexpr ULONG IMAGES_MAX_BYTES = 0x80000;  // all loaded images together
constexpr ULONG LOOP_STACK_BYTES = 0x4000;   // the run loop thread's kernel stack
constexpr ULONG POOL_TAG = 'txEX';
constexpr ULONG IMPORT_SLOTS_MAX = 1024;     // kernel imports of all extensions together
constexpr NTSTATUS STATUS_NOT_SUPPORTED = (NTSTATUS)0xC00000BB;
constexpr ULONG EXPORT_ORDINAL_MAX = 512; // above the kernel's highest export (kernel/exports.tsv)

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

// A, as kept in the index (at most Max characters), equals B (compared as far as the index keeps it).
bool SameText(const char *A, const char *B, ULONG Max) {
    ULONG i = 0;
    for (; i < Max && A[i] && A[i] == B[i]; i++) {
        // Compare up to the first difference or the end.
    }
    return i == Max || A[i] == B[i];
}

// Copies Text into an index field of Max characters and its 0.
void CopyText(char *Field, const char *Text, ULONG Max) {
    ULONG n = 0;
    for (; n < Max && Text[n]; n++)
        Field[n] = Text[n];
    Field[n] = 0;
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
        if (SameText(e.Name, Name, STATE_NAME_MAX) && !e.Used) {
            if (!SameText(e.Version, Version, STATE_VERSION_MAX)) {
                RtlZeroMemory(e.Page, PAGE_SIZE);
                CopyText(e.Version, Version, STATE_VERSION_MAX);
            }
            e.Used = TRUE;
            return e.Page;
        }
    }
    if (!index) {
        index = (STATE_INDEX *)MmAllocateContiguousMemory(PAGE_SIZE);
        if (!index)
            return nullptr;
        RtlZeroMemory(index, PAGE_SIZE);
        index->Magic = STATE_INDEX_MAGIC;
        MmPersistContiguousMemory(index, PAGE_SIZE, TRUE);
        StateIndex() = index;
    }
    if (index->Count == STATE_INDEX_ENTRIES)
        return nullptr;
    PVOID page = MmAllocateContiguousMemory(PAGE_SIZE);
    if (!page)
        return nullptr;
    RtlZeroMemory(page, PAGE_SIZE);
    MmPersistContiguousMemory(page, PAGE_SIZE, TRUE);
    STATE_INDEX_ENTRY &e = index->Entries[index->Count++];
    CopyText(e.Name, Name, STATE_NAME_MAX);
    CopyText(e.Version, Version, STATE_VERSION_MAX);
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
// OXOS's runtime exports for extensions (memcpy, ..., the compiler's 64-bit helpers): titles never import them.
constexpr ULONG RUNTIME_ORDINALS_FIRST = 402, RUNTIME_ORDINALS_LAST = 415;

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
    RtlCopyMemory(copy, Text.Text, Text.Length);
    copy[Text.Length] = 0;
    return copy;
}

// The settings of Entry as OXOS_SETTINGs, keys and values copied (null when it has none or memory is short).
OXOS_SETTING *PoolSettings(const XK_EXTENSION_ENTRY &Entry) {
    if (!Entry.SettingCount)
        return nullptr;
    auto settings = (OXOS_SETTING *)ExAllocatePoolWithTag(Entry.SettingCount * sizeof(OXOS_SETTING), POOL_TAG);
    for (ULONG i = 0; settings && i < Entry.SettingCount; i++)
        settings[i] = {PoolText(Entry.Keys[i]), PoolText(Entry.Values[i])};
    return settings;
}

} // namespace

// Loads the extension Entry names as LoadedExtensions->Extensions[Count] and calls its Load; true when it stays. *ImagesBytes counts the loaded images' sizes
// against IMAGES_MAX_BYTES.
static bool LoadExtension(const XK_EXTENSION_ENTRY &Entry, IMPORT_SLOTS *Slots, ULONG *ImagesBytes) {
    XK_EXTENSIONS_STATE *state = LoadedExtensions;
    char *section = PoolText(Entry.Section);
    const char *name = section ? section : "?";
    char path[sizeof(FOLDER) + XK_EXTENSION_FILE_NAME_MAX];
    RtlCopyMemory(path, FOLDER, sizeof(FOLDER) - 1);
    RtlCopyMemory(path + sizeof(FOLDER) - 1, Entry.File.Text, Entry.File.Length);
    path[sizeof(FOLDER) - 1 + Entry.File.Length] = 0;

    ULONG size;
    NTSTATUS status;
    auto file = (PUCHAR)titles::ReadOxosFile(path, FILE_MAX_BYTES, POOL_TAG, &size, &status);
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
    context.Settings = PoolSettings(Entry);
    context.SettingCount = context.Settings ? Entry.SettingCount : 0;
    context.ImageBase = image;
    context.ImageSize = imageSize;
    context.State = statePage;
    context.Api = &LOADER_API;
    context.Kernel = &state->Kernel;
    context.LogTarget = OXOS_LOG_SERIAL | OXOS_LOG_FILE;

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
    char *text = titles::ReadOxosFile(LIST_PATH, LIST_MAX_BYTES, POOL_TAG, &got, &status);
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
        RtlZeroMemory(LoadedExtensions, sizeof(XK_EXTENSIONS_STATE));
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
