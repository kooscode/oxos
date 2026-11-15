#!/usr/bin/env python3
"""Fails when OXOS's own code uses the Xbox kernel's Rtl* string, memory or formatting functions instead of the kernel's C library (strlen, strcasecmp,
memcpy, snprintf, ...: kernel/src/runtime/c_library.h). Those Rtl* functions are exports for titles only; the kernel implements them in kernel/src/runtime/.

usage: check_c_library_use.py ROOT

Checked: kernel/src (but runtime/, which implements them), the SDK (sdk/src, sdk/examples), the test extensions (tests/extensions). Not checked: the kernel's
self-tests, which call the title exports as a title does, and the console apps, which are titles.
"""
import glob, os, re, sys

TITLE_ONLY = """RtlCopyMemory RtlZeroMemory RtlMoveMemory RtlFillMemory RtlFillMemoryUlong RtlCompareMemory RtlCompareMemoryUlong RtlSnprintf RtlSprintf
RtlVsnprintf RtlVsprintf RtlInitAnsiString RtlInitUnicodeString RtlEqualString RtlCompareString RtlEqualUnicodeString RtlCompareUnicodeString RtlUpperChar
RtlLowerChar RtlCopyString RtlUpperString RtlCharToInteger RtlIntegerToChar RtlAppendStringToString RtlFreeAnsiString""".split()


def main():
    root = sys.argv[1]
    files = [f for pattern in ("kernel/src/**/*.cpp", "kernel/src/**/*.h", "sdk/src/*.cpp", "sdk/examples/**/*.cpp", "tests/extensions/*.c*")
             for f in glob.glob(os.path.join(root, pattern), recursive=True)
             if not os.path.relpath(f, root).startswith(os.path.join("kernel", "src", "runtime"))]
    found = []
    pattern = re.compile(r"\b(" + "|".join(TITLE_ONLY) + r")\s*\(")
    for path in sorted(files):
        for number, line in enumerate(open(path, encoding="utf-8"), 1):
            code = line.split("//")[0]
            if pattern.search(code) and not code.lstrip().startswith("XBAPI"):
                found.append(f"{os.path.relpath(path, root)}:{number}: {line.strip()}")
    if found:
        sys.exit("check_c_library_use.py: OXOS's own code uses a title-only Rtl* function; use the C library (kernel/src/runtime/c_library.h):\n  " +
                 "\n  ".join(found))


if __name__ == "__main__":
    main()
