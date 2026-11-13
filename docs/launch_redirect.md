# Launch redirects: start another XBE in place of one a title launches

When a title or a dashboard starts another XBE, OXOS can start a different one in its place. You list the swaps in a
small text file on the hard disk; nothing else changes. For example, the Xbox dashboard's legacy Xbox Live updater
no longer works, and an Xbox Live revival service can have its own updater start instead, without changing the
dashboard.

Needs OXOS v0.4.5 or later. The details and the rules behind them are in
[`spec/oxos_internal/33_launch_redirect.md`](../spec/oxos_internal/33_launch_redirect.md).

## Set it up
1. **Put the replacement XBE on the console**, for example in `E:\Apps\XBLive\`, with the OXOS File Manager
   (X at boot) or FTP.
2. **Write the file** `launch_redirect.ini`, one section per redirect:

   ```ini
   ; Replace the legacy Xbox Live updater with ours
   [Xbox Live updater]
   source = C:\xodash\xonlinedash.xbe
   target = E:\Apps\XBLive\default.xbe
   ```

   - `source`: the XBE titles try to start. `target`: the XBE to start instead.
   - The name in the brackets is yours to choose; it shows in the serial log.
3. **Copy it to `C:\OXOS\launch_redirect.ini`.** On an exFAT disk that is `/C/OXOS/launch_redirect.ini` (the File
   Manager shows it as `/C/OXOS`). Create the `OXOS` folder if it is not there; `title_patches.ini` goes in the same
   folder.
4. **That is all.** The file is read each time a title launches another XBE, so a change counts from the next
   launch, with no reboot.
5. **Try it:** do what starts the original (in the example, choose Xbox Live in the dashboard). Your target starts.

## Writing paths
- Drive letters as titles see them: `C:`, `E:`, `F:`, `G:`, `X:`, `Y:`, `Z:`, and `D:` for the DVD drive (the disc,
  not the launching title's own `D:`). Or a device path such as `\Device\Harddisk0\Partition1\Apps\x.xbe`.
- Letters in any case, `/` or `\`: `e:/apps/xblive/default.xbe` is the same path.
- A path names one XBE: `source = C:\xodash\xonlinedash.xbe` matches only that file.
- At most 359 characters.

## More than one redirect
Add a section for each:

```ini
[Xbox Live updater]
source = C:\xodash\xonlinedash.xbe
target = E:\Apps\XBLive\default.xbe

[My launcher]
source = F:\Apps\old\default.xbe
target = F:\Apps\new\default.xbe
```

If two sections name the same source, the first one counts. Lines starting with `;` or `#` are comments.

## What the target gets
- **`D:` is the target's own folder**, so it finds its files (`D:\media\...`) as when it is started directly.
- **The launching title's launch data, unchanged**: whatever the dashboard (or game) passed to the original XBE
  arrives at the target, with the same title ID and launch type.

## Safe by design
- **No loops:** a target is never redirected again, even if another section names it as a source.
- **A wrong entry does no harm:** if the target is missing or does not load, OXOS starts the original XBE, as if the
  file were not there.
- **Without the file, nothing changes.**

## What is not redirected
Only a title starting another XBE is redirected. These are not:
- what the console starts at power-on (the disc, the dashboard);
- returning to the dashboard (a title that quits without naming an XBE);
- XBEs started from the OXOS File Manager.

## Undo it
Delete the section, or the whole file.

## When it does not work
- Check the file's place and name: `C:\OXOS\launch_redirect.ini` (exFAT: `/C/OXOS/launch_redirect.ini`).
- Check that `source` is exactly the path the title launches: the folder and the file name, for example
  `C:\xodash\xonlinedash.xbe`, not `C:\xodash`.
- Each section needs both a `source =` and a `target =` line, under a `[name]` line.
- A serial-log build of OXOS (`SERIAL=1`, for the debug serial port) logs every redirect
  (`redirect: [name] SOURCE -> TARGET`), every fallback to the source and why, and how many lines of the file it
  could not read.
