# Tools

Python scripts for the build machine (the `kernel/Makefile` targets call them; each script's `--help` and docstring
give the details), and nxdk console apps (built with `make -C kernel <name>`).

## Running and debugging in xemu
| Tool | What it does |
|---|---|
| `run.py` | Boots a BIOS in an interactive xemu window with an optional disc image, on a copy of the hard disk (or a persistent one with `--state`). Options: widescreen, window size, render scale; `--serial` (debug serial port), `--qmp`, `--memory 128`, `--hd` (HDTV AV pack), `--avpack`, `--encoder`, `--trace-map` (call record on a crash), `--gdb PORT` (paused for gdb). Used by `make run` and its variants. |
| `trace.py` | Prints the kernel call tracer's record: the title's last kernel calls with return values, arguments and file names, read from a running xemu over QMP (`make print-trace-dump`), or decoded from a saved dump (`--load`). Filters: `--last`, `--only`, `--hide`, `--all`, `--thread`. |
| `debug-app.py` | Debugs an nxdk app built with `make DEBUG=y`: builds the debug kernel when needed, packs the app's `bin/` folder as a disc, converts its symbols for gdb and starts xemu paused with the GDB server, printing when a debugger can attach. `--stop` closes it again. |
| `vscode/` | `launch.json`, `tasks.json` and `settings.json` for an nxdk app's `.vscode/` folder: F5 builds the app, starts xemu through `debug-app.py` and attaches gdb ([`kernel/README.md`](../kernel/README.md), "Debugging"). |
| `ftp-server/` | Console app: an FTP server for the console's disks, any login. [`ftp-server/README.md`](ftp-server/README.md). |
| `iso-launcher/` | Console app: mounts the disc image named in its `iso.txt` (or the image next to it) and starts its title; `mount_iso.h` gives dashboards the same `MountISO` / `UnmountISO` ([`spec/oxos-internal/16`](../spec/oxos-internal/16-virtual-discs.md)). `make run-iso ISO='F:\titles\x.iso'` runs it in xemu; `make test-iso` tests it. |
| `oxos-isonator/` | Makes game folders (disc image + launcher) from disc images or extracted games, for OXOS and Cerbios. [`oxos-isonator/README.md`](oxos-isonator/README.md). |
| `title-launcher/` | Console app: lists the disc images, game folders and XBEs in `F:\games`, `F:\titles` and `F:\Tools` and starts the one picked (D-pad, A). |
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
| `statedump/` | A header that prints the CPU, chipset and GPU state a kernel leaves behind, for comparing kernels (`make STATEDUMP=1`). [`statedump/README.md`](statedump/README.md). |
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
