# 38. Boot video: a video of the user's choice before the first title (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "a custom boot animation thats an kernel extension that plays a video as first thing before the
dashboard or first title launch happens on boot (ini for enabling on cold/hot boots)"). Nothing here is decided until the user says so.

The cold boot shows OXOS's own splash and jingle (specs 00 and 17), drawn by the kernel. A boot video lets the user add their own: a short video with sound,
played before the dashboard or the first title starts, on a cold boot, after quick reboots, or both.

## 1. What the user sees
- After the OXOS splash (or instead of it, open), the video plays full screen with its sound, then the dashboard or title starts as usual.
- Any button skips it.
- **The videos** are in `C:\OXOS\boot_videos\` (exFAT disk: `/C/OXOS/boot_videos/`).
- **The settings** are the `[boot_video]` section of **`C:\OXOS\oxos_boot.ini`**, the file for how the console boots (§1.1). `oxos_extensions.ini` only lists
  the extension (spec 34 §3.1).

### 1.1 `oxos_boot.ini`
One file for the boot options that apply once the hard disk is up: the boot video and which dashboard starts.

```ini
; C:\OXOS\oxos_boot.ini
[boot_video]               ; read by oxos_boot_video.xos
cold_boot = intro.oxv      ; a file in C:\OXOS\boot_videos\; none = off; random = any file there
quick_reboot = none        ; when a title starts the dashboard or another title
skippable = yes            ; any button skips it

[dashboard]                ; read by the kernel (spec 19): the dashboards tried, in order
order = C:\evoxdash.xbe, E:\evoxdash.xbe, C:\xboxdash.xbe, C:\Dashboard\default.xbe
```

- **The kernel** reads `[dashboard]` with its ini reader (`ini_file.cpp`, as for `oxos_launch_redirect.ini`) when it starts a dashboard; a missing file or
  section keeps today's fixed order (spec 19).
- **The extension** reads `[boot_video]` with the SDK's file functions.
- **Not the splash and the jingle:** they start at the very beginning of a cold boot, while the hard disk is still spinning up, before any file can be read
  (exFAT and FATX are mounted after the disk is up). Skipping them is an OXOS byte in the EEPROM instead, set by OXOS Setup (spec 39 §1.1).
- **Safe boot (B held at a cold boot, spec 39 §1.1)** ignores `oxos_boot.ini` and the boot video with it.

## 2. How it works
- **An extension, `oxos_boot_video.xos`:** its `ExtensionStart` runs before each title (spec 34 §4.3), when nothing else is on the screen. It checks the launch
  kind (`oxos_launch_number`, `OXOS_LAUNCH_*`) against the settings, plays the video there, then returns FALSE: it is unloaded before the title starts, so the
  title loses no memory to it (or, as a run-once extension, spec 34 §4.3a, the same with events available).
- **The video file** is streamed from the disk, a frame at a time; only a few frames are in memory.
- **Drawing and sound:** the extension draws into the kernel's own frame buffer, as the splash does, and plays sound through the audio path the jingle uses
  (spec 17). The SDK has neither today: it needs a small screen and audio interface (open, §4), which toasts (spec 36) and OXOS Setup (spec 39) could share.

## 3. The video format
The console has no video decoder; its 733 MHz CPU decodes in software. The extension plays one format; a PC tool converts any video to it with FFmpeg,
including XMV (the video format of Xbox games: WMV-family video, ADPCM sound) and MP4. XMV is not played directly: a WMV decoder in an extension is large, and
whether the CPU keeps up with it at full size is uncertain. The format the extension plays, two ways, to choose before building:

| | Own simple format (`.oxv`) | MPEG-1 |
|---|---|---|
| How | a host tool (like `tools/svg_splash.py`) converts any video (ffmpeg) to compressed frames at the TV's size, plus PCM sound | a small decoder in the extension (pl_mpeg, MIT, MPEG-1 video + MP2 sound: its fit and speed to check) |
| Decoding | very cheap (copy and decompress) | heavier; 640x480 at 30 frames per second is to be measured on the console |
| File size | larger (a few MB per 10 s) | small |
| Effort | the converter, a simple player | porting the decoder to the extension's environment (no C library beyond the SDK's) |

The first choice is simpler and certain to keep up; the second takes any MPEG-1 file directly.

## 4. Open
- The SDK's screen and audio interface: what an extension may draw and play while no title runs (frame buffer, video mode, the APU's voices), shared with spec
  36 and 39.
- Splash and video: the video after the OXOS splash, or replacing it (and the jingle).
- The splash and jingle switches: in the EEPROM, set by OXOS Setup (spec 39 §1.1); which free byte, to check.
- Length limit and what happens if the file is missing or damaged (skip, log a line in `extensions.log`).
- 720p/1080i modes: the video scaled to the mode, or played at 480 lines.

## 5. Tests
- Host: the converter and the frame decoder (unit tests: a known frame decodes exactly).
- xemu: a cold boot with the extension and a short test video reaches the dashboard; the screen is captured during the video (`tools/fbshot.py`); with
  `cold_boot = no` it does not play; a pressed button skips it.
- Console: a cold boot with the HDMI capture (picture and sound).
