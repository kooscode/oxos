#!/usr/bin/env python3
"""Dump an Xbox's MCPX boot ROM (the 512-byte secret ROM in the southbridge) over the network.

README.md in this folder has the step-by-step guide; spec/oxos-internal/12-mcpx-dump.md has the design.

  mcpx_dump.py build --mcpx 1.1 --bios RETAIL.bin --out dump.bin   make a flash image from your own retail BIOS
  mcpx_dump.py receive [--bios RETAIL.bin] [--out mcpx_1.1.bin]    wait for the console's dump, check and save it

Flash the built image to a modchip and power the console on with a network cable to this PC. The console sends
its ROM within a few seconds of power-on, then boots on as an OXOS netboot image.

How it gets at the ROM. The boot ROM runs the X-codes in flash, checks the next stage, jumps to it and hides itself
later. OXOS images normally enter through the X-codes' overflow trick, which runs our code only after the ROM has
hidden itself. Here the check passes instead, so our code runs while the ROM is still mapped:
  - MCPX 1.1 (TEA attack): the ROM checks the first-stage loader (FBL, flash 0x3D400) with TEA used as a hash.
    Bit 31 of key words 2 and 3 can be flipped together without changing the result, and the FBL's first dwords
    are such key words. Flipping them turns its first instruction (a jump) into a jump to RAM 0x7FD588, where two
    extra X-codes leave `jmp 0xFFFC1000`, our 2BL.
  - MCPX 1.0 (RC4 attack): the ROM RC4-decrypts the 2BL (flash 0x39E00) and checks one dword deep inside it, then
    calls the entry point stored in its first dword. RC4 is a stream cipher: XOR-ing that ciphertext dword with
    (wanted ^ actual) sets the entry point to 0xFFFC1000 without touching the checked dword.
The attacks are those of XboxDev/mcpx-tools (mcpx-attack); this is an independent implementation in Python.

The payload is an OXOS netboot image built with `make -C kernel mcpx-dump`. Its 2BL copies 0xFFFFFE00-0xFFFFFFFF
at entry and broadcasts the copy (netboot op 6, UDP port 5141) once the link is up. The retail BIOS's X-codes, init
tables and (1.1) first-stage loader stay as they are; everything else in the bank is the payload's.

Standard library only. The ROM and the retail BIOS are Microsoft code: keep them to yourself.
"""
import argparse
import hashlib
import socket
import struct
import sys

M32 = 0xFFFFFFFF
BANK = 0x40000                      # the images work on one 256 KiB bank (the top one of a larger file)
PAYLOAD_START = 0x1000              # our 2BL's entry, 0xFFFC1000
FBL, FBL_END = 0x3D400, 0x3FC80     # MCPX 1.1: first-stage loader, the range the ROM hashes (flash 0xFFFFD400...)
BL_ENC = 0x39E00                    # MCPX 1.0: the encrypted 2BL (0x6000 bytes, flash 0xFFFF9E00)
STUB = 0x007FD588                   # where the flipped FBL jump lands (the CPU drops the address bits above 23 here)
ENTRY = 0xFFFC1000
RC4_FIRST_KEYSTREAM_DWORD = 0x79277645  # the 1.0 key's first keystream dword (mcpx-tools' rc4_attack)
PORT, MAGIC, OP_MCPX = 5141, 0x424E584F, 6


def tea_encrypt(v, k):
    v0, v1, s = v[0], v[1], 0
    for _ in range(32):
        s = (s + 0x9E3779B9) & M32
        v0 = (v0 + ((((v1 << 4) + k[0]) & M32) ^ ((v1 + s) & M32) ^ (((v1 >> 5) + k[1]) & M32))) & M32
        v1 = (v1 + ((((v0 << 4) + k[2]) & M32) ^ ((v0 + s) & M32) ^ (((v0 >> 5) + k[3]) & M32))) & M32
    return [v0, v1]


def fbl_hash(bank):
    """The MCPX 1.1 ROM's TEA hash of 0xFFFFD400-0xFFFFFC80 (two rounds per 8 bytes, Davies-Meyer style)."""
    start, end, seed = 0xFFFFD400, 0xFFFFFC80, 0x8F000
    a, b = [start, end], [end, seed]
    for off in range(FBL, FBL_END, 8):
        lo, hi = struct.unpack_from("<II", bank, off)
        for _ in range(2):
            a = tea_encrypt(a, [a[0], a[1], lo, hi])
            a, b = b, a
    return struct.pack("<II", *a)


def rc4(key, data):
    s, j = list(range(256)), 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out, i, j = bytearray(), 0, 0
    for c in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(c ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def top_bank(path):
    data = open(path, "rb").read()
    if len(data) < BANK or len(data) % BANK:
        sys.exit(f"{path}: not a flash image (size {len(data):#x})")
    return bytearray(data[-BANK:])


def insert_stub_xcodes(bank, stub, target):
    """Two X-code pokes before the first `exit` (0xEE) that write `jmp target` at stub."""
    at = 0x80
    while bank[at] != 0xEE:
        at += 9
        if at >= PAYLOAD_START:
            sys.exit("no X-code exit before 0x1000: not a retail BIOS?")
    exit_op = bytes(bank[at:at + 9])
    ops = struct.pack("<BII", 0x03, stub, 0xE9) + struct.pack("<BII", 0x03, stub + 1, (target - stub - 5) & M32)
    bank[at:at + len(ops)] = ops
    bank[at + len(ops):at + len(ops) + 9] = exit_op
    return at


def build(a):
    bank, payload = top_bank(a.bios), top_bank(a.payload)
    if payload[0x6000:0x6004] != b"OXKP":
        sys.exit(f"{a.payload}: not an OXOS image (make -C kernel mcpx-dump)")
    if a.mcpx == "1.1":
        if bank[FBL] != 0xE9:
            sys.exit(f"{a.bios}: no first-stage loader jump at {FBL:#x}: not an MCPX 1.1 retail BIOS")
        before = fbl_hash(bank)
        w0, w1 = struct.unpack_from("<II", bank, FBL)
        struct.pack_into("<II", bank, FBL, w0 ^ 0x80000000, w1 ^ 0x80000000)
        if fbl_hash(bank) != before:
            sys.exit("internal error: the TEA hash changed")
        rel = struct.unpack_from("<i", bank, FBL + 1)[0]
        land = (0xFFFFD400 + 5 + rel) & 0x007FFFFF
        if land != STUB:
            sys.exit(f"the patched jump lands at {land:#x}, not {STUB:#x}: unexpected first-stage loader")
        exit_at = insert_stub_xcodes(bank, STUB, ENTRY)
        end = FBL
        detail = f"TEA hash {before.hex()} unchanged; FBL jump -> {STUB:#x} -> {ENTRY:#x} (X-codes at {exit_at:#x})"
    else:
        rel_dword = struct.unpack_from("<I", bank, BL_ENC)[0]
        struct.pack_into("<I", bank, BL_ENC, (ENTRY ^ RC4_FIRST_KEYSTREAM_DWORD) & M32)
        end = BL_ENC
        detail = f"2BL entry dword {rel_dword:08x} -> entry {ENTRY:#x}"
    lost = [o for o in range(end, 0x3FE00) if payload[o] != 0xFF]
    bank[PAYLOAD_START:end] = payload[PAYLOAD_START:end]
    open(a.out, "wb").write(bank)
    print(f"{a.out}: MCPX {a.mcpx} attack, payload {PAYLOAD_START:#x}-{end:#x}; {detail}")
    if lost:
        print(f"  note: {len(lost)} payload bytes above {end:#x} were not copied (its flash kernel's quick-reboot "
              f"data); netboot and cold boots are unaffected")


def check(rom, bios_path):
    """Problems with a dump, as a list of strings (empty: looks good)."""
    issues = []
    if len(rom) != 512:
        return [f"{len(rom)} bytes, not 512"]
    if rom[:2] != b"\x33\xC0":
        issues.append("does not start with 33 C0 (xor eax, eax): the ROM was probably hidden already")
    if rom[-3:] != b"\xB0\x02\xEE":
        issues.append("does not end with B0 02 EE (mov al, 2; out dx, al)")
    tea = b"\xB9\x79\x37\x9E" in rom
    if bios_path:
        bank = top_bank(bios_path)
        if tea:
            if rom[0x1A8:0x1B0] != fbl_hash(bank):
                issues.append("the FBL hash stored at ROM 0x1A8 does not match that BIOS's first-stage loader")
        else:
            key, where = rom[0x1A5:0x1B5], struct.unpack_from("<I", rom, 0x182)[0] - 0x90000
            plain = rc4(key, bank[BL_ENC:BL_ENC + 0x6000])
            if not 0 <= where <= 0x6000 - 4 or plain[where:where + 4] != rom[0x187:0x18B]:
                issues.append("the check value stored in the ROM does not match that BIOS's 2BL")
    return issues


def udp_of_frame(frame):
    if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
        return None
    u = 14 + (frame[14] & 0x0F) * 4
    dport, length = struct.unpack_from(">HH", frame, u + 2)
    return frame[u + 8:u + length] if dport == PORT else None


def receive(a):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if a.xemu:
        s.bind(("127.0.0.1", a.xemu))
        print(f"waiting for the dump from xemu (tools/run.py --net-udp {a.xemu}) ...")
    else:
        s.bind(("", PORT))
        print(f"waiting for the dump on UDP port {PORT}: power the console on now ...")
    while True:
        data, peer = s.recvfrom(65536)
        if a.xemu:
            data = udp_of_frame(data)
            if data is None:
                continue
        if len(data) < 20:
            continue
        magic, op, _ver, _res, _session, _offset, length = struct.unpack_from("<IBBHIII", data)
        if magic != MAGIC or op != OP_MCPX or length != 512 or len(data) < 20 + 512:
            continue
        rom = data[20:20 + 512]
        kind = "1.1" if b"\xB9\x79\x37\x9E" in rom else "1.0"
        out = a.out or f"mcpx_{kind}.bin"
        issues = check(rom, a.bios)
        print(f"dump from {peer[0] if not a.xemu else 'xemu'}: MCPX {kind}, md5 {hashlib.md5(rom).hexdigest()}, "
              f"sha1 {hashlib.sha1(rom).hexdigest()}")
        for i in issues:
            print(f"  problem: {i}")
        if issues and not a.keep_bad:
            print("  not saved (--keep-bad saves it anyway); waiting for another copy ...")
            continue
        open(out, "wb").write(rom)
        print(f"saved {out}" + ("" if a.bios else " (checked without the BIOS: pass --bios for the full check)"))
        return 0 if not issues else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="make the flash image")
    b.add_argument("--mcpx", choices=("1.0", "1.1"), required=True,
                   help="the console's MCPX: 1.0 (consoles 1.0) or 1.1 (consoles 1.1-1.5; not 1.6)")
    b.add_argument("--bios", required=True, help="your retail BIOS for that MCPX (any size, top 256 KiB used)")
    b.add_argument("--payload", default="kernel/build/mcpx-dump/bios-256k.bin",
                   help="the OXOS payload image (default: %(default)s, from make -C kernel mcpx-dump)")
    b.add_argument("--out", required=True)
    r = sub.add_parser("receive", help="wait for the dump and save it")
    r.add_argument("--bios", help="the retail BIOS the image was built from: also check the dump against it")
    r.add_argument("--out", help="file name (default mcpx_1.0.bin or mcpx_1.1.bin)")
    r.add_argument("--xemu", type=int, metavar="PORT", help="an xemu console (tools/run.py --net-udp PORT)")
    r.add_argument("--keep-bad", action="store_true", help="save a dump that fails the checks too")
    a = ap.parse_args()
    return build(a) if a.cmd == "build" else receive(a)


if __name__ == "__main__":
    sys.exit(main())
