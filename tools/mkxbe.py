#!/usr/bin/env python3
"""Convert a PE image linked at 0x10000 into an XBE (headers, certificate, section digests, encoded entry point
and kernel thunk address). For our test titles; signature fields are left zero.

usage: mkxbe.py --pe title.exe --map title.map --thunks _XkThunks --out title.xbe [--title-id 0x584B0001]
                [--name NAME] [--no-preload .extra]
"""
import argparse, hashlib, re, struct

BASE = 0x10000
XOR_ENTRY_RETAIL, XOR_THUNK_RETAIL = 0xA8FC57AB, 0x5B6D40B6
WRITABLE, PRELOAD, EXECUTABLE = 1, 2, 4


def pe_sections(pe):
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, lfanew + 6)[0]
    optsize = struct.unpack_from("<H", pe, lfanew + 20)[0]
    entry_rva = struct.unpack_from("<I", pe, lfanew + 24 + 16)[0]
    image_base = struct.unpack_from("<I", pe, lfanew + 24 + 28)[0]
    out = []
    for i in range(nsec):
        o = lfanew + 24 + optsize + 40 * i
        name = pe[o:o + 8].rstrip(b"\0").decode()
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", pe, o + 8)
        chars = struct.unpack_from("<I", pe, o + 36)[0]
        out.append(dict(name=name, va=image_base + va, vsize=max(vsize, rawsize if rawsize < vsize else vsize),
                        data=pe[rawptr:rawptr + min(rawsize, vsize)], chars=chars))
    return image_base, image_base + entry_rva, out


def map_symbol(path, name):
    for line in open(path):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})", line)
        if m and m.group(1) == name:
            return int(m.group(2), 16)
    raise SystemExit(f"{name} not in {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--thunks", default="_XkThunks")
    ap.add_argument("--out", required=True)
    ap.add_argument("--title-id", type=lambda v: int(v, 0), default=0x584B0001)
    ap.add_argument("--name", default="XK test title")
    ap.add_argument("--no-preload", action="append", default=[])
    a = ap.parse_args()

    pe = open(a.pe, "rb").read()
    image_base, entry, sections = pe_sections(pe)
    if image_base != BASE:
        raise SystemExit("link the title at /BASE:0x10000")
    thunks = map_symbol(a.map, a.thunks)

    # Header layout: image header, certificate, section headers, page reference counters, names.
    n = len(sections)
    cert_off = 0x178
    sec_off = cert_off + 0x1D0
    ref_off = sec_off + 0x38 * n
    names_off = ref_off + 4 * n
    names = b""
    name_addr = []
    for s in sections:
        name_addr.append(BASE + names_off + len(names))
        names += s["name"].encode() + b"\0"
    size_of_headers = (names_off + len(names) + 3) & ~3
    if BASE + size_of_headers > min(s["va"] for s in sections):
        raise SystemExit("headers overlap the first section")
    image_end = max(s["va"] + s["vsize"] for s in sections)
    size_of_image = (image_end - BASE + 0xFFF) & ~0xFFF

    raw_off = (size_of_headers + 0xFFF) & ~0xFFF
    body = bytearray()
    sec_headers = b""
    for i, s in enumerate(sections):
        flags = PRELOAD if s["name"] not in a.no_preload else 0
        if s["chars"] & 0x80000000:
            flags |= WRITABLE
        if s["chars"] & 0x20000000:
            flags |= EXECUTABLE
        ptr = raw_off + len(body) if s["data"] else 0
        body += s["data"]
        body += bytes((-len(body)) % 16)
        digest = hashlib.sha1(struct.pack("<I", len(s["data"])) + s["data"]).digest()
        head_ref, tail_ref = BASE + ref_off + 4 * i, BASE + ref_off + 4 * i + 2
        sec_headers += struct.pack("<IIIIIIIII", flags, s["va"], s["vsize"], ptr, len(s["data"]), name_addr[i], 0,
                                   head_ref, tail_ref) + digest

    title_name = a.name.encode("utf-16-le")[:80].ljust(80, b"\0")
    cert = struct.pack("<III", 0x1D0, 0, a.title_id) + title_name + bytes(64)
    cert += struct.pack("<IIIII", 0x00FFFFFF, 0x7FFFFFFF, 0, 0, 0)  # media types, region, ratings, disk, version
    cert += bytes(0x1D0 - len(cert))

    hdr = bytearray(0x178)
    hdr[0:4] = b"XBEH"
    struct.pack_into("<IIIIIIIIIIIIII", hdr, 0x104, BASE, size_of_headers, size_of_image, 0x178, 0,
                     BASE + cert_off, n, BASE + sec_off, 0, entry ^ XOR_ENTRY_RETAIL, 0, 0x10000, 0x100000, 0x1000)
    struct.pack_into("<I", hdr, 0x158, thunks ^ XOR_THUNK_RETAIL)
    headers = bytes(hdr) + cert + sec_headers + bytes(4 * n) + names
    headers += bytes(raw_off - len(headers))
    open(a.out, "wb").write(headers + body)
    print(f"{a.out}: {len(headers) + len(body):#x} bytes, {n} sections, entry {entry:#x}, thunks {thunks:#x}")


if __name__ == "__main__":
    main()
