// SPDX-License-Identifier: MIT
// example_toast.xos: a toast over the running title (spec/oxos_internal/36). When the title opens or creates a file whose name ends with one listed in
// [files] of C:\OXOS\extensions\example_toast.ini ("Saving = savegame.dat"), a toast says so at the top right for 3 seconds: "Saving: savegame.dat".
// It shows a hook, data read once in ExtensionStart, and oxos_toast_enqueue from a hook (it never waits, so the title's thread is not held up).
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "example_toast", .Version = "1.0.0", .Start = ExtensionStart);

#define FILES_MAX 8      // lines of [files] kept
#define TEXT_MAX 42      // a label and a file name together (one line of a toast, OXOS_TOAST_LINE_MAX)
#define REPEAT_MS 5000   // the same file again within 5 seconds: no second toast

// One line of [files]: the toast's text ("Saving: savegame.dat") and the end of the file names it is for.
typedef struct WATCHED_FILE {
    char Text[TEXT_MAX + 1];
    const char *Ending; // in Text, after ": "
    ULONG EndingLength;
    volatile BOOLEAN Toasted; // a toast for it has been queued
    volatile ULONG LastToast; // KeTickCount of the last one
} WATCHED_FILE;

static WATCHED_FILE Files[FILES_MAX];
static ULONG FileCount;

// The title opens or creates Name: a toast when it is a watched file. Called in the title's thread, from the hooks.
static void CheckFile(POBJECT_ATTRIBUTES Name) {
    if (!Name || !Name->ObjectName)
        return;
    const char *name = Name->ObjectName->Buffer;
    ULONG length = Name->ObjectName->Length;
    for (ULONG i = 0; i < FileCount; i++) {
        WATCHED_FILE &file = Files[i];
        if (length < file.EndingLength || strncasecmp(name + length - file.EndingLength, file.Ending, file.EndingLength) != 0)
            continue;
        ULONG now = KeTickCount;
        if (file.Toasted && now - file.LastToast < REPEAT_MS)
            return;
        file.Toasted = TRUE;
        file.LastToast = now;
        OXOS_TOAST toast = {file.Text, OXOS_TOAST_TOP_RIGHT, 3, 0};
        oxos_toast_enqueue(toast); // STATUS_DEVICE_BUSY (the queue is full): this toast is skipped
        return;
    }
}

static NTSTATUS NTAPI My_NtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    CheckFile(Name);
    return oxos_next(NtOpenFile)(File, Access, Name, Iosb, Share, Options);
}

static NTSTATUS NTAPI My_NtCreateFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, PLARGE_INTEGER Size,
                                      ULONG Attributes, ULONG Share, ULONG Disposition, ULONG Options) {
    CheckFile(Name);
    return oxos_next(NtCreateFile)(File, Access, Name, Iosb, Size, Attributes, Share, Disposition, Options);
}

// Reads [files] (data: from the disk, so here and never in a hook) into Files.
static void ReadFiles() {
    OXOS_INI_KEY *keys;
    uint32_t count;
    if (!NT_SUCCESS(oxos_ini_section("files", keys, count)))
        return;
    for (uint32_t i = 0; i < count && FileCount < FILES_MAX; i++) {
        WATCHED_FILE &file = Files[FileCount];
        int length = snprintf(file.Text, sizeof(file.Text), "%s: %s", keys[i].Key, keys[i].Value);
        if (length < 0 || length > TEXT_MAX || !keys[i].Value[0]) {
            oxos_log("[files] %s: too long for a toast, left out.", keys[i].Key);
            continue;
        }
        file.Ending = file.Text + strlen(keys[i].Key) + 2;
        file.EndingLength = strlen(file.Ending);
        FileCount++;
    }
    oxos_ini_free(keys);
}

static BOOLEAN ExtensionStart(void) {
    ReadFiles();
    oxos_log("Watching %lu files.", FileCount);
    return FileCount && oxos_hook(NtOpenFile, My_NtOpenFile) && oxos_hook(NtCreateFile, My_NtCreateFile);
}
