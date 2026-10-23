#!/usr/bin/env python3
"""Print the boot log of a NETLOG=1 OXOS build (spec/oxos-internal/03-debug-serial.md §7).

The kernel sends its serial log as UDP broadcasts to port 5140. On the PC:
  netlog.py                 listen for the broadcasts on the local network (a console on the same LAN)
  netlog.py --xemu PORT     read the raw Ethernet frames of an xemu run started with run.py --net-udp PORT
Each line is printed as it arrives, prefixed with the arrival time (seconds since start); --out FILE also saves it.
"""
import argparse, os, re, socket, struct, sys, time


def payload_from_frame(frame):
    """The UDP payload of an Ethernet/IPv4/UDP frame to port 5140, else None."""
    if len(frame) < 42 or frame[12:14] != b"\x08\x00":
        return None
    ihl = (frame[14] & 0x0F) * 4
    if frame[23] != 17:
        return None
    udp = 14 + ihl
    dport, length = struct.unpack_from(">HH", frame, udp + 2)
    if dport != 5140:
        return None
    return frame[udp + 8:udp + length]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xemu", type=int, metavar="PORT", help="raw frames from xemu's udp backend on 127.0.0.1:PORT")
    ap.add_argument("--port", type=int, default=5140, help="UDP port of the broadcasts (default 5140)")
    ap.add_argument("--out", help="also append the log to this file")
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if a.xemu:
        s.bind(("127.0.0.1", a.xemu))
        print(f"netlog: xemu frames on 127.0.0.1:{a.xemu}", file=sys.stderr)
    else:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(("", a.port))
        print(f"netlog: listening for OXOS broadcasts on UDP port {a.port}", file=sys.stderr)
    out = open(a.out, "a") if a.out else None
    names = {}  # kernel export names for the trace lines ("ordinal N")
    exports = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "exports.tsv")
    if os.path.exists(exports):
        for row in open(exports):
            f = row.rstrip("\n").split("\t")
            if len(f) >= 2 and f[0].strip().isdigit():
                names[int(f[0])] = f[1].strip()
    t0, pending = time.monotonic(), ""
    while True:
        data, peer = s.recvfrom(65536)
        text = payload_from_frame(data) if a.xemu else data
        if text is None:
            continue
        pending += text.decode("latin-1")
        while "\n" in pending:
            line, pending = pending.split("\n", 1)
            line = re.sub(r"ordinal (\d+)", lambda m: f"{names.get(int(m.group(1)), 'ordinal ' + m.group(1))}", line)
            stamp = f"[{time.monotonic() - t0:7.2f}] "
            print(stamp + line, flush=True)
            if out:
                out.write(stamp + line + "\n")
                out.flush()


if __name__ == "__main__":
    main()
