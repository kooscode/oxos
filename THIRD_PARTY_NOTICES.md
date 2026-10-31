# Third-party notices

The OXOS BIOS images (`bios_*.bin`) contain the third-party material below. Everything else in them is OXOS's own
code (GPL-2.0, see `LICENSE`).

## Intel CPU microcode

The kernel carries two of Intel's microcode updates for the Xbox's Pentium III (signatures 0x686 and 0x68A,
platform 4), unmodified, and loads the matching one at start-up. They come from
[Intel's microcode repository](https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files)
(`intel-ucode/06-08-06` and `06-08-0a`) and are kept unmodified in this repository in `third_party/intel-ucode/`,
with Intel's licence and their source commit. They are not covered by OXOS's licence. Intel's licence:

```
Copyright (c) 2018-2021 Intel Corporation.
All rights reserved.

Redistribution.

Redistribution and use in binary form, without modification, are permitted,
provided that the following conditions are met:

1.  Redistributions must reproduce the above copyright notice and the
    following disclaimer in the documentation and/or other materials provided
    with the distribution.

2.  Neither the name of Intel Corporation nor the names of its suppliers may
    be used to endorse or promote products derived from this software without
    specific prior written permission.

3.  No reverse engineering, decompilation, or disassembly of this software
    is permitted.


"Binary form" includes any format that is commonly used for electronic
conveyance that is a reversible, bit-exact translation of binary
representation to ASCII or ISO text, for example "uuencode".

DISCLAIMER.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

Intel does not endorse OXOS.

## XboxDev/cromwell

The X-code table and the EEPROM key states come from [XboxDev/cromwell](https://github.com/XboxDev/cromwell)
(GPL-2.0, the same licence as OXOS), at the commit pinned in `kernel/Makefile`.

## Hardware tables

The video encoder and NV2A memory pad calibration tables are functional hardware constants taken from retail BIOS
images; see [`dmca-compliance.md`](dmca-compliance.md).
