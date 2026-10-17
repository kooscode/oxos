#!/usr/bin/env python3
"""Boot a BIOS in xemu interactively, with an optional disc image.

usage: run.py BIOS [--iso GAME.iso] [--hdd HDD.qcow2] [--state DIR [--fresh]] [--widescreen] [--size WxH]
              [--scale N] [--fullscreen] [--keep]

Disk and EEPROM:
- default: a temporary copy of the HDD image and a new EEPROM, deleted when xemu exits (unless --keep);
- --state DIR: a persistent copy in DIR (hdd.qcow2, eeprom.bin), made from --hdd the first time and reused after, so
  saves and settings survive; --fresh starts DIR over from --hdd.
The --hdd image itself is never written.

Display:
- --widescreen sets the widescreen video flag in the EEPROM; games that support it then render 16:9 and tell the AV
  connector so, and xemu's default ("auto") aspect follows that signal per game (4:3 games keep 4:3);
- --aspect forces xemu's aspect ratio instead (auto, native, 4x3, 16x9);
- --size picks the window size (an xemu startup size such as 1920x1080), --scale the render resolution multiplier
  (1 = native 640x480; 2 or 3 look sharp at 1080), --fullscreen starts full screen.
The kernel has no HD (720p/1080i) video modes yet, so games output 480 lines and xemu scales them.
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SIZES = ["640x480", "720x480", "1280x720", "1280x800", "1280x960", "1920x1080", "2560x1440", "2560x1600",
         "2560x1920", "3840x2160"]
ap = argparse.ArgumentParser()
ap.add_argument("bios")
ap.add_argument("--iso", help="disc image (XISO) for the DVD drive")
ap.add_argument("--hdd", default=os.path.join(root, "xbox-files/hdd/xbox_hdd.qcow2"))
ap.add_argument("--mcpx-rom", help="MCPX boot ROM dump (needed only for a Microsoft BIOS)")
ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
ap.add_argument("--state", help="directory for a persistent HDD copy and EEPROM")
ap.add_argument("--fresh", action="store_true", help="start the --state directory over from --hdd")
ap.add_argument("--widescreen", action="store_true", help="EEPROM widescreen flag (16:9 in games that support it)")
ap.add_argument("--aspect", choices=["auto", "native", "4x3", "16x9"], help="xemu aspect ratio (default auto)")
ap.add_argument("--size", choices=SIZES, help="window size")
ap.add_argument("--scale", type=int, choices=range(1, 11), metavar="1-10", help="render resolution multiplier")
ap.add_argument("--fullscreen", action="store_true")
ap.add_argument("--keep", action="store_true", help="keep the temporary directory (HDD copy, EEPROM, log)")
a = ap.parse_args()

work = tempfile.mkdtemp(prefix="xk-run-")
if a.state:
    os.makedirs(a.state, exist_ok=True)
    hdd, eeprom = os.path.join(a.state, "hdd.qcow2"), os.path.join(a.state, "eeprom.bin")
    if a.fresh:
        for f in (hdd, eeprom):
            if os.path.exists(f):
                os.remove(f)
    if not os.path.exists(hdd):
        print(f"new persistent HDD copy: {hdd}")
        shutil.copyfile(a.hdd, hdd + ".part")
        os.replace(hdd + ".part", hdd)
else:
    hdd, eeprom = os.path.join(work, "hdd.qcow2"), os.path.join(work, "eeprom.bin")
    shutil.copyfile(a.hdd, hdd)


def config(path, extra=""):
    display = []
    display.append(f"[display.ui]\naspect_ratio = '{a.aspect or 'auto'}'\n")
    if a.size or a.fullscreen:
        display.append("[display.window]\n" + (f"startup_size = '{a.size}'\n" if a.size else "")
                       + ("fullscreen_on_startup = true\n" if a.fullscreen else ""))
    if a.scale:
        display.append(f"[display.quality]\nsurface_scale = {a.scale}\n")
    open(path, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n" + "\n".join(display) + "\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.abspath(eeprom)}'\n"
        f"hdd_path = '{os.path.abspath(hdd)}'\n" + extra)


def create_eeprom():
    """xemu writes a new EEPROM when the configured one is missing: start it paused until the file appears."""
    cfg, sock = os.path.join(work, "init.toml"), os.path.join(work, "init.sock")
    config(cfg)
    p = subprocess.Popen([a.xemu, "-config_path", cfg, "-S", "-qmp", f"unix:{sock},server,nowait"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(eeprom) and os.path.getsize(eeprom) == 256:
                break
            time.sleep(0.1)
    finally:
        p.terminate()
        p.wait(10)
    return os.path.exists(eeprom)


def set_widescreen():
    """User settings section 0x60-0xBF: video flags (setting 8) at 0x94, widescreen = 0x00010000; the section's
    first dword makes it sum to 0xFFFFFFFF (32-bit sum with end-around carry, spec 12)."""
    data = bytearray(open(eeprom, "rb").read())
    flags, = struct.unpack_from("<I", data, 0x94)
    if flags & 0x00010000:
        return
    struct.pack_into("<I", data, 0x94, flags | 0x00010000)
    struct.pack_into("<I", data, 0x60, 0)
    total = 0
    for i in range(0x60, 0xC0, 4):
        total += struct.unpack_from("<I", data, i)[0]
        total = (total & 0xFFFFFFFF) + (total >> 32)
    struct.pack_into("<I", data, 0x60, ~total & 0xFFFFFFFF)
    open(eeprom, "wb").write(data)
    print("EEPROM: widescreen on")


try:
    if a.widescreen:
        if os.path.exists(eeprom) or create_eeprom():
            set_widescreen()
        else:
            print("warning: no EEPROM to set the widescreen flag in", file=sys.stderr)
    cfg = os.path.join(work, "xemu.toml")
    config(cfg, f"dvd_path = '{os.path.abspath(a.iso)}'\n" if a.iso else "")
    print(f"xemu: {a.bios}" + (f" + {a.iso}" if a.iso else " (no disc)")
          + (f"; state {a.state}" if a.state else f"; work dir {work}"))
    subprocess.run([a.xemu, "-config_path", cfg], stdout=open(os.path.join(work, "xemu.log"), "w"),
                   stderr=subprocess.STDOUT)
finally:
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
