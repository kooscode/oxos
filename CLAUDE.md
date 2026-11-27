# OXOS for coding agents

How to build, run, test, debug and look at OXOS (the Open Xbox Operating System: a BIOS and kernel for the original Xbox) from a terminal. Setting up the
machine, the source layout and the details of each target are in [`kernel/README.md`](kernel/README.md); the tests in [`tests/README.md`](tests/README.md); the
tools in [`tools/README.md`](tools/README.md); the specifications in [`spec/`](spec/README.md).

## Rules of the repo
- **C++23, written as [`docs/cpp_style.md`](docs/cpp_style.md) says.** Read it before writing code and check your diff against it: verb-first names, a comment
  on every piece of state, lines within 160 characters, format names in capitals (`FATXLookup`), `OXOS` or `oxos`, never `Oxos`. The only `.c` file is
  `tests/extensions/plain_c.c`.
- **Plain GNU make**, no CMake. All build output goes under `build/`.
- **Clean room:** behaviour comes from the specifications in `spec/` and from observing hardware, never from disassembled or other source code.
- **Everything OXOS puts on a console is named `oxos_...`** (`C:\OXOS\extensions\oxos_trainers.ini`), with descriptive names a user understands.
- Screen text is short: a title and a line or a list.
- **Research, experiments and pilots live in `experimental/`** (git-ignored): notes, research specs (REA's included), trial code and proof-of-concept
  patches. Nothing from there is committed or cited. Once the user is good with a proof of concept, its findings are written as a new spec in `spec/`, the
  implementation is cleaned up, and only then committed. Do not commit code that may not end up in the project.

## Build
```sh
make -C kernel bios                 # build/kernel/self_tests/bios.bin: the kernel with its boot self-tests (the default build)
make -C kernel BUILD=../build/kernel/release SELFTEST=0 ../build/kernel/release/bios.bin   # the release image (what make run boots)
make -C kernel extensions           # the shipped kernel extensions and apps (oxos_trainers.xos, its menu .xbe, its .ini)
make -C kernel apps                 # OXOS's console apps in apps/ (oxos_ftp, oxos_boot_video; nxdk); one with make -C kernel oxos_ftp
make -C sdk                         # the kernel extension SDK (build/sdk/oxos_sdk/) and its examples
make -C kernel release SKIP_TESTS=1 # the release images only, quickly
make -C kernel release VERSION=X.Y.Z # every test, then build/kernel/release/dist/oxos_vX.Y.Z.zip (C:\OXOS as it goes on the console)
```
nxdk apps need an nxdk checkout: `make -C kernel nxdk` fetches one; the boot video player also needs its decoders (`make -C kernel video_libraries`). The
first build fetches xemu's test disk (`make -C kernel hdd_image`).

## Test
- **After every kernel change:** `tools/unit_tests.sh` (host-only unit tests, seconds).
- **One xemu phase:** `make -C kernel test_dash` (boots to the dashboard), `test_extensions`, `test_relaunch`, `test_iso`, ... (the list: `tests/README.md`).
- **Free memory for titles:** `make -C kernel test_memory` (about a minute) starts an nxdk title that counts its free pages on a 64 MB xemu; the regression
  fails below 15 960. See "Resident memory" below.

### The full regression
After a large change (file systems, memory, boot, the title loader, quick reboot, or many files), and before committing it:
```sh
make -C kernel regression MCPX_ROM=/path/to/mcpx_boot_rom.bin             # or: python3 -I tools/regression.py --mcpx-rom ...
make -C kernel regression ONLY=test_extensions,test_memory                # some phases (python: --only; "unit_tests" names the unit tests)
```
- **What runs:** the unit tests, then the xemu phases in order: `test` (the self-test kernel's boot, which also runs `test_dash`, `test_relaunch`,
  `test_fatal`, `test_udf`), `test_mcpx` (the real boot ROM; skipped without `MCPX_ROM`), `test_exfat` (about 8 minutes alone), `test_lba48`,
  `test_fatx_layouts`, `test_extensions`, `test_format`, `test_upload`, `test_iso`, `test_encoders`, `test_controller`, `test_boot_video`, `test_memory`. About 20 minutes.
- **Result:** one line per phase as it finishes (`test_iso: PASS (N checks, S s)`); `build/regression/summary.txt` and each phase's log
  (`build/regression/PHASE.log`). It goes on after a failure and fails if any phase fails or `test_memory` reports fewer than 15 960 free pages
  (`--pages`). Every phase must pass before the commit.
- **Run it in the background** and wait for it to finish; it takes the PC's xemu for 20 minutes. It builds kernels from the working tree as it goes, so do
  not edit sources while it runs (a later phase would test a half-done change): stop it first (and `pkill -x xemu`), then start it again afterwards.
- `make release` runs only the unit tests and `test_dash`.

### The MCPX boot ROM (for `test_mcpx` and stock BIOS images)
`test_mcpx` boots OXOS through the console's real 512-byte MCPX boot ROM, and xemu needs one to run any Microsoft BIOS. Nothing from Microsoft is in the repo:
each user dumps the ROM from their own console with `tools/mcpx_dump/` (its README has every step and the troubleshooting; spec/oxos_internal/12):
```sh
make -C kernel mcpx_dump                                                      # the payload: build/kernel/mcpx_dump/bios_256k.bin
python3 tools/mcpx_dump/mcpx_dump.py build --mcpx 1.1 --bios MY_RETAIL_BIOS.bin --out mcpx_dump.bin   # 1.1: a 5xxx retail BIOS; 1.0: a 4xxx one
# flash mcpx_dump.bin to the modchip (any 256 KiB-capable chip: Aladdin, Xenium, Modxo), then on a PC on the same network segment:
python3 tools/mcpx_dump/mcpx_dump.py receive --bios MY_RETAIL_BIOS.bin    # power the console on: saves mcpx_1.1.bin (or mcpx_1.0.bin) in seconds
```
- MCPX 1.0 (RC4 attack) and 1.1 (TEA attack, consoles 1.1-1.5) work; 1.6 (Xyclops) does not; a debug kit has no boot ROM. Flash the normal BIOS back after.
- The receiver checks the dump (start `33 C0`, end `B0 02 EE`, and against the retail BIOS given) and saves only a good one. It listens on UDP 5141, the same
  port as `netboot.py`: do not run both.
- Keep the file out of the repository (a git-ignored folder of your own) and pass it as `MCPX_ROM=...` / `--mcpx-rom`.

### Driving xemu's controller
- The tests press buttons with a **virtual gamepad** (`tests/virtual_gamepad.py`, `VirtualPad`): a uinput device that SDL sees as an Xbox 360 pad. It needs
  write access to `/dev/uinput` (`sudo setfacl -m u:$USER:rw /dev/uinput`).
- **It takes over controller port 1:** each test writes `XEMU_PORT1_BINDING` into its own `xemu.toml`, which binds port 1 to the virtual pad's SDL GUID.
  Without it xemu gives port 1 to the first pad it finds: a real controller plugged into the PC would take port 1, and the virtual pad's presses would
  reach no port. xemu also needs `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` to read a pad while its window has no focus (`controller_test.py`
  `xemu_env`).
- In code: `pad = VirtualPad()`, then `pad.set("A", 1)` / `pad.set("A", 0)` for buttons (`A B X Y LB RB BACK START LS RS`; LB/RB are White/Black), and axes
  `pad.set("HATY", -1)` (d-pad up; `HATY` 1 down, `HATX` -1/1 left/right, 0 releases), `LX`/`LY`/`RX`/`RY` (-32768..32767), `LT`/`RT` (0..255).
  `boot_test.py` holds X at power-on for the File Manager and walks the trainer menu this way (`TRAINER_MENU_KEYS`); `test_controller` checks every input.
- By hand: `tools/xemu_screenshot.py --steps` presses the same buttons by name between screenshots (below).

## Resident memory: every page counts
Titles get what the kernel leaves free, and a game that ran on the stock kernel (15 964 free pages) can fail with fewer. OXOS keeps 15 960; a change that costs
a page fails the regression. Where a piece of kernel code goes decides what it costs:

| Kind | Where | Costs titles | For |
|---|---|---|---|
| Resident | `kernel/src/` | always | what runs while a title runs (its kernel calls, interrupts, quick reboots) |
| Boot-only | `#pragma code_seg(".xkb")`, `XK_BOOT_DATA` (spec/oxos_internal/18) | nothing: freed before the first title | cold boot only (splash, disk checks, the cold boot's choices) |
| Kernel module | `kernel/modules/` (spec/oxos_internal/43) | nothing: mapped during a launch, freed before the title starts | launch-time code, every launch including quick reboots: the extension loader, ini reading, launch redirects, the dashboard order, title patches |
| Kernel extension | `.xos` file, `sdk/` (spec/oxos_internal/34) | its own pages, only when listed in `oxos_extensions.ini` | optional features that hook a title's kernel calls or events |

- **Kernel modules:** one image (`build/kernel/*/kernel_modules.exe`), linked at 0xB0000000 against the kernel's map (`tools/link_kernel_modules.py`), appended
  to the BIOS, cached at every cold boot as `C:\OXOS\cache\oxos_kernel.cache` and read from there after a quick reboot. A new one: a function in
  `kernel/modules/NAME.cpp`, a field in `XK_KERNEL_MODULES` (`kernel/src/kernel_modules/kernel_modules.h`), its entry in `kernel/modules/kernel_modules.cpp`;
  the kernel calls it as `kernel_modules::Modules->Field(...)` and must work when `Modules` is null (no cache). Modules may call any non-static resident
  kernel function, never boot-only code; the link fails on a symbol the kernel does not have (e.g. `isspace`: the module link says `not in the kernel`).
- **What decides the page count:** the kept video encoder tables (`.xka$c*`, spec 18 §7) sit after the resident code; a few hundred bytes more can push
  them across a page boundary and cost a page, a few hundred less can win one. The release kernel's section sizes are in its PE headers
  (`build/kernel/release/oxoskrnl.exe`) and map (`oxoskrnl.map`).
- **Things that are resident without looking it:** string literals (a `const_seg` pragma does not move them; a named `constexpr char[]` inside the pragma
  region does), inline functions and constants a boot-only file shares with resident ones (`tools/check_kernel_layout.py` catches those), `.data` (it is
  also copied for quick reboots: `KI_DATA_COPY_SIZE` in `kernel/src/arch/entry.S`, which the layout check enforces), and `XK_PERSIST` variables (kept across
  quick reboots).
- **The kernel is built `-Oz` on purpose** (free pages for titles); do not switch it to `-O2`.
- **Finding what cost a page:** build the last good commit in a `git worktree` and compare `test_memory` and the section sizes, then move code boot-only or
  into a module.

## Settings files on the console
OXOS's own files are in `C:\OXOS\` (exFAT disk: `/C/OXOS/`), shipped from `kernel/settings/` in the release zip; formats in the specs:
- `oxos_settings.ini`: `[dashboard]` (`dashboard_1..8`, spec 19), `[launch_redirect]` (`name = SOURCE >> TARGET`, spec 33, `docs/launch_redirect.md`),
  `[boot_video]` (spec 38), `[toasts]` (spec 36) and `[storage]` (`show_exfat_as_fatx`, spec 05 §3.1), the last three read at a cold boot only. Read by
  the kernel modules with the parsers in `shared/ini_file/` (host unit tests in `tests/unit/`).
- `oxos_title_patches.ini` (per-game fixes, spec 25 §3); `oxos_extensions.ini` and `extensions\oxos_NAME.ini` (spec 34 §3).
- Safe boot (B held at power-on): no extensions, no boot video, and only `C:\xboxdash.xbe` as the dashboard; launch redirects and title patches still apply.

## Kernel extensions and the SDK
- An extension is a 32-bit PE `.xos` built against `sdk/` (C or C++; `sdk/README.md` walks through one; examples in `sdk/examples/`, OXOS's own in
  `kernel/extensions/`). It hooks kernel exports titles import, gets title events and one run loop, and has a state page that survives quick reboots.
- The kernel loads only those listed with `enabled = yes` in `C:\OXOS\extensions\oxos_extensions.ini`, before each title, and unloads them when the title
  ends. No FPU, MMX or SSE in extension code (the kernel does not save those registers for it).
- The SDK's kernel headers and import library are generated from the kernel (`tools/gen_oxos_sdk.py`, which `make -C kernel test_extensions` runs): after
  changing an export or an `oxos_*` function, regenerate and run `test_extensions`, which loads the test extensions in `tests/extensions/`.

## Boot video
- The player is an nxdk app, `apps/oxos_boot_video/` (`C:\OXOS\apps\oxos_boot_video.xbe`), built on the shared player in `shared/video_player/` (libmpeg2 +
  minimp3, SSE copies, polled AC'97 sound). The kernel starts it in place of the first title and gets the title back after its quick reboot (spec 38 §2).
- Video files: `.mpg` (MPEG-1/2 video, MP2/MP3 sound), 16:9 at 848x480 with the middle 640x480 shown; the ffmpeg command is in `docs/boot_video.md`.
- The release ships OXOS's own boot video, `kernel/splash/oxos_boot_video.mpg` (2.2 MB, the user's), in `OXOS\boot_videos\`, and `oxos_settings.ini` with
  the boot video on.
- `make -C kernel oxos_intro_video` makes OXOS's own test video from the boot splash picture and a tone (never use another BIOS's video); `make -C kernel
  test_boot_video` plays it in xemu (plays, skipped, missing file, off, no player); `video_decode_title` measures decoding speed on a console.

## Reverse engineering: REA writes the spec, the main agent implements it
Behaviour of the stock kernel or EvoX is studied with **REA**, the reverse-engineering MCP server (`mcp__rea__*` tools on Ghidra: `open_binary`,
`search_procedures`, `procedure_pseudo_code`, `xrefs`, `read_bytes`, ...; the `reverse-engineer-anything` skill explains them). The work is split in two, so no
reference code ever reaches OXOS (clean room, [`spec/README.md`](spec/README.md), [`dmca_compliance.md`](dmca_compliance.md)):
**If the `mcp__rea__*` tools are missing** (`claude mcp list` shows no `rea`, or it is not connected), ask the user before installing anything. REA is the
npm package `rea-agents` (MIT, [github.com/morluto/rea](https://github.com/morluto/rea)); it needs Node.js 22.19+ and, for native binaries like the Xbox
kernel, Ghidra with a JDK 21 (this repo's specs used Ghidra 12.1.4):
```sh
npm install --global rea-agents          # the `rea` command (or run it as npx rea-agents ...)
npx rea-agents setup                     # registers the MCP server and its workflow skill with Claude Code (asks first, backs up the config)
# or by hand, for all projects, pointing it at Ghidra and the JDK (and putting the JDK and Node first on its PATH):
claude mcp add rea --scope user -e GHIDRA_INSTALL_DIR=/path/to/ghidra -e JAVA_HOME=/path/to/jdk21 \
    -e PATH=/path/to/jdk21/bin:/path/to/node/bin:/usr/local/bin:/usr/bin:/bin -- rea mcp
```
Restart Claude Code afterwards; `claude mcp list` should show `rea: ... ✔ Connected`. Ghidra's first analysis of a binary takes minutes.

1. **A subagent with REA writes a research spec** into `experimental/` (not `spec/`, and not listed in `spec/README.md`). Once the findings are clear and the
   implementation built from them works, the spec is written into `spec/stock/` or `spec/evox/` (next free number, listed in
   `spec/README.md`) and committed with the implementation.
2. **The main agent implements from the spec only.** It does not open the reference binaries or read REA's output itself; a gap in the spec goes back to
   the subagent as a question, and the answer goes into the spec.

**What a spec may hold:** behaviour, order of steps, conditions and their outcomes, constants, register writes and their values, data formats and file
layouts, error codes, timings, and the reference's addresses as citations (so a later study can check them). Tables over prose.
**Never:** source code, pseudocode or decompiled C, disassembly or instruction listings, byte sequences of code, or anything copied that could be compiled
back. The only data taken from Microsoft images are the encoder and pad calibration tables (`dmca_compliance.md` §5). The model of the level of detail:
[`spec/stock/23_boot_hardware_setup.md`](spec/stock/23_boot_hardware_setup.md) (sources and binary hashes, the code ranges searched, every hardware write in
order with its value, what was checked and ruled out, the versions compared).

**The hand-off prompt** (Agent tool), in short:
```text
Using the REA MCP tools, study <behaviour> in <binary, version, sha256> (compare <other versions>). Write a clean-room behavioural spec to
experimental/<stock|evox>_NN_<name>.md in the style and detail of spec/stock/23_boot_hardware_setup.md: sources and hashes, what was searched, then the
behaviour as numbered steps and tables (conditions, constants, register values, data formats, error paths), citing the reference's addresses. Answer
these questions: <list>. NO code of any kind: no disassembly, no decompiled or pseudocode, no instruction sequences, no code bytes; describe what it does,
not how it is written. Report back: the spec's path, a summary of its findings, and what stays unknown.
```
Then build a proof of concept from that research spec and test it (on xemu, then the console). When the user is good with it: write the spec into `spec/`,
clean the implementation up, cite the `spec/` spec in its comments (`spec/stock/NN §x`, never `experimental/`), and commit both together.

## Run and look at it
- **Interactive:** `make -C kernel run` (xemu with the dashboard disk), or `python3 tools/run.py build/kernel/release/bios.bin --iso GAME.iso`.
- **Screenshots, unattended:** [`tools/xemu_screenshot.py`](tools/xemu_screenshot.py) boots a BIOS in xemu, optionally with a disc and your own files copied
  onto C:, presses controller buttons and saves 640x480 PNGs:
  ```sh
  python3 -I tools/xemu_screenshot.py build/kernel/release/bios.bin /tmp/dash --steps wait:40,shot
  python3 -I tools/xemu_screenshot.py build/kernel/release/bios.bin /tmp/menu --iso GAME.iso \
      --put build/kernel/extensions/oxos_trainers/app/bin/default.xbe='C:\OXOS\extensions\oxos_trainers.xbe' --steps wait:30,shot,DOWN:3,wait:2,shot
  ```
  Steps: `wait:SECONDS`, `shot`, and buttons (`A`, `B`, `X`, `Y`, `BLACK`, `WHITE`, `BACK`, `START`, `LS`, `RS`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `LT`, `RT`,
  with `:N` to repeat); buttons joined by `+` are held together (`BLACK+WHITE`, half a second; `LT+RT+BACK+BLACK@3.5` for 3.5 s).
  - **Output:** `OUT_1.png`, `OUT_2.png`, ... one per `shot`, 640x480. Paths on C: in `--put` use `\` (quote them); the folders are made.
  - **Options:** `--iso` a disc; `--hdd` another disk image (copied, never written; default xemu's dashboard disk); `--eeprom` an EEPROM to start from
    (`--avpack hdtv` and an EEPROM from `run.py --hd --state DIR` for HD modes);
    `--save-hdd FILE` keeps the disk as the console left it (read it with `tests/fatx.py`); `--serial FILE` keeps the debug serial port (with a `SERIAL=1`
    BIOS, `make -C kernel BUILD=../build/kernel/serial SELFTEST=0 SERIAL=1 ../build/kernel/serial/bios.bin`: the kernel's log and titles' `DbgPrint`).
  - **Timing:** a cold boot reaches the dashboard in about 30-40 s in xemu; wait before the first `shot`, and a second or two after each press.
  - **Alongside other xemus** (a regression, another session's tests): fine, the tool grabs only the window of the xemu it started (its `_NET_WM_PID`).
  - **A Microsoft BIOS:** `--mcpx-rom` with the boot ROM (below); `C:\xboxdash.xbe` on the disk is what it starts.
  What works and what does not:
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
| Game folders from disc images | `tools/oxos_isonator/` | for OXOS and any BIOS with the NKPatcher virtual disc device |
| Copy files to and from a console, start an XBE | `apps/oxos_ftp/` (the OXOS FTP server; `SITE LAUNCH /F/path`, `SITE REBOOT`) | Python's `ftplib` in one session is fastest for many files |
| Boot a new kernel on a console, run an XBE straight away | `tools/netboot/` (`netboot.py IMAGE --title DIR --title-name NAME`) | flash the netboot image once |
| Power a console on or off, read its COM1 and POST codes | `tools/modxo/` (`console.py` lists the Modxos; `on`, `off`, `cycle`, `state`, `log` + the Pico's serial number) | the Modxo rig; one logger per Pico |
| See a console's screen | an HDMI capture card with ffmpeg (`-f v4l2 ... -ss 3`) | skip the first 3 s: the card shows colour bars first |
| Kernel extensions | `sdk/` (its README), `kernel/extensions/` (the ones OXOS ships) | spec 34 |
| Trainer pack, trainer ini | `third_party/xbox_trainers/` (its README, `index.csv`), `tools/gen_trainers_ini.py` | spec 37 |
| Boot splash | `tools/svg_splash.py`, `tools/vsplash/` | one picture, faded in, 4 s (spec oxos_internal/00) |
| BIOS image internals | `tools/mkbios.py`, `tools/lzx.py`, `tools/harden_xcodes.py`, `tools/link_kernel_modules.py` | `make` runs them |
| Checks that run after every kernel link | `tools/check_kernel_layout.py`, `tools/check_c_library_use.py` | a failure stops the build |
| Generated sources | `tools/gen_def.py` (exports), `tools/gen_oxos_sdk.py` (SDK), `tools/gen_av_tables.py` (video encoders), `tools/mkucode.py` | regenerate, never edit by hand |

## Commits
- One commit per finished feature, with a message of one short sentence: what changed and why. No body, no lists, no test results; the changed files
  hold the details.
- Generated files are regenerated, not edited (their first line names the tool).

## Pitfalls
- `pkill -f PATTERN` also matches the shell running it when the pattern is in its own command line; match the process name (`pkill -x xemu`) instead.
- A Unix socket path over 107 characters fails (`AF_UNIX path too long`): put sockets in `/tmp`.
- xemu needs `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` to read a controller while its window has no focus (`tests/controller_test.py`, `xemu_env`).
- **xemu sometimes freezes outright** (every thread waiting, 0 % CPU, no QMP answer); a test waiting on it once held a release build for 7 hours. The tests
  give up after 60 s without a QMP answer (`[FAIL] xemu: ... (xemu froze)`) and `make regression` stops a phase after 30 minutes (`tests/README.md`); a
  frozen run is not a kernel failure, run it again.
  - **Anything new that waits gets a limit:** a QMP or socket read (`settimeout`), a subprocess (`timeout=`), a polling loop (a deadline). Never wait for
    ever on xemu, a console or the network.
  - **Watch long background runs** (`make release`, a regression, a console test): check that the log still moves. Unchanged for several minutes: look (`ps`,
    xemu's CPU time) instead of waiting; an AppImage xemu shows as `AppRun` in `ps`, so `pkill -x xemu` misses it: kill the test's xemu by its PID.
