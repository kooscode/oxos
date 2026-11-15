// SPDX-License-Identifier: MIT
// example_kernel_hook.xos: a hook on a kernel function, and a setting the hook itself uses. It counts the files a title opens (only those whose name ends
// with its "ending" setting, from C:\OXOS\extensions\example_kernel_hook.ini, when it is set), by sending the title's NtOpenFile calls to its own function
// first, and logs the count when the title ends. Note this Example has no "RunLoop" function, so it only reacts when the Title calls NtOpenFile and
// when the title is exiting:
//
//   [example_kernel_hook] Starting extension example_kernel_hook version 1.0.0.
//   [example_kernel_hook] NtOpenFile called 85 times.
//   [example_kernel_hook] Terminating extension example_kernel_hook version 1.0.0.
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void);
static void ExtensionTerminate(void);

#define EXTENSION_NAME "example_kernel_hook"
#define EXTENSION_VERSION "1.0.0"

// Define the extension's descriptor, which the kernel reads. It names the extension and its functions, and the kernel calls ExtensionStart before each title.
OXOS_DEFINE_EXTENSION(.Name = EXTENSION_NAME, .Version = EXTENSION_VERSION, .Start = ExtensionStart, .Terminate = ExtensionTerminate);

// Set at each launch (the kernel loads the extension afresh before every title, so it starts at 0 each time).
static LONG Opens;

// The "ending" setting, or null when it is not set (count every file). A setting is in memory from the moment the extension loads, so the hook may read it
// at every call; data (oxos_ini_section) never: it reads the disk, which a hook must not.
static const char *Ending;
static ULONG EndingLength;

// The name the title opens ends with Ending (any case), or no ending is set.
static BOOLEAN NameIsCounted(POBJECT_ATTRIBUTES Name) {
    if (!Ending)
        return TRUE;
    if (!Name || !Name->ObjectName || Name->ObjectName->Length < EndingLength)
        return FALSE;
    const char *nameEnd = Name->ObjectName->Buffer + Name->ObjectName->Length;
    return strncasecmp(nameEnd - EndingLength, Ending, EndingLength) == 0;
}

// The hook: the title's NtOpenFile calls come here first. It has exactly NtOpenFile's arguments, result and calling convention (oxos_kernel.h).
static NTSTATUS NTAPI My_NtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    // Increment our counter for every file the title opens that we count (InterlockedIncrement: several of its threads may call at once).
    if (NameIsCounted(Name))
        InterlockedIncrement(&Opens);

    // Pass the call on to NtOpenFile unchanged, so the title gets the kernel's answer (or the next extension's hook gets the call first).
    // We could optionally change the arguments or the result, or answer the call ourselves, but we don't.
    NTSTATUS status = oxos_next(NtOpenFile)(File, Access, Name, Iosb, Share, Options);
    return status;
}

// Before each title starts.
static BOOLEAN ExtensionStart(void) {
    // Logging ExtensionStart message to the log file and serial port.
    oxos_log("Starting extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);

    // Reading the "ending" setting once (it stays valid until the title ends); the hook uses it at every call.
    Ending = oxos_setting("ending");
    EndingLength = Ending ? strlen(Ending) : 0;

    // Registering hook to Kernel's NtOpenFile function and replacing it with our My_NtOpenFile function.
    // The My_NtOpenFile function will be called first when the title calls NtOpenFile.
    BOOLEAN res = oxos_hook(NtOpenFile, My_NtOpenFile);
    return res;
}

// The title is starting another title or the dashboard.
static void ExtensionTerminate(void) {
    oxos_log("NtOpenFile called %ld times.", Opens);
    oxos_log("Terminating extension %s version %s.", EXTENSION_NAME, EXTENSION_VERSION);
}
