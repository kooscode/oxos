// SPDX-License-Identifier: MIT
// The OXOS kernel extension SDK's functions (oxos_sdk.h), in lib/oxos_sdk.lib next to the kernel's import library. C names, so C and C++ extensions link the
// same code. They read this launch's context from oxos_context, which the kernel fills through the descriptor before ExtensionStart.
#include <oxos_sdk.h>
#include "oxos_extension_context.h"

// OXOS's own export 416 (the kernel's extension runtime): one line of an extension's log, to where Context->LogTarget says.
extern "C" OXOS_IMPORT void NTAPI OXOSWriteExtensionLog(const OXOS_CONTEXT *Context, PCSZ Text);

extern "C" {

OXOS_CONTEXT *oxos_context;

// --- Logging

void oxos_log(const char *Format, ...) {
    char text[224];
    va_list args;
    va_start(args, Format);
    RtlVsnprintf(text, sizeof(text), Format, args);
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
        ULONG n = 0;
        while (key && key[n] && Key[n] && (key[n] | 0x20) == (Key[n] | 0x20))
            n++;
        if (key && !key[n] && !Key[n])
            return oxos_context->Settings[i].Value;
    }
    return nullptr;
}

ULONG oxos_setting_number(const char *Key, ULONG Default) {
    const char *digit = oxos_setting(Key);
    if (!digit || *digit < '0' || *digit > '9')
        return Default;
    ULONG value = 0;
    for (; *digit >= '0' && *digit <= '9'; digit++)
        value = value * 10 + static_cast<ULONG>(*digit - '0');
    return value;
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

// --- Files

char *oxos_read_file(const char *Path, ULONG MaxSize, ULONG *Size) {
    *Size = 0;
    OBJECT_STRING name;
    RtlInitAnsiString(&name, Path);
    OBJECT_ATTRIBUTES attributes = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    if (!NT_SUCCESS(NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
        return nullptr;
    ULONG info[6] = {}; // FILE_STANDARD_INFORMATION: EndOfFile at +8
    char *text = nullptr;
    if (NT_SUCCESS(NtQueryInformationFile(file, &iosb, info, sizeof(info), FileStandardInformation)) && !info[3] && info[2] && info[2] <= MaxSize)
        text = static_cast<char *>(ExAllocatePoolWithTag(info[2], 0x6472786F)); // "oxrd"
    if (text) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        if (NT_SUCCESS(NtReadFile(file, nullptr, nullptr, nullptr, &iosb, text, info[2], &at))) {
            *Size = iosb.Information;
        } else {
            ExFreePool(text);
            text = nullptr;
        }
    }
    NtClose(file);
    return text;
}

void oxos_free_file(char *Text) {
    if (Text)
        ExFreePool(Text);
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
