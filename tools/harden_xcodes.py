#!/usr/bin/env python3
"""Add write checks to the X-code program of a built X-code bank (boot/xcodes.S -> xcodes.bin), relocating jumps.

On the 1.4 test console the GPU ignores some of the X-codes' writes (spec/oxos_internal/27): the first ones when it
leaves reset after the MCPX has started the X-codes (on some warm reboots all of them), and single writes on cold
boots. The memory interface and DRAM controller settings have to hold before the X-codes initialise the DRAM, so:
- after the first write to NV2A 0x10B0 (the 1.0-1.5 or the 1.6 value): until it reads back the value written, write it
  again. While the GPU is not taking writes it reads its power-on value (0x76086673), or, when the GPU does not
  claim its registers yet, whatever the LPC bus returns (the writes then show up on the modchip's bus). xemu reads
  0x10B0 as 0: a 0 together with xemu's GPU id (PMC 0x0 = 0x02A000A3) also ends the wait. The usual single retry
  follows;
- after each write to the memory interface (0x10B0-0x10E8), the DRAM controller set-up (0x100328-0x100338,
  0x100410) and the refresh enable (0x100210 bit 31), read it back and, if it differs, write it once more.
The program is decoded from bank offset 0x80 to its END, checks are inserted after the writes, and every goto/ifgoto
is moved to its original target. The rest of the bank (header, pad calibration table) is kept.

usage: harden_xcodes.py IN.bin OUT.bin"""
import struct, sys

START = 0x80                     # program start; it must end before the pad calibration table (header 0x7C)
WAIT_REGISTER = 0x0F0010B0
WAIT_VALUES = (0x07633461, 0x01000010)  # the first 0x10B0 write: 1.0-1.5 (Cromwell), 1.6 (boot/xcodes_1_6.inc)
XEMU_ID_REGISTER, XEMU_ID = 0x0F000000, 0x02A000A3   # PMC boot id; xemu reads 0x10B0 as 0
CHECKED = {
    0x0F0010B0: ~0, 0x0F0010B4: ~0, 0x0F0010B8: ~0, 0x0F0010BC: ~0, 0x0F0010C4: ~0, 0x0F0010C8: ~0,
    0x0F0010CC: ~0, 0x0F0010D4: ~0, 0x0F0010D8: ~0, 0x0F0010DC: ~0, 0x0F0010E8: ~0,
    0x0F100328: ~0, 0x0F10032C: ~0, 0x0F100330: ~0, 0x0F100338: ~0, 0x0F100410: ~0,
    0x0F100210: 0x80000000,
}
PEEK, POKE, AND_OR, IFGOTO, GOTO, END = 0x02, 0x03, 0x06, 0x08, 0x09, 0xEE


def harden(bank):
    limit = struct.unpack_from("<I", bank, 0x7C)[0]
    ops, off = [], START
    while True:
        op, a, v = struct.unpack_from("<BII", bank, off)
        ops.append([op, a, v, None])
        off += 9
        if op == END:
            break
    old_end = off
    if any(bank[old_end:limit]):
        sys.exit(f"harden_xcodes: bytes {old_end:#x}-{limit:#x} after the program are not free")
    for i, o in enumerate(ops):
        if o[0] in (IFGOTO, GOTO):
            rel = o[2] - (1 << 32) if o[2] & 0x80000000 else o[2]
            assert rel % 9 == 0, f"jump at X-code {i} is not a whole number of X-codes"
            o[3] = ("old", i + 1 + rel // 9)
    new, moved, waited = [], {}, False
    for i, (op, a, v, _) in enumerate(ops):
        moved[i] = len(new)
        new.append(ops[i])
        if op != POKE or a not in CHECKED:
            continue
        if a == WAIT_REGISTER and v in WAIT_VALUES and not waited:
            waited = True
            # held: on to the single retry below; 0 with xemu's GPU id: xemu, also on; anything else: write again
            w = len(new) - 1
            new += [[PEEK, a, 0, None], [IFGOTO, v, 9, None], [GOTO, 0, 0, ("new", w + 8)],
                    [IFGOTO, 0, 0, ("new", w)], [PEEK, XEMU_ID_REGISTER, 0, None],
                    [IFGOTO, XEMU_ID, 0, ("new", w)], [GOTO, 0, 0, ("new", w + 8)]]
        mask = CHECKED[a] & 0xFFFFFFFF
        new.append([PEEK, a, 0, None])
        if mask != 0xFFFFFFFF:
            new.append([AND_OR, mask, 0, None])
        # differs: skip the goto and write again; holds: skip the write
        new += [[IFGOTO, v & mask, 9, None], [GOTO, 0, 9, None], [POKE, a, v, None]]
    out = bytearray(bank)
    end = START + 9 * len(new)
    if end > limit:
        sys.exit(f"harden_xcodes: the program would end at {end:#x}, past the table at {limit:#x}")
    for j, (op, a, v, target) in enumerate(new):
        if target:
            kind, t = target
            v = ((moved[t] if kind == "old" else t) - (j + 1)) * 9 & 0xFFFFFFFF
        struct.pack_into("<BII", out, START + 9 * j, op, a, v)
    if end < old_end:
        out[end:old_end] = bytes(old_end - end)
    return bytes(out), len(ops), len(new), end


def main():
    bank = open(sys.argv[1], "rb").read()
    out, n_old, n_new, end = harden(bank)
    open(sys.argv[2], "wb").write(out)
    print(f"harden_xcodes: {n_old} -> {n_new} X-codes, end {end:#x}")


if __name__ == "__main__":
    main()
