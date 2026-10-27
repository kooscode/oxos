#!/usr/bin/env python3
"""Check the kernel image's layout after the link (kernel/Makefile runs it).

usage: check_kernel_layout.py --exe XBOXKRNL.exe --map XBOXKRNL.map --boot-objs A.obj,B.obj --boot-entries F,G
                              --data-copy SIZE OBJECTS...

1. Boot-only memory (spec/oxos-internal/18-boot-memory.md): the boot-only sections (.xka data, .xkb code) are
   freed before the first title starts, so kept code must not use them. The linker keeps one copy of an inline
   function or a shared constant that several objects define, and that copy can be the one a boot-only file put in
   .xkb. So every global symbol the map places in a boot-only section must be absent from every other object,
   except the boot objects (--boot-objs: the splash and the jingle, which call one another before the pages are
   freed) and the boot-only functions that kept code calls during a cold boot, before the pages are freed
   (--boot-entries, function names).
2. The quick-reboot copy of .data (arch/entry.S, KI_DATA_COPY_SIZE = --data-copy): the initialised part of .data, as
   tools/mkbios.py stores it, must fit, or a quick reboot of a network-booted kernel would take another kernel's.
"""
import argparse, os, re, struct, subprocess, sys


def boot_only_users(map_path, boot, entries, objects):
    sections, boot_syms = set(), {}
    for line in open(map_path):
        m = re.match(r"\s*([0-9a-f]{4}):[0-9a-f]{8} [0-9a-f]{8}H (\.xka|\.xkb)\S*", line)
        if m:
            sections.add(m[1])
            continue
        m = re.match(r"\s*([0-9a-f]{4}):[0-9a-f]{8}\s+(\S+)\s+[0-9a-f]{16}\s+(?:\S+\s+)*?(\S+\.obj)\s*$", line)
        if m and m[1] in sections and re.match(r"[?_]?([A-Za-z0-9_]+)", m[2])[1] not in entries:
            boot_syms[m[2]] = m[3]
    used = {}
    out = subprocess.run(["nm", "-A"] + objects, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(r"(.+?\.obj):(?:[0-9a-f]+)?\s+([A-Z])\s+(\S+)$", line)
        if m and m[3] in boot_syms:
            used.setdefault(m[3], set()).add(m[1])
    bad = [f"  {sym} (boot-only, from {owner}) is used by {obj}"
           for sym, owner in sorted(boot_syms.items()) for obj in sorted(used.get(sym, ()))
           if os.path.basename(obj) not in (owner, *boot)]
    return len(boot_syms), bad


def data_init_size(exe):
    pe = open(exe, "rb").read()
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", pe, lfanew + 6)[0], struct.unpack_from("<H", pe, lfanew + 20)[0]
    for i in range(nsec):
        sh = lfanew + 24 + optsz + 40 * i
        if pe[sh:sh + 8].rstrip(b"\0") == b".data":
            vsize, _, rawsize = struct.unpack_from("<III", pe, sh + 8)
            return min((rawsize + 0xF) & ~0xF, vsize)  # as tools/mkbios.py flatten()
    sys.exit(f"{exe}: no .data section")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--boot-objs", default="")
    ap.add_argument("--boot-entries", default="")
    ap.add_argument("--data-copy", type=lambda v: int(v, 0), required=True)
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args()
    errors = []
    count, bad = boot_only_users(a.map, set(filter(None, a.boot_objs.split(","))),
                                 set(filter(None, a.boot_entries.split(","))), a.objects)
    if bad:
        errors.append("boot-only code or data used by kept code (make it static, or move it out of the boot-only "
                      "file):\n" + "\n".join(bad))
    init = data_init_size(a.exe)
    if init > a.data_copy:
        errors.append(f"the initialised .data is {init:#x} bytes, more than the quick-reboot copy holds "
                      f"({a.data_copy:#x}, KI_DATA_COPY_SIZE in src/arch/entry.S)")
    if errors:
        sys.exit("\n".join(errors))
    print(f"kernel layout: {count} boot-only symbols, none used by kept code; .data initialiser {init:#x} of "
          f"{a.data_copy:#x} bytes")


if __name__ == "__main__":
    main()
