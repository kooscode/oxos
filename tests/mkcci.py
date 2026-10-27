"""Write a CCI image (the compressed disc image format of spec/cerbios/07 §4) from an ISO, for the virtual disc tests.

Each 2048-byte block is LZ4-compressed (a plain greedy compressor, block format) when that saves space, else kept as
it is; the index after the blocks holds each block's offset / 4 (bit 31 set for a compressed block, as writers do)
plus one entry for the end.

usage: mkcci.py IN.iso OUT.cci
"""
import struct
import sys

BLOCK = 0x800


def lz4_block(data):
    """LZ4 block format: sequences of (token, literals, offset, match length); the last 5 bytes are literals."""
    out = bytearray()
    n = len(data)
    table = {}
    anchor = i = 0
    limit = n - 12  # no match starts in the last 12 bytes (the format's end rules)

    def lengths(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    while i < limit:
        key = data[i:i + 4]
        j = table.get(key)
        table[key] = i
        if j is None or i - j > 0xFFFF:
            i += 1
            continue
        m = 4
        while i + m < n - 5 and data[j + m] == data[i + m]:
            m += 1
        lit = i - anchor
        out.append((min(lit, 15) << 4) | min(m - 4, 15))
        if lit >= 15:
            lengths(lit - 15)
        out += data[anchor:i]
        out += struct.pack("<H", i - j)
        if m - 4 >= 15:
            lengths(m - 4 - 15)
        i += m
        anchor = i
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        lengths(lit - 15)
    out += data[anchor:]
    return bytes(out)


def write(iso, path):
    blocks = (len(iso) + BLOCK - 1) // BLOCK
    body = bytearray()
    index = []
    at = 32
    for b in range(blocks):
        raw = bytes(iso[b * BLOCK:(b + 1) * BLOCK]).ljust(BLOCK, b"\0")
        packed = lz4_block(raw)
        pad = (4 - (1 + len(packed)) % 4) % 4
        if 1 + len(packed) + pad < BLOCK:
            stored = bytes([pad]) + packed + bytes(pad)
            index.append((at >> 2) | 0x80000000)
        else:
            stored = raw
            index.append(at >> 2)
        body += stored
        at += len(stored)
    index.append(at >> 2)
    header = b"CCIM" + struct.pack("<IQQIBBH", 0x20, blocks * BLOCK, at, BLOCK, 1, 2, 0)
    with open(path, "wb") as f:
        f.write(header + body + struct.pack(f"<{len(index)}I", *index))


if __name__ == "__main__":
    write(open(sys.argv[1], "rb").read(), sys.argv[2])
