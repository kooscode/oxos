# example_title_event: a title event, the state page, and a kernel function

`example_title_event.cpp` logs each title's name when it has been loaded, and the name of the title before it. When the title is the one `led_title` names in
`oxos_extensions.ini` (this folder's sample names the OXOS FTP server), it turns the front LED orange:

```
[example_title_event] Starting extension example_title_event version 1.0.0.
[example_title_event] "OXOS FTP server" is starting; the title before it was "Xbox Dashboard".
[example_title_event] "OXOS FTP server": the LED is orange.
[example_title_event] Terminating extension example_title_event version 1.0.0.
```

- **The event:** `oxos_on_title_loaded(Example_TitleLoaded_Callback, last)` in `ExtensionStart` has the kernel call `Example_TitleLoaded_Callback` once the
  title is loaded, before it starts, with the title's ID and name (plain text), and `last` as its user data. `oxos_on_title_loading` and
  `oxos_on_title_exiting` work the same way.
- **The state page:** `oxos_state` gives 4 KB that keep their contents from one launch to the next, until the next cold boot; the example keeps the last
  title's name there.
- **The event and the run loop together:** the event only notes that the LED is wanted, and when; `ExtensionRunLoop` turns it orange a second later, since
  the kernel turns the LED green just after the event, as the title starts.
- **Kernel functions:** an extension calls any kernel function by name, as `oxos_kernel.h` declares it: here `strcasecmp` (the kernel's C library) compares
  the names, and `HalWriteSMBusValue` sets the LED (the SMC, on the SMBus at 0x20).

Build and install as [the SDK's README](../../README.md) says, with this folder's `oxos_extensions.ini`.
