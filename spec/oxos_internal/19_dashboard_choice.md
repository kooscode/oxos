# 19. Which dashboard starts

OXOS addition, implemented 2026-10-27 (a fixed list); since 2026-11-17 the list is the user's, in `C:\OXOS\oxos_settings.ini` (user: "default to standard
msdash, like stock does, and the oxos ini has the boot order"). Code: `kernel/src/titles/title_loader.cpp` (`titles::LaunchDashboard`), the dashboard
resolver module `kernel/modules/dashboard_resolver.cpp` (`LoadListedDashboard`, spec 43), `shared/ini_file/settings_ini.cpp` (`oxos::ini_file::FindDashboard`).

## 1. The order
Wherever the kernel starts the dashboard (no disc, the tray open, a title that failed to load, a title asking for the dashboard, spec/stock/05), it tries, and
starts the first that loads:
1. **The dashboards `[dashboard]` lists**, `dashboard_1` to `dashboard_8` in that order (a number missing is skipped), from `C:\OXOS\oxos_settings.ini`
   (exFAT disk: `/C/OXOS/oxos_settings.ini`):

   ```ini
   [dashboard]
   ; tried in this order; the first one on the disk starts
   dashboard_1 = C:\evoxdash.xbe
   dashboard_2 = E:\evoxdash.xbe
   dashboard_3 = C:\xboxdash.xbe
   dashboard_4 = C:\Dashboard\default.xbe
   ```

   A path starts with a drive letter, as in the launch redirect file (spec 33 §1: C: E: F: G: X: Y: Z:, D: the DVD drive; `/` may stand for `\`; quotes
   around it are allowed); a path on another drive, or an empty one, is skipped. A key given twice: the last one counts.
2. **`C:\xboxdash.xbe`**, the Xbox dashboard, as the stock kernel starts it: also when the file or the section is missing, and in safe boot (B held at a cold
   boot, spec 34 §3.3), which reads no ini file.

A missing file fails at its open, before anything is loaded, so the next is tried. When none loads, the kernel behaves as before: on a cold boot the "No
Dashboard Found" screen (spec 32 §1; under its title `No dashboard could be loaded.` / `Check C:\OXOS\oxos_settings.ini` / `or add the default
C:\xboxdash.xbe.`, and X for the File Manager), a return to the firmware otherwise. The dashboard gets the same launch data page (reason and code) whichever it
is.

## 2. Reading the file
- The file is read at **every** dashboard start (a cold boot, and every quick reboot to the dashboard: a title returning to it, the boot video's end), with
  the kernel's ini reader, so a change applies at the next dashboard start. The kernel reads at most 64 KB of it.
- The lookup is the dashboard resolver, a kernel module (`kernel/modules/dashboard_resolver.cpp`, spec 43): mapped at every launch, quick reboots included,
  not resident. Without the kernel modules (no cache) only `C:\xboxdash.xbe` is tried. The `[boot_video]` part of the same file is read once at a cold boot
  by the kernel settings module (spec 38 §2).
- **Memory:** a first version kept the lookup resident and cost titles one page (test_memory 15 959, 2026-11-17, accepted by the user for the moment); as a
  kernel module it costs nothing (15 960 again).

## 3. The release's file
The release zip ships `OXOS\oxos_settings.ini` (from `kernel/settings/oxos_settings.ini`) listing the dashboards OXOS used to try on its own, in the old
order: `C:\evoxdash.xbe`, `E:\evoxdash.xbe`, `C:\xboxdash.xbe`, `C:\Dashboard\default.xbe` (where the XBL Dashboard installs its loader), and the boot video
off (spec 38). A console whose owner keeps an older `C:\OXOS` without this file starts only `C:\xboxdash.xbe`: an EvoX or XBL Dashboard console then needs the
file (or the section) copied.

Until 2026-11-17 that list was fixed in the kernel (EvoX first; the XBL loader added 2026-11-08 as `E:\XBLDashboard\default.xbe`, moved to `C:\Dashboard` the
same day, where the installer puts it).

## 4. Tests
`make test_dash` boots the xemu dashboard disk four times:
- as it is, no `oxos_settings.ini`: `XeImageFileName` is `C:\xboxdash.xbe`;
- with a copy of its dashboard as `C:\evoxdash.xbe` but no `[dashboard]` (`boot_test.py --dash --evox`): still `C:\xboxdash.xbe`;
- the same with `[dashboard]` listing `C:\evoxdash.xbe, C:\Dashboard\default.xbe` (`--dash-order`): `C:\evoxdash.xbe`;
- with `C:\xboxdash.xbe` moved to `C:\Dashboard\default.xbe` and the same list (`--dash --xbl --dash-order`): the missing EvoX is skipped and the XBL loader
  starts.

Host unit tests: `tests/unit/test_settings_ini.cpp` (the lookup: the shipped file, numbers outside 1-8, keys only in their section, unknown drives).
