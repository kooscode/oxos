# Boot video: play a video of your own after the boot logo

OXOS plays a video after its boot logo, with sound, before the dashboard or the disc starts. Any button on any controller skips it. Since v0.4.13 the
release zip ships OXOS's own boot video (`OXOS\boot_videos\oxos_boot_video.mpg`, 8 s) and turns it on in `OXOS\oxos_settings.ini`. The details are in
[`spec/oxos_internal/38_boot_video.md`](../spec/oxos_internal/38_boot_video.md).

**Turn it off:** `boot_video_enabled = no` in `C:\OXOS\oxos_settings.ini`. **Use your own:** make it as below, put it next to the OXOS video and name it in
`boot_video_file`.

## 1. Make the video
The Xbox decodes video in software, so phone and YouTube videos (H.264 MP4) are too heavy for it. Convert yours once to MPEG-2 in a `.mpg` file:

```sh
ffmpeg -i in.mp4 -vf "scale=848:480:force_original_aspect_ratio=increase,crop=848:480" -r 30 -c:v mpeg2video -q:v 3 -c:a mp2 -ar 48000 -b:a 192k my_intro.mpg
```

- **Any input works:** MP4, MOV, AVI, WMV, whatever ffmpeg reads.
- **16:9, 848x480:** the screen shows the middle 640x480 (4:3); the sides are cut off. Keep what matters in the middle.
- **At most 40 MB.** A short clip (5 to 15 seconds) is best: the boot waits for it.
- **Smaller file:** `-q:v 8` instead of 3 (the OXOS video: 2.2 MB for 8 s).
- **Loud sound:** a track mastered right up to full scale can overload a TV or an HDMI adapter; add `-af volume=-6dB`.
- MPEG-1 video and MP3 sound play too; other sizes are scaled.

## 2. Put it on the console
1. Copy `my_intro.mpg` to `C:\OXOS\boot_videos\` (exFAT disk: `/C/OXOS/boot_videos/`), with the OXOS File Manager (X at boot) or FTP.
2. Check that `C:\OXOS\apps\oxos_boot_video.xbe` is there (the release zip has it).
3. In `C:\OXOS\oxos_settings.ini`:

   ```ini
   [boot_video]
   boot_video_enabled = yes
   ; a file in C:\OXOS\boot_videos\, or a full path
   boot_video_file = my_intro.mpg
   ; no = only after the console is switched on; yes = also after every return to the dashboard
   boot_video_always = no
   ```

4. **Switch the console off and on:** the settings are read when the console starts.

## When it does not play
- `boot_video_enabled = yes`, and the file name matches the file in `C:\OXOS\boot_videos\`.
- Hold B while the console starts (safe boot) and no video plays; that is on purpose.
- A file the player cannot read is skipped and the boot goes on.
