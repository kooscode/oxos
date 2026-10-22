#!/usr/bin/env python3
"""Pack a kernel PE into an MCPX 1.0 BIOS image, reusing the base image's init tables and 2BL.

usage: mkbios.py --base BIOS.bin --mcpx-rom MCPX.bin --kernel xboxkrnl.exe --out bios.bin
                 [--flat-kernel OUT.exe] [--splash IMAGE]
       mkbios.py --open --xcodes X.bin --loader BL.bin --reset RESET.bin --cromwell DIR --kernel xboxkrnl.exe
                 --out bios.bin [--flat-kernel OUT.exe] [--splash IMAGE] [--size 256K|1M]

The second form builds the open image (no Microsoft parts, no MCPX dump needed; spec/stock/01 §5), see build_open().

Layout (one 256 KiB bank, see spec/stock/00):
  [0, inittblSize)              init tables + X-codes (kept from the base image)
  [kstart, dstart)              kernel: RC4(kernel key, LDI stream of LZX frames)
  [dstart, 0x39E00)             kernel .data initializer (plaintext)
  [0x39E00, 0x3FE00)            2BL, RC4(MCPX 1.0 key); boot params + digest updated here
The kernel is flattened (file offset == RVA, .data incl. BSS stored in full) and its DOS-header
fields 0x28..0x37 describe the .data copy used by quick reboot (spec/stock/02 §1).

Boot splash (optional): the second 256 KiB bank of a 1 MiB image (flash offset 0x40000, 0xFFF40000) holds
  "XKSPLASH", u32 version 1, u32 length, u32 CRC-32, then a 640x480 256-colour palette PNG with one IDAT chunk.
  The open image also keeps a copy in each boot bank when it fits (see build_open), for 256 KiB images.
The boot ROM reads banks 0 (init table at 0xFF000000) and 3 only; the kernel shows the image when the header is
valid and falls back to its drawn emblem otherwise (spec/oxos-internal/00-boot-splash.md).

LZX: tools/lzx.py compresses each 32 KiB frame as one verbatim block (window 2^17, like the stock 2BL
stream); its own decoder checks the result before the image is written.
"""
import argparse, hashlib, io, os, struct, sys, zlib

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


# Some modded BIOSes carry a 2BL stub that patches the decompressed kernel at fixed addresses:
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


SPLASH_BANK, SPLASH_W, SPLASH_H = 1, 640, 480


SPLASH_BLACK = 6  # pixels no brighter than this become black: background noise costs bytes, not quality


def png_chunks(png):
    """(kind, data) for each chunk of a PNG file."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    chunks, o = [], 8
    while o + 8 <= len(png):
        n, kind = struct.unpack_from(">I4s", png, o)
        chunks.append((kind, png[o + 8:o + 8 + n]))
        o += 12 + n
    return chunks


def splash_png(chunks):
    """The splash PNG the kernel decodes: signature, IHDR, PLTE, one IDAT with the whole zlib stream, IEND (the
    decoder wants one IDAT); None unless chunks are a 640x480 8-bit palette image, not interlaced."""
    ihdr = next((d for k, d in chunks if k == b"IHDR"), b"")
    if len(ihdr) != 13 or struct.unpack(">IIBBBBB", ihdr) != (SPLASH_W, SPLASH_H, 8, 3, 0, 0, 0):
        return None
    out = [(k, d) for k, d in chunks if k in (b"IHDR", b"PLTE")]
    out += [(b"IDAT", b"".join(d for k, d in chunks if k == b"IDAT")), (b"IEND", b"")]
    return b"\x89PNG\r\n\x1a\n" + b"".join(struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
                                          for k, d in out)


def convert_splash(path, quantize):
    """Any image Pillow reads, as a 640x480 palette PNG: scaled to fit keeping its aspect ratio (black bars), its
    near-black noise cleared and reduced to 256 colours without dithering. `quantize`: "octree" (smallest; suits
    textured art) or "median" (median cut: larger, but no banding in smooth gradients)."""
    try:
        from PIL import Image
    except ImportError:
        sys.exit(f"{path} is not a 640x480 256-colour PNG: converting it needs Pillow (sudo apt install "
                 "python3-pil). The converted image can be saved with --save-splash and used without Pillow.")
    im = Image.open(path)
    if im.mode != "P" or im.size != (SPLASH_W, SPLASH_H):
        im = im.convert("RGB")
        if im.size != (SPLASH_W, SPLASH_H):
            scale = min(SPLASH_W / im.width, SPLASH_H / im.height)
            w, h = round(im.width * scale), round(im.height * scale)
            canvas = Image.new("RGB", (SPLASH_W, SPLASH_H))
            canvas.paste(im.resize((w, h), Image.LANCZOS), ((SPLASH_W - w) // 2, (SPLASH_H - h) // 2))
            im = canvas
        px = bytearray(im.tobytes())
        for i in range(0, len(px), 3):
            if max(px[i], px[i + 1], px[i + 2]) <= SPLASH_BLACK:
                px[i] = px[i + 1] = px[i + 2] = 0
        im = Image.frombytes("RGB", im.size, bytes(px))
        method = {"octree": Image.Quantize.FASTOCTREE, "median": Image.Quantize.MEDIANCUT}[quantize]
        im = im.quantize(256, method=method, dither=Image.Dither.NONE)
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=True)
    return splash_png(png_chunks(buf.getvalue()))


def splash_bank(path, quantize="octree", save=None):
    """The splash bank contents for an image file. A 640x480 256-colour PNG (such as kernel/splash/splash.png, or
    one written by --save-splash) is used as it is, without Pillow; anything else is converted with Pillow."""
    raw = open(path, "rb").read()
    chunks = png_chunks(raw)
    png = (splash_png(chunks) if chunks else None) or convert_splash(path, quantize)
    if save:
        open(save, "wb").write(png)
    blob = b"XKSPLASH" + struct.pack("<III", 1, len(png), zlib.crc32(png)) + png
    if len(blob) > BANK:
        sys.exit(f"splash image too large: {len(blob):#x} bytes after compression, {BANK:#x} available")
    return blob + b"\xff" * (BANK - len(blob)), len(blob)


# --- Open image (spec/stock/01 §5). Each 256 KiB bank:
#   0x00000 MCPX header + X-codes (kernel/boot/xcodes.S: XboxDev/cromwell's table, ending in the overflow trick)
#   0x01000 our 2BL (kernel/boot/start.S + bl.cpp), entered at 0xFFFC1000
#   0x06000 payload header {'OXKP', kernel offset, packed size, image size, keys offset, keys size,
#                           splash offset, splash size} (splash 0, 0 when there is none in the bank)
#   0x06040 key block (below), then the kernel as a raw deflate stream
#   ...     a copy of the splash ("XKSPLASH" header + PNG) when it fits in the space left, so a 256 KiB image (one
#           bank: --size 256K, or the first 256 KiB of a 1 MiB image) shows it too
#   ...     the kernel's .data initializer (quick reboot), ending at 0x3FE00
#   0x3FE00 our reset code (kernel/boot/reset.S) for running without an MCPX boot ROM
OPEN_LOADER, OPEN_PAYLOAD, OPEN_KEYS, OPEN_RESET = 0x1000, 0x6000, 0x6040, BANK - 0x200
OPEN_FLASH = 0xFFFC0000


def cromwell_key_states(cromwell_dir):
    """EEPROM-key HMAC states from XboxDev/cromwell (drivers/ide/BootHddKey.c): for each EEPROM encryption version
    9-12, the SHA-1 state after the inner (key ^ ipad) and outer (key ^ opad) block, five words each."""
    import re
    text = open(os.path.join(cromwell_dir, "drivers/ide/BootHddKey.c")).read()
    states = {}
    for fn, slot in (("HMAC1hddReset", 0), ("HMAC2hddReset", 1)):
        body = text[text.index(f"int {fn}("):]
        body = body[:body.index("\n}")]
        for case in re.finditer(r"case\s+(\d+):(.*?)break;", body, re.S):
            words = [int(v, 16) for v in re.findall(r"Intermediate_Hash\[\d\]\s*=\s*(0x[0-9a-fA-F]+)", case.group(2))]
            if len(words) != 5:
                sys.exit(f"unexpected Cromwell key state layout in {fn}, case {case.group(1)}")
            states.setdefault(int(case.group(1)), [None, None])[slot] = words
    out = [states[v][0] + states[v][1] for v in sorted(states) if None not in states[v]]
    if not out:
        sys.exit("no EEPROM key states found in the Cromwell sources")
    return out


def key_block(states):
    """Handed to KernelEntry with arg0 = 'OXOS': no EEPROM/certificate key (32 zero bytes), then 'OXBK', the state
    count and the states (spec/stock/01 §1)."""
    block = bytes(32) + struct.pack("<II", 0x4B42584F, len(states))
    for s in states:
        block += struct.pack("<10I", *s)
    return block


def build_open(a):
    import zlib
    xcodes, loader, reset = (open(p, "rb").read() for p in (a.xcodes, a.loader, a.reset))
    if len(xcodes) > OPEN_LOADER or len(loader) > OPEN_PAYLOAD - OPEN_LOADER or len(reset) != 0x200:
        sys.exit(f"open image parts do not fit: x-codes {len(xcodes):#x}, 2BL {len(loader):#x}, reset {len(reset):#x}")
    pe = open(a.kernel, "rb").read()
    data_end = OPEN_RESET
    image, data_init = flatten(pe, lambda n: OPEN_FLASH + ((data_end - n) & ~0xF))
    if a.flat_kernel:
        open(a.flat_kernel, "wb").write(image)
    dstart = (data_end - len(data_init)) & ~0xF
    keys = key_block(cromwell_key_states(a.cromwell))
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(image) + packer.flush()
    kstart = (OPEN_KEYS + len(keys) + 0xF) & ~0xF
    if kstart + len(packed) > dstart:
        sys.exit(f"kernel too large for the open image: {len(packed):#x} bytes packed, {dstart - kstart:#x} available")

    bank = bytearray(b"\xff" * BANK)
    bank[:len(xcodes)] = xcodes
    bank[OPEN_LOADER:OPEN_LOADER + len(loader)] = loader
    bank[OPEN_PAYLOAD:OPEN_PAYLOAD + 24] = struct.pack("<6I", 0x504B584F, kstart, len(packed), len(image),
                                                       OPEN_KEYS, len(keys))
    bank[OPEN_KEYS:OPEN_KEYS + len(keys)] = keys
    bank[kstart:kstart + len(packed)] = packed
    bank[dstart:dstart + len(data_init)] = data_init
    bank[OPEN_RESET:] = reset

    splash, splash_full, free = "", None, dstart - kstart - len(packed)
    struct.pack_into("<II", bank, OPEN_PAYLOAD + 24, 0, 0)
    if a.splash:
        splash_full, size = splash_bank(a.splash, a.splash_quantize, a.save_splash)
        sstart = (dstart - size) & ~0xF
        if sstart >= kstart + len(packed):  # a copy in the boot bank's free space, below the .data initializer
            bank[sstart:sstart + size] = splash_full[:size]
            struct.pack_into("<II", bank, OPEN_PAYLOAD + 24, sstart, size)
            free = sstart - kstart - len(packed)
            splash = f"; splash in the boot bank at {sstart:#x}"
        elif a.size == "256K":
            sys.exit(f"no room for the splash in a 256 KiB image: {size:#x} bytes, {free:#x} free")
        else:
            print(f"warning: the splash ({size:#x} bytes) does not fit in the boot bank ({free:#x} free): "
                  "only the 1 MiB image's bank 1 has it")

    if a.size == "256K":
        out = bytes(bank)
    else:
        banks = [bytes(bank)] * 4
        if splash_full:
            banks[SPLASH_BANK] = splash_full
            splash += f" and bank {SPLASH_BANK}"
        out = b"".join(banks)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out):#x} bytes, open image; kernel image {len(image):#x} -> deflate {len(packed):#x} at "
          f"{kstart:#x}, {len(keys) // 4 - 10} key-state words, free {free:#x}{splash}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base")
    ap.add_argument("--mcpx-rom")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--open", action="store_true", help="build the open image (no Microsoft parts)")
    ap.add_argument("--xcodes")
    ap.add_argument("--loader")
    ap.add_argument("--reset")
    ap.add_argument("--cromwell", help="XboxDev/cromwell source tree (EEPROM key states)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--flat-kernel")
    ap.add_argument("--splash", help="boot splash image: flash bank 1 of a 1 MiB image, and (open image) a copy in "
                    "each boot bank when it fits")
    ap.add_argument("--size", choices=("256K", "1M"), default="1M", help="open image size (default 1M)")
    ap.add_argument("--splash-quantize", choices=("octree", "median"), default="octree")
    ap.add_argument("--save-splash", metavar="PNG", help="also write the splash as converted (usable without Pillow)")
    a = ap.parse_args()
    if a.open:
        if not (a.xcodes and a.loader and a.reset and a.cromwell):
            sys.exit("--open needs --xcodes, --loader, --reset and --cromwell")
        return build_open(a)
    if not (a.base and a.mcpx_rom):
        sys.exit("--base and --mcpx-rom are required (or --open)")

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

    banks = [bytes(bank)] * (len(raw) // BANK)
    splash = ""
    if a.splash:
        if len(banks) > SPLASH_BANK + 1:
            banks[SPLASH_BANK], size = splash_bank(a.splash, a.splash_quantize, a.save_splash)
            splash = f"; splash in bank {SPLASH_BANK} ({size:#x} bytes)"
        else:
            print(f"warning: {len(raw):#x}-byte image has no spare bank for the splash; not included")
    out = b"".join(banks)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out):#x} bytes; kernel image {len(image):#x} -> stream {ksz:#x} at {kstart:#x}, "
          f".data init {dsz:#x} at {dstart:#x}, free {kstart - itsz:#x}{splash}")


if __name__ == "__main__":
    main()
