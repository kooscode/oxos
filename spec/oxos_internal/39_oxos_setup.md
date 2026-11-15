# 39. OXOS Setup: one place for every OXOS setting (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "an oxos custom global menu/config/dash of some sort that if you hold a button (maybe A) it will boot
into our setup/config/dash where you can manage all kinds of kernel options like the enabling of extensions, trainers, primary dashboard (we hardcode them
now)"). Nothing here is decided until the user says so.

OXOS's settings are plain files in `C:\OXOS\` today, edited by hand over FTP or the File Manager: `oxos_extensions.ini`, `oxos_launch_redirect.ini`,
`oxos_title_patches.ini`, and soon `oxos_trainers.ini` (spec 37). Some choices have no file at all: which dashboard starts is a fixed list in the kernel (spec
19). OXOS Setup is one screen for all of them, opened by holding a button at boot.

## 1. What the user sees
- **Hold A during the boot splash** (X opens the File Manager, spec 32; B is safe mode, spec 34 §3.3; A is free): OXOS Setup starts instead of the dashboard.
  The splash's label says so, as it does for X.
- A menu of pages, driven by the controller, in the OXOS look (the File Manager's and error screens' font and colours):

| Page | What it manages | Stored in |
|---|---|---|
| Dashboard | which dashboard starts, in order; replaces the fixed list of spec 19 (the list stays the default) | `oxos_settings.ini` (new) |
| Extensions | each one on/off, its order, its settings (the ini keys of its section) | `oxos_extensions.ini` |
| Trainers | per title: trainer on/off, its cheats, ask before the game or not (spec 37) | `oxos_trainers.ini` |
| Boot | the boot video on cold boot / quick reboot (spec 38), the jingle on/off | `oxos_extensions.ini`, `oxos_settings.ini` |
| Titles | title patches on/off, launch redirects (list, add, remove) | `oxos_title_patches.ini`, `oxos_launch_redirect.ini` |
| Video and audio | the console's own settings: video standard, widescreen, 480p/720p/1080i, audio | the EEPROM (as the Xbox dashboard sets them) |
| Toasts, in-game reset | toasts on/off (spec 36), the in-game reset on/off | `oxos_settings.ini` |
| Disk | cache partitions (X/Y/Z) cleared, disk format (spec 30), free space | the disk |
| Network | the File Manager and the FTP server: start, address | |
| System | OXOS version, console revision and encoder, MCPX, memory, disk, temperatures, the last log lines (spec 35) | read only |

- Every page writes the same plain files a user could edit by hand: Setup is a view of the files, not a second store. A file it does not understand is left as
  it is.
- Done: back to the boot (the dashboard), or a reboot when a setting needs one.

## 2. How it could be built
| | Built into the kernel (as the File Manager is) | An XBE in `C:\OXOS\` (`oxos_setup.xbe`), started by the kernel |
|---|---|---|
| Works on a blank or broken disk | yes | no: the File Manager stays the way out (spec 32) |
| Kernel size and title memory | grows the kernel; the code must be boot-only (freed before titles, spec 18) | none |
| Look and abilities | the kernel's own text screens | nxdk + SDL3 (`tools/sdl_screen`), like the other OXOS apps: richer, quicker to build |
| Updating it | a BIOS flash | copy a file |

The XBE is the likely choice, with the kernel's part kept small: read A at boot, start `C:\OXOS\oxos_setup.xbe`, and fall back to the File Manager when it is
missing. The release zip would ship it next to `ftp.xbe`.

## 3. Kernel changes it needs
- A at boot (`kernel/src/gamepad/gamepad.cpp`, as X and B).
- `oxos_settings.ini`: the kernel reads the dashboard order (spec 19) and the other kernel options from it at boot, with today's behaviour as the default when
  the file or a key is missing.
- EEPROM settings are written the way the Xbox dashboard writes them (`ExSaveNonVolatileSetting`, its checksums).

## 4. Open
- The button: A (proposed), or a combination, to avoid starting Setup by accident.
- Whether Setup also replaces the File Manager's screen as the home of disk tools, or links to it.
- Settings that need a reboot (dashboard order no, extensions yes, video mode yes): Setup says which.
- A PIN or lock for the pages that format disks.

## 5. Tests
- xemu: A held on a virtual pad (`tests/virtual_gamepad.py`, as `make test_upload` holds X) starts Setup; a page changed writes its file; the next boot uses it
  (the dashboard order, an extension switched off).
- Host: unit tests of the ini editing (keys changed, unknown lines and comments kept).
