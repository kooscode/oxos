# Intel CPU microcode

Intel's microcode update files for the Pentium III family, kept here unmodified so the kernel build does not depend
on an outside download. The kernel build picks the two updates for the Xbox's CPU (signatures 0x686 and 0x68A,
platform 4) out of them with `tools/mkucode.py` and loads the matching one at start-up.

| File | From |
|---|---|
| `06-08-06`, `06-08-0a` | `intel-ucode/` in [Intel-Linux-Processor-Microcode-Data-Files](https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files) at commit `bdc92abe5c499c3fd7988b76a128d05c9120a520` |
| `license` | `license` in the same repository and commit |

`SHA256SUMS` holds their checksums; `kernel/Makefile` checks them before each use, so a changed file stops the build.

These files are Intel's, not OXOS's, and are not covered by OXOS's licence (GPL-2.0). They are redistributed in
binary form without modification under Intel's licence (`license`): Copyright (c) 2018-2021 Intel Corporation, all
rights reserved; provided "as is", without warranty. Intel does not endorse OXOS.
