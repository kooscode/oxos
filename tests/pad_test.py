#!/usr/bin/env python3
"""Controller input test: boot a BIOS with the padtest disc, press every input of a virtual gamepad and check
which Xbox controller input the title receives for each.

The title (tests/padtest) is built with nxdk and keeps its state in a block with the signature "XKPADTST"; this
script finds it in a RAM dump (QMP pmemsave) and then reads it through its virtual address after each press.

usage: pad_test.py --bios BIOS --mcpx-rom MCPX --iso padtest.iso [--hdd HDD] [--xemu PATH]
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpad import VPAD_PRODUCT, VPAD_VENDOR, VirtualPad  # noqa: E402

ANALOG = ["A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"]
DIGITAL = ["UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS"]
AXES = ["LX", "LY", "RX", "RY"]
SLOT_SIZE, SLOT_BASE, SLOTS = 64, 28, 4

# Host input (virtual pad control, value) -> the Xbox input it should produce (xemu's default pad mapping).
STEPS = [
    ("A", 1, "A"), ("B", 1, "B"), ("X", 1, "X"), ("Y", 1, "Y"),
    ("RB", 1, "BLACK"), ("LB", 1, "WHITE"), ("LT", 255, "LT"), ("RT", 255, "RT"),
    ("HATY", -1, "UP"), ("HATY", 1, "DOWN"), ("HATX", -1, "LEFT"), ("HATX", 1, "RIGHT"),
    ("START", 1, "START"), ("BACK", 1, "BACK"), ("LS", 1, "LS"), ("RS", 1, "RS"),
    ("LX", -32768, "LX-"), ("LX", 32767, "LX+"), ("LY", -32768, "LY+"), ("LY", 32767, "LY-"),
    ("RX", -32768, "RX-"), ("RX", 32767, "RX+"), ("RY", -32768, "RY+"), ("RY", 32767, "RY-"),
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
                # SDL ignores gamepads while the app has no focus unless told otherwise: without this, a test fails
                # whenever focus moves elsewhere (e.g. back to the terminal).
                SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1",
                # The user's own controllers would take controller port 1 and could press things mid-test.
                SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=f"0x{VPAD_VENDOR:04x}/0x{VPAD_PRODUCT:04x}")


def qmp(path, command, **arguments):
    s = socket.socket(socket.AF_UNIX)
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
        at = data.find(b"XKPADTST\x01\x00\x00\x00")
        while at >= 0:  # copies of the title image (file cache) still hold Self = 0
            self.addr = struct.unpack_from("<I", data, at + 12)[0]
            if self.addr:
                return True
            at = data.find(b"XKPADTST\x01\x00\x00\x00", at + 1)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mcpx-rom", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--hdd")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    ap.add_argument("--hold", type=float, default=0.3, help="seconds each input is held")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xk-pad-")
    sock, cfg, hdd = (os.path.join(work, n) for n in ("qmp.sock", "xemu.toml", "hdd.qcow2"))
    if a.hdd:
        shutil.copyfile(a.hdd, hdd)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom)}'\nflashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\ndvd_path = '{os.path.abspath(a.iso)}'\n"
        + (f"hdd_path = '{hdd}'\n" if a.hdd else ""))
    pad = VirtualPad()  # before xemu starts, so it is bound to a controller port at startup
    log = open(os.path.join(work, "xemu.log"), "w")
    env = xemu_env()
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"],
                            stdout=log, stderr=subprocess.STDOUT, env=env)
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
                sys.exit(f"padtest title did not start (logs in {work})")
            time.sleep(2)
        print(f"  padtest running, state at {state.addr:#x}")
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
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        pad.close()
    if os.path.exists(hdd):
        os.remove(hdd)
    print("RESULT:", "PASS" if ok else f"FAIL (logs in {work})")
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
