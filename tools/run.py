#!/usr/bin/env python3
"""Boot a BIOS in xemu interactively, with an optional disc image.

usage: run.py BIOS [--iso GAME.iso] [--hdd HDD.qcow2] [--state DIR [--fresh]] [--widescreen] [--size WxH]
              [--scale N] [--fullscreen] [--serial] [--qmp SOCK] [--memory 128] [--trace-map MAP [--dump-dir DIR]]
              [--gdb PORT] [--keep]

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
Debugging: --serial prints the debug serial port (spec/oxos-internal/03-debug-serial.md) here; --qmp opens xemu's QMP socket at SOCK (used by
tools/trace.py); --memory 128 gives the console 128 MB (needed by TRACE_IMPORTS=2 builds).
--trace-map MAP (an import-trace build's xboxkrnl.map) dumps the title's last kernel calls when the kernel reports a
crash on the serial port (a line starting with "***", needs --serial) and when xemu is closed: the last 30 here, all
of them in DIR/calls-<time>-<crash|exit>.txt, and the raw record next to it (.bin, for tools/trace.py --load).
xemu quits at once when its window is closed, so run.py also copies the record every second while the title runs
(without pausing it) and saves the latest copy as the exit dump when xemu is gone.
--gdb PORT starts xemu's GDB server on localhost:PORT, paused at power-on until a debugger continues it (see
tools/debug-app.py).
"""
import argparse, json, os, shutil, signal, socket, struct, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SIZES = ["640x480", "720x480", "1280x720", "1280x800", "1280x960", "1920x1080", "2560x1440", "2560x1600",
         "2560x1920", "3840x2160"]
ap = argparse.ArgumentParser()
ap.add_argument("bios")
ap.add_argument("--iso", help="disc image (XISO) for the DVD drive")
ap.add_argument("--hdd", default=os.path.join(root, "third_party/xemu-dashboard/xemu-dashboard.qcow2"),
                help="hard-disk image (default: the xemu-dashboard disk; `make -C kernel hdd-image` fetches it)")
ap.add_argument("--mcpx-rom", help="MCPX boot ROM dump (needed only for a Microsoft BIOS)")
ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
ap.add_argument("--state", help="directory for a persistent HDD copy and EEPROM")
ap.add_argument("--fresh", action="store_true", help="start the --state directory over from --hdd")
ap.add_argument("--widescreen", action="store_true", help="EEPROM widescreen flag (16:9 in games that support it)")
ap.add_argument("--hd", nargs="?", const="480p,720p,1080i", metavar="MODES",
                help="HDTV AV pack and the EEPROM's HD mode flags (default all: 480p,720p,1080i), so games that "
                "support HD modes use them (component output, as HDMI adapters take)")
ap.add_argument("--encoder", choices=["conexant", "focus", "xcalibur"],
                help="the video encoder xemu presents (default conexant; focus = 1.4-1.5 consoles, xcalibur = 1.6)")
ap.add_argument("--avpack", choices=["composite", "svideo", "scart", "hdtv", "vga", "rfu", "none"],
                help="the AV cable xemu presents (default: xemu's own, composite; hdtv with --hd)")
ap.add_argument("--aspect", choices=["auto", "native", "4x3", "16x9"], help="xemu aspect ratio (default auto)")
ap.add_argument("--size", choices=SIZES, help="window size")
ap.add_argument("--scale", type=int, choices=range(1, 11), metavar="1-10", help="render resolution multiplier")
ap.add_argument("--fullscreen", action="store_true")
ap.add_argument("--serial", action="store_true", help="attach xemu's SuperIO and print the debug serial port here")
ap.add_argument("--qmp", help="open xemu's QMP socket at this path (tools/trace.py reads the import trace there)")
ap.add_argument("--memory", choices=["64", "128"], help="console memory in MB (default xemu's setting)")
ap.add_argument("--trace-map", help="import-trace build's map: dump the kernel calls on a crash and on exit")
ap.add_argument("--dump-dir", default=".", help="where --trace-map writes the dumps (default: here)")
ap.add_argument("--exports", default=os.path.join(root, "kernel/exports.tsv"), help="export names for the dumps")
ap.add_argument("--gdb", type=int, metavar="PORT", help="GDB server on localhost:PORT, paused at power-on")
ap.add_argument("--keep", action="store_true", help="keep the temporary directory (HDD copy, EEPROM, log)")
ap.add_argument("--net-udp", type=int, metavar="PORT",
                help="network: xemu's udp backend, sending the console's Ethernet frames to 127.0.0.1:PORT "
                "(tools/netlog.py --xemu PORT prints a NETLOG=1 build's log from them)")
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
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n" + "\n".join(display)
        + ("\n[sys]\n" + (f"mem_limit = '{a.memory}'\n" if a.memory else "")
           + (f"avpack = '{a.avpack}'\n" if a.avpack else "") if a.memory or a.avpack else "") + "\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.abspath(eeprom)}'\n"
        f"hdd_path = '{os.path.abspath(hdd)}'\n" + extra
        + (f"\n[net]\nenable = true\nbackend = 'udp'\n[net.udp]\nbind_addr = '127.0.0.1:{a.net_udp + 1}'\n"
           f"remote_addr = '127.0.0.1:{a.net_udp}'\n" if a.net_udp else ""))


def follow(path, proc, on_line=None):
    """Print what xemu writes to the serial log file as it arrives; on_line gets each complete line."""
    while not os.path.exists(path) and proc.poll() is None:
        time.sleep(0.1)
    partial = ""
    with open(path, "rb") as f:
        while True:
            data = f.read()
            if data:
                text = data.decode("latin1").replace("\r\n", "\n")
                sys.stdout.write(text)
                sys.stdout.flush()
                if on_line:
                    *lines, partial = (partial + text).split("\n")
                    for line in lines:
                        on_line(line)
            elif proc.poll() is not None:
                return
            else:
                time.sleep(0.1)


class CallDumper:
    """--trace-map: the title's last kernel calls (tools/trace.py), on a kernel crash and when xemu is closed."""

    def __init__(self, map_path, exports, out_dir, events_sock, cmd_sock):
        import importlib.util
        spec = importlib.util.spec_from_file_location("xk_trace", os.path.join(root, "tools/trace.py"))
        self.trace = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.trace)
        self.map, self.table, self.out = map_path, self.trace.names(exports), out_dir
        self.events, self.cmd = events_sock, cmd_sock
        self.lock, self.last_crash, self.exit_dumped = threading.Lock(), 0.0, False
        self.latest = None  # (count, data, time) of the last background copy
        self.dumped = None  # (count, file) of the last dump written

    def snapshots(self, proc):
        """Copy the record every second, for the exit dump when xemu quits before it can be read."""
        while proc.poll() is None and not self.exit_dumped:
            time.sleep(1)
            with self.lock:
                if self.exit_dumped:
                    return
                try:
                    count, data = self.trace.grab(self.cmd, self.map, pause=False)
                    self.latest = (count, data, time.monotonic())
                except Exception:
                    pass  # nothing recorded yet, or xemu is going away

    def dump(self, reason):
        with self.lock:
            if reason == "exit":
                if self.exit_dumped:
                    return
                self.exit_dumped = True
            note = ""
            try:
                count, data = self.trace.grab(self.cmd, self.map)
            except (self.trace.TraceError, OSError) as e:
                if reason != "exit" or not self.latest:
                    print(f"\n[no call dump ({reason}): {e}]", flush=True)
                    return
                count, data, taken = self.latest  # xemu already quit: the last background copy
                note = f" (copied {time.monotonic() - taken:.1f} s before xemu quit)"
            if self.dumped and self.dumped[0] == count:  # e.g. closed after a crash: nothing new to show
                print(f"\n[{reason}: no kernel calls since the dump above ({self.dumped[1]})]", flush=True)
                return
            os.makedirs(self.out, exist_ok=True)
            base = os.path.join(self.out, f"calls-{time.strftime('%Y%m%d-%H%M%S')}-{reason}")
            self.trace.save(base + ".bin", count, data)
            full = self.trace.report(count, data, self.table, last=0, show_all=True)
            open(base + ".txt", "w").write("\n".join(full) + "\n")
            self.dumped = (count, base + ".txt")
            print(f"\n=== {reason}: the title's last kernel calls{note} (all of them: {base}.txt) ===")
            print("\n".join(self.trace.report(count, data, self.table, last=30)))
            print("===", flush=True)

    def on_serial(self, line):
        # One dump per crash: an unexpected exception is often followed by a bug check line.
        if line.startswith("***") and time.monotonic() - self.last_crash > 2:
            self.last_crash = time.monotonic()
            self.dump("crash")

    def finish(self, proc):
        """run.py is stopping (Ctrl-C, kill, terminal closed): dump while xemu still runs, then quit it."""
        if proc.poll() is None:
            self.dump("exit")
            try:
                self.trace.Qmp(self.cmd).command("quit")
            except Exception:
                pass

    def watch(self, proc):
        """Wait for xemu to shut down, dump, then make sure it quits. A powered-off console pauses (-no-shutdown) and
        is dumped live; a closed window or a signal to xemu itself makes it quit at once, so the dump is the last
        background copy. run.py starts xemu in its own session, so Ctrl-C reaches only run.py, which dumps live
        first (finish)."""
        q = None
        try:
            for _ in range(300):
                if proc.poll() is not None:
                    return
                try:
                    q = self.trace.Qmp(self.events)
                    break
                except self.trace.TraceError:
                    time.sleep(0.1)
            else:
                print("\n[no QMP connection to xemu: no dump on exit]", flush=True)
                return
            while True:
                line = q.f.readline()
                if not line or json.loads(line).get("event") == "SHUTDOWN":
                    break  # shutting down, or already gone (window closed)
            self.dump("exit")
        except Exception as e:  # never leave a paused xemu behind
            print(f"\n[call dump on exit failed: {e}]", flush=True)
        finally:
            if proc.poll() is None:
                try:
                    self.trace.Qmp(self.cmd).command("quit")
                except Exception:
                    pass
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()


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


def set_video_flags(bits, what):
    """User settings section 0x60-0xBF: video flags (setting 8) at 0x94: widescreen 0x00010000, HD modes 0x00020000
    (720p), 0x00040000 (1080i), 0x00080000 (480p) (as the Microsoft dashboard writes them with all HD modes on); the
    section's first dword makes it sum to 0xFFFFFFFF (32-bit sum with end-around carry, spec 12)."""
    data = bytearray(open(eeprom, "rb").read())
    flags, = struct.unpack_from("<I", data, 0x94)
    if flags & bits == bits:
        return
    struct.pack_into("<I", data, 0x94, flags | bits)
    struct.pack_into("<I", data, 0x60, 0)
    total = 0
    for i in range(0x60, 0xC0, 4):
        total += struct.unpack_from("<I", data, i)[0]
        total = (total & 0xFFFFFFFF) + (total >> 32)
    struct.pack_into("<I", data, 0x60, ~total & 0xFFFFFFFF)
    open(eeprom, "wb").write(data)
    print(f"EEPROM: {what} on")


# Stopped with a signal (kill, closing the terminal): close xemu too and clean up, instead of leaving it running.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
signal.signal(signal.SIGHUP, lambda *_: sys.exit(1))
p = dumper = watcher = None
try:
    if a.hd and not a.avpack:
        a.avpack = "hdtv"
    hd_bits = sum({"480p": 0x00080000, "720p": 0x00020000, "1080i": 0x00040000}[m] for m in (a.hd or "").split(",") if m)
    for wanted, bits, what in ((a.widescreen, 0x00010000, "widescreen"), (a.hd, hd_bits, a.hd)):
        if wanted:
            if os.path.exists(eeprom) or create_eeprom():
                set_video_flags(bits, what)
            else:
                print(f"warning: no EEPROM to set the {what} flags in", file=sys.stderr)
    cfg = os.path.join(work, "xemu.toml")
    config(cfg, f"dvd_path = '{os.path.abspath(a.iso)}'\n" if a.iso else "")
    print(f"xemu: {a.bios}" + (f" + {a.iso}" if a.iso else " (no disc)")
          + (f"; state {a.state}" if a.state else f"; work dir {work}"))
    # The serial port goes to a file that a thread copies to this terminal (a stdio chardev would put the terminal
    # in raw mode).
    serial_log = os.path.join(work, "serial.log")
    serial = ["-device", "lpc47m157", "-serial", f"file:{serial_log}"] if a.serial else []
    qmp = []
    if a.qmp:
        if os.path.exists(a.qmp):
            os.remove(a.qmp)  # left by an earlier run
        qmp = ["-qmp", f"unix:{os.path.abspath(a.qmp)},server,nowait"]
        print(f"QMP socket: {a.qmp}")
    if a.trace_map:
        # Two private QMP sockets (each takes one client): xemu's events, and the dumps' commands.
        events, cmd = os.path.join(work, "events.sock"), os.path.join(work, "cmd.sock")
        dumper = CallDumper(a.trace_map, a.exports, a.dump_dir, events, cmd)
        qmp += ["-qmp", f"unix:{events},server,nowait", "-qmp", f"unix:{cmd},server,nowait", "-no-shutdown"]
        print(f"kernel call dumps (on a crash and on exit): {a.dump_dir}")
    gdb = []
    if a.gdb:
        gdb = ["-gdb", f"tcp:127.0.0.1:{a.gdb}", "-S"]
        print(f"GDB server: localhost:{a.gdb} (paused at power-on until the debugger continues)")
    encoder = ["-machine", f"video-encoder={a.encoder}"] if a.encoder else []
    p = subprocess.Popen([a.xemu, "-config_path", cfg] + serial + qmp + gdb + encoder, stdout=open(os.path.join(work, "xemu.log"), "w"),
                         stderr=subprocess.STDOUT, start_new_session=True)
    if a.serial:
        threading.Thread(target=follow, args=(serial_log, p, dumper.on_serial if dumper else None),
                         daemon=True).start()
    if dumper:
        watcher = threading.Thread(target=dumper.watch, args=(p,), daemon=True)
        watcher.start()
        threading.Thread(target=dumper.snapshots, args=(p,), daemon=True).start()
    p.wait()
    time.sleep(0.3)  # the last lines
except KeyboardInterrupt:
    pass
finally:
    if p and dumper:
        dumper.finish(p)
        watcher.join(30)  # still writing the exit dump
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(15)
        except subprocess.TimeoutExpired:
            p.kill()
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
