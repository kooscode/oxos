#!/usr/bin/env python3
"""Check the BIOS packer's LZX compression (tools/lzx.py, as tools/mkbios.py streams it) across several 32 KiB
frames, including a single byte, exact frame multiples and an odd-length final frame.

usage: python3 tests/test_lzx_frames.py

Each test input is compressed into mkbios's stream ({u16 compressed length, u16 uncompressed length, LZX bytes} per
frame, then four zero bytes), split back into frames the way the 2BL reads it, and decoded twice:
1. with tools/lzx.py's own decoder;
2. independently, when the system's libarchive is installed (Ubuntu: libarchive13t64, usually present): the frames
   are wrapped in a Microsoft CAB file (one LZX folder, window 2^17, one data block per frame, as the original
   kernel's packer uses) and libarchive's CAB reader decodes them. Without libarchive this step is skipped.
Exits 1 on any mismatch.
"""
import ctypes, ctypes.util, importlib.util, os, random, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


sys.path.insert(0, os.path.join(ROOT, "tools"))  # mkbios imports lzx
mkbios = load("mkbios", os.path.join(ROOT, "tools", "mkbios.py"))
lzx = load("lzx", os.path.join(ROOT, "tools", "lzx.py"))


def split_stream(stream):
    """[(compressed frame, uncompressed size)] from mkbios's stream, as the 2BL walks it."""
    frames, o = [], 0
    while True:
        clen, ulen = struct.unpack_from("<HH", stream, o)
        o += 4
        if clen == 0:
            return frames
        frames.append((stream[o:o + clen], ulen))
        o += clen


def make_cab(frames, name=b"data.bin"):
    """A one-file Microsoft CAB (MS-CAB format): header, one LZX folder, one file, one data block per frame."""
    total = sum(u for _, u in frames)
    header_size, folder_size = 36, 8
    file_entry = struct.pack("<IIHHHH", total, 0, 0, 0x5821, 0, 0x20) + name + b"\0"  # size, offset, folder, date, time, attrs
    data_start = header_size + folder_size + len(file_entry)
    blocks = b"".join(struct.pack("<IHH", 0, len(c), u) + c for c, u in frames)  # checksum 0 = none
    cab_size = data_start + len(blocks)
    header = (b"MSCF" + struct.pack("<IIIIIBBHHHHH", 0, cab_size, 0, header_size + folder_size, 0, 3, 1, 1, 1, 0, 0, 0))
    folder = struct.pack("<IHH", data_start, len(frames), 0x0003 | (lzx.WINDOW_BITS << 8))  # LZX, window 2^17
    return header + folder + file_entry + blocks


def libarchive():
    path = ctypes.util.find_library("archive")
    if not path:
        return None
    lib = ctypes.CDLL(path)
    lib.archive_read_new.restype = ctypes.c_void_p
    lib.archive_read_support_format_cab.argtypes = [ctypes.c_void_p]
    lib.archive_read_open_memory.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    lib.archive_read_next_header.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.archive_read_data.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    lib.archive_read_data.restype = ctypes.c_ssize_t
    lib.archive_read_free.argtypes = [ctypes.c_void_p]
    lib.archive_error_string.argtypes = [ctypes.c_void_p]
    lib.archive_error_string.restype = ctypes.c_char_p
    return lib


def cab_extract(lib, cab):
    """The single file in cab, decoded by libarchive."""
    a = lib.archive_read_new()
    try:
        lib.archive_read_support_format_cab(a)
        if lib.archive_read_open_memory(a, cab, len(cab)) != 0:
            raise RuntimeError(lib.archive_error_string(a))
        entry = ctypes.c_void_p()
        if lib.archive_read_next_header(a, ctypes.byref(entry)) != 0:
            raise RuntimeError(lib.archive_error_string(a))
        out, buf = bytearray(), ctypes.create_string_buffer(0x10000)
        while True:
            n = lib.archive_read_data(a, buf, len(buf))
            if n < 0:
                raise RuntimeError(lib.archive_error_string(a))
            if n == 0:
                return bytes(out)
            out += buf.raw[:n]
    finally:
        lib.archive_read_free(a)


def main():
    lib = libarchive()
    if not lib:
        print("libarchive not found: checking with tools/lzx.py's decoder only")
    ok = True
    for size in (1, 0x8000, 0x8001, 3 * 0x8000 + 0x1235, 0x9B501):
        rnd = random.Random(size)
        data = bytes(rnd.choice(b"abcdefgh") if i % 3 else rnd.randrange(256) for i in range(size))
        frames = split_stream(mkbios.lzx_stream(data))
        try:
            own = lzx.decompress_frames(frames) == data
        except Exception:  # bad data can make the decoder fail outright
            own = False
        line = f"{size:#8x} bytes, {len(frames):2d} frames: lzx.py {'ok' if own else 'MISMATCH'}"
        ok &= own
        if lib:
            try:
                ext = cab_extract(lib, make_cab(frames)) == data
            except RuntimeError as e:
                ext, line = False, line + f", libarchive error {e}"
            line += f", libarchive {'ok' if ext else 'MISMATCH'}"
            ok &= ext
        print(line)
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
