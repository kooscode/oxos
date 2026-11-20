# 39. OXOS Setup: one place for every OXOS setting (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "an oxos custom global menu/config/dash of some sort that if you hold a button (maybe A) it will boot
into our setup/config/dash where you can manage all kinds of kernel options like the enabling of extensions, trainers, primary dashboard (we hardcode them
now)"). Nothing here is decided until the user says so.

OXOS's settings are plain files in `C:\OXOS\` today, edited by hand over FTP or the File Manager: `oxos_settings.ini` (the dashboards, spec 19; the boot
video, spec 38), `oxos_extensions.ini`, `oxos_title_patches.ini`, and soon each extension's own ini file in
`C:\OXOS\extensions\` (spec 34 §3.1). OXOS Setup is one screen for all of them, opened by holding a button at boot.

## 1. What the user sees
- **Hold A during the boot splash** (X opens the File Manager, spec 32; B is safe mode, spec 34 §3.3; A is free): OXOS Setup starts instead of the dashboard.
  The splash's label says so, as it does for X.
- A menu of pages, driven by the controller, in the OXOS look (the File Manager's and error screens' font and colours):

| Page | What it manages | Stored in |
|---|---|---|
| Dashboard | which dashboards start, in order (spec 19: `dashboard_1` ... `dashboard_8`, then `C:\xboxdash.xbe`) | `oxos_settings.ini` `[dashboard]` (spec 38 §1.1) |
| Extensions | each one on/off, its order, its settings (the ini keys of its section) | `oxos_extensions.ini` |
| Trainers | per title: trainer on/off, its cheats, ask before the game or not (spec 37) | `extensions\oxos_trainers.ini` |
| Boot | the boot video: on/off, the file, after every reboot or cold boots only (spec 38); the splash on/off, from the next cold boot (§1.1) | `oxos_settings.ini` `[boot_video]`; the EEPROM |
| Titles | title patches on/off, launch redirects (list, add, remove) | `oxos_title_patches.ini`, `oxos_settings.ini` `[launch_redirect]` |
| Video and audio | the console's own settings: video standard, widescreen, 480p/720p/1080i, audio | the EEPROM (as the Xbox dashboard sets them) |
| Toasts, in-game reset | toasts on/off (spec 36), the in-game reset on/off | `oxos_settings.ini` |
| Disk | cache partitions (X/Y/Z) cleared, disk format (spec 30), free space | the disk |
| Network | the File Manager and the FTP server: start, address | |
| System | OXOS version, console revision and encoder, MCPX, memory, disk, temperatures, the last log lines (spec 35) | read only |

- Every page writes the same plain files a user could edit by hand: Setup is a view of the files, not a second store. A file it does not understand is left as
  it is.
- Done: back to the boot (the dashboard), or a reboot when a setting needs one.

### 1.1 Buttons at boot, splash or not (user, 2026-11-15)
The buttons held at a cold boot work whether the splash shows or not: the kernel reads the controllers from the moment USB is up, while the hard disk spins up,
so skipping the splash adds no wait (and they would work without an AV pack too, which today reads no button).

| Held at a cold boot | What happens |
|---|---|
| nothing | the usual boot: the splash unless skipped in the EEPROM (below), extensions, `oxos_settings.ini`, the dashboard or the disc |
| **X** only | the File Manager (spec 32) at once |
| **B** | **safe boot:** every customization is skipped: no extensions, none of OXOS's ini files (`oxos_settings.ini`, `oxos_extensions.ini` and the extensions' own, `oxos_title_patches.ini`), and OXOS's EEPROM settings ignored. The standard splash shows, with **`[ SAFE MODE ]`** in yellow at the top centre; the dashboards are tried in the built-in order (spec 19). Safe boot lasts until the next cold boot (as safe mode does today, spec 34 §3.3) |
| **B**, then **X** held during the safe boot's splash | safe boot, then the File Manager |
| **B**, then **A** held during the safe boot's splash | safe boot, then OXOS Setup (`C:\OXOS\apps\oxos_settings.xbe`), to put a setting right |
| **A** | OXOS Setup |

Safe boot is wider than today's safe mode, which skips only the extensions and keeps launch redirects (spec 34 §3.3, §10): with B, nothing OXOS reads from the
disk or the EEPROM is used, so a console always starts the way a fresh OXOS install does.

**Skipping the splash:** one OXOS byte in the EEPROM, outside the sections the kernel uses (security up to 0x2F, factory 0x30–0x5F, user 0x60–0xBF; which
byte of 0xC0–0xFF is free of other dashboards and BIOSes is to check): bit 0 no splash (bit 1 was to be "no jingle"; the jingle was removed 2026-11-17). The
kernel reads the EEPROM before the splash
(`hardware::chipset::InitEeprom` comes before `boot_splash::StartSplash` in `core/startup.cpp`), so a change applies at the next boot. OXOS Setup's Boot page
sets it through one kernel call; apps do not write EEPROM bytes themselves. It stays with the console when the disk is changed, and other BIOSes ignore it.

**Writing it safely.** An nxdk app runs in kernel mode and could write any EEPROM byte itself (`HalWriteSMBusValue` to the EEPROM at SMBus address 0xA8),
but a wrong offset can damage the security section (0x00–0x2F, which holds the encrypted hard-disk key: lost, the console cannot unlock its own drive) or a
section checksum (factory or user settings reset, wrong video standard or region). So:
- **One kernel call writes only OXOS's byte**, at the offset the kernel knows: OXOS Setup asks for "skip the splash", never for an offset. It is
  either a new OXOS export or an OXOS-only setting number of `ExSaveNonVolatileSetting`, which another kernel refuses instead of writing anything.
- **The EEPROM is backed up first:** the first time OXOS Setup runs, it saves all 256 bytes to `C:\OXOS\oxos_eeprom_backup.bin` (and keeps an existing one).
  That file is what saves a console whose EEPROM is ever damaged, by OXOS or anything else.
- **The byte is checked to be free** before it is chosen: none of the stock dashboard, XBL Dashboard, EvoX, XBMC4Gamers or the common EEPROM tools may
  write it.
- A power cut during the write can only affect that byte (each takes a few milliseconds); the kernel treats a value it does not recognise as "everything on".
  Wear is no concern: EEPROMs are rated for about a million writes per byte.

## 2. How it could be built
| | Built into the kernel (as the File Manager is) | An XBE in `C:\OXOS\apps\` (`oxos_settings.xbe`), started by the kernel |
|---|---|---|
| Works on a blank or broken disk | yes | no: the File Manager stays the way out (spec 32) |
| Kernel size and title memory | grows the kernel; the code must be boot-only (freed before titles, spec 18) | none |
| Look and abilities | the kernel's own text screens | nxdk + SDL3 (`tools/sdl_screen`), like the other OXOS apps: richer, quicker to build |
| Updating it | a BIOS flash | copy a file |

The XBE is the likely choice, with the kernel's part kept small: read A at boot, start `C:\OXOS\apps\oxos_settings.xbe`, and fall back to the File Manager when it is
missing. The release zip would ship it in `OXOS\apps\`, next to the FTP server (`oxos_ftp.xbe`).

## 3. Kernel changes it needs
- A at boot (`kernel/src/gamepad/gamepad.cpp`, as X and B).
- `oxos_settings.ini`, the one file for the kernel's own options: the kernel reads the dashboards (spec 19) and the boot video (spec 38) from it today; the
  other kernel options will join it, with today's behaviour as the default when the file or a key is missing.
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
