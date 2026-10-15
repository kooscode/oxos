#!/usr/bin/env python3
"""Pack a kernel PE into an MCPX 1.0 BIOS image, reusing the base image's init tables and 2BL.

usage: mkbios.py --base BIOS.bin --mcpx-rom MCPX.bin --kernel xboxkrnl.exe --out bios.bin
                 [--flat-kernel OUT.exe]

Layout (one 256 KiB bank, see xbox-files/../tmp4/xbox_bios_unpack.py and spec/00):
  [0, inittblSize)              init tables + X-codes (kept from the base image)
  [kstart, dstart)              kernel: RC4(kernel key, LDI stream of LZX frames)
  [dstart, 0x39E00)             kernel .data initializer (plaintext)
  [0x39E00, 0x3FE00)            2BL, RC4(MCPX 1.0 key); boot params + digest updated here
The kernel is flattened (file offset == RVA, .data incl. BSS stored in full) and its DOS-header
fields 0x28..0x37 describe the .data copy used by quick reboot (spec/02 §1).

LZX: tools/lzx.py compresses each 32 KiB frame as one verbatim block (window 2^17, like the stock 2BL
stream); its own decoder checks the result before the image is written.
"""
import argparse, hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzx  # noqa: E402

BANK = 0x40000
FLASH_BASE = 0xFFFC0000
OFF_2BL, LEN_2BL = 0x39E00, 0x6000
BLDR_PARAMS, BLDR_KEYS, BLDR_MAGIC = 0x5FDC, 0x6C, 0x7854794A
FRAME = 0x8000
XBOX_SUBSYSTEM = 14


def rc4(key, data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data)); i = j = 0
    for n, b in enumerate(data):
        i = (i + 1) & 0xFF; j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


u32 = lambda d, o: struct.unpack_from("<I", d, o)[0]
u16 = lambda d, o: struct.unpack_from("<H", d, o)[0]


def flatten(pe, data_src_addr_for):
    """Return (image, data_init) with raw == RVA layout and quick-reboot header fields set.

    data_src_addr_for(len) -> flash address where the .data initializer of that length will live.
    """
    lfanew = u32(pe, 0x3C)
    nsec, optsz = u16(pe, lfanew + 6), u16(pe, lfanew + 20)
    oh = lfanew + 24
    size_of_image = u32(pe, oh + 56)
    img = bytearray(size_of_image)
    img[:u32(pe, oh + 60)] = pe[:u32(pe, oh + 60)]
    data = None
    for i in range(nsec):
        sh = oh + optsz + i * 40
        name = pe[sh:sh + 8].rstrip(b"\0")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", pe, sh + 8)
        img[va:va + rawsize] = pe[rawptr:rawptr + rawsize]
        span = (vsize + 0x1F) & ~0x1F
        struct.pack_into("<II", img, sh + 16, span, va)      # SizeOfRawData, PointerToRawData
        if name == b".data":
            data = (va, vsize, rawsize)
    if data is None:
        sys.exit("kernel has no .data section")
    struct.pack_into("<H", img, oh + 68, XBOX_SUBSYSTEM)

    va, vsize, rawsize = data
    init_len = min((rawsize + 0xF) & ~0xF, vsize)
    base = u32(pe, oh + 28)
    struct.pack_into("<IIII", img, 0x28, vsize - init_len, init_len, data_src_addr_for(init_len), base + va)
    # PE checksum over the final image.
    struct.pack_into("<I", img, oh + 64, 0)
    s = 0
    for (w,) in struct.iter_unpack("<H", bytes(img[:len(img) & ~1])):
        s += w; s = (s & 0xFFFF) + (s >> 16)
    struct.pack_into("<I", img, oh + 64, ((s & 0xFFFF) + (s >> 16)) + len(img))
    return bytes(img), bytes(img[va:va + init_len])


def lzx_stream(image):
    """LDI stream: per 32 KiB frame {u16 compressed_len, u16 uncompressed_len, LZX bytes}, then 0."""
    frames = lzx.compress_frames(image)
    if lzx.decompress_frames(frames) != image:
        sys.exit("LZX self-check failed")
    return b"".join(struct.pack("<HH", len(c), u) + c for c, u in frames) + b"\0\0\0\0"


# Modded BIOSes (e.g. "Complex 4627") carry a 2BL stub that patches the decompressed kernel at fixed addresses:
#   call $+5; pop esi; cld; xor ecx,ecx; add esi,imm8   -> table of {u8 len, u32 address, len bytes}, 0xFF end
# Those addresses belong to the stock kernel, so the table is emptied for our kernel.
PATCHER_STUB = bytes.fromhex("e8000000005efc33c983c6")


def disable_kernel_patcher(bl):
    i = bl.find(PATCHER_STUB)
    if i < 0:
        return
    table = i + 5 + bl[i + len(PATCHER_STUB)]
    n, t = 0, table
    while t < len(bl) and bl[t] != 0xFF:
        t += 5 + bl[t]
        n += 1
    bl[table] = 0xFF
    print(f"2BL kernel patcher found: disabled its table of {n} patches at 2BL+{table:#x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True)
    ap.add_argument("--mcpx-rom", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--flat-kernel")
    a = ap.parse_args()

    raw = open(a.base, "rb").read()
    bank = bytearray(raw[:BANK])
    rom = open(a.mcpx_rom, "rb").read()[-0x200:]
    mcpx_key = rom[0x1A5:0x1B5]
    bl = bytearray(rc4(mcpx_key, bank[OFF_2BL:OFF_2BL + LEN_2BL]))
    if u32(bl, 0x5FE4) != BLDR_MAGIC or u32(bl, BLDR_PARAMS + 8) != BLDR_MAGIC:
        sys.exit("base image is not an MCPX 1.0 BIOS with a recognised 2BL (or wrong MCPX ROM)")
    disable_kernel_patcher(bl)
    _old_dsz, itsz = struct.unpack_from("<II", bl, BLDR_PARAMS)
    kkey = bytes(bl[BLDR_KEYS + 32:BLDR_KEYS + 48])

    pe = open(a.kernel, "rb").read()
    image, data_init = flatten(pe, lambda n: FLASH_BASE + OFF_2BL - n)
    if a.flat_kernel:
        open(a.flat_kernel, "wb").write(image)

    kernel = rc4(kkey, lzx_stream(image))
    dsz, ksz = len(data_init), len(kernel)
    dstart = OFF_2BL - dsz
    kstart = dstart - ksz
    if kstart < itsz:
        sys.exit(f"kernel too large: needs {ksz + dsz:#x} bytes, {OFF_2BL - itsz:#x} available")

    bank[itsz:OFF_2BL] = bytes(OFF_2BL - itsz)
    bank[kstart:dstart] = kernel
    bank[dstart:OFF_2BL] = data_init

    inner = hashlib.sha1(kkey + struct.pack("<I", ksz) + kernel + struct.pack("<I", dsz) + data_init
                         + struct.pack("<I", itsz) + bytes(bank[:itsz])).digest()
    struct.pack_into("<I", bl, BLDR_PARAMS, dsz)
    struct.pack_into("<I", bl, BLDR_PARAMS + 12, ksz)
    bl[BLDR_PARAMS + 16:BLDR_PARAMS + 36] = hashlib.sha1(kkey + inner).digest()
    bank[OFF_2BL:OFF_2BL + LEN_2BL] = rc4(mcpx_key, bytes(bl))

    out = bytes(bank) * (len(raw) // BANK)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out):#x} bytes; kernel image {len(image):#x} -> stream {ksz:#x} at {kstart:#x}, "
          f".data init {dsz:#x} at {dstart:#x}, free {kstart - itsz:#x}")


if __name__ == "__main__":
    main()
