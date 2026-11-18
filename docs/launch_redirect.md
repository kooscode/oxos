# Launch redirects: start another XBE in place of one a title launches

When a title or a dashboard starts another XBE, OXOS can start a different one in its place. You list the swaps in OXOS's settings file on the hard disk;
nothing else changes. For example, the Xbox dashboard's legacy Xbox Live updater no longer works, and an Xbox Live revival service can have its own updater
start instead, without changing the dashboard.

Needs OXOS v0.4.5 or later. Since v0.4.13 the redirects are a `[launch_redirect]` section of `C:\OXOS\oxos_settings.ini`; up to v0.4.12 they were a file of
their own, `oxos_launch_redirect.ini` (`launch_redirect.ini` in v0.4.5 and v0.4.6), which is no longer read: move its entries into the new section (below).
The details and the rules behind them are in [`spec/oxos_internal/33_launch_redirect.md`](../spec/oxos_internal/33_launch_redirect.md).

## Set it up
1. **Put the replacement XBE on the console**, for example in `E:\Apps\XBLive\`, with the OXOS File Manager (X at boot) or FTP.
2. **Add the redirect to `C:\OXOS\oxos_settings.ini`** (exFAT: `/C/OXOS/oxos_settings.ini`; the release zip ships the file), in its `[launch_redirect]`
   section:

   ```ini
   [launch_redirect]
   ; Replace the legacy Xbox Live updater with ours
   xbox_live_updater = C:\xodash\xonlinedash.xbe >> E:\Apps\XBLive\default.xbe
   ```

   - One line per redirect: `name = SOURCE >> TARGET`. SOURCE is the XBE titles try to start, TARGET the XBE to start instead.
   - The name is yours to choose, one word (no blanks: `my_xbe_redirect`); it shows in the serial log.
3. **That is all.** The file is read each time a title launches another XBE, so a change counts from the next launch, with no reboot.
4. **Try it:** do what starts the original (in the example, choose Xbox Live in the dashboard). Your target starts.

## Writing paths
- Drive letters as titles see them: `C:`, `E:`, `F:`, `G:`, `X:`, `Y:`, `Z:`, and `D:` for the DVD drive (the disc, not the launching title's own `D:`). Or a
  device path such as `\Device\Harddisk0\Partition1\Apps\x.xbe`.
- Letters in any case, `/` or `\`: `e:/apps/xblive/default.xbe` is the same path.
- A path names one XBE: `C:\xodash\xonlinedash.xbe` matches only that file.
- At most 359 characters.

## More than one redirect
One line each:

```ini
[launch_redirect]
xbox_live_updater = C:\xodash\xonlinedash.xbe >> E:\Apps\XBLive\default.xbe
my_launcher = F:\Apps\old\default.xbe >> F:\Apps\new\default.xbe
```

If two lines name the same source, the first one counts. Lines starting with `;` or `#` are comments.

## What the target gets
- **`D:` is the target's own folder**, so it finds its files (`D:\media\...`) as when it is started directly.
- **The launching title's launch data, unchanged**: whatever the dashboard (or game) passed to the original XBE arrives at the target, with the same title ID
  and launch type.

## Safe by design
- **No loops:** a target is never redirected again, even if another line names it as a source.
- **A wrong entry does no harm:** if the target is missing or does not load, OXOS starts the original XBE, as if the entry were not there.
- **Without the section, nothing changes.**

## What is not redirected
Only a title starting another XBE is redirected. These are not:
- what the console starts at power-on (the disc, the dashboard);
- returning to the dashboard (a title that quits without naming an XBE);
- XBEs started from the OXOS File Manager.

## Undo it
Delete the line (or put `;` in front of it), or the whole section.

## When it does not work
- Check the file's place and name: `C:\OXOS\oxos_settings.ini` (exFAT: `/C/OXOS/oxos_settings.ini`), and that the lines are under `[launch_redirect]`.
- Check that the source is exactly the path the title launches: the folder and the file name, for example `C:\xodash\xonlinedash.xbe`, not `C:\xodash`.
- Each line needs a one-word name, `=`, the source, `>>` and the target.
- A serial-log build of OXOS (`SERIAL=1`, for the debug serial port) logs every redirect (`redirect: [name] SOURCE -> TARGET`), every fallback to the source
  and why, and how many lines of the section it could not read.
