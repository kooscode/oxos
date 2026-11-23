# example_toast: a toast over the running title

`example_toast.cpp` shows a toast when the title opens or creates a file listed in `[files]` of its `example_toast.ini`: with this folder's ini, a title
writing `savegame.dat` gets "Saving: savegame.dat" at the top right for 3 seconds.

```ini
[files]
Saving = savegame.dat
Loading = level1.map
```

- **The toast:** `oxos_toast_enqueue` copies an `OXOS_TOAST` (its text, place and seconds) into the kernel's queue and returns at once; the kernel shows it
  on the GPU's video overlay once the title has set its video mode, after any toasts queued before it. `oxos_toast("...")` is the short form (one line, bottom
  centre, 5 seconds); `oxos_on_toast` tells an extension when its toasts show and hide.
- **From a hook:** the hooks run in the title's own thread, so they only compare names and queue; `oxos_toast_enqueue` never waits. A full queue
  (`STATUS_DEVICE_BUSY`) just skips that toast; `oxos_toast_enqueue_wait` would wait, but only outside hooks.
- **Data, read once:** `[files]` is data (`oxos_ini_section`, from the disk), so `ExtensionStart` reads it and keeps the lines; a hook never reads the disk.

Build and install as [the SDK's README](../../README.md) says, with the SDK's `oxos_extensions.ini` and this folder's `example_toast.ini`.
