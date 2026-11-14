// SPDX-License-Identifier: MIT
// OXOS kernel extension SDK runtime (part of lib/oxos_sdk.lib): the helpers of oxos_sdk.h, built on the
// kernel's exports. Its functions have C names, so C and C++ extensions link the same library. memcpy and the other
// functions the compiler calls come from the kernel (OXOS's own exports 402-415), not from here.
#include <oxos_sdk.h>

extern "C" void oxos_log(const OXOS_CONTEXT *Context, const char *Format, ...) {
    char line[256];
    va_list args;
    va_start(args, Format);
    oxos::RtlVsnprintf(line, sizeof(line), Format, args);
    va_end(args);
    oxos::DbgPrint("[%s] %s\n", Context->SectionName, line);
}

extern "C" const char *oxos_get_setting(const OXOS_CONTEXT *Context, const char *Key) {
    for (ULONG i = 0; i < Context->SettingCount; i++) {
        const char *k = Context->Settings[i].Key;
        ULONG n = 0;
        while (k && k[n] && Key[n] && (k[n] | 0x20) == (Key[n] | 0x20))
            n++;
        if (k && !k[n] && !Key[n])
            return Context->Settings[i].Value;
    }
    return nullptr;
}

extern "C" BOOLEAN oxos_wait_for_stop(const OXOS_CONTEXT *Context, ULONG Milliseconds) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10000LL * Milliseconds;
    return oxos::KeWaitForSingleObject(Context->StopEvent, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS;
}

extern "C" char *oxos_read_file(const char *Path, ULONG MaxSize, ULONG *Size) {
    *Size = 0;
    OBJECT_STRING name;
    oxos::RtlInitAnsiString(&name, Path);
    OBJECT_ATTRIBUTES attributes = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    if (!NT_SUCCESS(oxos::NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ,
                                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
        return nullptr;
    ULONG info[6] = {}; // FILE_STANDARD_INFORMATION: EndOfFile at +8
    char *text = nullptr;
    if (NT_SUCCESS(oxos::NtQueryInformationFile(file, &iosb, info, sizeof(info), FileStandardInformation)) &&
        !info[3] && info[2] && info[2] <= MaxSize) {
        text = static_cast<char *>(oxos::ExAllocatePoolWithTag(info[2], 'drxo'));
    }
    if (text) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        if (NT_SUCCESS(oxos::NtReadFile(file, nullptr, nullptr, nullptr, &iosb, text, info[2], &at))) {
            *Size = iosb.Information;
        } else {
            oxos::ExFreePool(text);
            text = nullptr;
        }
    }
    oxos::NtClose(file);
    return text;
}
