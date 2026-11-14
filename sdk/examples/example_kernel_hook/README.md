# example_kernel_hook: a hook on a kernel function

`example_kernel_hook.cpp` counts the files each title opens and logs the count when the title ends:

```
[example_kernel_hook] Starting extension example_kernel_hook version 1.0.0.
[example_kernel_hook] NtOpenFile called 85 times.
[example_kernel_hook] Terminating extension example_kernel_hook version 1.0.0.
```

`oxos_hook(NtOpenFile, My_NtOpenFile)` in `ExtensionStart` sends the title's `NtOpenFile` calls to `My_NtOpenFile` first. It has exactly `NtOpenFile`'s
arguments (as `oxos_kernel.h` declares them), counts each call with `InterlockedIncrement` (the title may open files from several threads at once), and passes
it on with `oxos_next(NtOpenFile)`. Any kernel function in `oxos_kernel.h` is hooked the same way. Build and install as [the SDK's README](../../README.md)
says, with this folder's `oxos_extensions.ini`.
