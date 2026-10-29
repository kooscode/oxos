# Host tools

Python 3 scripts run on the build machine (the `kernel/Makefile` targets call them; each script's `--help` and
docstring give the details).

## Running and debugging in xemu
| Tool | What it does |
|---|---|
| `run.py` | Boots a BIOS in an interactive xemu window, with an optional disc image, on a copy of the hard-disk image (or a persistent one with `--state`) and its own EEPROM. Options for widescreen, window size and render scale; `--serial` prints the kernel's debug serial port, `--qmp` opens a QMP socket, `--memory 128` gives the console 128 MB, `--hd [480p,720p,1080i]` presents the HDTV AV pack and enables the EEPROM's HD modes (titles then use 480p/720p/1080i), `--avpack` picks another cable, `--encoder focus|xcalibur` another video encoder, `--trace-map` dumps the kernel call record on a crash and on exit, and `--gdb PORT` starts xemu's GDB server paused at power-on. Used by `make run`, `run-stock`, `run-trace-basic`, `run-trace-full` and `run-gdb`. |
| `trace.py` | Prints the kernel call tracer's record: the title's last kernel calls with return values, arguments and file names, read from a running xemu over QMP (`make print-trace-dump`), or decoded from a saved dump (`--load`). Filters: `--last`, `--only`, `--hide`, `--all`, `--thread`. |
| `debug-app.py` | Debugs an nxdk app built with `make DEBUG=y`: builds the debug kernel when needed, packs the app's `bin/` folder as a disc, converts its symbols for gdb and starts xemu paused with the GDB server, printing when a debugger can attach. `--stop` closes it again. |
| `vscode/` | `launch.json`, `tasks.json` and `settings.json` to copy into an nxdk app's `.vscode/` folder: F5 builds the app, starts xemu through `debug-app.py`, attaches gdb and runs to the first breakpoint (see the main README's "Debugging" section). |
| `ftp-server/` | An nxdk title: an FTP server for the console's disks (`/F/games` = `F:\games`), any user name and password. Lists, uploads, downloads (with resume), makes and removes folders, deletes, and renames or moves files and folders within a drive; `SITE REBOOT` / `SITE POWEROFF`. DHCP, else 192.168.1.223; the address is on screen and on the serial log. `make -C kernel ftp-server` builds it; `tools/netboot/netboot.py IMAGE --title tools/ftp-server/bin --title-name ftp` runs it on a console; `tools/run.py BIOS --iso tools/ftp-server/ftp-server.iso --net-forward 2121:21` and `curl -P 10.0.2.2 ftp://127.0.0.1:2121/` in xemu. See `ftp-server/README.md`. |
| `iso-launcher/` | An nxdk title that mounts the disc image named in its `iso.txt` (an image, a folder, or nothing: the image next to it) and starts its title; `mount_iso.h` gives other titles (dashboards) the same `MountISO(path)` / `UnmountISO()` ([`spec/oxos-internal/16-virtual-discs.md`](../spec/oxos-internal/16-virtual-discs.md)). `make -C kernel iso-launcher` builds it; `make run-iso ISO='F:\titles\x.iso'` boots it in xemu on the run-xbe disk; `make test-iso` tests it. |
| `oxos-isonator/` | Makes game folders for dashboards from images of your own discs or extracted game folders: an XISO (built from a folder; `--split` for FATX), and a `default.xbe` launcher next to it, the ISO launcher carrying the game's title ID, name and title image. Works on OXOS and Cerbios. Python standard library only; see `oxos-isonator/README.md`. |
| `title-launcher/` | An nxdk title that lists the disc images and XBEs in `F:\games` and `F:\titles`, and the tools in `F:\Tools` (one folder each, e.g. `F:\Tools\ftp\ftp.xbe`; listed last) (`.iso`, `.cci`, split images by their `.1` part, `.xbe`), and each game folder in them by its `default.xbe` (or, without one, by the images in it); D-pad up/down moves, A starts: an image is mounted (`../iso-launcher/mount_iso.h`) and its `default.xbe` started, an XBE started directly. `make -C kernel title-launcher` builds it; `tools/netboot/netboot.py IMAGE --title tools/title-launcher/bin --title-name launcher` runs it on a console. |
| `mkcci/` | A host tool that converts a disc image (.iso) to CCI: `make -C tools/mkcci`, then `tools/mkcci/bin/mkcci IN.iso OUT [--split]`. It writes `OUT.cci`, or `OUT.1.cci` + `OUT.2.cci` when the CCI would pass 4 GB (or with `--split`, cut at the middle sector). It then decodes every part and compares it with the input. |
| `jingle/` | Renders the boot jingle (the kernel's own synthesizer, `kernel/src/hal/jingle.cpp`) to a WAV file and prints its levels: `make -C tools/jingle && tools/jingle/bin/jingle out.wav [MUTE]` (MUTE leaves instruments out: bit 1 cello and bass, 2 drums, 3 reverb) ([`spec/oxos-internal/17-boot-jingle.md`](../spec/oxos-internal/17-boot-jingle.md)). |
| `xbe-launcher/` | An nxdk title that launches the XBE named in its disc's `launch.txt` (e.g. `F:\apps\demo\default.xbe`) the way a dashboard does, or shows why it cannot. `make run-xbe XBE=...` builds it and a disc with it, and boots that disc on an exFAT disk. |
| `fbshot.py` | Boots a BIOS with no disc, waits, and saves the screen (the NV2A scanout surface) as a PNG; for checking what the kernel or dashboard drew without a window. |
| `qmphmp.py` | Sends monitor (HMP) commands to a running xemu through its QMP socket, with optional `sleep:N` steps; a small helper for poking at a running console by hand. |
| `fetch.py` | Downloads a file and saves it only if its SHA-256 matches the pinned one; the Makefile uses it to fetch the default hard-disk image (`make hdd-image`). Python standard library only. |
| `xemu.toml.example` | An example xemu configuration file (boot ROM, BIOS, EEPROM and hard-disk paths), for running xemu by hand. |

## Real consoles
| Tool | What it does |
|---|---|
| `netboot/` | Boots kernels over the network: flash `make -C kernel netboot` once, then `netboot/netboot.py IMAGE` serves any OXOS image to the console at each power-on (the console boots its own kernel when no PC answers). See `netboot/README.md`. |
| `modxo/` | The Modxo debug rig's PC side: `console.py` logs COM1, POST codes and the modchip's LPC status, and presses the console's power button (`on`, `off`, `cycle`); `bootrun.py` boots N times and classifies each boot; `flash-uf2.sh` reflashes the Pico over USB. Needs Modxo's `oxos-debug` firmware branch. See `modxo/README.md`. |
| `mcpx-dump/` | Dumps a console's MCPX boot ROM (MCPX 1.0 and 1.1) over the network: `mcpx_dump.py build` makes a flash image from your own retail BIOS and `make -C kernel mcpx-dump`'s payload, `mcpx_dump.py receive` saves and checks the dump. See `mcpx-dump/README.md`. |

## Building the BIOS and test titles
| Tool | What it does |
|---|---|
| `mkbios.py` | Packs the kernel into a 1 MB BIOS image: our 2BL, reset code and Cromwell's X-codes (`--open`, the default build), or into your own BIOS's layout (`--base`). Compresses the kernel with `lzx.py` and adds the boot splash: a 640x480 256-colour PNG as it is, any other picture converted with Pillow (`--save-splash` keeps the converted PNG). |
| `lzx.py` | The LZX compressor (and a small decoder for self-checks) for the kernel stream the 2BL unpacks at boot. |
| `gen_def.py` | Generates the kernel's export `.def` file from `kernel/exports.tsv`: every function and variable at its ordinal (variables as data exports); the two debugger-only rows are kept in the table but not exported. |
| `gen_av_tables.py` | Writes the video encoder tables (`kernel/src/hal/av_modes.inc`) from the encoder specs `spec/stock/18-conexant-encoder.md`, `19-focus-encoder.md` and `20-xcalibur-encoder.md` (their register maps), with the NV2A timing of each SD row derived from the encoder's own settings (`spec/oxos-internal/02-display-modes.md` §1). `--out FILE` writes the tables; `--check FILE` fails if FILE is stale. |
| `check_kernel_layout.py` | Runs after every kernel link: fails when kept code uses a symbol of the boot-only sections (`.xka`, `.xkb`; an inline function or constant the linker took from a boot-only file), or when the initialised `.data` outgrows its quick-reboot copy ([`spec/oxos-internal/18-boot-memory.md`](../spec/oxos-internal/18-boot-memory.md) §5). |
| `mkxbe.py` | Converts a PE linked at 0x10000 into an XBE (headers, certificate, section digests); used for the kernel test title (`tests/kernel-test-title`) the boot tests run. |

The tests' own helpers (`tests/boot_test.py`, `tests/mkxiso.py`, `tests/mkudf.py`, `tests/qcow2.py`, ...) live in
`tests/`.
