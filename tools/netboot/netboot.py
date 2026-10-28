#!/usr/bin/env python3
"""Serve OXOS kernels to a console over the network (README.md in this folder; spec/oxos-internal/09-netboot.md).

Flash a netboot image once (make -C kernel netboot -> kernel/build/netboot/bios-256k.bin or bios.bin). At every
power-on its 2BL asks the network for a boot image; this tool answers with IMAGE, any OXOS open image (a 256 KiB
bios-256k.bin, or a 1 MiB bios.bin, whose first bank is sent; an image up to 1 MiB whose kernel stream runs past
the first bank is sent whole). The console boots the kernel inside it, or its own
kernel from flash when nobody answers. IMAGE is read again at every request, so rebuilding it between power-ons is
enough: each boot gets the newest build.

  netboot.py IMAGE                   serve on the local network (UDP port 5141, broadcasts)
  netboot.py IMAGE --xemu PORT       serve an xemu console started with tools/run.py --net-udp PORT

The kernels log on the debug serial port (SERIAL=1 builds; on the Modxo rig tools/modxo/console.py log shows it).

Standard library only. No root needed on the LAN: the console has no IP stack, so both sides use UDP broadcasts.
"""
import argparse, os, select, socket, struct, sys, time, zlib

PORT, MAGIC, BANK, MAX_IMAGE = 5141, 0x424E584F, 0x40000, 0x400000
MAX_KERNEL_IMAGE = 0x100000  # an image whose kernel stream runs past the first bank: at most 1 MiB
OP_HELLO, OP_OFFER, OP_READ, OP_DATA, OP_BOOT = 1, 2, 3, 4, 5
HEADER = struct.Struct("<IBBHIII")  # magic, op, version, reserved, session, offset, length
PC_MAC, PC_IP = bytes.fromhex("025842505331"), bytes([192, 168, 1, 2])  # only used for frames injected into xemu


TITLE_AT = 0x5FE0  # 'OXTI' + package offset + package size: a title's files after the image (kernel: xe/nettitle.cpp)


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


def load(path, title=None):
    """The image to serve (with the title package appended) and its CRC-32: the first 256 KiB bank of an OXOS open
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
    """This tool's messages, with times since it started."""

    def __init__(self, out):
        self.t0 = time.monotonic()
        self.out = open(out, "a") if out else None

    def say(self, line):
        stamp = f"[{time.monotonic() - self.t0:7.2f}] "
        print(stamp + line, flush=True)
        if self.out:
            self.out.write(stamp + line + "\n")
            self.out.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="OXOS open image to serve (bios-256k.bin or bios.bin), read again for every boot")
    ap.add_argument("--xemu", type=int, metavar="PORT", help="an xemu console (tools/run.py --net-udp PORT)")
    ap.add_argument("--to", default="255.255.255.255",
                    help="address the replies go to (default the broadcast address; use the LAN's broadcast address, "
                         "e.g. 192.168.1.255, on a PC with several networks)")
    ap.add_argument("--out", help="also append the printed lines to this file")
    ap.add_argument("--verbose", action="store_true", help="print every request")
    ap.add_argument("--once", action="store_true", help="exit after one console has booted")
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
        log.say(f"netboot: serving {a.image} on UDP port {PORT} (replies to {a.to})")

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
                if dport != PORT:
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
                    image, crc = load(a.image, title_package(a.title, a.title_name) if a.title else None)
                except (OSError, ValueError) as e:
                    log.say(f"netboot: console {who} asked, but {e}")
                    continue
                if sess != session:
                    console_mac = payload[HEADER.size:HEADER.size + 6].hex(":")
                    log.say(f"netboot: console {who} ({console_mac}) asks for a kernel; offering {a.image} "
                            f"(CRC-32 {crc:08x}, built {time.ctime(os.path.getmtime(a.image))}"
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
