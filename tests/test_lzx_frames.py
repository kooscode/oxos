#!/usr/bin/env python3
"""Check that mkbios's uncompressed-LZX stream decodes correctly across several 32 KiB frames,
including an odd-length final frame, using the unpacker's libarchive-based CAB decoder."""
import importlib.util, os, random, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mkbios = load("mkbios", os.path.join(ROOT, "tools", "mkbios.py"))
unpack = load("unpack", os.path.join(ROOT, "..", "tmp4", "xbox_bios_unpack.py"))

for size in (1, 0x8000, 0x8001, 3 * 0x8000 + 0x1235, 0x9B501):
    data = random.Random(size).randbytes(size)
    blocks = unpack.lzx_blocks(mkbios.lzx_uncompressed_stream(data))
    cab, total = unpack.make_cab(blocks)
    out = unpack.cab_extract(cab, total)
    status = "ok" if out == data else "MISMATCH"
    print(f"{size:#8x} bytes, {len(blocks)} frames: {status}")
    if out != data:
        sys.exit(1)
