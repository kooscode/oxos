# example_run_loop: work while a title runs

`example_run_loop.cpp` logs once every `interval` seconds while a title runs (a setting in its own `example_run_loop.ini`, read with `oxos_setting_number`), and
how long the title ran when it ends:

```
[example_run_loop] Starting extension example_run_loop version 1.0.0.
[example_run_loop] 10 s into this title.
[example_run_loop] 20 s into this title.
[example_run_loop] This title ran 20 s.
[example_run_loop] Terminating extension example_run_loop version 1.0.0.
```

`ExtensionRunLoop` is called about every millisecond, on one kernel thread that every extension shares, so it checks the kernel's millisecond count
(`KeTickCount`) and returns at once until a line is due; it never waits. Build and install as [the SDK's README](../../README.md) says, with the SDK's
`oxos_extensions.ini` and this folder's `example_run_loop.ini`.
