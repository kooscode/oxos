# Host tools

Python 3 scripts run on the build machine (the `kernel/Makefile` targets call them; each script's `--help` and
docstring give the details).

## Running and debugging in xemu
| Tool | What it does |
|---|---|
| `run.py` | Boots a BIOS in an interactive xemu window, with an optional disc image, on a copy of the hard-disk image (or a persistent one with `--state`) and its own EEPROM. Options for widescreen, window size and render scale; `--serial` prints the kernel's debug serial port, `--qmp` opens a QMP socket, `--memory 128` gives the console 128 MB, `--trace-map` dumps the kernel call record on a crash and on exit, and `--gdb PORT` starts xemu's GDB server paused at power-on. Used by `make run`, `run-stock`, `run-trace-basic`, `run-trace-full` and `run-gdb`. |
| `trace.py` | Prints the kernel call tracer's record: the title's last kernel calls with return values, arguments and file names, read from a running xemu over QMP (`make print-trace-dump`), or decoded from a saved dump (`--load`). Filters: `--last`, `--only`, `--hide`, `--all`, `--thread`. |
| `debug-app.py` | Debugs an nxdk app built with `make DEBUG=y`: builds the debug kernel when needed, packs the app's `bin/` folder as a disc, converts its symbols for gdb and starts xemu paused with the GDB server, printing when a debugger can attach. `--stop` closes it again. |
| `vscode/` | `launch.json`, `tasks.json` and `settings.json` to copy into an nxdk app's `.vscode/` folder: F5 builds the app, starts xemu through `debug-app.py`, attaches gdb and runs to the first breakpoint (see the main README's "Debugging" section). |
| `fbshot.py` | Boots a BIOS with no disc, waits, and saves the screen (the NV2A scanout surface) as a PNG; for checking what the kernel or dashboard drew without a window. |
| `qmphmp.py` | Sends monitor (HMP) commands to a running xemu through its QMP socket, with optional `sleep:N` steps; a small helper for poking at a running console by hand. |
| `xemu.toml.example` | An example xemu configuration file (boot ROM, BIOS, EEPROM and hard-disk paths), for running xemu by hand. |

## Building the BIOS and test titles
| Tool | What it does |
|---|---|
| `mkbios.py` | Packs the kernel into a 1 MB BIOS image: our 2BL, reset code and Cromwell's X-codes (`--open`, the default build), or into your own BIOS's layout (`--base`). Compresses the kernel with `lzx.py` and adds the boot splash. |
| `lzx.py` | The LZX compressor (and a small decoder for self-checks) for the kernel stream the 2BL unpacks at boot. |
| `gen_def.py` | Generates the kernel's export `.def` file (ordinals, data exports) from `kernel/exports.tsv` and `kernel/exports.lst`. |
| `mkxbe.py` | Converts a PE linked at 0x10000 into an XBE (headers, certificate, section digests); used for the boot tests' own title. |

The tests' own helpers (`tests/boot_test.py`, `tests/mkxiso.py`, `tests/mkudf.py`, `tests/qcow2.py`, ...) live in
`tests/`.
