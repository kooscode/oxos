# OXOS for coding agents

How to build, run, test, debug and look at OXOS (the Open Xbox Operating System: a BIOS and kernel for the original Xbox) from a terminal. Setting up the
machine, the source layout and the details of each target are in [`kernel/README.md`](kernel/README.md); the tests in [`tests/README.md`](tests/README.md); the
tools in [`tools/README.md`](tools/README.md); the specifications in [`spec/`](spec/README.md).

## Rules of the repo
- **C++20, written as [`docs/cpp_style.md`](docs/cpp_style.md) says.** Read it before writing code and check your diff against it: verb-first names, a comment
  on every piece of state, lines within 160 characters, format names in capitals (`FATXLookup`), `OXOS` or `oxos`, never `Oxos`. The only `.c` file is
  `tests/extensions/plain_c.c`.
- **Plain GNU make**, no CMake. All build output goes under `build/`.
- **Clean room:** behaviour comes from the specifications in `spec/` and from observing hardware, never from leaked Microsoft code.
- **Everything OXOS puts on a console is named `oxos_...`** (`C:\OXOS\extensions\oxos_trainers.ini`), with descriptive names a user understands.
- Screen text is short: a title and a line or a list.

## Build
```sh
make -C kernel bios                 # build/kernel/self_tests/bios.bin: the kernel with its boot self-tests (the default build)
make -C kernel BUILD=../build/kernel/release SELFTEST=0 ../build/kernel/release/bios.bin   # the release image (what make run boots)
make -C kernel extensions           # the shipped kernel extensions and apps (oxos_trainers.xos, its menu .xbe, its .ini)
make -C kernel ftp_server           # the OXOS FTP server (nxdk app)
make -C kernel release SKIP_TESTS=1 # the release images only, quickly
make -C kernel release VERSION=X.Y.Z # every test, then build/kernel/release/dist/oxos_vX.Y.Z.zip (C:\OXOS as it goes on the console)
```
nxdk apps need an nxdk checkout: `make -C kernel nxdk` fetches one. The first build fetches xemu's test disk (`make -C kernel hdd_image`).

## Test
- **After every kernel change:** `tools/unit_tests.sh` (host-only unit tests, seconds).
- **One xemu phase:** `make -C kernel test_dash` (boots to the dashboard), `test_extensions`, `test_relaunch`, `test_iso`, ... (the list: `tests/README.md`).
- **After a large change** (file systems, memory, boot, the title loader, quick reboot, or many files): the full regression, `make -C kernel regression`
  (about 15 minutes; summary in `build/regression/`; `MCPX_ROM=<a boot ROM dump>` adds the real-boot-ROM phase). Every phase must pass.
- The xemu tests drive a virtual controller through `/dev/uinput` (needs write access: `sudo setfacl -m u:$USER:rw /dev/uinput`), bound to controller port 1
  so a real controller plugged into the PC cannot take its place (`tests/virtual_gamepad.py`).

## Run and look at it
- **Interactive:** `make -C kernel run` (xemu with the dashboard disk), or `python3 tools/run.py build/kernel/release/bios.bin --iso GAME.iso`.
- **Screenshots, unattended:** [`tools/xemu_screenshot.py`](tools/xemu_screenshot.py) boots a BIOS in xemu, optionally with a disc and your own files copied
  onto C:, presses controller buttons and saves 640x480 PNGs:
  ```sh
  python3 -I tools/xemu_screenshot.py build/kernel/release/bios.bin /tmp/dash --steps wait:40,shot
  python3 -I tools/xemu_screenshot.py build/kernel/release/bios.bin /tmp/menu --iso GAME.iso \
      --put build/kernel/extensions/oxos_trainers/app/bin/default.xbe='C:\OXOS\extensions\oxos_trainers.xbe' --steps wait:30,shot,DOWN:3,wait:2,shot
  ```
  Steps: `wait:SECONDS`, `shot`, and buttons (`A`, `B`, `X`, `Y`, `BLACK`, `WHITE`, `BACK`, `START`, `LS`, `RS`, `UP`, `DOWN`, `LEFT`, `RIGHT`, with `:N`
  to repeat). What works and what does not:
  - It grabs xemu's own window (`"xemu | v..."`) by its X11 window id (ffmpeg `x11grab -window_id`): this works while other windows or the lock screen cover it,
    but needs the PC's GPU-backed X display (`$DISPLAY`).
  - xemu has no QMP `screendump`; reading the frame from guest memory gives a stale picture (xemu keeps rendered surfaces on the GPU); a nested X server
    (Xephyr) gives xemu software OpenGL, too slow to get past the first frames of a 3D screen; xemu's hidden 640x480 "SDL Offscreen Window"s cannot be grabbed.
  - Open the PNGs to look at them; compare two with Python's PIL (`ImageChops.difference(a, b).getbbox()`) to see whether the screen changed.
- **QMP:** xemu's QMP socket (`-qmp unix:PATH,server,nowait`; keep PATH short, Unix sockets allow 107 characters) takes `human-monitor-command` for memory reads
  (`xp`, `pmemsave`); `tests/boot_test.py` reads kernel variables by name from the linker map this way.

## Debug
- **Serial log:** builds with `SERIAL=1` print the kernel's debug log on the serial port; `make -C kernel run_trace_basic` shows it in the terminal.
- **gdb:** `make -C kernel run_gdb` (xemu paused at power-on, symbols in `build/kernel/debug/oxoskrnl.elf`); `tools/debug_app.py` for an nxdk app.
- **Call traces:** `make -C kernel run_trace_full` records the last kernel calls a title made (`print_trace_dump`).
- **Real hardware:** a console with a netboot BIOS loads each new kernel over the network (`tools/netboot/README.md`), with an XBE to run straight away
  (`--title DIR --title-name NAME`); the unattended rig is described in [`docs/hwitl.md`](docs/hwitl.md).

## Which tool for which task
Each tool's invocation and options are in its README (folders) or its docstring and `--help` (scripts); the tables in [`tools/README.md`](tools/README.md) and
[`tests/README.md`](tests/README.md) list them all. Start from the task:

| Task | Tool | Notes |
|---|---|---|
| Boot a BIOS and use it by hand | `tools/run.py` (`make -C kernel run`) | disc image, persistent state (`--state`), serial log, gdb, HD and AV options |
| See a screen without anyone at the PC | `tools/xemu_screenshot.py` | xemu's window grabbed by id; buttons in between; `--put` files onto C:, `--save-hdd` to read back what the console wrote |
| See what the kernel drew (no window) | `tools/fbshot.py` | reads the scan-out surface from guest memory: right for screens the CPU draws (boot splash, error screens), stale for GPU-rendered apps |
| Poke a running xemu | `tools/qmphmp.py` | HMP commands over QMP (`xp`, `pmemsave`, `info registers`), with `sleep:N` steps |
| Why did a title crash / what did it call | `tools/trace.py` (`make -C kernel run_trace_full`, `print_trace_dump`) | the title's last kernel calls with arguments |
| Step through an nxdk app | `tools/debug_app.py`, `tools/vscode/` | xemu paused, gdb with the app's symbols; F5 in VS Code |
| Put files on a test disk, read what the kernel wrote | `tests/qcow2.py` + `tests/fatx.py` (`FATX`, `FATXWriter`), `tests/exfat.py` | `FATX(Qcow2(path), "C").read_file(r"OXOS\extensions\oxos_trainers.ini")` |
| Make a disc image | `tests/mkxiso.py` (XISO, any folder), `tests/mkudf.py`, `tools/mkcci/` (CCI) | |
| Press controller buttons in a test | `tests/virtual_gamepad.py` (`VirtualPad`, `XEMU_PORT1_BINDING`) | needs write access to `/dev/uinput` |
| Run all tests | `make -C kernel regression` (`tools/regression.py`) | `ONLY=test_a,test_b` for some phases |
| Run the host unit tests | `tools/unit_tests.sh` | GoogleTest, `tests/unit/` |
| Build a console app | `tools/nxdk_app.mk` + `tools/sdl_screen/` | nxdk + SDL3; output in `build/<folder>` |
| Start an XBE in xemu | `make -C kernel run_xbe XBE=...` (`tools/xbe_launcher/`), `run_iso` (`tools/iso_launcher/`) | |
| Game folders from disc images | `tools/oxos_isonator/` | for OXOS and Cerbios |
| Copy files to and from a console, start an XBE | `tools/ftp_server/` (the OXOS FTP server; `SITE LAUNCH /F/path`, `SITE REBOOT`) | Python's `ftplib` in one session is fastest for many files |
| Boot a new kernel on a console, run an XBE straight away | `tools/netboot/` (`netboot.py IMAGE --title DIR --title-name NAME`) | flash the netboot image once |
| Power a console on or off, read its COM1 and POST codes | `tools/modxo/` (`console.py on`, `off`, `cycle`, `state`, `log`) | the Modxo rig; keep `console.py log` running |
| See a console's screen | an HDMI capture card with ffmpeg (`-f v4l2 ... -ss 3`) | skip the first 3 s: the card shows colour bars first |
| Kernel extensions | `sdk/` (its README), `kernel/extensions/` (the ones OXOS ships) | spec 34 |
| Trainer pack, trainer ini | `third_party/xbox_trainers/` (its README, `index.csv`), `tools/gen_trainers_ini.py` | spec 37 |
| Boot splash, jingle | `tools/svg_splash.py`, `tools/vsplash/`, `tools/jingle/` | |
| BIOS image internals | `tools/mkbios.py`, `tools/lzx.py`, `tools/harden_xcodes.py`, `tools/link_extension_loader.py` | `make` runs them |
| Checks that run after every kernel link | `tools/check_kernel_layout.py`, `tools/check_c_library_use.py` | a failure stops the build |
| Generated sources | `tools/gen_def.py` (exports), `tools/gen_oxos_sdk.py` (SDK), `tools/gen_av_tables.py` (video encoders), `tools/mkucode.py` | regenerate, never edit by hand |

## Commits
- One commit per finished feature, with a message that says what changed, why, and how it was checked (the tests that ran).
- Generated files are regenerated, not edited (their first line names the tool).

## Pitfalls
- `pkill -f PATTERN` also matches the shell running it when the pattern is in its own command line; match the process name (`pkill -x xemu`) instead.
- A Unix socket path over 107 characters fails (`AF_UNIX path too long`): put sockets in `/tmp`.
- xemu needs `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` to read a controller while its window has no focus (`tests/controller_test.py`, `xemu_env`).
