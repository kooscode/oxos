# 33. Launch redirects (OXOS's own)

Status: implemented 2026-11-12; the file renamed from `launch_redirect.ini` to `oxos_launch_redirect.ini` on 2026-11-13 (OXOS's own files get the `oxos_`
prefix); merged into `oxos_settings.ini` as its `[launch_redirect]` section on 2026-11-17 (§1). Code: the launch redirect module
(`kernel/modules/launch_redirect.cpp`, a kernel module since 2026-11-17, spec/oxos_internal/43: reads the file and finds the target) with the parser
`shared/ini_file/launch_redirect_ini.cpp` (`shared/ini_file/launch_redirect_ini.h`), on the ini line reader it shares with oxos_title_patches.ini
(`shared/ini_file/ini_file.cpp`, spec/oxos_internal/25 §3.1); `kernel/src/titles/title_loader.cpp` (`titles::RedirectLaunch`, in `titles::LaunchTitle`, loads
the target; `titles::ReadOXOSFile` reads the files). Without the kernel modules (no cache) there is no redirect at that launch.

When a title starts another XBE (`XLaunchNewImage`, `XLaunchXBE`), the kernel can start a different XBE in its place, named in `C:\OXOS\oxos_settings.ini`. The
first user: the XBL team replaces the Xbox dashboard's legacy Xbox Live updater (`C:\xodash\xonlinedash.xbe`), which no longer works, with their own, without
changing the dashboard.

User guide: [`docs/launch_redirect.md`](../../docs/launch_redirect.md).

## 1. The section
Since 2026-11-17 (user: "move launch_redirect.ini options into oxos_settings.ini as a launch redirect section") the redirects are the `[launch_redirect]`
section of `C:\OXOS\oxos_settings.ini` (spec 19, 38: OXOS's one settings file), read through `\Device\Harddisk0\Partition2`: C: on a FATX disk,
`/C/OXOS/oxos_settings.ini` on an exFAT disk. Until then they were sections of a file of their own, `C:\OXOS\oxos_launch_redirect.ini`, which is no longer
read. Text, at most 64 KB, lines ending in LF or CR LF:

```ini
[launch_redirect]
; a comment (a line of its own)
; name = the XBE a title asks for >> the XBE started instead
xbox_live_updater = C:\xodash\xonlinedash.xbe >> E:\Apps\XBLive\default.xbe
```

- **Lines:** one redirect per line, `name = SOURCE >> TARGET` (user, 2026-11-17: "separate the source and target in a single setting, the name can be
  anything, an easy pattern like >>"). The name is the key: any one word (the ini reader's keys have no blanks), shown in the serial log, cut to 63
  characters there; names need not differ. The value is split at its first `>>`; the blanks around each path are removed. No limit on the number of lines.
- **Paths:** a drive letter path or a device path, `/` or `\`, at most 359 characters:
  - `C:` `E:` `F:` `G:` `X:` `Y:` `Z:` are the hard disk's partitions, as titles see them (`C:` is `\Device\Harddisk0\Partition2`, `E:` Partition1, `X:`–`Z:`
    3–5, `F:` 6, `G:` 7);
  - `D:` is the DVD drive, `\Device\CdRom0` (in the file, not the launching title's own D:);
  - `\Device\...` paths are used as they are.
- **Matching:** a source matches when it names the same path as the launch, ignoring the case of letters. The first line whose source matches decides.
- **Comments:** a line starting with `;` or `#`. The value is the rest of its line (blanks around it removed), so a path may hold `;`. Lines outside the
  section, in other sections, are not read for redirects.
- **Errors**, counted for the serial log: a line of the section that cannot be read (no `=`, a key with blanks), no `>>`, and a source or target that is
  empty, too long or names no drive OXOS knows. Such a line is skipped.

## 1.1 Memory
The parser and the lookup are a kernel module (spec 43, since 2026-11-17): no resident cost. Until then they added about 1.5 KB to the resident kernel, paid
for by moving the disk checks of a cold boot to the boot-only pages (spec/oxos_internal/18 §2, 30 §4).

## 2. When the kernel redirects
- **Only launches through the launch data page** (spec/stock/05 §2 A): a title or a dashboard named an XBE and quick-rebooted. The path in the page
  (`\Device\...\folder;name.xbe`) is looked up as `\Device\...\folder\name.xbe`.
- **Not redirected:** a cold boot (the disc, the dashboard, a netbooted title), a return to the dashboard (a page with no path, `XLaunchNewImage(NULL)`), and
  launches from the OXOS File Manager.
- **Once per launch:** the target is not looked up again, so two redirects cannot send a launch around in a loop.
- **Extensions** can redirect such a launch too, from the title-loading event (`oxos_title_redirect`, spec/oxos_internal/34 §7.2): after this file, when no
  redirect matched. The target is loaded the same way (§3, §4).
- **No file, no section, no matching redirect, or no kernel modules:** the launch goes ahead unchanged. The file is read at each launch through the page, so a
  change counts from the next launch.

## 3. What the target gets
- **D:** the target's own folder (as for any launch: `\??\D:` is linked to the folder the image is loaded from).
- **The launch data page, unchanged:** its type, title ID, the launching title's data and its path. The target can read what the launching title meant to hand
  over (the page's `LaunchData`), as the original XBE would have.
- `XeImageFileName` is the target's path.

## 4. When the target cannot be started
If the target does not load (missing, not an XBE, refused by the loader), the kernel logs why and starts the source as the title asked. A wrong entry therefore
never leaves the console without the original program. If the source does not load either, the usual fallback follows (the dashboard with reason 1,
spec/stock/05 §2).

## 5. Serial log (SERIAL=1 builds)
- `redirect: [NAME] SOURCE -> TARGET` when a line matches;
- `redirect: target did not load (STATUS); starting SOURCE` for §4;
- `redirect: oxos_settings.ini: N lines not understood` when the section has errors.

## 6. Tests
- Host unit tests `tests/unit/test_launch_redirect_ini.cpp`: drive letters, device paths, `/`, case, comments, lines without `>>` or a path, blanks around
  `>>`, the first match deciding, the section among others, paths too long, CR LF; `tests/unit/test_ini_file.cpp`: the shared line reader.
- `make test_relaunch` (boot_test.py `--redirect`): the test disk's `C:\OXOS\oxos_settings.ini` `[launch_redirect]` sends the kernel test title's relaunch
  (`D:\media\second.xbe`) to `D:\redirected\second.xbe` on the same disc, and its third launch (`D:\media\missing.xbe`, which does not exist) to a target that
  does not exist either. Checked: the kernel loaded the target (`XeImageFileName`), the first run's results arrived through the unchanged launch data, D: is
  linked (the title reads `D:\a.txt`, which the target's folder holds too), and the missing launch still ends at the dashboard with reason 1 (§4: the target
  failed, then the source). With a SERIAL=1 kernel the log shows both redirects and the fallback (§5).
- Debug kit (2026-11-12, exFAT disk, the FTP server's SITE LAUNCH): a redirect from one XBE in an F: folder to another started the target, which read its
  picture from D: (its own folder); a section whose target does not exist started the source; the serial log showed both (§5). The memory test title had the
  same free pages as with the kernel before this change (15 954, 128 MB console, 64 MB XBE).
