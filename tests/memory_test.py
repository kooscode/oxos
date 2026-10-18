#!/usr/bin/env python3
"""Memory test: boot a BIOS with the memory test title (tests/memory-test-title) at 64 or 128 MB and report how much
memory the title is given, with and without the XBE's "limit to 64 MB" init flag (0x4).

The title keeps its results in a block with the signature "XKMEMTST"; this script finds it in a RAM dump (QMP
pmemsave), waits until the title has finished, and prints the measurements. With --both it runs the title twice:
as built (the flag set, like retail games) and with the flag cleared.

usage: memory_test.py --bios BIOS --xbe default.xbe [--memory 64|128] [--both | --no-limit] [--mcpx-rom MCPX]
                      [--hdd HDD] [--xemu PATH]
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkxiso  # noqa: E402

MAGIC = b"XKMEMTST\x01\x00\x00\x00"
FIELDS = ("Done InitFlags TotalPages AvailablePages HighContiguous Blocks Bytes PagesAbove64MB HighestPhysical "
          "FailStatus TotalPagesEnd AvailablePagesEnd").split()


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


def results(sock, work, addr):
    """The result block at addr (virtual), as a dict."""
    dump = os.path.join(work, "results.bin")
    qmp(sock, "memsave", val=addr + 16, size=4 * len(FIELDS), filename=dump)
    return dict(zip(FIELDS, struct.unpack(f"<{len(FIELDS)}I", open(dump, "rb").read())))


def locate(sock, work):
    """The result block's virtual address, once the title runs (the title image is in the low 64 MB)."""
    ram = os.path.join(work, "ram.bin")
    qmp(sock, "pmemsave", val=0, size=0x4000000, filename=ram)
    if not os.path.exists(ram):
        return None
    data = open(ram, "rb").read()
    os.remove(ram)
    at = data.find(MAGIC)
    while at >= 0:  # copies of the title image (file cache) still hold Self = 0
        addr = struct.unpack_from("<I", data, at + 12)[0]
        if addr:
            return addr
        at = data.find(MAGIC, at + 1)
    return None


def run(a, xbe, label):
    work = tempfile.mkdtemp(prefix="xk-mem-")
    sock, cfg, hdd, iso = (os.path.join(work, n) for n in ("qmp.sock", "xemu.toml", "hdd.qcow2", "test.iso"))
    mkxiso.write_image(iso, {"default.xbe": xbe})
    if a.hdd:
        shutil.copyfile(a.hdd, hdd)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n"
        f"[sys]\nmem_limit = '{a.memory}'\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\nflashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\ndvd_path = '{iso}'\n"
        + (f"hdd_path = '{hdd}'\n" if a.hdd else ""))
    log = open(os.path.join(work, "xemu.log"), "w")
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"],
                            stdout=log, stderr=subprocess.STDOUT)
    r = None
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        deadline = time.time() + 90
        addr = None
        while addr is None and time.time() < deadline:
            time.sleep(2)
            addr = locate(sock, work)
        while addr and time.time() < deadline:
            r = results(sock, work, addr)
            if r["Done"]:
                break
            time.sleep(1)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if not r or not r["Done"]:
        print(f"  [FAIL] {label}: the memory test title did not finish (logs in {work})")
        return False
    shutil.rmtree(work, ignore_errors=True)
    print(f"  {label}: init flags {r['InitFlags']:#x}")
    print(f"    kernel reports {r['TotalPages']} pages ({r['TotalPages'] * 4096 >> 20} MB), "
          f"{r['AvailablePages']} available ({r['AvailablePages'] * 4096 >> 20} MB)")
    print(f"    contiguous page above 64 MB: {r['HighContiguous']:#x}" + (" (none)" if not r["HighContiguous"] else ""))
    print(f"    title memory: {r['Bytes'] >> 20} MB in {r['Blocks']} blocks, {r['PagesAbove64MB']} pages above 64 MB, "
          f"highest physical {r['HighestPhysical']:#x}; ended by status {r['FailStatus']:#010x}")
    print(f"    at the end: {r['AvailablePagesEnd']} pages available")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--xbe", required=True, help="the memory test title's default.xbe (tests/memory-test-title/bin)")
    ap.add_argument("--memory", choices=["64", "128"], default="64")
    ap.add_argument("--no-limit", action="store_true", help="clear the XBE's 'limit to 64 MB' init flag")
    ap.add_argument("--both", action="store_true", help="run as built, then with the flag cleared")
    ap.add_argument("--mcpx-rom")
    ap.add_argument("--hdd")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    a = ap.parse_args()
    xbe = open(a.xbe, "rb").read()
    flags = struct.unpack_from("<I", xbe, 0x124)[0]
    unlimited = bytearray(xbe)
    struct.pack_into("<I", unlimited, 0x124, flags & ~4)
    runs = [(xbe, "as built"), (bytes(unlimited), "flag 0x4 cleared")] if a.both else \
           [(bytes(unlimited), "flag 0x4 cleared")] if a.no_limit else [(xbe, "as built")]
    print(f"memory test: {os.path.basename(a.bios)} at {a.memory} MB")
    ok = all([run(a, x, label) for x, label in runs])
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
