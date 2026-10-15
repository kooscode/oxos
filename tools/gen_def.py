#!/usr/bin/env python3
"""Generate the kernel's NONAME ordinal .def file from the reference export table.

usage: gen_def.py EXPORTS_TSV EXPORTS_LST OUT_DEF
Every name in EXPORTS_LST must exist in the TSV; data exports get the DATA keyword.
"""
import sys


def main():
    tsv, lst, out = sys.argv[1:4]
    table = {}
    for line in open(tsv).read().splitlines()[1:]:
        ordinal, name, _conv, _args, kind, _addr, _sect = line.split("\t")
        table[name] = (int(ordinal), kind)

    names = [l.strip() for l in open(lst) if l.strip() and not l.lstrip().startswith("#")]
    missing = [n for n in names if n not in table]
    if missing:
        sys.exit(f"unknown export(s): {', '.join(missing)}")

    lines = ["LIBRARY xboxkrnl.exe", "EXPORTS"]
    for n in sorted(names, key=lambda n: table[n][0]):
        ordinal, kind = table[n]
        lines.append(f"    {n:<40} @{ordinal} NONAME{' DATA' if kind == 'data' else ''}")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"{out}: {len(names)} exports")


if __name__ == "__main__":
    main()
