#!/usr/bin/env python3
"""Serve OXOS kernels to a console over the network (README.md in this folder; spec/oxos-internal/09-netboot.md).

Flash a netboot image once (make -C kernel netboot -> kernel/build/netboot/bios-256k.bin or bios.bin). At every
power-on its 2BL asks the network for a boot image; this tool answers with IMAGE, any OXOS open image (a 256 KiB
bios-256k.bin, or a 1 MiB bios.bin, whose first bank is sent; an image up to 1 MiB whose kernel stream runs past
the first bank is sent whole). The console boots the kernel inside it, or its own
kernel from flash when nobody answers. IMAGE is read again at every request, so rebuilding it between power-ons is
enough: each boot gets the newest build.

  netboot.py IMAGE                   serve on the local network (UDP port 5141, broadcasts)
  netboot.py IMAGE --log             ... and print the kernel's network log (UDP port 5140; NETLOG=1 builds)
  netboot.py IMAGE --xemu PORT       serve an xemu console started with tools/run.py --net-udp PORT (log included)

Standard library only. No root needed on the LAN: the console has no IP stack, so both sides use UDP broadcasts.
"""
import argparse, os, re, select, socket, struct, sys, time, zlib

PORT, LOG_PORT, MAGIC, BANK, MAX_IMAGE = 5141, 5140, 0x424E584F, 0x40000, 0x400000
MAX_KERNEL_IMAGE = 0x100000  # an image whose kernel stream runs past the first bank: at most 1 MiB
OP_HELLO, OP_OFFER, OP_READ, OP_DATA, OP_BOOT = 1, 2, 3, 4, 5
HEADER = struct.Struct("<IBBHIII")  # magic, op, version, reserved, session, offset, length
PC_MAC, PC_IP = bytes.fromhex("025842505331"), bytes([192, 168, 1, 2])  # only used for frames injected into xemu


DEBUG_CONFIG_AT = 0x6020  # free space after the payload header (kernel: src/ke/dbgstep.cpp)
EXPERIMENT_FLAGS_AT = 0x5FF0  # 'OXEF' + u32: run-time experiment switches (kernel: KiExperimentFlags, ke/dbgstep.cpp)
TITLE_AT = 0x5FE0  # 'OXTI' + package offset + package size: a title's files after the image (kernel: xe/nettitle.cpp)


def debug_config(a):
    """The debug-register block for network-log kernels (--dr0/--dr1/--dr2 execution breakpoints, --watch a 4-byte
    write watch in DR3, --steps single steps from the first DR0 hit), or None."""
    drs, dr7 = [0, 0, 0, 0], 0
    for i, v in enumerate((a.dr0, a.dr1, a.dr2)):
        if v is not None:
            drs[i] = v
            dr7 |= 1 << (2 * i)  # Li, execution (RW 00, LEN 00)
    if a.watch is not None:
        drs[3] = a.watch & ~3
        dr7 |= (1 << 6) | (0b01 << 28) | (0b11 << 30)  # L3, data writes, 4 bytes
    if not dr7:
        return None
    dr7 |= 0x300  # LE / GE: exact breakpoints
    return struct.pack("<4s4IIII", b"DBGC", *drs, dr7, a.steps, 1 if a.step_dr1 else 0)


def experiment_flags(a):
    """--flags-file: a file holding one number (e.g. 0x1), read again at every request: the experiment switches the
    kernel tests at run time, so one binary can be tried with and without each change."""
    if not a.flags_file or not os.path.exists(a.flags_file):
        return None
    text = open(a.flags_file).read().split("#")[0].strip()
    return struct.pack("<4sI", b"OXEF", int(text, 0)) if text else None


def title_package(folder, name):
    """A title's files as the kernel installs them (spec/oxos-internal/09-netboot.md §7): the folder's files, with
    default.xbe stored as NAME.xbe. Header {'OXTP', version 1, count, name[16]}, then per file {name[64], offset,
    size} (from the package's start), then the files, each 16-byte aligned; padded to a multiple of 1 KiB."""
    if not name or len(name) > 15 or any(c in name for c in "\\/:"):
        raise ValueError(f"title name {name!r}: 1-15 characters, no path separators")
    files = []
    for f in sorted(os.listdir(folder)):
        full = os.path.join(folder, f)
        if os.path.isfile(full):
            stored = f"{name}.xbe" if f.lower() == "default.xbe" else f
            if len(stored) > 63:
                raise ValueError(f"{f}: file name longer than 63 characters")
            files.append((stored, open(full, "rb").read()))
    if not any(n == f"{name}.xbe" for n, _ in files):
        raise ValueError(f"{folder}: no default.xbe")
    at = 28 + 72 * len(files)
    entries, blobs = b"", b""
    for stored, data in files:
        at = (at + 15) & ~15
        entries += struct.pack("<64sII", stored.encode(), at, len(data))
        blobs = blobs.ljust(at - (28 + 72 * len(files)), b"\0") + data
        at += len(data)
    package = struct.pack("<4sII16s", b"OXTP", 1, len(files), name.encode()) + entries + blobs
    return package + bytes(-len(package) % 1024)


def load(path, config=None, flags=None, title=None):
    """The image to serve (with the debug block patched in) and its CRC-32: the first 256 KiB bank of an OXOS open
    image, or the whole file (up to 1 MiB, a multiple of 1 KiB) when its kernel stream runs past the first bank."""
    data = bytearray(open(path, "rb").read())
    if len(data) < BANK or data[0x6000:0x6004] != b"OXKP":
        raise ValueError(f"{path}: not an OXOS open image (no 'OXKP' payload header at 0x6000)")
    kstart, packed = struct.unpack_from("<II", data, 0x6004)
    if kstart + packed <= BANK:
        data = data[:BANK]
    elif len(data) > MAX_KERNEL_IMAGE or len(data) % 1024:
        raise ValueError(f"{path}: a large image must be at most 1 MiB and a multiple of 1 KiB ({len(data):#x})")
    if title:
        data += bytes(-len(data) % 4096)  # the package starts on a page (the kernel keeps those pages)
        struct.pack_into("<4sII", data, TITLE_AT, b"OXTI", len(data), len(title))
        data += title
        if len(data) > MAX_IMAGE:
            raise ValueError(f"image and title: {len(data):#x} bytes, more than the 2BL's {MAX_IMAGE:#x}")
    if config:
        data[DEBUG_CONFIG_AT:DEBUG_CONFIG_AT + len(config)] = config
    if flags:
        data[EXPERIMENT_FLAGS_AT:EXPERIMENT_FLAGS_AT + len(flags)] = flags
    data = bytes(data)
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
    num = lambda v: int(v, 0)
    ap.add_argument("--dr0", type=num, help="network-log kernels: execution breakpoint (DR0); --steps start here")
    ap.add_argument("--dr1", type=num, help="execution breakpoint (DR1), reported when hit")
    ap.add_argument("--dr2", type=num, help="execution breakpoint (DR2), reported when hit")
    ap.add_argument("--watch", type=num, help="report writes to this 4-byte location (DR3)")
    ap.add_argument("--steps", type=num, default=0, help="single-step this many instructions from the first DR0 hit")
    ap.add_argument("--step-dr1", action="store_true",
                    help="the first DR1 hit starts a second run of --steps (after the DR0 run has ended)")
    ap.add_argument("--once", action="store_true", help="exit after one console has booted")
    ap.add_argument("--flags-file", help="file with the kernel's experiment switches (a number), read at every request")
    ap.add_argument("--title", metavar="DIR", help="also send this folder's files (a title: default.xbe and its data); "
                    "the kernel installs them as E:\\NAME\\ and runs NAME.xbe instead of the disc or dashboard")
    ap.add_argument("--title-name", default="imgtest", help="the title's folder and XBE name on E: (default %(default)s)")
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
                    image, crc = load(a.image, debug_config(a), experiment_flags(a),
                                      title_package(a.title, a.title_name) if a.title else None)
                except (OSError, ValueError) as e:
                    log.say(f"netboot: console {who} asked, but {e}")
                    continue
                if sess != session:
                    console_mac = payload[HEADER.size:HEADER.size + 6].hex(":")
                    log.say(f"netboot: console {who} ({console_mac}) asks for a kernel; offering {a.image} "
                            f"(CRC-32 {crc:08x}, built {time.ctime(os.path.getmtime(a.image))}"
                            + (f", flags {struct.unpack_from('<I', image, EXPERIMENT_FLAGS_AT + 4)[0]:#x}" if image[EXPERIMENT_FLAGS_AT:EXPERIMENT_FLAGS_AT + 4] == b"OXEF" else "")
                            + (f", title {a.title} as E:\\{a.title_name}\\{a.title_name}.xbe" if a.title else "") + ")")
                    session, served, started = sess, set(), time.monotonic()
                reply(HEADER.pack(MAGIC, OP_OFFER, 1, 0, sess, crc, len(image)), mac)
            elif op == OP_READ and sess == session and image:
                if offset + length > len(image) or length > 1400:
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
