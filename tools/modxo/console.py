#!/usr/bin/env python3
"""Host side of the Modxo debug rig (README.md in this folder): log a console's boot and press its power button.

The rig is an RP2040 Modxo modchip running the oxos_debug firmware module. Its USB serial port carries, in one stream:
  - the emulated 16550 UART's output (COM1 at 0x3F8: an OXOS kernel's serial log), as plain bytes;
  - port 0x80 (POST code) writes, as the two bytes 0xFE <code>;
  - the Pico's own status lines (power and reset pins, LPC cycle counts, early memory reads), as 0xFC <text> 0x0A.
0xFE and 0xFC are reserved, so binary COM1 output (a kernel debugger's packets) can be misread; use --raw for it. The Pico answers the console's Super I/O probe
only while this port is open with DTR set, so the logger keeps it open.

  console.py                                            list the connected Modxos: serial number, port, logger and power state
  console.py log [SERIAL] [--out FILE] [--raw FILE] [--no-dtr] [--gdb PORT]   run the logger (one per Pico; it owns the serial port); --gdb:
                                                        also a bridge for gdb to a GDB=1 kernel's stub (tools/gdb_bridge.py), on localhost:PORT
  console.py state SERIAL                               print the power state (on / off / unknown / no pico)
  console.py on | off SERIAL                            switch the console on / off (no press if it already is)
  console.py power SERIAL                               press the power button once (a toggle)
  console.py cycle SERIAL [--off S]                     off, wait S seconds (default 8), on

SERIAL is the Pico's USB serial number (the list shows them; /dev/serial/by-id/usb-Raspberry_Pi_Pico_SERIAL-if00). A command without SERIAL lists the
Modxos instead. The commands talk to that Pico's logger over UDP on 127.0.0.1: each logger takes the first free port from 47800 and answers with its serial
number; when the Pico has no logger, one is started in the background (its log in /data/sources/tmp/console_SERIAL.txt). --port talks to the logger on that
port instead, whatever its Pico. The press lasts 2 s (the firmware holds the pin). The power state comes from the status lines' 3v3 level (LPC power).
The firmware (kooscode/modxo, branch oxos) takes the press wherever 0xFD 'P' is in the stream, and writes its status lines whole. As a safety net, when a
press does not get through, on and off reset the Pico's USB port (UDP command R), which empties its buffers; the logger does the same when the Pico stops
taking bytes. The logger also answers UDP command F<text> with its latest log line that contains <text> (tools/debug_console.py finds the FTP server's
address that way).

Log lines: "HH:MM:SS.mmm post XX" per POST code, "... uart <text>" per COM1 line (a line without its newline is printed after 0.5 s without COM1 bytes, marked
"[partial line]"), "... pico <status>" per status line. Needs pyserial.
"""
import argparse
import collections
import glob
import os
import select
import socket
import subprocess
import sys
import time

PORT = 47800        # the first logger's UDP port; later loggers take the next free ones
PORTS = 20          # ports searched for loggers
BY_ID = "/dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00"
LOG_DIR = "/data/sources/tmp"  # where a logger started for a command writes its log


def picos():
    """The connected Picos: {serial number: device (/dev/ttyACMn)}."""
    found = {}
    for link in sorted(glob.glob(BY_ID)):
        found[os.path.basename(link)[len("usb-Raspberry_Pi_Pico_"):-len("-if00")]] = os.path.realpath(link)
    return found


def serial_of(dev):
    """The serial number of the Pico at dev, or "" when it is not one."""
    real = os.path.realpath(dev)
    return next((sn for sn, d in picos().items() if d == real), "")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))  # tools/: gdb_bridge


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)


def log(args):
    import serial
    out = open(args.out, "a", buffering=1) if args.out else None
    raw = open(args.raw, "ab", buffering=0) if args.raw else None  # COM1 bytes exactly as sent
    recent = collections.deque(maxlen=2000)  # the latest log lines, for the F command

    def emit(line):
        line = stamp() + " " + line
        print(line, flush=True)
        if out:
            out.write(line + "\n")
        recent.append(line)

    if args.serial:
        dev = picos().get(args.serial)
        if not dev:
            sys.exit("no Pico with serial number %s is connected" % args.serial)
        args.dev = "/dev/serial/by-id/usb-Raspberry_Pi_Pico_%s-if00" % args.serial  # stays right when the ttyACM numbers change
    ident = (args.serial or serial_of(args.dev)).encode()
    gdb = None  # the gdb bridge (--gdb): COM1 bytes are split into log text and the stub's packets
    if args.gdb:
        import gdb_bridge
        ser_holder = []
        gdb = gdb_bridge.GdbServer(args.gdb, lambda data: ser_holder[0].write(data) if ser_holder else None, emit)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.port is not None:
        sock.bind(("127.0.0.1", args.port))
    else:  # the first free port from PORT
        for port in range(PORT, PORT + PORTS):
            try:
                sock.bind(("127.0.0.1", port))
                break
            except OSError:
                continue
        else:
            sys.exit("no free UDP port in %d-%d" % (PORT, PORT + PORTS - 1))
    emit("commands on 127.0.0.1:%d, Pico %s" % (sock.getsockname()[1], ident.decode() or "unknown"))
    sock.setblocking(False)
    stalled_ready = False  # the gdb bridge's readiness, kept over a reopen after a stalled write (the stub is still there)
    power = None  # the last 3v3 level reported by the Pico: True, False or None (not seen yet)
    while True:
        try:
            # A write limit: the Pico stops taking bytes when its receive side stalls, and a blocked write would freeze the logger (and the power button)
            ser = serial.Serial(args.dev, 115200, timeout=0.05, write_timeout=2)
            ser.dtr = not args.no_dtr  # without DTR the Pico does not answer the Super I/O probe: no COM1
        except (serial.SerialException, OSError) as e:
            emit("port %s: %s (retrying)" % (args.dev, e))
            power = None
            end = time.time() + 1
            while time.time() < end:  # meanwhile answer state queries: "no pico"
                try:
                    cmd, peer = sock.recvfrom(16)
                    if cmd == b"S":
                        sock.sendto(b"no pico", peer)
                    elif cmd == b"I":
                        sock.sendto(ident, peer)
                    elif cmd == b"P":
                        emit("power button pulse not sent: no Pico")
                except BlockingIOError:
                    time.sleep(0.05)
            continue
        emit("opened " + args.dev)
        splitter = None
        if gdb:
            ser_holder[:] = [ser]
            gdb.console_off()  # until the stub says it is ready
            gdb.ready = args.gdb_ready or stalled_ready
            stalled_ready = False

            def on_line(line):
                gdb.on_line(line)
                emit("uart " + line)

            splitter = gdb_bridge.RspSplitter(on_line, gdb.to_gdb, gdb.to_gdb)
        text = bytearray()
        status = None
        want_code = False
        last_rx = time.time()
        try:
            while True:
                try:
                    cmd, peer = sock.recvfrom(256)
                    if cmd[:1] == b"F":  # the latest log line with this text
                        found = next((line for line in reversed(recent) if cmd[1:].decode("latin-1") in line), "")
                        sock.sendto(found.encode("latin-1")[-1000:], peer)
                    elif cmd == b"P":
                        if args.no_dtr:  # the Pico takes commands only while DTR is set: raise it for the press
                            ser.dtr = True
                            time.sleep(0.3)
                        ser.write(b"\xfdP")
                        emit("power button pulse")
                        if args.no_dtr:
                            time.sleep(2.5)
                            ser.dtr = False
                    elif cmd == b"S":
                        sock.sendto({True: b"on", False: b"off", None: b"unknown"}[power], peer)
                    elif cmd == b"I":
                        sock.sendto(ident, peer)
                    elif cmd == b"G":  # the gdb bridge: whether the stub has said it is ready since the kernel last started
                        sock.sendto(b"ready" if gdb and gdb.ready else (b"waiting" if gdb else b"no bridge"), peer)
                    elif cmd == b"R":
                        raise PicoUsbReset()
                except BlockingIOError:
                    pass
                if gdb:  # wake on either side at once: a 50 ms read would add 50 ms to every gdb round trip
                    readable, _, _ = select.select([ser.fileno()] + gdb.sockets(), [], [], 0.05)
                    data = ser.read(ser.in_waiting) if ser.fileno() in readable else b""
                    gdb.poll(readable)
                    splitter.flush_partial()
                else:
                    data = ser.read(4096)
                if text and time.time() - last_rx > 0.5:  # 0.5 s without COM1 bytes (status lines don't count)
                    emit("uart " + text.decode("latin-1").rstrip("\r") + "  [partial line]")
                    text.clear()
                for b in data:
                    if status is not None:
                        if b == 0x0A:
                            line = status.decode("latin-1")
                            if line.startswith("3v3="):
                                power = line[4] == "1"
                                if gdb and not power:
                                    gdb.console_off()
                            emit("pico " + line)
                            status = None
                        else:
                            status.append(b)
                    elif want_code:
                        emit("post %02x" % b)
                        want_code = False
                    elif b == 0xFE:
                        want_code = True
                    elif b == 0xFC:
                        status = bytearray()
                    elif splitter:
                        splitter.feed(bytes([b]))
                        if raw:
                            raw.write(bytes([b]))
                    elif b == 0x0A:
                        if raw:
                            raw.write(b"\n")
                        emit("uart " + text.decode("latin-1").rstrip("\r"))
                        text.clear()
                    else:
                        text.append(b)
                        last_rx = time.time()
                        if raw:
                            raw.write(bytes([b]))
        except (serial.SerialTimeoutException, PicoUsbReset) as e:
            if isinstance(e, PicoUsbReset):
                emit("port: resetting the Pico's USB port (empties its buffers)")
            else:
                emit("port: the Pico takes no bytes (its receive side stalled): resetting its USB port")
            stalled_ready = bool(gdb and gdb.ready)
            if gdb:
                gdb.held.clear()  # what gdb sent meanwhile: it sends again
            try:
                ser.reset_output_buffer()  # the bytes the Pico would not take: closing would wait 30 s (the tty's closing_wait) for them
                ser.close()
            except Exception:
                pass
            try:
                reset_pico_usb(args.dev)
            except OSError as e:
                emit("port: USB reset failed: %s" % e)
            time.sleep(1)  # the Pico enumerates again
        except (serial.SerialException, OSError) as e:
            emit("port lost: %s" % e)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1)


class PicoUsbReset(Exception):
    """The UDP command R: reset the Pico's USB port."""


def reset_pico_usb(dev):
    """A USB port reset of the Pico (USBDEVFS_RESET on its /dev/bus/usb node): TinyUSB empties both serial buffers, so bytes a stopped console never read
    no longer sit in front of the power button command. The Pico keeps serving the console (LPC) and its flash is untouched; the tty gets a hang-up."""
    import fcntl
    usb = os.path.realpath("/sys/class/tty/%s/device/.." % os.path.basename(os.path.realpath(dev)))
    with open(usb + "/busnum") as f:
        bus = int(f.read())
    with open(usb + "/devnum") as f:
        number = int(f.read())
    fd = os.open("/dev/bus/usb/%03d/%03d" % (bus, number), os.O_WRONLY)
    try:
        fcntl.ioctl(fd, 0x5514, 0)  # USBDEVFS_RESET
    finally:
        os.close(fd)


def send(port, cmd):
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(cmd, ("127.0.0.1", port))


def state(port, timeout=2.0):
    """'on', 'off' or 'unknown' from the running logger; None if no logger answers."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(b"S", ("127.0.0.1", port))
    try:
        return s.recv(16).decode()
    except socket.timeout:
        return None


def loggers(timeout=0.5):
    """The running loggers: {serial number: UDP port}. A logger that does not know its Pico answers with an empty serial number."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for port in range(PORT, PORT + PORTS):
        s.sendto(b"I", ("127.0.0.1", port))
    found = {}
    end = time.time() + timeout
    while True:
        s.settimeout(max(0.01, end - time.time()))
        try:
            data, peer = s.recvfrom(64)
        except (socket.timeout, ConnectionRefusedError):
            if time.time() >= end:
                return found
            continue
        found.setdefault(data.decode(), peer[1])


def holders(dev):
    """The PIDs of the processes that have dev open (a logger this script cannot reach must not get a second one beside it)."""
    real, pids = os.path.realpath(dev), []
    for fd_dir in glob.glob("/proc/[0-9]*/fd"):
        try:
            if any(os.path.realpath(os.path.join(fd_dir, fd)) == real for fd in os.listdir(fd_dir)):
                pids.append(int(fd_dir.split("/")[2]))
        except OSError:
            continue
    return pids


def logger_port(serial):
    """The UDP port of serial's logger, started in the background when there is none."""
    port = loggers().get(serial)
    if port:
        return port
    if serial not in picos():
        sys.exit("no Pico with serial number %s is connected (console.py lists them)" % serial)
    busy = holders(picos()[serial])
    if busy:
        sys.exit("%s is open in process %s, which does not answer as its logger (an older console.py?): stop it, or use --port" %
                 (picos()[serial], ", ".join(map(str, busy))))
    out = os.path.join(LOG_DIR, "console_%s.txt" % serial)
    subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "log", serial, "--out", out], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=open(os.path.join(LOG_DIR, "console_%s.err" % serial), "a"), start_new_session=True)  # errors only
    print("started a logger for %s (log: %s)" % (serial, out))
    end = time.time() + 10
    while time.time() < end:
        port = loggers().get(serial)
        if port and state(port) not in (None, "unknown"):  # its first status line: the power state is known
            return port
        time.sleep(0.5)
    if port:
        return port
    sys.exit("the logger for %s did not start (see %s)" % (serial, out.replace(".txt", ".err")))


def list_picos():
    """Print the connected Modxos with their loggers and power states."""
    found, running = picos(), loggers()
    if not found:
        print("no Modxo (Raspberry Pi Pico) connected")
    for sn, dev in found.items():
        port = running.get(sn)
        print("%-18s %-14s %s" % (sn, dev, "logger on %d, %s" % (port, state(port)) if port else "no logger"))
    if "" in running:
        print("a logger on port %d does not know its Pico's serial number (started with --dev, or an older console.py)" % running[""])


def wait_state(port, want, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if state(port) == want:
            return True
        time.sleep(0.5)
    return False


def switch(port, want):
    """Bring the console to 'on' or 'off'; True when it got there. A press while it boots can be missed: retried."""
    now = state(port)
    if now is None:
        sys.exit("no logger answers on 127.0.0.1:%d (run: console.py log)" % port)
    if now == "no pico":
        sys.exit("the logger has no Pico (is it connected? see README.md, Troubleshooting)")
    for attempt in range(2):
        if now == want:
            return True
        if attempt:  # the press did not get through: bytes a stopped console never read can sit in front of it in the Pico
            send(port, b"R")
            time.sleep(3)
        send(port, b"P")
        if wait_state(port, want, 12):
            return True
        now = state(port)
    return now == want


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("what", nargs="?", choices=["list", "log", "state", "on", "off", "power", "cycle"])
    p.add_argument("serial", nargs="?", help="the Pico's USB serial number (console.py lists them)")
    p.add_argument("--dev", default="/dev/ttyACM0", help="log without SERIAL: the Pico's serial port (default %(default)s)")
    p.add_argument("--port", type=int, help="the logger's UDP command port on 127.0.0.1 (default: log takes the first free one from %d, the other "
                   "commands find SERIAL's logger)" % PORT)
    p.add_argument("--out", help="log: also append the log lines to this file")
    p.add_argument("--raw", help="log: also append the raw COM1 bytes to this file")
    p.add_argument("--no-dtr", action="store_true", help="log: keep DTR low, so the console finds no COM1")
    p.add_argument("--gdb", type=int, metavar="PORT", help="log: a gdb bridge on localhost:PORT (e.g. 2159) to a GDB=1 kernel's stub")
    p.add_argument("--gdb-ready", action="store_true", help="log --gdb: the console already runs a stub (it started before this logger)")
    p.add_argument("--off", type=float, default=8.0, help="cycle: seconds off (default %(default)s)")
    args = p.parse_args()
    if args.what in (None, "list") or (args.what != "log" and not args.serial and args.port is None):
        list_picos()
        return 0 if args.what in (None, "list") else 2
    if args.what == "log":
        log(args)
        return 0
    if args.port is None:
        args.port = logger_port(args.serial)
    if args.what == "state":
        print(state(args.port) or "no logger")
    elif args.what in ("on", "off"):
        return 0 if switch(args.port, args.what) else 1
    elif args.what == "power":
        send(args.port, b"P")
    else:
        if not switch(args.port, "off"):
            return 1
        time.sleep(args.off)
        return 0 if switch(args.port, "on") else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
