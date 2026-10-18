#!/usr/bin/env python3
"""Debug an nxdk app on OXOS in xemu at source level, with gdb or VS Code (tools/vscode/).

usage: debug-app.py APP [--port 1234] [--hdd HDD.qcow2]
       debug-app.py APP --stop
       debug-app.py [--iso GAME.iso] [--port 1234] [--hdd HDD.qcow2]     (the kernel alone)
       debug-app.py --stop

APP is the app's folder, built with `make DEBUG=y` (main.exe and bin/), or an XISO of it.
1. Builds the debug kernel (kernel/build/debug, the same as `make run-gdb`) when it is out of date.
2. Writes APP/.oxos/main.elf (main.exe's debug info in the form gdb reads best) and APP/.oxos/app.iso (bin/ as a
   disc image; an ISO given as APP is used as it is).
3. Starts xemu with that disc, the debug serial port printed here, and the GDB server on localhost:PORT, paused at
   power-on. "OXOS debug: ready" is printed once a debugger can attach; the kernel boots the disc's default.xbe when
   the debugger continues. Symbols: APP/.oxos/main.elf and kernel/build/debug/xboxkrnl.elf.
--stop closes the xemu this script started for APP (VS Code's postDebugTask).
Without APP it debugs the kernel alone (the repository's .vscode/ uses this): the same debug kernel, booting --iso
if given, otherwise the hard disk's dashboard; the state is kept in kernel/build/debug/.
"""
import argparse, importlib.util, os, signal, subprocess, sys, time

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
kernel = os.path.join(root, "kernel")


def say(text):
    print(f"OXOS debug: {text}", flush=True)


def fail(text):
    say(f"error: {text}")
    sys.exit(1)


def listening(port):
    """Whether something listens on 127.0.0.1:port (read from /proc: connecting would let xemu's GDB server
    resume the paused console when the probe disconnects)."""
    want = f"0100007F:{port:04X}"
    try:
        for line in open("/proc/net/tcp").read().splitlines()[1:]:
            f = line.split()
            if f[1] == want and f[3] == "0A":
                return True
    except OSError:
        pass
    return False


def build_kernel():
    r = subprocess.run(["make", "-s", "-C", kernel, "BUILD=build/debug", "SELFTEST=0", "SERIAL=1", "DEBUG=1",
                        "build/debug/bios.bin", "hdd-image"])  # hdd-image: the default disk, fetched once
    if r.returncode:
        fail("building the debug kernel failed")


def prepare(app, out):
    """The disc image to boot and the app's symbol file (or None)."""
    os.makedirs(out, exist_ok=True)
    if app.lower().endswith(".iso"):
        iso, folder = app, os.path.dirname(app)
    else:
        folder, bin_dir = app, os.path.join(app, "bin")
        if not os.path.isfile(os.path.join(bin_dir, "default.xbe")):
            fail(f"no {bin_dir}/default.xbe: build the app first (make DEBUG=y)")
        spec = importlib.util.spec_from_file_location("mkxiso", os.path.join(root, "tests/mkxiso.py"))
        mkxiso = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mkxiso)
        iso = os.path.join(out, "app.iso")
        mkxiso.write_image(iso, mkxiso.tree_from_dir(bin_dir))
    exe, elf = os.path.join(folder, "main.exe"), os.path.join(out, "main.elf")
    if not os.path.isfile(exe):
        say(f"warning: no {exe}: no source-level symbols for the app")
        return iso, None
    r = subprocess.run(["objcopy", "-O", "elf32-i386", exe, elf])
    if r.returncode:
        fail(f"converting {exe} for gdb failed")
    return iso, elf


def stop(pid_file):
    try:
        pid = int(open(pid_file).read())
    except (OSError, ValueError):
        return
    try:
        os.kill(pid, signal.SIGTERM)  # run.py closes xemu and cleans up
        for _ in range(100):
            os.kill(pid, 0)
            time.sleep(0.1)
    except ProcessLookupError:
        pass
    try:
        os.remove(pid_file)
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("app", nargs="?", help="the nxdk app's folder (or an XISO); none: the kernel alone")
    ap.add_argument("--iso", help="without APP: a disc image to boot (default: none, the dashboard)")
    ap.add_argument("--port", type=int, default=1234, help="GDB server port (default 1234)")
    ap.add_argument("--hdd", help="hard disk image (default: tools/run.py's)")
    ap.add_argument("--stop", action="store_true", help="close the xemu started for this app")
    a = ap.parse_args()
    app = os.path.abspath(a.app) if a.app else None
    if app:
        out = os.path.join(app if os.path.isdir(app) else os.path.dirname(app), ".oxos")
    else:
        out = os.path.join(kernel, "build/debug")
    pid_file = os.path.join(out, "run.pid")
    if a.stop:
        stop(pid_file)
        return
    say("starting")
    stop(pid_file)  # an earlier session's xemu, if it is still running
    if listening(a.port):
        fail(f"port {a.port} is in use (another debug session?)")
    build_kernel()
    if app:
        iso, elf = prepare(app, out)
    else:
        iso, elf = (os.path.abspath(a.iso) if a.iso else None), None  # an empty --iso (VS Code prompt): no disc
        if iso and not os.path.isfile(iso):
            fail(f"no disc image {iso}")
    bios = os.path.join(kernel, "build/debug/bios.bin")
    cmd = [sys.executable, "-I", os.path.join(root, "tools/run.py"), bios, "--serial", "--gdb", str(a.port)]
    hdd = a.hdd
    msdash = os.path.join(root, "xbox-files/hdd/xbox_hdd_msdash.qcow2")
    if not hdd and not app and os.path.isfile(msdash):
        hdd = msdash  # like make run: the stock dashboard when there is one (a copy is used, never the image)
    cmd += (["--iso", iso] if iso else []) + (["--hdd", hdd] if hdd else [])
    run = subprocess.Popen(cmd)
    open(pid_file, "w").write(str(run.pid))
    signal.signal(signal.SIGTERM, lambda *_: run.terminate())
    signal.signal(signal.SIGINT, lambda *_: run.terminate())
    while not listening(a.port):
        if run.poll() is not None:
            fail("xemu did not start")
        time.sleep(0.1)
    say(f"symbols: {elf or ('(none for the app)' if app else '(kernel only)')} and "
        f"{os.path.join(kernel, 'build/debug/xboxkrnl.elf')}")
    say(f"ready: gdb server on localhost:{a.port} (paused at power-on)")
    run.wait()
    try:
        os.remove(pid_file)
    except OSError:
        pass
    sys.exit(run.returncode)


if __name__ == "__main__":
    main()
