#!/usr/bin/env python3
"""Generate the kernel's NONAME ordinal .def file from the export table (kernel/exports.tsv).

usage: gen_def.py EXPORTS_TSV OUT_DEF

The table has one row per ordinal of the 4627 kernel: ordinal, name, calling convention, argument bytes, kind.
Calling conventions: stdcall, fastcall, cdecl, and compiler: a 64-bit arithmetic helper the compiler calls itself (_alldiv, ...), with its own register
convention. OXOS exports these and memcpy/memset/memmove/memcmp to kernel extensions at its own ordinals 402-415 (spec/oxos_internal/34 §9.1).

Kinds:
  func   a function, exported at its ordinal;
  data   a variable, exported at its ordinal with the DATA keyword;
  debug  a debugger-only export (DbgLoadImageSymbols, DbgUnLoadImageSymbols) that the retail kernel leaves out;
         kept in the table for a future debug build, not exported.
"""
import sys

KINDS = ("func", "data", "debug")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    tsv, out = sys.argv[1:3]
    lines = ["LIBRARY oxoskrnl.exe", "EXPORTS"]
    count = 0
    for n, line in enumerate(open(tsv).read().splitlines()[1:], 2):
        ordinal, name, _conv, _args, kind = line.split("\t")[:5]
        if kind not in KINDS:
            sys.exit(f"{tsv}:{n}: unknown kind {kind!r} (one of {', '.join(KINDS)})")
        if kind == "debug":
            continue
        lines.append(f"    {name:<40} @{int(ordinal)} NONAME{' DATA' if kind == 'data' else ''}")
        count += 1
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"{out}: {count} exports")


if __name__ == "__main__":
    main()
