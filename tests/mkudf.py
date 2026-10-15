#!/usr/bin/env python3
"""Build a small UDF 1.02 test image (DVD-video style, 2048-byte blocks), written from ECMA-167 / OSTA UDF 1.02.

Layout: VRS (BEA01, NSR02, TEA01) at 16-18; main VDS (PVD, PD, LVD, terminator) at 32; AVDP at 256; the partition
starts at 257 with the FSD and a terminator, then a File Entry per file/directory followed by its data.
Exercises short_ad, long_ad with two extents, embedded data, 8- and 16-bit OSTA CS0 names.
"""
import struct
import sys

BLOCK = 2048
PART_START = 257


def pattern(n, seed):
    return bytes(((i * 13 + seed) ^ (i >> 7)) & 0xFF for i in range(n))


# name -> bytes (file) | dict (directory). Names marked with "w:" are stored as 16-bit CS0.
TREE = {
    "readme.txt": b"udf root file",
    "w:Wide": b"sixteen-bit name",
    "VIDEO_TS": {
        "VIDEO_TS.IFO": pattern(5000, 1),
        "VTS_01_1.VOB": pattern(300 * 1024 + 77, 9),  # two extents, long_ad
        "small.txt": b"hello udf",                     # embedded in its File Entry
    },
}
FRAGMENTED, EMBEDDED = "VTS_01_1.VOB", "small.txt"


def crc_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def tag(ident, location, body):
    """Descriptor tag (16 bytes) + body, with checksum and CRC filled in."""
    t = bytearray(struct.pack("<HHBBHHHI", ident, 2, 0, 0, 1, crc_ccitt(body), len(body), location))
    t[4] = sum(t[i] for i in range(16) if i != 4) & 0xFF
    return bytes(t) + body


def charspec():
    return b"\0OSTA Compressed Unicode".ljust(64, b"\0")


def entity(ident, suffix=b""):
    return (b"\0" + ident).ljust(24, b"\0") + suffix.ljust(8, b"\0")


def dstring(text, size):
    raw = b"\x08" + text.encode()
    return raw.ljust(size - 1, b"\0") + bytes([len(raw)])


def timestamp():
    return struct.pack("<HhBBBBBBBB", 0x1000, 2004, 5, 6, 7, 8, 9, 0, 0, 0)


def long_ad(length, block):
    return struct.pack("<IIH", length, block, 0) + bytes(6)


def cs0(name):
    if name.startswith("w:"):
        return b"\x10" + name[2:].encode("utf-16-be")
    return b"\x08" + name.encode()


def plain(name):
    return name[2:] if name.startswith("w:") else name


def build(path=None):
    blocks = {}            # partition block -> bytes
    nxt = [2]              # 0 = FSD, 1 = terminator

    def alloc(n):
        b = nxt[0]
        nxt[0] += n
        return b

    def file_entry(block, ftype, size, ad_type, ads, recorded):
        body = bytearray(176 - 16)
        icb = struct.pack("<IHHHBB", 0, 4, 0, 1, 0, ftype) + bytes(6) + struct.pack("<H", ad_type)
        body[0:20] = icb
        struct.pack_into("<IIIHBBI", body, 20, 0xFFFFFFFF, 0xFFFFFFFF, 0x14A5, 1, 0, 0, 0)
        struct.pack_into("<QQ", body, 40, size, recorded)
        for off in (56, 68, 80):
            body[off:off + 12] = timestamp()
        struct.pack_into("<I", body, 92, 1)
        body[112:144] = entity(b"*xk mkudf")
        struct.pack_into("<QII", body, 144, block + 16, 0, len(ads))
        blocks[block] = tag(261, block, bytes(body) + ads)

    def add(node, name, parent_icb):
        icb = alloc(1)
        if isinstance(node, dict):
            fids = bytearray()

            def fid(chars, child_icb, ident):
                body = struct.pack("<HBB", 1, chars, len(ident)) + long_ad(BLOCK, child_icb) + struct.pack("<H", 0)
                body += ident
                body += bytes((-(16 + len(body))) % 4)
                return tag(257, 0, body)  # location is not meaningful inside directory data here

            children = []
            for child, sub in node.items():
                children.append((child, sub))
            fids += fid(0x0A, parent_icb, b"")
            child_icbs = []
            for child, sub in children:
                child_icbs.append(add(sub, child, icb))
            for (child, sub), ci in zip(children, child_icbs):
                fids += fid(0x02 if isinstance(sub, dict) else 0, ci, cs0(child))
            n = (len(fids) + BLOCK - 1) // BLOCK
            data = alloc(n)
            for i in range(n):
                blocks[data + i] = bytes(fids[i * BLOCK:(i + 1) * BLOCK])
            file_entry(icb, 4, len(fids), 0, struct.pack("<II", len(fids), data), n)
            return icb
        data = node
        if plain(name) == EMBEDDED:
            file_entry(icb, 5, len(data), 3, data, 0)
        elif plain(name) == FRAGMENTED:
            first = (len(data) // 2 // BLOCK) * BLOCK
            n1, n2 = first // BLOCK, (len(data) - first + BLOCK - 1) // BLOCK
            a = alloc(n1)
            alloc(1)  # a hole between the two extents
            b = alloc(n2)
            for i in range(n1):
                blocks[a + i] = data[i * BLOCK:(i + 1) * BLOCK]
            for i in range(n2):
                blocks[b + i] = data[first + i * BLOCK:first + (i + 1) * BLOCK]
            file_entry(icb, 5, len(data), 1, long_ad(first, a) + long_ad(len(data) - first, b), n1 + n2)
        else:
            n = (len(data) + BLOCK - 1) // BLOCK
            a = alloc(n) if n else 0
            for i in range(n):
                blocks[a + i] = data[i * BLOCK:(i + 1) * BLOCK]
            file_entry(icb, 5, len(data), 0, struct.pack("<II", len(data), a) if n else b"", n)
        return icb

    root = add(TREE, "", 0)
    # The root's parent link points at itself.
    fsd = bytearray(512 - 16)
    fsd[0:12] = timestamp()
    struct.pack_into("<HHIIII", fsd, 12, 3, 3, 1, 1, 0, 0)
    fsd[32:96] = charspec()
    fsd[96:224] = dstring("XKUDF", 128)
    fsd[224:288] = charspec()
    fsd[288:320] = dstring("XKUDF", 32)
    fsd[384:400] = long_ad(BLOCK, root)
    fsd[400:432] = entity(b"*OSTA UDF Compliant", struct.pack("<H", 0x102))
    blocks[0] = tag(256, 0, bytes(fsd))
    blocks[1] = tag(8, 1, bytes(496))
    part_len = nxt[0]

    img = bytearray((PART_START + part_len) * BLOCK)

    def put(sector, data):
        img[sector * BLOCK:sector * BLOCK + len(data)] = data

    for i, ident in enumerate((b"BEA01", b"NSR02", b"TEA01")):
        put(16 + i, b"\0" + ident + b"\x01")
    pvd = bytearray(512 - 16)
    struct.pack_into("<II", pvd, 0, 1, 0)
    pvd[8:40] = dstring("XKUDF", 32)
    struct.pack_into("<HHHHII", pvd, 40, 1, 1, 2, 2, 1, 1)
    pvd[56:184] = dstring("XKUDF set", 128)
    pvd[184:248] = charspec()
    pvd[248:312] = charspec()
    put(32, tag(1, 32, bytes(pvd)))
    pd = bytearray(512 - 16)
    struct.pack_into("<IHH", pd, 0, 2, 1, 0)
    pd[8:40] = entity(b"+NSR02")
    struct.pack_into("<III", pd, 168, 1, PART_START, part_len)
    put(33, tag(5, 33, bytes(pd)))
    lvd = bytearray(446 - 16)
    struct.pack_into("<I", lvd, 0, 3)
    lvd[4:68] = charspec()
    lvd[68:196] = dstring("XKUDF", 128)
    struct.pack_into("<I", lvd, 196, BLOCK)
    lvd[200:232] = entity(b"*OSTA UDF Compliant", struct.pack("<H", 0x102))
    lvd[232:248] = long_ad(2 * BLOCK, 0)  # the FSD sequence: FSD + terminator
    struct.pack_into("<II", lvd, 248, 6, 1)
    lvd[424:430] = struct.pack("<BBHH", 1, 6, 1, 0)
    put(34, tag(6, 34, bytes(lvd)))
    put(35, tag(8, 35, bytes(496)))
    avdp = struct.pack("<IIII", 4 * BLOCK, 32, 4 * BLOCK, 32).ljust(496, b"\0")
    put(256, tag(2, 256, avdp))
    for b, data in blocks.items():
        put(PART_START + b, data)
    if path:
        open(path, "wb").write(img)
    return bytes(img)


if __name__ == "__main__":
    build(sys.argv[1])
