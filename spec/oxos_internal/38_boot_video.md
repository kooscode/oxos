# 38. Boot video: a video of the user's choice before the first title

Status: **implemented 2026-11-17, released in v0.4.13, on by default with the OXOS boot video (§3.2)**. Decided by the user on 2026-11-17: the settings (§1),
when it plays (§1), that it is separate from the splash (§1.1), the video's shape, 16:9 cut to 4:3 (§3), the player in `apps/` with its code in `shared/` (§2).
The user leans to MPEG video in a `.mpg` file (§3), which the console plays with time to spare (§4.1). First proposal 2026-11-15 (an extension; replaced, §2).

The cold boot shows OXOS's own splash (spec 00), drawn by the kernel. A boot video adds the user's own: a video with sound, played before
the dashboard or the first title starts.

## 1. What the user sees
- After a cold boot (and, if asked for, after every reboot to the dashboard or the disc), the video plays full screen with its sound; then the dashboard or
  the disc starts as it would have without it.
- Any button on any controller skips it.
- A missing or unplayable file skips it: one line in the debug log, and the boot goes on.
- **The videos** are in `C:\OXOS\boot_videos\` (exFAT disk: `/C/OXOS/boot_videos/`).
- **The settings** are in the kernel's own file, `C:\OXOS\oxos_settings.ini` (user, 2026-11-17; until then they were to be the extension's own
  `extensions\oxos_boot_video.ini`), read by `shared/ini_file/settings_ini.cpp` in the kernel and the player alike. The kernel reads the switches at a cold
  boot (§2); the player reads `boot_video_file` each time it runs:

```ini
; C:\OXOS\oxos_settings.ini
[boot_video]
; yes = play a boot video; no or missing = no boot video
boot_video_enabled = yes
; a file in C:\OXOS\boot_videos\, or a full path (C: E: F: G: X: Y: Z:); quotes around it are allowed
boot_video_file = oxos_boot_video.mpg
; no or missing = only after a cold boot; yes = also after every reboot that would start the dashboard or the disc
boot_video_always = no
```

- **Safe boot (B held at a cold boot, spec 34 §3.3, spec 39 §1.1):** no boot video.
- **OXOS Setup** (`oxos_settings.xbe`, spec 39) will edit these keys and preview the videos with the same player; the file can be edited by hand meanwhile.

### 1.1 The splash is separate (user, 2026-11-17)
The boot video does not change the splash: on a cold boot it shows as always (since 2026-11-17 one picture for 4 s, no jingle, spec 00 §2), then the video,
which takes over the splash's picture without a gap (§4). Skipping the splash is a separate feature (an OXOS EEPROM byte, spec 39 §1.1), set later by OXOS
Setup.

## 2. How it works
**An app, `C:\OXOS\apps\oxos_boot_video.xbe` (`apps/oxos_boot_video`), started by the kernel in place of the title.**
- **The kernel's part** (`kernel/src/titles/title_loader.cpp`, `StartBootVideo`):
  - **When:** at a cold boot, where it would start the dashboard (boot flag 0x08) or the disc, else the dashboard; with `boot_video_always = yes` also at a
    quick reboot that would start the disc or the dashboard, but not one that starts a title another title asked for (a path in the launch data page not yet
    tried). Since 2026-11-17 also before the cold boot's other title launches (user: "if enabled, play the boot video always"): the first-boot checks'
    dashboard (a clock not set, unreadable user settings: `LaunchTitle(reason)`) and the disc-first launch of boot flag 0x04 or a manufacturing region; the
    player's quick reboot resumes them (`BOOT_VIDEO_TITLE`, the reason kept in `BootVideoPlace` bits 8 and up). Not in safe mode, not for a netboot title, the
    File Manager (X at boot), or the two unusual cold boots of boot flags 0x80 (the dashboard with reason 5) and 0x40 (the tray opened during boot), whose code
    is boot-only and gone by the time the player ends.
  - **How:** at the cold boot, the kernel settings module (`kernel/modules/kernel_settings.cpp`, spec 43) reads `[boot_video]` once
    and keeps its two switches in `BootVideoSwitches`, a variable that survives quick reboots (`XK_PERSIST`); 0 in safe mode. **A change to the file takes
    effect at the next cold boot.** With the boot video on, the kernel loads the player (its folder as `D:`) and keeps in `BootVideoPlace` (also `XK_PERSIST`)
    what the player runs in place of. The player cannot be loaded (not on the disk): the title starts as usual.
  - **After the player:** its quick reboot comes back to the kernel (`LaunchAfterQuickReboot`), which sees `BootVideoPlace` and starts the title it would have
    started: after a cold boot the dashboard (boot flag 0x08), else the disc or the dashboard; after a quick reboot what the launch data page asks for. The
    player leaves the launch data page as it found it.
  - **Resident memory:** the quick reboots' part is all that stays resident (about 200 bytes with the player's path); the parser and the cold boot's part are
    freed with the boot-only pages. On 2026-11-17 a first version with the parser resident cost titles one page (test_memory 15 959 free pages): the resident
    code had pushed the video encoder tables the kernel keeps (spec 18) across one more page boundary.
- **The app** (`apps/oxos_boot_video/oxos_boot_video.cpp`): mounts the hard disk's drives, reads `boot_video_file` from `oxos_settings.ini`, plays the file
  with the shared video player (§4), stopping at a button press (SDL3's controllers), then stops SDL and the USB host and quick-reboots
  (`HalReturnToFirmware(HalQuickRebootRoutine)`). Started from a dashboard it does the same, and the dashboard comes back.
- **Why an app** (and not the kernel or an extension):
  - In the kernel the decoder would take most of the 60 KB still free in the 256 KB image, could stop a boot with any bug, and would need a flash to update.
  - An extension may not use the FPU, MMX or SSE (spec 34 §9.2: the kernel does not save those registers for extensions), which a video decoder needs, and
    the SDK has no screen or sound interface.
  - An nxdk app has the C library, the FPU, MMX and SSE, costs titles no memory (the next launch frees it) and is updated by copying a file. Cerbios plays
    its boot video from an app too (spec cerbios/09 §2.1).
- **The player's code is shared** (`shared/video_player/`, user 2026-11-17): OXOS Setup will preview boot videos with it.
- The disc started after the video: a game with a trainer still gets the trainer menu first (spec 37), as the launch goes through the extensions as usual.

## 3. The video file
The console has no video decoder: its 733 MHz Pentium III decodes in software. The files people have (phone and YouTube MP4 or MOV: H.264 at 720p or 1080p,
CABAC and deblocking on) are beyond it: XBMC's notes give H.264 without CABAC and deblocking up to about 480x576 as its limit. Every boot video is therefore
converted once on a PC or a website, to the format that is easy to make and sure to play.

- **Format (leaning, user 2026-11-17):** MPEG video (MPEG-1 or MPEG-2) with MP2 or MP3 sound in a `.mpg` file (MPEG program stream), at most 40 MB (the
  player reads it whole). Every online converter offers "MP4 to MPG"; the Xbox's own DVD player decodes MPEG-2 at 720x480 in software on the same CPU.
- **Shape (user, 2026-11-17):** 16:9. On the 640x480 screen the middle 4:3 is shown and the sides are cut off; a picture of another size is scaled (nearest
  pixel). Planned: on a 720p screen the whole picture, on a 480-line widescreen setting the whole picture at 16:9.
- **Size:** 848x480 (16:9 at 480 lines; a width that is a multiple of 16, as MPEG needs), up to 30 frames per second: about half the CPU (§4.1); its middle
  640x480 is copied as it is. 1280x720 plays at 30 frames per second with little to spare, so the user guide recommends 848x480.
- **Sound:** 48 kHz stereo; other rates and mono are converted by the player.
- **PC command** (in the user guide, `docs/boot_video.md`):
  `ffmpeg -i in.mp4 -vf "scale=848:480:force_original_aspect_ratio=increase,crop=848:480" -r 30 -c:v mpeg2video -q:v 3 -c:a mp2 -ar 48000 -b:a 192k intro.mpg`
- **Websites:** free online converters (Convertio, XConvert, Zamzar) make `.mpg` from MP4, MOV, AVI or WMV; which of them set the size and write MPEG-1/2
  video with MP2 sound is to be checked before the README names them.
- **The OXOS test video** (`make -C kernel oxos_intro_video`, `build/apps/oxos_boot_video/oxos_intro.mpg`): OXOS's own boot splash picture (drawn by
  `tools/vsplash`) fading in and out over 8 s with a 440 Hz tone, 848x480 with the 640x480 picture in the middle. Until 2026-11-17 it was the splash's
  animation with the jingle, both removed (spec 00 §2, 17). Made at build time, with no video kept in the repository; the boot video test plays it. The user's
  own boot video (an 8 s OXOS intro, kept outside the repository) is the candidate for shipping (open, §5).

### 3.1 The decoders (open source, credited in `THIRD_PARTY_NOTICES.md`; `make -C kernel video_libraries` fetches them into `third_party/`)
| Part | Library | Licence |
|---|---|---|
| Video: MPEG-1 and MPEG-2, with MMX code | libmpeg2 0.5.1 (Debian's copy of its last release, SHA-256 checked) | GPL v2 or later |
| Sound: MP2 and MP3 | minimp3 (a pinned commit) | CC0 |
| The `.mpg` file's packets | OXOS's own (`shared/video_player/mpeg_program_stream.cpp`) | GPL v2 (OXOS) |

Set aside:
- **pl_mpeg** (MIT, one header): MPEG-1 only and no SIMD code; online converters often write MPEG-2 into a `.mpg`.
- **FFmpeg built for nxdk** (MP4, MOV, AVI, WMV and games' XMV files directly): no nxdk build of it exists yet (kodi4xbox, on nxdk, lists video playback as
  missing), the port is large, and an everyday MP4 would still be too heavy to play smoothly.
- **XMV, as Cerbios plays it:** Cerbios's player is linked with Microsoft's XDK XMV library (spec cerbios/09 §2.1), which OXOS cannot use.
- **Converting in the browser:** a "Boot video" page in the File Manager (spec 31) that converts any video the browser plays and uploads it. Easiest for users;
  needs an encoder written in JavaScript on that page. A follow-up.


### 3.2 The OXOS boot video (shipped, on by default)
User, 2026-11-17: "the release must include our boot video, and boot video on by default". `kernel/splash/oxos_boot_video.mpg` (2.2 MB, 8 s): the user's
own OXOS intro (the logo, a glitch, the X building up, the logo again), made by the user and converted with §3's command at `-q:v 8` with the sound 6 dB lower
(it opens on the same OXOS logo the splash ends on, so the splash runs straight into it, §4). The release zip puts it in `OXOS\boot_videos\` and ships
`OXOS\oxos_settings.ini` with `boot_video_enabled = yes` and `boot_video_file = oxos_boot_video.mpg`. Turning it off: `boot_video_enabled = no`; another
video: put it in `C:\OXOS\boot_videos\` and name it in `boot_video_file`.

## 4. The player (`shared/video_player/`, apps only)
- **Reading:** the file is read whole and split in place into its first video stream and its first MPEG audio stream (`mpeg_program_stream.cpp`); the video
  is then decoded by libmpeg2 and turned into 32-bit RGB by its MMX converter.
- **The screen:** 640x480 at 32 bits (nxdk's `XVideoSetMode`), the picture enabled (`XVideoSetVideoEnable`). Each frame's middle 4:3 is copied into the frame
  buffer with SSE streaming stores (848x480: as it is; other sizes scaled).
- **No mode change (2026-11-17):** the player keeps the display mode already set when it is a 640x480 one (the splash's, spec 02 §7: 480p on the HDTV pack
  when the EEPROM allows it): it draws its first picture into its own frame buffer and calls `AvSetDisplayMode` with the same mode word, which only switches
  the frame buffer, so a TV or an HDMI adapter keeps its signal. Another mode set (after a title, `boot_video_always`): nxdk's `XVideoSetMode`. The kernel
  keeps the splash on screen until then (spec 02 §7), and the player's first picture is the splash's last one (`ShowSplash`), so the splash runs straight
  into the video.
- **Lead-in:** what is on the screen (the splash, or black) stays at least the time the caller gives (`PlayVideoFile`'s `LeadInMs`) after the screen came
  on, before the first frame and the sound start: the boot video app asks for 500 ms (user, 2026-11-17: the HDMI adapter "takes a little bit to wake up"
  after a mode change and missed the start). A button press during it stops the video too.
- **Time:** frame n is shown at the start + n frame periods (the sequence header's frame period), after the CPU waited for it in 1 ms sleeps; then the player
  waits for the screen's next frame to begin (the NV2A's raster position, PCRTC at 0xFD600808, falls back to the top) and copies the picture, faster than the
  raster, so nothing tears. A late frame is shown at once and the next ones catch up. When the raster does not move (xemu), the wait is dropped after three
  frames without a new screen frame.
- **Sound:** minimp3 decodes one MP2/MP3 frame at a time, converted to 48 kHz stereo (nearest sample), into a ring of 32 buffer descriptors that both outputs of
  the AC'97 controller (PCM and S/PDIF) play (spec 17 §2's registers); the player keeps about 0.3 s queued and reads the controller's current descriptor to
  know how far it has played. **No interrupt is connected:** nxdk's `XAudio` and `XVideoWaitForVBlank` connect interrupts they never disconnect, which would
  point into freed memory after the player's quick reboot. The codec is cold-reset first and unmuted, as the kernel's boot jingle did (spec 17 §2).
- **Skip:** a button press on any controller (SDL3's gamepad events; the player asks between frames and while it waits).
- **The end:** after the last picture the sound plays out (at most 2 s); the outputs are reset, the screen is cleared.
- **Log:** `video player: 848x480, 200 frames shown, 0 late, shown at the vertical blank, with sound` and the app's
  `boot video: C:\OXOS\boot_videos\oxos_boot_video.mpg: played to its end` (`stopped`, `missing, empty, too large or unreadable`, `no MPEG-1 or MPEG-2
  video in it`) on the debug serial port (DbgPrint).

### 4.1 Measured on the console (2026-11-17)
The test title `tests/video_decode_title` (`make -C kernel video_decode_title`) on the Modxo test console (Pentium III 733 MHz; libmpeg2 finds MMX and the
SSE integer instructions), netbooted with the FTP server, which copied the files and started it. The clips, all made by OXOS's build or ffmpeg's own
generators, 10 s each (the OXOS boot video 8 s), `-q:v 3`: the OXOS boot video (§3), ffmpeg's moving test pattern (`testsrc2`) and a zoom into the Mandelbrot
set (`mandelbrot`, full of fine detail: 12 Mbit/s at 848x480, 22 at 1280x720, far more than a converted video needs). Milliseconds per frame, averaged; "shown"
adds the copy of the middle 640x480 into the frame buffer:

| Video | Decoded | + turned into RGB | + shown | Slowest frame shown |
|---|---|---|---|---|
| MPEG-2, 848x480, OXOS boot video, 25 fps | 3.8 | 7.2 | 11.6 | 32.4 |
| MPEG-2, 848x480, test pattern, 30 fps | 6.1 | 9.5 | 13.9 | 36.0 |
| MPEG-2, 848x480, Mandelbrot, 30 fps | 9.7 | 13.1 | 17.5 | 36.9 |
| MPEG-1, 848x480, Mandelbrot, 30 fps | 9.6 | 13.0 | 17.3 | 38.4 |
| MPEG-2, 1280x720, test pattern, 30 fps | 12.8 | 20.6 | 24.8 | 77.8 |
| MPEG-2, 1280x720, Mandelbrot, 30 fps | 20.6 | 28.4 | 32.8 | 80.9 |
| MPEG-1, 1280x720, Mandelbrot, 30 fps | 20.1 | 28.2 | 32.4 | 80.8 |

- **Sound:** MP2, 48 kHz, decoded by minimp3 in 3.7 % (mono) to 4.2 % (stereo) of the CPU.
- **MMX matters:** without it (libmpeg2's C code, a first build that missed its configuration) 848x480 took about 13 ms to decode and 20 ms with RGB.
- **The copy to the screen:** 4.4 ms for 640x480 with SSE streaming stores. nxdk's `memcpy` (a byte at a time) took 21 ms; an SDL texture (its GPU renderer
  rearranges the pixels into the GPU's texture layout) 27 ms at 848x480 and 62 ms at 1280x720. The player writes to the frame buffer itself.
- **The boot on the console** (serial log and HDMI capture with sound): the OXOS boot video, 200 frames, 0 late, each shown at the screen's vertical blank, its
  sound heard; then the dashboard.
- **What follows:**
  - MPEG-1 and MPEG-2 cost the same: the player takes both.
  - 848x480 at 30 frames per second needs 14 to 18 of every 33 ms.
  - 1280x720 needs 25 to 33 ms with the 640x480 copy: 30 frames per second only just; on a 720p screen the copy is three times larger (about 13 ms).
  - The slowest frames (the I-frames, pictures without reference to others) take two to three frame times; the frames after them catch up.
  - Turning the picture into RGB on the GPU instead (the Y, U and V planes as textures) would save 3 to 8 ms a frame: a possible later step.

## 5. Open
- 720p and widescreen screens (§3): the player shows 640x480 on every setting today.
- A length limit (none; the file is limited to 40 MB).
- A log file for the player (spec 35); today the debug serial port only.

## 6. Tests
- `make test_boot_video` (`tests/boot_video_test.py`, in the regression run): cold boots of a serial-log kernel in xemu with the player, `oxos_settings.ini`
  and the OXOS boot video on the disk, checking the kernel's and the player's log lines and a screenshot: the video plays to its end and the dashboard follows
  (the screen shows the video), a button on a virtual pad skips it, a missing file boots straight on, `boot_video_enabled = no` plays nothing, and settings
  without the player on the disk boot straight on.
- Host unit tests: `oxos_settings.ini`'s parser (`tests/unit/test_settings_ini.cpp`).
- Console: `tests/video_decode_title` (§4.1) for the decoder's speed after a change to it or to the compiler options; a cold boot with the HDMI capture
  (picture and sound).
- Not yet tested: `boot_video_always = yes` at a quick reboot (xemu's test disk has no title that returns to the dashboard).
