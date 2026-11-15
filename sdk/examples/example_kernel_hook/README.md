# example_kernel_hook: a hook on a kernel function

`example_kernel_hook.cpp` counts the files each title opens (only those whose name ends with its `ending` setting, when set: `.xmv` in this folder's
`example_kernel_hook.ini`) and logs the count when the title ends:

```
[example_kernel_hook] Starting extension example_kernel_hook version 1.0.0.
[example_kernel_hook] NtOpenFile called 85 times.
[example_kernel_hook] Terminating extension example_kernel_hook version 1.0.0.
```

`oxos_hook(NtOpenFile, My_NtOpenFile)` in `ExtensionStart` sends the title's `NtOpenFile` calls to `My_NtOpenFile` first. It has exactly `NtOpenFile`'s
arguments (as `oxos_kernel.h` declares them), counts each call with `InterlockedIncrement` (the title may open files from several threads at once), and passes
it on with `oxos_next(NtOpenFile)`. Any kernel function in `oxos_kernel.h` is hooked the same way.

**A setting in a hook:** `ending` is a setting (the top of `example_kernel_hook.ini`), so it is in memory from the moment the extension loads, and the hook
compares every file name with it. A hook may use settings; it must never read data (`oxos_ini_section`), which reads the disk. Build and install as
[the SDK's README](../../README.md) says, with the SDK's `oxos_extensions.ini` and this folder's `example_kernel_hook.ini`.
