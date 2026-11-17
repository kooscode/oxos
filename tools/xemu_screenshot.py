#!/usr/bin/env python3
"""Screenshots of xemu running a BIOS: boots it with an optional disc, files of your own on the hard disk's C:, and a virtual controller that presses buttons
between the screenshots. For checking screens (the dashboard, the trainer menu, error screens) without anyone at the PC.

usage: xemu_screenshot.py BIOS OUT [--iso GAME.iso] [--hdd HDD.qcow2] [--eeprom FILE] [--put LOCAL=C:\\PATH ...] [--steps STEPS] [--save-hdd FILE]

OUT is the screenshots' name: OUT_1.png, OUT_2.png, ... (640x480). STEPS is a comma-separated list, run in order:
- wait:SECONDS   wait (boot, a game starting, an animation);
- shot           save the next screenshot;
- BUTTON[:N]     press a controller button N times (default 1): A, B, X, Y, BLACK (RB), WHITE (LB), BACK, START, LS, RS (stick clicks),
                 UP, DOWN, LEFT, RIGHT (D-pad).
Default: "wait:30,shot". --put copies a file onto C: (or E:, F:, ...) of a copy of the hard disk (FATX), creating folders and replacing a file
that is there; repeat it for more files.
The --hdd image itself is never written (default: the xemu-dashboard disk, `make -C kernel hdd_image`). --save-hdd keeps the disk as xemu left it, to
read what the console wrote (tests/fatx.py: FATX(Qcow2(FILE), "C").read_file("OXOS\\extensions\\oxos_trainers.ini")). --eeprom starts from a copy of
an EEPROM (default: a new one): the one the disk was used with (tools/run.py --state keeps both), for content signed with its key, such as a title update.

Example: the trainer menu before Halo 2, at its Cloaked cheat:
  xemu_screenshot.py build/kernel/release/bios.bin /tmp/menu --iso xiso/Halo2/halo2-xiso.iso \\
      --put kernel/extensions/oxos_extensions.ini=C:\\OXOS\\oxos_extensions.ini \\
      --put build/kernel/extensions/oxos_trainers/oxos_trainers.xos=C:\\OXOS\\extensions\\oxos_trainers.xos \\
      --put build/kernel/extensions/oxos_trainers/app/bin/default.xbe=C:\\OXOS\\extensions\\oxos_trainers.xbe \\
      --put build/kernel/extensions/oxos_trainers/oxos_trainers.ini=C:\\OXOS\\extensions\\oxos_trainers.ini \\
      "--put=third_party/xbox_trainers/trainers/Halo 2.etm=C:\\OXOS\\trainers\\Halo 2.etm" --steps wait:30,shot,DOWN:10,wait:2,shot

How the picture is taken: xemu has no QMP screendump, and reading the frame from guest memory gives a stale picture (xemu keeps rendered surfaces on the
GPU). So the screenshot is xemu's own window ("xemu | v..."), grabbed by its X11 window id with ffmpeg's x11grab: this works while other windows or the lock
screen cover it, but needs the PC's X display ($DISPLAY, GPU-rendered). Not xemu's hidden 640x480 "SDL Offscreen Window"s: grabbing those fails. Under a
nested X server (Xephyr) xemu renders with software OpenGL and does not get past the first frames of a 3D screen. The virtual controller is the tests' uinput
pad, bound to controller port 1 (tests/virtual_gamepad.py), so a real controller plugged into the PC does not take its place.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(root, "tests"))
from qcow2 import Qcow2  # noqa: E402
from fatx import FATXWriter  # noqa: E402
from controller_test import xemu_env  # noqa: E402
from virtual_gamepad import XEMU_PORT1_BINDING, VirtualPad  # noqa: E402

# Button names in STEPS: (the virtual pad's control, the value that presses it).
BUTTONS = {"A": ("A", 1), "B": ("B", 1), "X": ("X", 1), "Y": ("Y", 1), "BLACK": ("RB", 1), "WHITE": ("LB", 1), "BACK": ("BACK", 1), "START": ("START", 1),
           "LS": ("LS", 1), "RS": ("RS", 1), "UP": ("HATY", -1), "DOWN": ("HATY", 1), "LEFT": ("HATX", -1), "RIGHT": ("HATX", 1)}


def read_steps(text):
    """[(kind, value)] from STEPS: ("wait", seconds), ("shot", None), ("press", (button, times)); exits with a message on a bad step."""
    steps = []
    for item in filter(None, (part.strip() for part in text.split(","))):
        name, _, count = item.partition(":")
        if name == "wait":
            steps.append(("wait", float(count)))
        elif name == "shot":
            steps.append(("shot", None))
        elif name.upper() in BUTTONS:
            steps.append(("press", (name.upper(), int(count or 1))))
        else:
            sys.exit(f"unknown step {item!r} (wait:S, shot, or a button: {', '.join(BUTTONS)})")
    return steps


def write_files(hdd, files):
    """Copies each LOCAL=X:\\PATH onto drive X: of the disk image hdd (FATX: C, E, F, ...; no drive letter means C:), replacing a file already there."""
    image = Qcow2(hdd)
    disks = {}
    for item in files:
        local, _, target = item.partition("=")
        drive, path = (target[0].upper(), target[2:]) if len(target) > 1 and target[1] == ":" else ("C", target)
        path = path.lstrip("\\")
        if drive not in disks:
            disks[drive] = FATXWriter(image, drive)
        disk = disks[drive]
        if "\\" in path:
            disk.makedirs(path.rsplit("\\", 1)[0])
        if disk.lookup(path):
            disk.delete_entry(path)  # replaced: a new build over the one on the disk
        disk.write_file(path, open(local, "rb").read())
    image.save(hdd + ".new")
    os.replace(hdd + ".new", hdd)


def find_xemu_window():
    """The X11 id of xemu's main window, or None while it has none."""
    tree = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True, text=True).stdout
    for line in tree.splitlines():
        if '"xemu |' in line and '("xemu"' in line:
            return line.split()[0]
    return None


def take_screenshot(window, png):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-window_id", window, "-i", os.environ["DISPLAY"], "-frames:v", "1",
                    "-vf", "scale=640:480", png], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("bios")
    ap.add_argument("out", help="screenshot name: OUT_1.png, OUT_2.png, ...")
    ap.add_argument("--iso", help="disc image for the DVD drive")
    ap.add_argument("--hdd", default=os.path.join(root, "third_party/xemu_dashboard/xemu_dashboard.qcow2"), help="hard-disk image (copied, never written)")
    ap.add_argument("--put", action="append", default=[], metavar="LOCAL=C:\\PATH", help="a file to copy onto C:, E:, ... (repeatable)")
    ap.add_argument("--steps", default="wait:30,shot", help="wait:S, shot and button presses, comma-separated")
    ap.add_argument("--eeprom", help="EEPROM to start from (copied, never written)")
    ap.add_argument("--save-hdd", metavar="FILE", help="keep the disk as xemu left it")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    a = ap.parse_args()
    if not os.environ.get("DISPLAY"):
        sys.exit("needs the PC's X display ($DISPLAY): xemu's window is what is grabbed")
    steps = read_steps(a.steps)

    work = tempfile.mkdtemp(prefix="xemu-shot-")
    sock = os.path.join(tempfile.gettempdir(), os.path.basename(work) + ".sock")  # short: a Unix socket path has at most 107 characters
    hdd = os.path.join(work, "hdd.qcow2")
    shutil.copyfile(a.hdd, hdd)
    if a.put:
        write_files(hdd, a.put)
    if a.eeprom:
        shutil.copyfile(a.eeprom, os.path.join(work, "eeprom.bin"))
    cfg = os.path.join(work, "xemu.toml")
    open(cfg, "w").write(
        f"[general]\nshow_welcome = false\nskip_boot_anim = false\n\n[sys]\nmem_limit = '64'\n\n[sys.files]\nbootrom_path = ''\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\neeprom_path = '{os.path.join(work, 'eeprom.bin')}'\nhdd_path = '{hdd}'\n"
        + (f"dvd_path = '{os.path.abspath(a.iso)}'\n" if a.iso else "") + XEMU_PORT1_BINDING)
    pad = VirtualPad()  # before xemu starts, so xemu binds it to controller port 1
    xemu = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"], stdout=open(os.path.join(work, "xemu.log"), "w"),
                            stderr=subprocess.STDOUT, env=xemu_env())
    shots = 0
    try:
        for kind, value in steps:
            if kind == "wait":
                time.sleep(value)
            elif kind == "press":
                control, pressed = BUTTONS[value[0]]
                for _ in range(value[1]):
                    pad.set(control, pressed)
                    time.sleep(0.12)
                    pad.set(control, 0)
                    time.sleep(0.25)
            else:
                window = find_xemu_window()
                if not window:
                    sys.exit(f"xemu has no window (its log: {os.path.join(work, 'xemu.log')})")
                shots += 1
                take_screenshot(window, f"{a.out}_{shots}.png")
                print(f"{a.out}_{shots}.png")
    finally:
        xemu.terminate()
        xemu.wait(10)
        pad.close()
        if os.path.exists(sock):
            os.remove(sock)
        if a.save_hdd:
            shutil.copyfile(hdd, a.save_hdd)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
