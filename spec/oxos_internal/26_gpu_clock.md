# 26 — GPU clock and memory interface checks before the kernel runs

Status: implemented 2026-11-03 (boot/bl.cpp `BlCheckGpuClock`, `BlRestoreMemoryInterface`, `BlCheckXcodeValues`;
kernel/src/core/startup.cpp). An OXOS addition, like the
DRAM refresh check (20).

## 1. The problem
The X-codes set the NV2A core clock: NVPLL coefficients, NV2A 0x680500 ← 0x00011C01 (M 1, N 28, P 1: 16.667 MHz × 28
/ 1 / 2 = 233 MHz), then the PLL coefficient select 0x68050C ← 0x000A0400, unconditionally. Nothing later in the boot
chain writes them. On the Modxo test console they nevertheless read other values when the 2BL starts, on about one
cold boot in three:
- **NVPLL 0x0006F3FF** (M 255, N 243, P 6: about 0.25 MHz), or 0x0003CA5D, with the select right;
- **NVPLL 0x00012501 with the select 0x000A0000** (bit 10 clear). The core clock then does not follow NVPLL: a write
  of NVPLL alone leaves it reading 0x00012501.

Either way titles run "super slow": the dashboard crawls, and the display's vertical-blank interrupt never comes (the
title's graphics set-up never enables it; 0 GPU interrupts in 4 hours, against about 66 a second normally).

Why the values differ is not known. Adding the retail tables' host 0:0.3 register 0x6C ← 0x01000000 step before the
GPU clock (spec/stock/22 §6.3; Cromwell's table lacks it) changed nothing: 6 of 15 cold boots still started with a
wrong clock. The DRAM refresh bit (spec 20) is another X-code setting found lost on some cold boots.

## 2. The check
The 2BL compares NVPLL and the select with the X-codes' values and, when either differs, writes both in the X-codes'
order (NVPLL, then the select) and reads them back, up to three times:
1. when it starts, before the pad calibration (the X-codes also set the clock before the pads), when nothing has used
   the GPU yet;
2. again just before it jumps to the kernel.

Its record at physical 0x23E0 (cold boots): 'NVPL' (0x4C50564E), NVPLL and the select at step 1, NVPLL and the select at
step 2. The kernel copies it before the memory manager starts and prints, on a serial build, `gpu clock: NVPLL 00011c01
select 000a0400`, or `*** gpu clock: NVPLL ... select ... at 2BL start, ... before the kernel; the 2BL set 00011c01
000a0400` when it had to put them back.

## 3. Result
Netboot BIOS with the check, 15 cold boots to the dashboard: 6 started with a wrong clock (0x0006F3FF, 0x0003CA5D, and
0x00012501 with the select 0x000A0000); the 2BL put both back on all of them, and no boot was slow. (An earlier version
that wrote only NVPLL fixed the 0x0006F3FF boots but not the 0x00012501 one.)

## 4. The memory interface
### 4.1 The problem
On about one cold boot in three the dashboard came up garbled, with the GPU clock right or put right. Two kinds:
- every line of text drawn as one glyph repeated to the screen edge, tiles smeared;
- speckled, blurred textures and text.

A read-back of every register the X-codes write (§5) found, when the 2BL starts, the GPU's memory interface
settings NV2A 0x10B0-0x10E8 (the X-codes write them first of all, before the DRAM is set up) changed on many cold
boots, often to the same wrong values:

| Register | X-codes | Read back | Seen with |
|---|---|---|---|
| 0x10BC | 0x00005866 | 0xFF885066 | the repeated glyphs (every such boot) |
| 0x10B0 | 0x07633461 | 0x76086673 | the speckles (every such boot) |
| 0x10E8 | 0x0C6558C6 | 0x0DA669D7 | with 0x10B0 |
| 0x10B8, 0x10CC, 0x10C4, 0x10C8, 0x10D4, 0x10DC | ... | 0, 0, 0x2400AA05, 0x7D67, 5, 0xB0645236 | no visible fault seen alone |

Over 9 cold boots, all 4 garbled boots had 0x10BC or 0x10B0 wrong, and none of the 5 clean boots did.

### 4.2 The fix
When the 2BL starts (before the GPU clock and the pad calibration, the X-codes' order), if any of the eleven registers
differs from the X-codes' value, the 2BL writes all eleven again, in the X-codes' order, through the write window the
pad calibration uses (`BlWriteWindow`, spec/stock/21 §3.6a: the stores change how the DRAM is driven, so from the
first to the last no memory is touched: wbinvd, a dry pass that brings the loop and the value array into the caches,
then the storing pass run from the caches with interrupts off).

### 4.3 Result
Netboot BIOS with the fix, 20 cold boots to the dashboard: 13 started with memory interface settings changed
(including 0x10BC 0xFF885066 and 0x10B0 0x76086673), the check before the kernel found all of them right, and every
dashboard was drawn correctly.

## 5. The X-code read-back (debug log)
`BlCheckXcodeValues` reads back the registers Cromwell's table writes (the last value of each, under a mask; not the
memory size, which start.S decides, the DRAM mode commands, or the registers the table changes by read-modify-write)
when the 2BL starts and again before the kernel, and leaves the ones that differ in records at physical 0x2400 and
0x2600 ('XCHK', the count, {address, value} pairs; NV2A registers as 0x0Fxxxxxx, PCI configuration addresses as
0x8xxxxxxx). The kernel prints them on a serial build (`xcodes: ...`). Expected to differ on every boot: PCI
0x80000804, 0x80000810, 0x80010014 (read-only or status bits), the pad registers 0x1214-0x1250 (the calibration
rewrites them), 0x1228 (left by the calibration's measurement), and at 2BL start 0x10032C, 0x100410, 0x1210, 0x1218,
0x100328, 0x100338 on some boots, with no visible fault.

## 4. Open
- Why X-code settings do not hold on some cold boots (the GPU clock, the memory interface, DRAM refresh in spec 20):
  the NV2A ignores the writes during the X-codes (measured 2026-11-06, spec 27); the X-codes now check and repeat the
  memory set-up writes. The GPU clock is still repaired here only.
- A 1.6 console (Xcalibur) uses the same X-code values; not checked on one.
