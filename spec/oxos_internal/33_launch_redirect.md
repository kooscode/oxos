# 33. Launch redirects (OXOS's own)

Status: implemented 2026-11-12. Code: `kernel/src/titles/launch_redirect.cpp` (the file's parser,
`include/xk/launch_redirect.h`), on the ini line reader it shares with title_patches.ini
(`kernel/src/titles/ini_file.cpp`, `include/xk/ini_file.h`, spec/oxos_internal/25 §3.1);
`kernel/src/titles/loader.cpp` (`XepRedirectLaunch`, in `XepLaunchTitle`, and `XepReadOxosFile`, which reads both
files).

When a title starts another XBE (`XLaunchNewImage`, `XLaunchXBE`), the kernel can start a different XBE in its
place, named in a file on the hard disk. The first user: the XBL team replaces the Xbox dashboard's legacy Xbox Live
updater (`C:\xodash\xonlinedash.xbe`), which no longer works, with their own, without changing the dashboard.

User guide: [`docs/launch_redirect.md`](../../docs/launch_redirect.md).

## 1. The file
`C:\OXOS\launch_redirect.ini`, read through `\Device\Harddisk0\Partition2`, next to `title_patches.ini`
(spec/oxos_internal/25 §3.1): C: on a FATX disk, `/C/OXOS/launch_redirect.ini` on an exFAT disk. Text, at most
64 KB, lines ending in LF or CR LF:

```ini
; a comment (a line of its own)
[Xbox Live updater]                     ; a section: one redirect (the name is shown in the serial log)
source = C:\xodash\xonlinedash.xbe      ; the XBE a title asks for
target = E:\Apps\XBLive\default.xbe     ; the XBE started instead
```

- **Sections:** each holds one `source =` and one `target =` line. The name in the brackets is free text. A section
  without both lines is skipped and counted as an error.
- **Paths:** a drive letter path or a device path, `/` or `\`, at most 359 characters:
  - `C:` `E:` `F:` `G:` `X:` `Y:` `Z:` are the hard disk's partitions, as titles see them (`C:` is
    `\Device\Harddisk0\Partition2`, `E:` Partition1, `X:`–`Z:` 3–5, `F:` 6, `G:` 7);
  - `D:` is the DVD drive, `\Device\CdRom0` (in the file, not the launching title's own D:);
  - `\Device\...` paths are used as they are.
- **Matching:** a source matches when it names the same path as the launch, ignoring the case of letters. The first
  section whose source matches decides.
- **Comments:** a line starting with `;` or `#`. The value of `source =` and `target =` is the rest of its line
  (blanks around it removed), so a path may hold `;`. Other keys are ignored, for later additions.
- **Errors:** a line that cannot be read is skipped and counted; the count goes to the serial log. A broken section
  header (no `]`, or text after it that is not a comment) ends the section before it, and the lines after it belong
  to no section until the next good header (each `source =` or `target =` there is counted too).

## 1.1 Memory
The parser and the launch-time code add about 1.5 KB to the resident kernel. To keep every page titles had, the disk
checks of a cold boot moved to the boot-only pages in the same change (spec/oxos_internal/18 §2, 30 §4): the memory
test title still starts with 15 960 free pages (xemu, 64 MB), as before this feature.

## 2. When the kernel redirects
- **Only launches through the launch data page** (spec/stock/05 §2 A): a title or a dashboard named an XBE and
  quick-rebooted. The path in the page (`\Device\...\folder;name.xbe`) is looked up as `\Device\...\folder\name.xbe`.
- **Not redirected:** a cold boot (the disc, the dashboard, a netbooted title), a return to the dashboard (a page
  with no path, `XLaunchNewImage(NULL)`), and launches from the OXOS File Manager.
- **Once per launch:** the target is not looked up again, so two sections cannot send a launch around in a loop.
- **No file, no matching section:** the launch goes ahead unchanged. The file is read at each launch through the page,
  so a changed file counts from the next launch.

## 3. What the target gets
- **D:** the target's own folder (as for any launch: `\??\D:` is linked to the folder the image is loaded from).
- **The launch data page, unchanged:** its type, title ID, the launching title's data and its path. The target can
  read what the launching title meant to hand over (the page's `LaunchData`), as the original XBE would have.
- `XeImageFileName` is the target's path.

## 4. When the target cannot be started
If the target does not load (missing, not an XBE, refused by the loader), the kernel logs why and starts the source
as the title asked. A wrong entry therefore never leaves the console without the original program. If the source
does not load either, the usual fallback follows (the dashboard with reason 1, spec/stock/05 §2).

## 5. Serial log (SERIAL=1 builds)
- `redirect: [NAME] SOURCE -> TARGET` when a section matches;
- `redirect: target did not load (STATUS); starting SOURCE` for §4;
- `redirect: launch_redirect.ini: N lines not understood` when the file has errors.

## 6. Tests
- Host unit tests `tests/unit/test_launch_redirect.cpp`: drive letters, device paths, `/`, case, comments, sections
  without a source or target, the first match deciding, paths too long, CR LF; `tests/unit/test_ini_file.cpp`: the
  shared line reader.
- `make test_relaunch` (boot_test.py `--redirect`): the test disk's `C:\OXOS\launch_redirect.ini` sends the kernel
  test title's relaunch (`D:\media\second.xbe`) to `D:\redirected\second.xbe` on the same disc, and its third launch
  (`D:\media\missing.xbe`, which does not exist) to a target that does not exist either. Checked: the kernel loaded
  the target (`XeImageFileName`), the first run's results arrived through the unchanged launch data, D: is linked (the
  title reads `D:\a.txt`, which the target's folder holds too), and the missing launch still ends at the dashboard
  with reason 1 (§4: the target failed, then the source). With a SERIAL=1 kernel the log shows both redirects and the
  fallback (§5).
- Debug kit (2026-11-12, exFAT disk, the FTP server's SITE LAUNCH): a redirect from one XBE in an F: folder to another
  started the target, which read its picture from D: (its own folder); a section whose target does not exist started
  the source; the serial log showed both (§5). The memory test title had the same free pages as with the kernel
  before this change (15 954, 128 MB console, 64 MB XBE).
