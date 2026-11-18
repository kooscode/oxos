#!/usr/bin/env python3
"""Boot video test (spec/oxos_internal/38 §6): cold boots in xemu with the boot video player (apps/oxos_boot_video) and C:\\OXOS\\oxos_settings.ini on the
disk, and checks on the debug serial port (a SERIAL=1 kernel) what the kernel and the player did:
  - plays:       boot_video_enabled = yes and the video there: the player runs, plays the video to its end, and the dashboard starts after it; the screen
                 during the video shows the picture (not black);
  - skipped:     A pressed during the video: the player stops it, and the dashboard starts;
  - no file:     boot_video_file names a file that is not there: the player says so and the dashboard starts;
  - off:         boot_video_enabled = no: no player, the dashboard starts;
  - no player:   the settings ask for a video but oxos_boot_video.xbe is not on the disk: the kernel cannot load it and starts the dashboard.
The video is the OXOS test video (`make -C kernel oxos_intro_video`: the boot splash's picture fading in and out with a tone, 16:9 at 848x480, MPEG-2 with
MP2 sound, the format of spec 38 §3), 8 s long.

usage: boot_video_test.py --bios SERIAL_BIOS --player oxos_boot_video.xbe --video oxos_intro.mpg [--xemu PATH]
"""
import argparse, os, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCREENSHOT = os.path.join(ROOT, "tools", "xemu_screenshot.py")
PLAYER_ON_DISK = r"C:\OXOS\apps\oxos_boot_video.xbe"
SETTINGS_ON_DISK = r"C:\OXOS\oxos_settings.ini"
VIDEO_ON_DISK = r"C:\OXOS\boot_videos\intro.mpg"
DASHBOARD_LOADED = r"load \Device\Harddisk0\Partition2\xboxdash.xbe: 0x0"
PLAYER_LOADED = "boot video: player 0x0"

failures = 0


def check(ok, what, detail=""):
    global failures
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}" + (f" ({detail})" if detail and not ok else ""), flush=True)
    failures += not ok


def settings(work, name, enabled, video_file="intro.mpg"):
    path = os.path.join(work, name)
    with open(path, "w", newline="\r\n") as f:
        f.write(f"; {name}\n[boot_video]\nboot_video_enabled = {'yes' if enabled else 'no'}\nboot_video_file = {video_file}\nboot_video_always = no\n")
    return path


def boot(args, work, name, files, steps):
    """Boots once; the serial log's lines and the screenshots' paths."""
    serial = os.path.join(work, f"{name}.serial")
    out = os.path.join(work, name)
    command = [sys.executable, "-I", SCREENSHOT, args.bios, out, "--serial", serial, "--steps", steps]
    for local, on_disk in files:
        command += ["--put", f"{local}={on_disk}"]
    if args.xemu:
        command += ["--xemu", args.xemu]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    text = open(serial, "rb").read().decode("latin-1") if os.path.exists(serial) else ""
    shots = sorted(f for f in os.listdir(work) if f.startswith(name + "_") and f.endswith(".png"))
    return [line.strip() for line in text.splitlines()], [os.path.join(work, f) for f in shots]


def after(lines, first, then):
    """True when a line containing `then` comes after one containing `first`."""
    for i, line in enumerate(lines):
        if first in line:
            return any(then in later for later in lines[i + 1:])
    return False


def brightness(png):
    from PIL import Image, ImageStat
    return sum(ImageStat.Stat(Image.open(png).convert("L")).mean)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bios", required=True, help="a SERIAL=1 BIOS (the kernel's log shows the launches)")
    ap.add_argument("--player", required=True, help="oxos_boot_video.xbe (build/apps/oxos_boot_video/bin/default.xbe)")
    ap.add_argument("--video", required=True, help="the OXOS boot video (build/apps/oxos_boot_video/oxos_intro.mpg)")
    ap.add_argument("--xemu")
    args = ap.parse_args()
    work = tempfile.mkdtemp(prefix="oxos-boot-video-")
    try:
        video = args.video
        on = settings(work, "on.ini", True)
        player = (args.player, PLAYER_ON_DISK)

        print("plays: a cold boot with the boot video")
        lines, shots = boot(args, work, "plays", [player, (on, SETTINGS_ON_DISK), (video, VIDEO_ON_DISK)], "wait:10,shot,wait:15")
        check(PLAYER_LOADED in lines, "the kernel loads the player in place of the first title")
        check(any(r"intro.mpg: played to its end" in line for line in lines), "the player plays the video to its end")
        check(after(lines, "played to its end", DASHBOARD_LOADED), "the dashboard starts after the video")
        check(bool(shots) and brightness(shots[0]) > 5, "the screen shows the video (not black)", f"brightness {brightness(shots[0]) if shots else 'no shot'}")

        print("skipped: A pressed during the video")
        lines, _ = boot(args, work, "skipped", [player, (on, SETTINGS_ON_DISK), (video, VIDEO_ON_DISK)], "wait:9,A,wait:8")
        check(any(r"intro.mpg: stopped" in line for line in lines), "the player stops the video at the press")
        check(after(lines, "intro.mpg: stopped", DASHBOARD_LOADED), "the dashboard starts after it")

        print("no file: boot_video_file names a file that is not there")
        missing = settings(work, "missing.ini", True, "not_there.mpg")
        lines, _ = boot(args, work, "nofile", [player, (missing, SETTINGS_ON_DISK)], "wait:20")
        check(any("not_there.mpg: missing" in line for line in lines), "the player reports the missing file")
        check(after(lines, "not_there.mpg: missing", DASHBOARD_LOADED), "the dashboard starts")

        print("off: boot_video_enabled = no")
        off = settings(work, "off.ini", False)
        lines, _ = boot(args, work, "off", [player, (off, SETTINGS_ON_DISK), (video, VIDEO_ON_DISK)], "wait:20")
        check(not any("boot video:" in line for line in lines), "no player")
        check(DASHBOARD_LOADED in lines, "the dashboard starts")

        print("no player: the settings ask for a video, the player is not on the disk")
        lines, _ = boot(args, work, "noplayer", [(on, SETTINGS_ON_DISK), (video, VIDEO_ON_DISK)], "wait:20")
        check(any(line.startswith("boot video: player 0xc") for line in lines), "the kernel cannot load the player")
        check(DASHBOARD_LOADED in lines, "the dashboard starts")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"RESULT: {'FAIL' if failures else 'PASS'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
