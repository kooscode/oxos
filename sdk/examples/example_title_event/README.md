# example_title_event: a title event, the state page, and a kernel function

`example_title_event.cpp` logs each title's name when it has been loaded, the name of the title before it, and the title's greeting from its own
`example_title_event.ini`. When the title is the one its `led_title` setting names (this folder's sample names the OXOS FTP server), it turns the front LED
orange:

```
[example_title_event] Starting extension example_title_event version 1.0.0.
[example_title_event] "OXOS FTP server" is starting; the title before it was "Xbox Dashboard".
[example_title_event] "OXOS FTP server": Ready to copy files.
[example_title_event] "OXOS FTP server": the LED is orange.
[example_title_event] Terminating extension example_title_event version 1.0.0.
```

- **The event:** `oxos_on_title_loaded(Example_TitleLoaded_Callback, last)` in `ExtensionStart` has the kernel call `Example_TitleLoaded_Callback` once the
  title is loaded, before it starts, with the title's ID and name (plain text), and `last` as its user data. `oxos_on_title_loading` and
  `oxos_on_title_exiting` work the same way.
- **A setting and data, from `example_title_event.ini`** (copy it to `C:\OXOS\extensions\`, next to the `.xos`):
  - **the setting** `led_title`, at the top of the file, is read into memory when the extension loads; `oxos_setting("led_title")` returns it at any time.
    Settings are for a few options: at most 32, values up to 255 characters.
  - **the data**, a `[section]` per title with its `greeting`, is never loaded with the extension: in the title-loaded event, `oxos_ini_section` reads only
    the running title's section from the disk. The file can hold a section for every title there is, at no cost in memory.
- **The state page:** `oxos_state` gives 4 KB that keep their contents from one launch to the next, until the next cold boot; the example keeps the last
  title's name there.
- **The event and the run loop together:** the event only notes that the LED is wanted, and when; `ExtensionRunLoop` turns it orange a second later, since
  the kernel turns the LED green just after the event, as the title starts.
- **Kernel functions:** an extension calls any kernel function by name, as `oxos_kernel.h` declares it: here `strcasecmp` (the kernel's C library) compares
  the names, and `HalWriteSMBusValue` sets the LED (the SMC, on the SMBus at 0x20).

Build and install as [the SDK's README](../../README.md) says, with the SDK's `oxos_extensions.ini` and this folder's `example_title_event.ini`.
