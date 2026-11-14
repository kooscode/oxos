#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lays out the SDK as it ships, apart from its library (the Makefile builds that into PACKAGE/lib).

usage: make_sdk_package.py PACKAGE

PACKAGE (../build/sdk/oxos_sdk) receives:
  include/oxos_sdk.h   the one header: include/oxos_sdk.h with the include/oxos/ parts it includes merged in, in order
  oxos_extension.mk    the build rules
  examples/            the examples (sources, Makefiles, READMEs; no build output)
  README.md, LICENSE
Files from earlier runs are replaced; lib/ is left as it is.
"""
import os, re, shutil, sys

SDK = os.path.dirname(os.path.abspath(__file__))
INCLUDE = re.compile(r'^#include <(oxos/[a-z_]+\.h)>\s*$')
EXAMPLE_FILES = re.compile(r'.*\.(c|cpp|h|md|ini)$|^Makefile$')


def merged_header(path, done):
    """The text of path with each #include <oxos/...> replaced by that file's text, each part once."""
    out = []
    for line in open(path).read().splitlines():
        m = INCLUDE.match(line)
        if m:
            if m[1] not in done:
                done.add(m[1])
                out.append(f"// --- {m[1]}")
                out += merged_header(os.path.join(SDK, "include", m[1]), done)
            continue
        if line.strip() == "#pragma once" and done:
            continue  # only the merged file's own
        out.append(line)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    package = sys.argv[1]
    for part in ("include", "examples"):
        shutil.rmtree(os.path.join(package, part), ignore_errors=True)
    os.makedirs(os.path.join(package, "include"))
    lines = merged_header(os.path.join(SDK, "include", "oxos_sdk.h"), set())
    header = ("// oxos_sdk.h: the OXOS kernel extension SDK in one header, made from the SDK's include/ by\n"
              "// make_sdk_package.py. Do not edit; the parts are in the SDK's source.\n")
    open(os.path.join(package, "include", "oxos_sdk.h"), "w").write(header + "\n".join(lines) + "\n")
    for name in ("oxos_extension.mk", "README.md", "LICENSE"):
        shutil.copy(os.path.join(SDK, name), os.path.join(package, name))
    for example in sorted(os.listdir(os.path.join(SDK, "examples"))):
        source = os.path.join(SDK, "examples", example)
        target = os.path.join(package, "examples", example)
        os.makedirs(target)
        for name in sorted(os.listdir(source)):
            if EXAMPLE_FILES.match(name):
                shutil.copy(os.path.join(source, name), os.path.join(target, name))
    print(f"{package}: include/oxos_sdk.h ({len(lines)} lines), oxos_extension.mk, examples, README.md, LICENSE")


if __name__ == "__main__":
    main()
