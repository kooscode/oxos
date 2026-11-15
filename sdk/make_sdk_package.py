#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lays out the SDK as it ships, apart from its library (the Makefile builds that into PACKAGE/lib).

usage: make_sdk_package.py PACKAGE

PACKAGE (../build/sdk/oxos_sdk) receives:
  include/             oxos_sdk.h (what an extension includes) and oxos_kernel.h (the Xbox kernel's functions)
  oxos_extension.mk    the build rules
  oxos_extensions.ini  the list of extensions OXOS loads, in order: a sample with the examples, for C:\OXOS\
  examples/            the examples (sources, Makefiles, READMEs; no build output)
  README.md, LICENSE
Files from earlier runs are replaced; lib/ is left as it is.
"""
import os, re, shutil, sys

SDK = os.path.dirname(os.path.abspath(__file__))
EXAMPLE_FILES = re.compile(r'.*\.(c|cpp|h|md|ini)$|^Makefile$')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    package = sys.argv[1]
    for part in ("include", "examples"):
        shutil.rmtree(os.path.join(package, part), ignore_errors=True)
    shutil.copytree(os.path.join(SDK, "include"), os.path.join(package, "include"))
    for name in ("oxos_extension.mk", "oxos_extensions.ini", "README.md", "LICENSE"):
        shutil.copy(os.path.join(SDK, name), os.path.join(package, name))
    for example in sorted(os.listdir(os.path.join(SDK, "examples"))):
        source = os.path.join(SDK, "examples", example)
        target = os.path.join(package, "examples", example)
        os.makedirs(target)
        for name in sorted(os.listdir(source)):
            if EXAMPLE_FILES.match(name):
                shutil.copy(os.path.join(source, name), os.path.join(target, name))
    print(f"{package}: include/ ({', '.join(sorted(os.listdir(os.path.join(package, 'include'))))}), oxos_extension.mk, examples, README.md, LICENSE")


if __name__ == "__main__":
    main()
