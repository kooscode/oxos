#!/usr/bin/env python3
"""Check that OXOS's NV2A pad calibration (kernel/boot/bl.cpp BlPadCalibrate) makes exactly the NV2A writes of a
reference routine (spec/stock/21 §3), by running both in the Unicorn CPU emulator (pip install unicorn) with the
same fake measurements, straps and upgraded-memory flag.

usage: padcal_check.py REF REF_LOADER REF_BIOS OUR_BL_ELF OUR_IMAGE [CASES]
  REF         cerbios: REF_LOADER = Cerbios's bootloader (runs at 0x90000; table routine 0x9103C(base, flag))
              4627:    REF_LOADER = the decrypted Complex 4627 2BL (0x400000; routine 0x400980(base), flag 0 only)
              5838:    REF_LOADER = the decrypted 5838 2BL (0x400000; the EEPROM method 0x400A5F(base), NV2A D3 and
                       later): compared with BlPadCalibrateFromEeprom, both reading a random EEPROM over a model
                       SMBus (word reads of slave 0xA8)
  REF_BIOS    the reference BIOS image (its table)
  OUR_BL_ELF  build/X/boot/bl.elf;  OUR_IMAGE  the matching open image (bios_256k.bin; its table is built in)
"""
import random, struct, subprocess, sys, tempfile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_INSN
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_INS_IN, UC_X86_INS_OUT

ref, refldr, refbios, elf, img = sys.argv[1:6]
cases = int(sys.argv[6]) if len(sys.argv) > 6 else 2000
RET = 0x7000
sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(["nm", elf], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3}
ourfn = sym["_ZL24BlPadCalibrateFromEepromv" if ref == "5838" else "_ZL14BlPadCalibrateb"]
ourtsc = next(v for k, v in sym.items() if "BlTscWait" in k)
with tempfile.NamedTemporaryFile() as t:
    subprocess.run(["llvm-objcopy", "-O", "binary", "-j", ".ram", elf, t.name], check=True)
    ourram = open(t.name, "rb").read()
refcode, refimg, ourimg = open(refldr, "rb").read(), open(refbios, "rb").read()[:0x40000], open(img, "rb").read()[:0x40000]

def run(code, base, entry, patches, args, readings, strap, regs=0, eeprom=None):
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0, 0x100000)
    if base >= 0x100000: mu.mem_map(base, 0x10000)
    mu.mem_map(0xFD000000, 0x1000000)
    mu.mem_map(0xFF000000, 0x1000000)      # flash: the 16 MB window and the 4 MB one at 0xFFC00000
    flash = refimg if code is refcode else ourimg
    for at in (0xFF000000, 0xFFC00000): mu.mem_write(at, flash)
    mu.mem_write(base, code)
    for a, b in patches: mu.mem_write(a, b)
    mu.mem_write(0xFD101000, struct.pack("<I", strap)); mu.mem_write(RET, b"\xf4")
    q, w = list(readings), []
    mu.hook_add(UC_HOOK_MEM_READ, lambda uc, ac, ad, sz, v, u: uc.mem_write(ad, struct.pack("<I", q.pop(0))) if ad == 0xFD00121C else None, begin=0xFD00121C, end=0xFD00121F)
    mu.hook_add(UC_HOOK_MEM_WRITE, lambda uc, ac, ad, sz, v, u: w.append((ad - 0xFD000000, v & 0xFFFFFFFF)), begin=0xFD000000, end=0xFDFFFFFF)
    if eeprom:  # a model SMBus host (status 0xC000, control +2, address +4, data +6, command +8) with the EEPROM at 0xA8
        smb = {"addr": 0, "cmd": 0, "data": 0, "status": 0}
        def port_in(uc, port, size, u):
            return {0xC000: smb["status"], 0xC006: smb["data"] & (0xFFFF if size > 1 else 0xFF)}.get(port, 0)
        def port_out(uc, port, size, val, u):
            if port == 0xC004: smb["addr"] = val & 0xFF
            elif port == 0xC008: smb["cmd"] = val & 0xFF
            elif port == 0xC000: smb["status"] = 0
            elif port == 0xC002 and val & 0x08:
                smb["data"] = eeprom[smb["cmd"]] | eeprom[(smb["cmd"] + 1) & 0xFF] << 8 if smb["addr"] == 0xA9 else 0
                smb["status"] = 0x10
        mu.hook_add(UC_HOOK_INSN, port_in, None, 1, 0, UC_X86_INS_IN)
        mu.hook_add(UC_HOOK_INSN, port_out, None, 1, 0, UC_X86_INS_OUT)
    sp = 0x80000
    for v in reversed(args): sp -= 4; mu.mem_write(sp, struct.pack("<I", v))
    sp -= 4; mu.mem_write(sp, struct.pack("<I", RET)); mu.reg_write(UC_X86_REG_ESP, sp)
    for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX): mu.reg_write(r, regs)
    mu.emu_start(entry, RET, count=2_000_000)
    assert not q, "not all readings used"
    return w

def nops(code, base, lo, hi):
    out, i = [], code.find(b"\x0f\x09", lo - base)
    while 0 <= i < hi - base: out.append((base + i, b"\x90\x90")); i = code.find(b"\x0f\x09", i + 2)
    return out

if ref == "cerbios":
    rbase, rentry, rpatch = 0x90000, 0x9103C, [(0x909F1, b"\xc2\x04\x00")] + nops(refcode, 0x90000, 0x90B08, 0x91187)
elif ref == "5838":
    rbase, rentry, rpatch = 0x400000, 0x400A5F, [(0x40095F, b"\xc2\x04\x00")] + nops(refcode, 0x400000, 0x400A5F, 0x401100)
else:
    rbase, rentry, rpatch = 0x400000, 0x400980, [(0x400919, b"\xc2\x04\x00")] + nops(refcode, 0x400000, 0x400980, 0x400F14)
opatch = [(ourtsc, b"\xc3")] + nops(ourram, 0x400000, ourfn, ourfn + 0x700)
rng, bad = random.Random(1), 0
def vec():
    m = rng.choice([rng.randrange(0, 0x100), rng.randrange(0x30, 0x80), rng.randrange(0, 0x1000), rng.randrange(0, 1 << 32)])
    return [max(0, min((1 << 32) - 1, m + rng.randrange(-3, 4))) for _ in range(4)]
for n in range(cases):
    if ref == "5838":  # 16 readings (A, B, A, B), a random EEPROM
        rd, st, ee = vec() + vec() + vec() + vec(), rng.choice([0, 0x861D4401]), [rng.randrange(256) for _ in range(256)]
        a = run(refcode, rbase, rentry, rpatch, [0xFD000000], rd, st, eeprom=ee)
        b = run(ourram, 0x400000, ourfn, opatch, [], rd, st, eeprom=ee)
        if a != b or len(a) != 27:
            bad += 1
            if bad <= 3: print("MISMATCH", [hex(x) for x in rd], "\n ref", a, "\n ours", b)
        continue
    rd, st = vec() + vec(), rng.choice([0, 0x40000, 0x80000, 0xC0000, 0x861D4401])
    fl = rng.choice([0, 1]) if ref == "cerbios" else 0
    a = run(refcode, rbase, rentry, rpatch, [0xFD000000, fl] if ref == "cerbios" else [0xFD000000], rd, st)
    b = run(ourram, 0x400000, ourfn, opatch, [fl], rd, st, regs=fl)
    if a != b or len(a) != 19:
        bad += 1
        if bad <= 3: print("MISMATCH", [hex(x) for x in rd], hex(st), fl, "\n ref", a, "\n ours", b)
print(f"{ref}: {cases} cases, {bad} mismatches")
