#!/usr/bin/env python3
"""Link the kernel modules into one image against a built kernel (spec/oxos_internal/43).

usage: link_kernel_modules.py --kernel KERNEL_EXE --map KERNEL_MAP --base ADDRESS --out MODULES_EXE
                              [--linker LLD-LINK] [--cc CLANG] OBJECTS...

The kernel modules (kernel/modules/: the extension loader, the kernel settings, launch redirects, the dashboard resolver, the title patcher; with
shared/ini_file/) are not part of the kernel's link: they run at their own fixed address (ADDRESS, mapped only during a launch) and call the kernel directly.
Every symbol their objects use but do not define is looked up in the kernel's map and given to the modules' link as an absolute symbol at the kernel's
address, so their calls and data references go straight to this kernel build. A symbol the kernel does not have, or one in the kernel's boot-only sections
(freed before the first title, spec/oxos_internal/18), stops the build. The image's entry point is XkKernelModulesEntry, which returns the modules' table.
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys, tempfile


def kernel_symbols(map_path):
    """{name: address} of the kernel map's public symbols."""
    symbols = {}
    for line in open(map_path):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m:
            symbols[m[1]] = int(m[2], 16)
    return symbols


def discardable_ranges(exe_path):
    """[(start, end)] of the kernel image's discardable (boot-only) sections, as addresses."""
    pe = open(exe_path, "rb").read()
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    count, optional = struct.unpack_from("<HxxxxxxxxxxxxH", pe, nt + 6)
    base = struct.unpack_from("<I", pe, nt + 24 + 28)[0]
    ranges = []
    for i in range(count):
        h = nt + 24 + optional + 40 * i
        size, rva = struct.unpack_from("<II", pe, h + 8)
        flags = struct.unpack_from("<I", pe, h + 36)[0]
        if flags & 0x02000000:  # IMAGE_SCN_MEM_DISCARDABLE
            ranges.append((base + rva, base + rva + size))
    return ranges


def find_llvm_nm():
    """llvm-nm of any LLVM version: the plain name on the PATH, else the newest installed one (Ubuntu's llvm packages install only llvm-nm-NN and
    /usr/lib/llvm-NN/bin/llvm-nm)."""
    if shutil.which("llvm-nm"):
        return "llvm-nm"
    found = glob.glob("/usr/lib/llvm-*/bin/llvm-nm") + glob.glob("/usr/bin/llvm-nm-[0-9]*")
    return max(found, key=lambda path: int(re.findall(r"\d+", path)[-1])) if found else "llvm-nm"


def needed_symbols(objects, nm):
    """The symbols the objects use and do not define."""
    defined, undefined = set(), set()
    out = subprocess.run([nm] + objects, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*(?:[0-9a-f]+)?\s+([A-Za-z])\s+(\S+)$", line)
        if not m:
            continue
        (undefined if m[1] == "U" else defined).add(m[2])
    return sorted(undefined - defined)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--base", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--out", required=True)
    ap.add_argument("--linker", default="lld-link")
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--nm", default=find_llvm_nm())
    ap.add_argument("objects", nargs="+")
    a = ap.parse_args()

    kernel = kernel_symbols(a.map)
    boot_only = discardable_ranges(a.kernel)
    lines, errors = [], []
    for name in needed_symbols(a.objects, a.nm):
        if name not in kernel:
            errors.append(f"{name}: not in the kernel")
            continue
        address = kernel[name]
        if any(start <= address < end for start, end in boot_only):
            errors.append(f"{name}: in the kernel's boot-only memory (freed before the first title)")
            continue
        lines.append(f'    .globl "{name}"\n    .set "{name}", {address:#x}\n')
    if errors:
        sys.exit("link_kernel_modules.py: the kernel modules use\n  " + "\n  ".join(errors))
    with tempfile.TemporaryDirectory() as work:
        source, symbols = os.path.join(work, "kernel_symbols.S"), os.path.join(work, "kernel_symbols.obj")
        open(source, "w").write("// The kernel's addresses for the kernel modules (tools/link_kernel_modules.py).\n" + "".join(lines))
        subprocess.run([a.cc, "--target=i386-pc-win32", "-c", source, "-o", symbols], check=True)
        subprocess.run([a.linker, "/DRIVER", "/NODEFAULTLIB", "/ENTRY:XkKernelModulesEntry", "/SUBSYSTEM:NATIVE", f"/BASE:{a.base:#x}", "/FIXED", "/ALIGN:0x20",
                        "/FILEALIGN:0x20", "/MERGE:.rdata=.text", "/MERGE:.bss=.data", "/SAFESEH:NO", "/Brepro", "/DEBUG:NONE",
                        f"/MAP:{os.path.splitext(a.out)[0]}.map", f"/OUT:{a.out}"] + a.objects + [symbols], check=True)
    print(f"{a.out}: kernel modules at {a.base:#x}, {len(lines)} kernel symbols")


if __name__ == "__main__":
    main()
