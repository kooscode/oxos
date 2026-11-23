#!/usr/bin/env python3
"""Controller input test: boot a BIOS with the controller test title's disc, press every input of a virtual gamepad and check which Xbox controller input the
title receives for each.

The title (tests/controller_test_title) is built with nxdk and keeps its state in a block with the signature "XKCTLTST"; this script finds it in a RAM dump (QMP
pmemsave) and then reads it through its virtual address after each press.

With --igs (oxos_igs.xos, a FATX --hdd), also the in-game shortcuts (spec/oxos_internal/48) on the same pad: the disk gets the extension with one shortcut,
BLACK + WHITE -> coldboot at once (launch_delay = 0); pressing it must power the console off and on (the title starts again: its report count starts over)
and log its line in C:\\OXOS\\log\\extensions.log. Then LT + RT + BACK + BLACK held 3.5 s, which no shortcut uses, must do the same through the kernel's last
resort (spec/oxos_internal/48 §5).

usage: controller_test.py --bios BIOS --iso controller_test_title.iso [--mcpx-rom MCPX] [--hdd HDD] [--igs oxos_igs.xos] [--xemu PATH]
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from virtual_gamepad import VPAD_PRODUCT, VPAD_VENDOR, XEMU_PORT1_BINDING, VirtualPad  # noqa: E402
from qcow2 import Qcow2  # noqa: E402
from fatx import FATX, FATXWriter  # noqa: E402

ANALOG = ["A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"]
DIGITAL = ["UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS"]
AXES = ["LX", "LY", "RX", "RY"]
SLOT_SIZE, SLOT_BASE, SLOTS = 64, 28, 4

# Host input (virtual pad control, value) -> the Xbox input it should produce (xemu's default pad mapping).
STEPS = [("A", 1, "A"), ("B", 1, "B"), ("X", 1, "X"), ("Y", 1, "Y"), ("RB", 1, "BLACK"), ("LB", 1, "WHITE"), ("LT", 255, "LT"), ("RT", 255, "RT"), ("HATY", -1,
    "UP"), ("HATY", 1, "DOWN"), ("HATX", -1, "LEFT"), ("HATX", 1, "RIGHT"), ("START", 1, "START"), ("BACK", 1, "BACK"), ("LS", 1, "LS"), ("RS", 1, "RS"), ("LX",
    -32768, "LX-"), ("LX", 32767, "LX+"), ("LY", -32768, "LY+"), ("LY", 32767, "LY-"), ("RX", -32768, "RX-"), ("RX", 32767, "RX+"), ("RY", -32768, "RY+"),
    ("RY", 32767, "RY-"),
]


def decode(report):
    """Active Xbox inputs in a 20-byte xid_gamepad_in report."""
    buttons = struct.unpack_from("<H", report, 2)[0]
    active = {DIGITAL[i] for i in range(8) if buttons & (1 << i)}
    active |= {ANALOG[i] for i in range(8) if report[4 + i] >= 0x80}
    for i, v in enumerate(struct.unpack_from("<4h", report, 12)):
        if v < -16000:
            active.add(AXES[i] + "-")
        elif v > 16000:
            active.add(AXES[i] + "+")
    return active


def xemu_env():
    """Environment for an xemu that sees only the virtual pad, also while its window is not focused."""
    return dict(os.environ,
                # SDL ignores gamepads while the app has no focus unless told otherwise: without this, a test fails whenever focus moves elsewhere (e.g. back to
                # the terminal).
                SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1",
                # The user's own controllers would take controller port 1 and could press things mid-test.
                SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=f"0x{VPAD_VENDOR:04x}/0x{VPAD_PRODUCT:04x}")


# Seconds a test waits for xemu to answer a QMP command. xemu answers within a second or two (a 64 MB pmemsave included), but now and then it freezes outright
# (every thread waiting, no CPU used, nothing answered), and a test waiting for the answer then waited for ever: a release build stood 7 hours (2026-11-18).
QMP_TIMEOUT = 60


def qmp(path, command, **arguments):
    """Send one QMP command to the xemu listening on path and return its answer (the whole reply: "return" or "error"). Without an answer within
    QMP_TIMEOUT seconds the test fails (exits), so the caller's cleanup stops xemu."""
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(QMP_TIMEOUT)
        s.connect(path)
        f = s.makefile("rw")
        f.readline()
        for msg in ({"execute": "qmp_capabilities"}, {"execute": command, "arguments": arguments}):
            f.write(json.dumps(msg) + "\n")
            f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r:
                    break
        s.close()
    except TimeoutError:  # converted here: callers catch OSError (TimeoutError's base) for other reasons
        sys.exit(f"  [FAIL] xemu: no answer to QMP {command} within {QMP_TIMEOUT} s (xemu froze)\nRESULT: FAIL")
    return r


class PadState:
    def __init__(self, sock, work):
        self.sock, self.dump = sock, os.path.join(work, "pad.bin")
        self.addr = None

    def locate(self):
        """Find the title's state block in physical RAM; returns True once found."""
        ram = os.path.join(os.path.dirname(self.dump), "ram.bin")
        out = qmp(self.sock, "pmemsave", val=0, size=0x4000000, filename=ram)
        if not os.path.exists(ram):
            sys.exit(f"pmemsave failed: {out}")
        data = open(ram, "rb").read()
        os.remove(ram)
        at = data.find(b"XKCTLTST\x01\x00\x00\x00")
        while at >= 0:  # copies of the title image (file cache) still hold Self = 0
            self.addr = struct.unpack_from("<I", data, at + 12)[0]
            if self.addr:
                return True
            at = data.find(b"XKCTLTST\x01\x00\x00\x00", at + 1)
        return False

    def read(self):
        qmp(self.sock, "memsave", val=self.addr, size=SLOT_BASE + SLOTS * SLOT_SIZE, filename=self.dump)
        return open(self.dump, "rb").read()

    def slots(self):
        d = self.read()
        out = []
        for s in range(SLOTS):
            o = SLOT_BASE + s * SLOT_SIZE
            present, uid, type_, reports = struct.unpack_from("<4I", d, o)
            out.append({"present": present, "type": type_, "reports": reports, "last": d[o + 16:o + 36]})
        return out


# The in-game shortcuts' test set-up (--igs): the extension, listed and logging, with one shortcut.
IGS_EXTENSIONS_INI = "[oxos_igs]\r\nextension = oxos_igs.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n"
IGS_INI = "[test]\r\nbuttons = black + white\r\naction = coldboot\r\nlaunch_delay = 0\r\n"
IGS_LOG_LINE = "[oxos_igs] [test] black + white: coldboot"


def install_igs(hdd, xos):
    """oxos_igs.xos, its oxos_igs.ini and an oxos_extensions.ini listing it, on C: of the FATX disk hdd."""
    q = Qcow2(hdd)
    w = FATXWriter(q, "C")
    w.makedirs("OXOS\\extensions")
    w.write_file("OXOS\\oxos_extensions.ini", IGS_EXTENSIONS_INI.encode())
    w.write_file("OXOS\\extensions\\oxos_igs.ini", IGS_INI.encode())
    w.write_file("OXOS\\extensions\\oxos_igs.xos", open(xos, "rb").read())
    q.save(hdd + ".new")
    os.replace(hdd + ".new", hdd)


def report_count(state, slot):
    """The pad's report count in the title's state block, or None when the block cannot be read (its address not mapped) or the pad is gone."""
    try:
        s = state.slots()[slot]
    except struct.error:  # memsave wrote nothing: the address is not mapped
        return None
    return s["reports"] if s["present"] else None


def wait_for_restart(state, slot, seconds=90):
    """True once the title runs again after a power cycle. First the reboot, within 20 s: the pad's report count stops (xemu keeps the RAM through a power
    cycle, so the old block stays readable), falls, or cannot be read. Then a title's block whose count climbs again."""
    deadline = time.time() + 20
    last, still = report_count(state, slot), 0
    while time.time() < deadline:
        time.sleep(0.5)
        count = report_count(state, slot)
        still = still + 1 if count is not None and count == last else 0
        if count is None or (last is not None and count < last) or still >= 6:
            break
        last = count
    else:
        print(f"    no reboot within 20 s: the report count went on ({last})")
        return False
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(3)
        try:
            if not state.locate():
                continue
        except struct.error:
            continue
        before = [report_count(state, i) for i in range(SLOTS)]
        time.sleep(1)
        after = [report_count(state, i) for i in range(SLOTS)]
        if any(b is not None and a is not None and a > b for b, a in zip(before, after)):
            return True
    print(f"    the title did not start again within {seconds} s")
    return False


def wait_for_pad(state, seconds=60):
    """The slot of the first pad reporting (more than 50 reports), or None."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            for i, s in enumerate(state.slots()):
                if s["present"] and s["reports"] > 50:
                    return i
        except struct.error:  # not mapped for a moment
            pass
        time.sleep(1)
    return None


def check_shortcuts(pad, state, slot):
    """The in-game shortcut and the kernel's last resort (--igs); True when both power-cycled the console (the log line is checked once xemu has stopped,
    check_shortcut_log)."""
    ok = True
    for name, controls, hold in (("in-game shortcut BLACK + WHITE -> coldboot", ("RB", "LB"), 0.5),
                                 ("the kernel's last resort: LT + RT + BACK + BLACK held 3 s", ("LT", "RT", "BACK", "RB"), 3.5)):
        for control in controls:
            pad.set(control, 255 if control in ("LT", "RT") else 1)
        time.sleep(hold)
        for control in controls:
            pad.set(control, 0)
        restarted = wait_for_restart(state, slot)
        slot = wait_for_pad(state) if restarted else slot
        passed = restarted and slot is not None
        ok &= passed
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}: the console powered off and on, the title started again")
        if slot is None:
            return False
    return ok


def check_shortcut_log(hdd):
    """The shortcut's line in C:\\OXOS\\log\\extensions.log, read once xemu has written the disk and stopped."""
    log = (FATX(Qcow2(hdd), "C").read_file("OXOS\\log\\extensions.log") or b"").decode("latin-1").split("\r\n")
    logged = IGS_LOG_LINE in log
    print(f"  [{'PASS' if logged else 'FAIL'}] C:\\OXOS\\log\\extensions.log: '{IGS_LOG_LINE}'")
    return logged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mcpx-rom")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--hdd")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    ap.add_argument("--hold", type=float, default=0.3, help="seconds each input is held")
    ap.add_argument("--igs", metavar="XOS", help="oxos_igs.xos: also test the in-game shortcuts and the kernel's last resort (needs --hdd)")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xk-pad-")
    sock, cfg, hdd = (os.path.join(work, n) for n in ("qmp.sock", "xemu.toml", "hdd.qcow2"))
    if a.hdd:
        shutil.copyfile(a.hdd, hdd)
        if a.igs:
            install_igs(hdd, a.igs)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\nflashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\ndvd_path = '{os.path.abspath(a.iso)}'\n"
        + (f"hdd_path = '{hdd}'\n" if a.hdd else "") + XEMU_PORT1_BINDING)
    pad = VirtualPad()  # before xemu starts, so it is bound to a controller port at startup
    log = open(os.path.join(work, "xemu.log"), "w")
    env = xemu_env()
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"], stdout=log, stderr=subprocess.STDOUT, env=env)
    ok = False
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        state = PadState(sock, work)
        deadline = time.time() + 60
        while not state.locate():
            if time.time() > deadline:
                sys.exit(f"controller test title did not start (logs in {work})")
            time.sleep(2)
        print(f"  controller test title running, state at {state.addr:#x}")
        while not any(sl["present"] and sl["reports"] > 50 for sl in state.slots()):
            if time.time() > deadline + 30:
                sys.exit(f"  [FAIL] no controller connected (logs in {work})")
            time.sleep(1)

        # Our pad's slot: the controller whose report shows START while it is held.
        slot = None
        for _ in range(20):
            pad.set("START", 1)
            time.sleep(a.hold)
            found = [i for i, s in enumerate(state.slots()) if s["present"] and "START" in decode(s["last"])]
            pad.set("START", 0)
            time.sleep(a.hold)
            if found:
                slot = found[0]
                break
            time.sleep(1)
        if slot is None:
            present = [s for s in state.slots() if s["present"]]
            sys.exit(f"  [FAIL] no controller reports the virtual pad ({len(present)} present; logs in {work})")
        print(f"  virtual pad is controller {slot + 1} (type {state.slots()[slot]['type']:#06x})")

        ok = True
        for control, value, expected in STEPS:
            pad.set(control, value)
            time.sleep(a.hold)
            held = decode(state.slots()[slot]["last"])
            pad.set(control, 0)
            time.sleep(a.hold)
            released = decode(state.slots()[slot]["last"])
            passed = held == {expected} and not released
            ok &= passed
            note = "" if passed else f"  (held: {sorted(held) or '-'}, after release: {sorted(released) or '-'})"
            print(f"  [{'PASS' if passed else 'FAIL'}] {control}={value} -> {expected}{note}")
        reports = state.slots()[slot]["reports"]
        print(f"  {reports} input reports received")
        if a.igs and a.hdd:
            ok &= check_shortcuts(pad, state, slot)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        pad.close()
    if a.igs and a.hdd and ok:
        ok &= check_shortcut_log(hdd)
    if os.path.exists(hdd):
        os.remove(hdd)
    print("RESULT:", "PASS" if ok else f"FAIL (logs in {work})")
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
