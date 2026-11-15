// SPDX-License-Identifier: MIT
// The OXOS kernel extension SDK's functions (oxos_sdk.h), in lib/oxos_sdk.lib next to the kernel's import library. C names, so C and C++ extensions link the
// same code. They read this launch's context from oxos_context, which the kernel fills through the descriptor before ExtensionStart.
#include <oxos_sdk.h>
#include "oxos_extension_context.h"

// OXOS's own export 416 (the kernel's extension runtime): one line of an extension's log, to where Context->LogTarget says.
extern "C" OXOS_IMPORT void NTAPI OXOSWriteExtensionLog(const OXOS_CONTEXT *Context, PCSZ Text);
// Export 432, the kernel's io::RenameFile: renames an open file or folder (oxos_file_rename and oxos_directory_rename are built on it).
extern "C" NTSTATUS oxos_file_rename_handle(HANDLE File, const char *NewName, BOOLEAN Replace);

extern "C" {

OXOS_CONTEXT *oxos_context;

// --- Logging

void oxos_log(const char *Format, ...) {
    char text[224];
    va_list args;
    va_start(args, Format);
    vsnprintf(text, sizeof(text), Format, args);
    va_end(args);
    OXOSWriteExtensionLog(oxos_context, text); // the kernel adds "[name] " and writes it where oxos_context->LogTarget says
}

void oxos_set_log_target(ULONG Target) {
    oxos_context->LogTarget = Target;
}

// --- Settings, the launch, the state page

const char *oxos_setting(const char *Key) {
    for (ULONG i = 0; i < oxos_context->SettingCount; i++) {
        const char *key = oxos_context->Settings[i].Key;
        if (key && strcasecmp(key, Key) == 0)
            return oxos_context->Settings[i].Value;
    }
    return nullptr;
}

ULONG oxos_setting_number(const char *Key, ULONG Default) {
    const char *text = oxos_setting(Key);
    if (!text || !isdigit(*text))
        return Default;
    return strtoul(text, nullptr, 10);
}

ULONG oxos_launch_number(void) {
    return oxos_context->Generation;
}

const char *oxos_kernel_version(void) {
    return oxos_context->KernelVersion;
}

void *oxos_state(void) {
    return oxos_context->State;
}

// --- Files and folders (oxos_sdk.h): the kernel's own io:: functions (exports 429-432), the rest built on them and on NtQueryDirectoryFile

namespace {

constexpr ULONG FILE_DIRECTORY_INFORMATION_CLASS = 1; // FileDirectoryInformation
constexpr ULONG FILE_ATTRIBUTE_DIRECTORY_FLAG = 0x10;
constexpr NTSTATUS STATUS_NO_SUCH_FILE_CODE = static_cast<NTSTATUS>(0xC000000F), STATUS_NO_MORE_FILES_CODE = static_cast<NTSTATUS>(0x80000006),
                   STATUS_BUFFER_TOO_SMALL_CODE = static_cast<NTSTATUS>(0xC0000023), STATUS_INSUFFICIENT_RESOURCES_CODE = static_cast<NTSTATUS>(0xC000009A);
constexpr ULONG SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Renames the open File (the kernel's io::RenameFile, export 432).
NTSTATUS RenameOpen(const char *Path, const char *NewName, ULONG Options) {
    HANDLE file;
    NTSTATUS status = oxos_file_open(Path, DELETE, SHARE_ALL, Options, &file);
    if (!NT_SUCCESS(status))
        return status;
    status = oxos_file_rename_handle(file, NewName, FALSE);
    NtClose(file);
    return status;
}

BOOLEAN Exists(const char *Path, ULONG Options) {
    HANDLE file;
    if (!NT_SUCCESS(oxos_file_open(Path, FILE_READ_ATTRIBUTES, SHARE_ALL, Options, &file)))
        return FALSE;
    NtClose(file);
    return TRUE;
}

// Calls Visit for each entry of Folder whose kind is Folders (folders, or files), with its name, the name's length and Context.
NTSTATUS ForEachEntry(const char *Folder, bool Folders, void (*Visit)(const char *Name, ULONG Length, void *Context), void *Context) {
    HANDLE folder;
    NTSTATUS status = oxos_file_open(Folder, GENERIC_READ, SHARE_ALL, FILE_DIRECTORY_FILE, &folder);
    if (!NT_SUCCESS(status))
        return status;
    UCHAR buffer[2048];
    for (BOOLEAN first = TRUE;; first = FALSE) {
        IO_STATUS_BLOCK iosb;
        status = NtQueryDirectoryFile(folder, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer), FILE_DIRECTORY_INFORMATION_CLASS, nullptr, first);
        if (!NT_SUCCESS(status))
            break;
        // FILE_DIRECTORY_INFORMATION: the next entry's offset at 0 (0 for the last), the attributes at 0x38, the name's length at 0x3C and the name at 0x40.
        for (PUCHAR e = buffer;; e += *reinterpret_cast<ULONG *>(e)) {
            bool isFolder = (*reinterpret_cast<ULONG *>(e + 0x38) & FILE_ATTRIBUTE_DIRECTORY_FLAG) != 0;
            if (isFolder == Folders)
                Visit(reinterpret_cast<const char *>(e + 0x40), *reinterpret_cast<ULONG *>(e + 0x3C), Context);
            if (!*reinterpret_cast<ULONG *>(e))
                break;
        }
    }
    NtClose(folder);
    // The end of the list: NT's answer, or the first query's in an empty folder.
    return status == STATUS_NO_MORE_FILES_CODE || status == STATUS_NO_SUCH_FILE_CODE ? STATUS_SUCCESS : status;
}

// The names, as ListNames gathers them: first counted (Count, Bytes), then copied into Names, which has room for Capacity of them and Bytes of text.
struct NAME_LIST {
    uint32_t Count, Capacity;
    ULONG Bytes, Room;
    char **Names;
    char *Text;
};

void CountName(const char *, ULONG Length, void *Context) {
    auto list = static_cast<NAME_LIST *>(Context);
    list->Count++;
    list->Bytes += Length + 1;
}

void CopyName(const char *Name, ULONG Length, void *Context) {
    auto list = static_cast<NAME_LIST *>(Context);
    if (list->Count == list->Capacity || Length + 1 > list->Room) // the folder grew between the two passes: the names that fit
        return;
    memcpy(list->Text, Name, Length);
    list->Text[Length] = 0;
    list->Names[list->Count++] = list->Text;
    list->Text += Length + 1;
    list->Room -= Length + 1;
}

// The names of Folder's files or folders, in one block of pool memory: the array of pointers (and a null after them), then the names it points to.
NTSTATUS ListNames(const char *Folder, bool Folders, char **&Names, uint32_t &Count) {
    Names = nullptr;
    Count = 0;
    NAME_LIST list = {};
    NTSTATUS status = ForEachEntry(Folder, Folders, CountName, &list);
    if (!NT_SUCCESS(status))
        return status;
    ULONG arrayBytes = (list.Count + 1) * sizeof(char *);
    auto block = static_cast<char **>(ExAllocatePoolWithTag(arrayBytes + list.Bytes, 0x7473696C)); // "list"
    if (!block)
        return STATUS_INSUFFICIENT_RESOURCES_CODE;
    list = {0, list.Count, list.Bytes, list.Bytes, block, reinterpret_cast<char *>(block) + arrayBytes};
    status = ForEachEntry(Folder, Folders, CopyName, &list);
    if (!NT_SUCCESS(status)) {
        ExFreePool(block);
        return status;
    }
    block[list.Count] = nullptr;
    Names = block;
    Count = list.Count;
    return STATUS_SUCCESS;
}

} // namespace

NTSTATUS oxos_file_read(const char *Path, void *Buffer, ULONG BufferSize, ULONG *Size) {
    *Size = 0;
    HANDLE file;
    NTSTATUS status = oxos_file_open(Path, GENERIC_READ, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE, &file);
    if (!NT_SUCCESS(status))
        return status;
    IO_STATUS_BLOCK iosb;
    ULONG info[6] = {}; // FILE_STANDARD_INFORMATION: EndOfFile at +8 (the high half at +12)
    status = NtQueryInformationFile(file, &iosb, info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status) && (info[3] || info[2] > BufferSize)) {
        *Size = info[3] ? ~0UL : info[2];
        status = STATUS_BUFFER_TOO_SMALL_CODE;
    }
    if (NT_SUCCESS(status) && info[2]) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        status = NtReadFile(file, nullptr, nullptr, nullptr, &iosb, Buffer, info[2], &at);
        if (NT_SUCCESS(status))
            *Size = iosb.Information;
    }
    NtClose(file);
    return status;
}

NTSTATUS oxos_file_write(const char *Path, const void *Data, ULONG Size) {
    HANDLE file;
    NTSTATUS status = oxos_file_create(Path, GENERIC_WRITE, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &file, Size);
    if (!NT_SUCCESS(status))
        return status;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = 0;
    if (Size)
        status = NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, const_cast<void *>(Data), Size, &at);
    NtClose(file);
    return status;
}

NTSTATUS oxos_file_rename(const char *Path, const char *NewName) {
    return RenameOpen(Path, NewName, FILE_NON_DIRECTORY_FILE);
}

BOOLEAN oxos_file_exists(const char *Path) {
    return Exists(Path, FILE_NON_DIRECTORY_FILE);
}

NTSTATUS oxos_file_list(const char *Folder, char **&Names, uint32_t &Count) {
    return ListNames(Folder, false, Names, Count);
}

NTSTATUS oxos_directory_create(const char *Path) {
    HANDLE folder;
    NTSTATUS status = oxos_file_create(Path, GENERIC_READ, SHARE_ALL, FILE_CREATE, FILE_DIRECTORY_FILE, &folder, 0);
    if (NT_SUCCESS(status))
        NtClose(folder);
    return status;
}

NTSTATUS oxos_directory_delete(const char *Path) {
    return oxos_file_delete(Path);
}

NTSTATUS oxos_directory_rename(const char *Path, const char *NewName) {
    return RenameOpen(Path, NewName, FILE_DIRECTORY_FILE);
}

BOOLEAN oxos_directory_exists(const char *Path) {
    return Exists(Path, FILE_DIRECTORY_FILE);
}

NTSTATUS oxos_directory_list(const char *Folder, char **&Names, uint32_t &Count) {
    return ListNames(Folder, true, Names, Count);
}

void oxos_list_free(char **Names) {
    if (Names)
        ExFreePool(Names);
}

// --- Hooks: Function is the extension's import of the kernel function, which during Start still holds the kernel's own address; the kernel finds the
// function by it

BOOLEAN oxos_hook_function(PVOID Function, PVOID Hook) {
    return oxos_context->Api && NT_SUCCESS(oxos_context->Api->HookFunction(oxos_context, Function, Hook));
}

// --- Events

static BOOLEAN RegisterEvent(ULONG Event, PVOID Callback, void *UserData) {
    return oxos_context->Api && NT_SUCCESS(oxos_context->Api->RegisterEvent(oxos_context, Event, Callback, UserData));
}

BOOLEAN oxos_on_title_loading(void (*Callback)(const OXOS_TITLE_LOADING *Title, void *UserData), void *UserData) {
    return RegisterEvent(OXOS_EVENT_TITLE_LOADING, reinterpret_cast<PVOID>(Callback), UserData);
}

BOOLEAN oxos_on_title_loaded(void (*Callback)(const OXOS_TITLE_LOADED *Title, void *UserData), void *UserData) {
    return RegisterEvent(OXOS_EVENT_TITLE_LOADED, reinterpret_cast<PVOID>(Callback), UserData);
}

BOOLEAN oxos_on_title_exiting(void (*Callback)(const OXOS_TITLE_EXITING *Title, void *UserData), void *UserData) {
    return RegisterEvent(OXOS_EVENT_TITLE_EXITING, reinterpret_cast<PVOID>(Callback), UserData);
}

} // extern "C"
