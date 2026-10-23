#!/usr/bin/env python3
"""Serve OXOS kernels to a console over the network (README.md in this folder; spec/oxos-internal/09-netboot.md).

Flash a netboot image once (make -C kernel netboot -> kernel/build/netboot/bios-256k.bin or bios.bin). At every
power-on its 2BL asks the network for a boot image; this tool answers with IMAGE, any OXOS open image (a 256 KiB
bios-256k.bin, or a 1 MiB bios.bin, whose first bank is sent). The console boots the kernel inside it, or its own
kernel from flash when nobody answers. IMAGE is read again at every request, so rebuilding it between power-ons is
enough: each boot gets the newest build.

  netboot.py IMAGE                   serve on the local network (UDP port 5141, broadcasts)
  netboot.py IMAGE --log             ... and print the kernel's network log (UDP port 5140; NETLOG=1 builds)
  netboot.py IMAGE --xemu PORT       serve an xemu console started with tools/run.py --net-udp PORT (log included)

Standard library only. No root needed on the LAN: the console has no IP stack, so both sides use UDP broadcasts.
"""
import argparse, os, re, select, socket, struct, sys, time, zlib

PORT, LOG_PORT, MAGIC, BANK = 5141, 5140, 0x424E584F, 0x40000
OP_HELLO, OP_OFFER, OP_READ, OP_DATA, OP_BOOT = 1, 2, 3, 4, 5
HEADER = struct.Struct("<IBBHIII")  # magic, op, version, reserved, session, offset, length
PC_MAC, PC_IP = bytes.fromhex("025842505331"), bytes([192, 168, 1, 2])  # only used for frames injected into xemu


def load(path):
    """The first 256 KiB bank of an OXOS open image and its CRC-32."""
    data = open(path, "rb").read()[:BANK]
    if len(data) != BANK or data[0x6000:0x6004] != b"OXKP":
        raise ValueError(f"{path}: not an OXOS open image (no 'OXKP' payload header at 0x6000)")
    return data, zlib.crc32(data)


def udp_of(frame):
    """(destination port, payload, source MAC) of an Ethernet/IPv4/UDP frame, else None."""
    if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
        return None
    u = 14 + (frame[14] & 0x0F) * 4
    dport, length = struct.unpack_from(">HH", frame, u + 2)
    return dport, frame[u + 8:u + length], frame[6:12]


def frame_for(payload):
    """An Ethernet/IPv4/UDP broadcast frame (5141 -> 5141) carrying payload, for xemu: what a PC's broadcast is."""
    udp = struct.pack(">HHHH", PORT, PORT, 8 + len(payload), 0) + payload
    ip = bytearray(struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, PC_IP, b"\xff" * 4))
    s = sum(struct.unpack(">10H", ip))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    struct.pack_into(">H", ip, 10, ~s & 0xFFFF)
    f = b"\xff" * 6 + PC_MAC + b"\x08\x00" + bytes(ip) + udp
    return f + bytes(max(0, 60 - len(f)))


class Log:
    """The kernel's network log: lines with arrival times; trace lines' "ordinal N" named from kernel/exports.tsv."""

    def __init__(self, out):
        self.t0, self.pending, self.names = time.monotonic(), "", {}
        self.out = open(out, "a") if out else None
        exports = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kernel", "exports.tsv")
        if os.path.exists(exports):
            for row in open(exports):
                f = row.rstrip("\n").split("\t")
                if len(f) >= 2 and f[0].strip().isdigit():
                    self.names[int(f[0])] = f[1].strip()

    def say(self, line):
        line = re.sub(r"ordinal (\d+)", lambda m: self.names.get(int(m.group(1)), m.group(0)), line)
        stamp = f"[{time.monotonic() - self.t0:7.2f}] "
        print(stamp + line, flush=True)
        if self.out:
            self.out.write(stamp + line + "\n")
            self.out.flush()

    def feed(self, data):
        self.pending += data.decode("latin-1")
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            self.say(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="OXOS open image to serve (bios-256k.bin or bios.bin), read again for every boot")
    ap.add_argument("--xemu", type=int, metavar="PORT", help="an xemu console (tools/run.py --net-udp PORT)")
    ap.add_argument("--log", action="store_true", help="also print the network log (UDP port 5140)")
    ap.add_argument("--to", default="255.255.255.255",
                    help="address the replies go to (default the broadcast address; use the LAN's broadcast address, "
                         "e.g. 192.168.1.255, on a PC with several networks)")
    ap.add_argument("--out", help="also append the printed lines to this file")
    ap.add_argument("--verbose", action="store_true", help="print every request")
    ap.add_argument("--once", action="store_true", help="exit after one console has booted")
    a = ap.parse_args()
    log = Log(a.out)
    socks = []
    if a.xemu:
        xemu = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        xemu.bind(("127.0.0.1", a.xemu))
        socks.append(xemu)
        log.say(f"netboot: serving {a.image} to xemu (frames on 127.0.0.1:{a.xemu}, replies to :{a.xemu + 1})")
    else:
        lan = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        lan.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        lan.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        lan.bind(("", PORT))
        socks.append(lan)
        if a.log:
            logsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            logsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            logsock.bind(("", LOG_PORT))
            socks.append(logsock)
        log.say(f"netboot: serving {a.image} on UDP port {PORT} (replies to {a.to})"
                + ("; network log on port 5140" if a.log else ""))

    image = crc = None
    session, served, started = None, set(), 0.0

    def reply(payload, mac):
        if a.xemu:
            xemu.sendto(frame_for(payload), ("127.0.0.1", a.xemu + 1))
        else:
            lan.sendto(payload, (a.to, PORT))

    while True:
        ready, _, _ = select.select(socks, [], [])
        for s in ready:
            data, peer = s.recvfrom(65536)
            if a.xemu:
                parsed = udp_of(data)
                if not parsed:
                    continue
                dport, payload, mac = parsed
                if dport == LOG_PORT:
                    log.feed(payload)
                    continue
                if dport != PORT:
                    continue
            elif s is not socks[0]:
                log.feed(data)
                continue
            else:
                payload, mac = data, None
            if len(payload) < HEADER.size:
                continue
            magic, op, version, _, sess, offset, length = HEADER.unpack_from(payload)
            if magic != MAGIC or op not in (OP_HELLO, OP_READ, OP_BOOT):
                continue  # not ours, or our own broadcast coming back
            who = peer[0] if not a.xemu else "xemu"
            if a.verbose:
                log.say(f"netboot: <- op {op} session {sess:08x} offset {offset:#x} length {length:#x} from {who}")
            if op == OP_HELLO:
                try:
                    image, crc = load(a.image)
                except (OSError, ValueError) as e:
                    log.say(f"netboot: console {who} asked, but {e}")
                    continue
                if sess != session:
                    console_mac = payload[HEADER.size:HEADER.size + 6].hex(":")
                    log.say(f"netboot: console {who} ({console_mac}) asks for a kernel; offering {a.image} "
                            f"(CRC-32 {crc:08x}, built {time.ctime(os.path.getmtime(a.image))})")
                    session, served, started = sess, set(), time.monotonic()
                reply(HEADER.pack(MAGIC, OP_OFFER, 1, 0, sess, crc, BANK), mac)
            elif op == OP_READ and sess == session and image:
                if offset + length > BANK or length > 1400:
                    continue
                reply(HEADER.pack(MAGIC, OP_DATA, 1, 0, sess, offset, length) + image[offset:offset + length], mac)
                served.add(offset)
            elif op == OP_BOOT and sess == session:
                took = time.monotonic() - started
                if offset == 0:
                    log.say(f"netboot: console {who} downloaded {len(served)} KiB in {took:.1f} s "
                            "and is booting it")
                else:
                    log.say(f"netboot: console {who} rejected the download (CRC or header mismatch); it boots "
                            "from flash")
                session = None
                if a.once:
                    return


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
