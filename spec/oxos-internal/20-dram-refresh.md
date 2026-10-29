# 20 — DRAM refresh check before the kernel runs

Status: implemented 2026-10-28 (boot/start.S, boot/bl.cpp, kernel/src/ke/startup.cpp). An OXOS addition; the
reference boot chain has no equivalent.

## 1. The problem
The NV2A's DRAM refresh is switched on by the X-codes: PFB 0x100210 ← 0x80000000, after the mode-register sequence
and before the timing register and the memory-clock (host 0:0.3 register 0x6C) writes. Every X-code table examined
does this with the same value, for every console revision (Complex 4627 and Cromwell for 1.0-1.5, 5838 for 1.6,
Cerbios and EvoX for both; spec/stock/22 §6.3). Nothing later in a reference chain reads or writes the register again:
neither retail 2BL (4627, 5838), nor Cerbios's loader, nor the 4627 / 5838 kernels (spec/stock/22 §6.3a).

On the no-DVD 1.4 console (2026-10-28) the register read 0 (bit 31 clear, its low counter fields stopped) right after
the X-codes in about one cold boot in five once the console had been powered for ~10 minutes (the 2BL's pad
measurement A had drifted to 0x5C-0x5E, spec/stock/21 §3.8). It stayed 0 through the 2BL and the whole kernel run.
Without refresh, DRAM cells lose their charge within seconds, the weakest first: single bits of kernel code, data
and the page database changed from 0 to 1 two to seven seconds into the kernel, and the boot crashed (bad pointers,
free-list damage, changed instructions). With refresh on (bit 31 set) the low bytes of the register count steadily.
Measured: 8 of 8 boots with the register 0 at kernel entry were bad; 0 of 49 with it set.

## 2. The check
Bit 31 of PFB 0x100210 clear means refresh is off; then 0x80000000 is written, the value of every X-code table. The
check runs at three places, each cheap and harmless when refresh is on:
1. boot/start.S, the first OXOS code after the X-codes, before the memory-size probe or anything else stores data
   in RAM. Record at physical 0x23D8: 'RFS0' (0x30534652) and the value found, or 0 when it was on.
2. boot/bl.cpp, after everything the 2BL does (pad calibration, chipset set-up, splash copy, netboot, kernel
   decompression), right before the jump to the kernel. Record at 0x23D0: 'RFEN' (0x4E454652) and the value found,
   or 0.
3. KiSystemStartup, the kernel's first code, for any boot loader (another modchip BIOS, an older OXOS image, a
   quick reboot). The value found is kept.
A debug build logs each one that acted ("*** refresh: ..."), after the memory manager starts (KiReportRefresh).

## 3. Result on the 1.4 console
Warm band (A 0x5C-0x5E), cold boots 8 s apart: before, 8 bad of 40; with the check, 0 bad of 56. The start.S check
found refresh off on 6 of those 56 boots and switched it on; the 2BL and kernel checks never had to act. The
released build (v0.3.5): 46 boots, no crash, refresh found off and switched on twice. xemu (make test-dash)
unaffected. The investigation that found the cause: spec/oxos-internal/21.

## 4. Open
- Whether the retail X-codes on the same console leave refresh off too (not tested: the Cromwell table is used; its
  memory writes equal the retail ones, but the surrounding steps and their timing differ).
- Why the enable is lost: the memory clock is reprogrammed after refresh is enabled in every table; a clock change
  that resets the refresh logic on a warm DRAM interface would explain it, but is not established.
