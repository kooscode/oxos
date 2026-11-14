// SPDX-License-Identifier: MIT
// example_kernel_hook.xos: a hook on a kernel function. It counts the files a title opens, by sending the title's NtOpenFile calls to its own function
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

// The hook: the title's NtOpenFile calls come here first. It has exactly NtOpenFile's arguments, result and calling convention (oxos_kernel.h).
static NTSTATUS NTAPI My_NtOpenFile(PHANDLE File, ACCESS_MASK Access, POBJECT_ATTRIBUTES Name, PIO_STATUS_BLOCK Iosb, ULONG Share, ULONG Options) {
    // Increment our counter for every NtOpenFile call made by the title (InterlockedIncrement: several of its threads may call at once).
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
