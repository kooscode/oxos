#!/usr/bin/env python3
"""Milestone 1 boot test: boot a BIOS image in xemu and check, through the QMP monitor, that
the kernel reached its idle loop with the 1 ms clock running.

Pass conditions (spec/01 §3, spec/02 §3.5):
  * KiBootStage == 0x30 (idle)            * no exception recorded
  * KeTickCount advances ~1000/s           * EIP inside the kernel image

usage: m1_boot.py --bios build/bios.bin --mcpx-rom mcpx.bin --map build/xboxkrnl.map [--xemu PATH]
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def map_symbols(path):
    syms = {}
    for line in open(path):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16)
    return syms


def symbol(syms, name):
    for k, v in syms.items():
        if k in ("_" + name, name) or k.startswith(f"?{name}@@"):
            return v
    sys.exit(f"symbol {name} not in map")


class Monitor:
    def __init__(self, path):
        self.path = path

    def hmp(self, cmd):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path)
        f = s.makefile("rw")
        f.readline()
        f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": cmd}}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                break
        s.close()
        return r.get("return", r)

    def read32(self, addr):
        out = self.hmp(f"x/1wx {addr:#x}")
        return int(out.split(":")[1].split()[0], 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mcpx-rom", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    ap.add_argument("--boot-wait", type=float, default=8.0)
    a = ap.parse_args()

    syms = map_symbols(a.map)
    stage_addr = symbol(syms, "KiBootStage")
    tick_addr = symbol(syms, "KeTickCount")
    exc_addr = symbol(syms, "KiLastExceptionVector")
    exc_eip_addr = symbol(syms, "KiLastExceptionEip")

    work = tempfile.mkdtemp(prefix="xk-m1-")
    sock = os.path.join(work, "qmp.sock")
    cfg = os.path.join(work, "xemu.toml")
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom)}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\n")
    log = open(os.path.join(work, "xemu.log"), "w")
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"],
                            stdout=log, stderr=subprocess.STDOUT)
    ok = False
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        mon = Monitor(sock)
        time.sleep(a.boot_wait)
        stage = mon.read32(stage_addr)
        exc = mon.read32(exc_addr)
        t0 = mon.read32(tick_addr); w0 = time.monotonic()
        time.sleep(2.0)
        t1 = mon.read32(tick_addr); w1 = time.monotonic()
        regs = mon.hmp("info registers")
        eip = int(re.search(r"EIP=([0-9a-f]+)", regs).group(1), 16)
        rate = (t1 - t0) / (w1 - w0)

        print(f"KiBootStage = {stage:#x}")
        print(f"KiLastExceptionVector = {exc:#x}" + (f" at EIP {mon.read32(exc_eip_addr):#x}" if exc != 0xFFFFFFFF else ""))
        print(f"KeTickCount {t0} -> {t1}: {rate:.0f} ticks/s")
        print(f"EIP = {eip:#x}")
        checks = {
            "reached idle (stage 0x30)": stage == 0x30,
            "no exception": exc == 0xFFFFFFFF,
            "clock ~1000 ticks/s": 900 <= rate <= 1100,
            "EIP inside kernel": 0x80010000 <= eip < 0x80100000,
        }
        for name, passed in checks.items():
            print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
        ok = all(checks.values())
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    print("MILESTONE 1:", "PASS" if ok else f"FAIL (logs in {work})")
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
