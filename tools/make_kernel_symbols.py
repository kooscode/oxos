#!/usr/bin/env python3
"""make_kernel_symbols.py KERNEL.exe KERNEL.map OUT.elf: gdb's symbol file for a kernel built without debug info (a release kernel; spec/oxos_internal/51 §5).

The kernel itself (KERNEL.exe, the very file the BIOS packs) converted to ELF, so every section sits at the kernel's addresses, plus a symbol for every
function and variable the linker's map (KERNEL.map) names, at the address the map gives. C names get their plain form too (_NtCreateFile@36 also as
NtCreateFile), so `break NtCreateFile` works; C++ names keep the linker's form. No source lines: the release kernel is built without debug info (a DEBUG=1
build's oxoskrnl.elf has them)."""
import os
import re
import subprocess
import sys
import tempfile

MAP_LINE = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s")


def elf_sections(objdump, elf):
    """[(name, start, end)] of the ELF file's allocated sections."""
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True, check=True).stdout
    found = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit():
            size, address = int(parts[2], 16), int(parts[3], 16)
            if size:
                found.append((parts[1], address, address + size))
    return found


def map_symbols(path):
    """{name: address} from the linker's map (its symbol lines; the section lines at the top have no address)."""
    found = {}
    for line in open(path, errors="replace"):
        match = MAP_LINE.match(line)
        if match:
            found[match.group(3)] = int(match.group(4), 16)
    return found


def plain_name(name):
    """The C name of a decorated one (_Name@12 or _Name -> Name), or None for a C++ name."""
    if name.startswith("?") or not name.startswith("_"):
        return None
    return re.sub(r"@\d+$", "", name[1:])


def main():
    exe, map_path, out = sys.argv[1:4]
    objcopy, objdump = os.environ.get("GNU_OBJCOPY", "objcopy"), os.environ.get("GNU_OBJDUMP", "objdump")
    subprocess.run([objcopy, "-O", "elf32-i386", exe, out], check=True)
    sections = elf_sections(objdump, out)
    arguments, names = [], set()
    for name, address in sorted(map_symbols(map_path).items(), key=lambda item: item[1]):
        place = next(((section, start) for section, start, end in sections if start <= address < end), None)
        if not place:
            continue
        section, start = place
        kind = "function" if section == ".text" else "object"
        for symbol in (name, plain_name(name)):
            if symbol and symbol not in names:
                names.add(symbol)
                arguments.append(f"--add-symbol={symbol}={section}:{address - start:#x},global,{kind}")
    with tempfile.NamedTemporaryFile("w", suffix=".rsp", delete=False) as response:
        response.write("\n".join(arguments))
    try:
        subprocess.run([objcopy, f"@{response.name}", out], check=True)
    finally:
        os.unlink(response.name)
    print(f"{out}: {len(names)} symbols")


if __name__ == "__main__":
    main()
