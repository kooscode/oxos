# OXOS apps

The console apps OXOS ships: nxdk titles in `C:\OXOS\apps\` on the console (`OXOS\apps\` in the release zip), one folder each here, named as the XBE they
build. Like the kernel extensions in [`../kernel/extensions/`](../kernel/extensions/), each is OXOS's own and is built from this repository; they draw and read
the controllers through SDL3 and the shared text screen in [`../tools/sdl_screen/`](../tools/sdl_screen/), with the build rules in
[`../tools/nxdk_app.mk`](../tools/nxdk_app.mk).

```sh
make -C kernel apps          # every app, into build/apps/<name>/bin/default.xbe
make -C kernel oxos_ftp      # one app (oxos_ftp, oxos_boot_video)
```

| Folder | On the console | What it is |
|---|---|---|
| [`oxos_ftp/`](oxos_ftp/) | `C:\OXOS\apps\oxos_ftp.xbe` | The OXOS FTP server: the console's disks over the network, any FTP client; on OXOS it also formats the disk as exFAT (`SITE FORMAT-EXFAT`) and starts titles (`SITE LAUNCH`). |
| [`oxos_boot_video/`](oxos_boot_video/) | `C:\OXOS\apps\oxos_boot_video.xbe` | The boot video player: the kernel starts it before the first title when `C:\OXOS\oxos_settings.ini` asks for a boot video; it plays the `.mpg` with the shared player ([`../shared/video_player/`](../shared/video_player/)) and quick-reboots into the title ([spec 38](../spec/oxos_internal/38_boot_video.md)). `make -C kernel oxos_boot_video`; `make -C kernel oxos_intro_video` makes the OXOS boot video from the boot splash. |

Planned: `oxos_settings/` (OXOS Setup, [spec 39](../spec/oxos_internal/39_oxos_setup.md)), which will preview boot videos with the same player.
