# Host tools

Python 3 scripts run on the build machine (the `kernel/Makefile` targets call them; each script's `--help` and
docstring give the details).

## Running and debugging in xemu
| Tool | What it does |
|---|---|
| `run.py` | Boots a BIOS in an interactive xemu window, with an optional disc image, on a copy of the hard-disk image (or a persistent one with `--state`) and its own EEPROM. Options for widescreen, window size and render scale; `--serial` prints the kernel's debug serial port, `--qmp` opens a QMP socket, `--memory 128` gives the console 128 MB, `--hd [480p,720p,1080i]` presents the HDTV AV pack and enables the EEPROM's HD modes (games then use 480p/720p/1080i), `--avpack` picks another cable, `--encoder focus|xcalibur` another video encoder, `--trace-map` dumps the kernel call record on a crash and on exit, and `--gdb PORT` starts xemu's GDB server paused at power-on. Used by `make run`, `run-stock`, `run-trace-basic`, `run-trace-full` and `run-gdb`. |
| `trace.py` | Prints the kernel call tracer's record: the title's last kernel calls with return values, arguments and file names, read from a running xemu over QMP (`make print-trace-dump`), or decoded from a saved dump (`--load`). Filters: `--last`, `--only`, `--hide`, `--all`, `--thread`. |
| `debug-app.py` | Debugs an nxdk app built with `make DEBUG=y`: builds the debug kernel when needed, packs the app's `bin/` folder as a disc, converts its symbols for gdb and starts xemu paused with the GDB server, printing when a debugger can attach. `--stop` closes it again. |
| `vscode/` | `launch.json`, `tasks.json` and `settings.json` to copy into an nxdk app's `.vscode/` folder: F5 builds the app, starts xemu through `debug-app.py`, attaches gdb and runs to the first breakpoint (see the main README's "Debugging" section). |
| `xbe-launcher/` | An nxdk title that launches the XBE named in its disc's `launch.txt` (e.g. `F:\apps\demo\default.xbe`) the way a dashboard does, or shows why it cannot. `make run-xbe XBE=...` builds it and a disc with it, and boots that disc on an exFAT disk. |
| `fbshot.py` | Boots a BIOS with no disc, waits, and saves the screen (the NV2A scanout surface) as a PNG; for checking what the kernel or dashboard drew without a window. |
| `qmphmp.py` | Sends monitor (HMP) commands to a running xemu through its QMP socket, with optional `sleep:N` steps; a small helper for poking at a running console by hand. |
| `fetch.py` | Downloads a file and saves it only if its SHA-256 matches the pinned one; the Makefile uses it to fetch the default hard-disk image (`make hdd-image`). Python standard library only. |
| `xemu.toml.example` | An example xemu configuration file (boot ROM, BIOS, EEPROM and hard-disk paths), for running xemu by hand. |

## Real consoles
| Tool | What it does |
|---|---|
| `netboot/` | Boots kernels over the network: flash `make -C kernel netboot` once, then `netboot/netboot.py IMAGE` serves any OXOS image to the console at each power-on (the console boots its own kernel when no PC answers). See `netboot/README.md`. |
| `mcpx-dump/` | Dumps a console's MCPX boot ROM (MCPX 1.0 and 1.1) over the network: `mcpx_dump.py build` makes a flash image from your own retail BIOS and `make -C kernel mcpx-dump`'s payload, `mcpx_dump.py receive` saves and checks the dump. See `mcpx-dump/README.md`. |
| `netlog.py` | Prints the kernel's network log (`NETLOG=1` builds: UDP broadcasts to port 5140) from a console on the LAN, or from xemu (`--xemu PORT` with `run.py --net-udp PORT`); names the kernel calls in trace lines. |

## Building the BIOS and test titles
| Tool | What it does |
|---|---|
| `mkbios.py` | Packs the kernel into a 1 MB BIOS image: our 2BL, reset code and Cromwell's X-codes (`--open`, the default build), or into your own BIOS's layout (`--base`). Compresses the kernel with `lzx.py` and adds the boot splash: a 640x480 256-colour PNG as it is, any other picture converted with Pillow (`--save-splash` keeps the converted PNG). |
| `lzx.py` | The LZX compressor (and a small decoder for self-checks) for the kernel stream the 2BL unpacks at boot. |
| `gen_def.py` | Generates the kernel's export `.def` file from `kernel/exports.tsv`: every function and variable at its ordinal (variables as data exports); the two debugger-only rows are kept in the table but not exported. |
| `gen_av_tables.py` | Writes the video encoder tables (`kernel/src/hal/av_modes.inc`) from the encoder specs `spec/stock/18-conexant-encoder.md`, `19-focus-encoder.md` and `20-xcalibur-encoder.md` (their register maps), with the NV2A timing of each SD row derived from the encoder's own settings (`spec/oxos-internal/02-display-modes.md` §1). `--out FILE` writes the tables; `--check FILE` fails if FILE is stale. |
| `mkxbe.py` | Converts a PE linked at 0x10000 into an XBE (headers, certificate, section digests); used for the kernel test title (`tests/kernel-test-title`) the boot tests run. |

The tests' own helpers (`tests/boot_test.py`, `tests/mkxiso.py`, `tests/mkudf.py`, `tests/qcow2.py`, ...) live in
`tests/`.
