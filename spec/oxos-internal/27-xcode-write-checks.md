# 27 — Write checks in the X-codes

OXOS addition, implemented 2026-11-06.
- **Code:** `tools/harden_xcodes.py` (run by `kernel/Makefile` on the assembled X-code bank), `kernel/boot/start.S`
  (entry snapshot, serial builds), `kernel/src/ke/startup.cpp` (its log line).
- **Related:** the 2BL's repairs (spec 26: GPU clock, memory interface; spec 20: DRAM refresh) stay as a second line.

## 1. The problem
On the 1.4 test console (Modxo modchip, Cromwell's X-codes) the NV2A ignores some of the X-codes' writes:
- **Cold boots:** about half lose one to three settings. Most often the memory interface (0x10B0, 0x10B8, 0x10BC,
  0x10CC, 0x10E8), the DRAM controller set-up (0x100328-0x100338, 0x100410), the GPU clock and DRAM refresh. The 2BL
  repaired the memory interface, the clock and refresh, but not the DRAM controller. And a repair after the X-codes
  comes too late for settings the X-codes' own DRAM initialisation depends on. About one cold boot in 50 failed: once
  the X-codes ended without reaching the 2BL (the RAM did not hold the overflow trick's jump; the SMC retried three
  times and gave up), once the 2BL ran with RAM returning garbage.
- **Warm reboots** (`HalReturnToFirmware(1)`): about one in ten lost every setting, including writes made late in the
  program, while start.S's writes a moment later held. The next boot never reached the title. The NV2A leaves reset
  after the MCPX has started the X-codes.

Measured with a snapshot taken at start.S's first instruction (§4): the settings are already wrong there, so the
writes are lost during the X-codes. Reading a register right after the write shows its power-on value (the write
was ignored, not undone later). A second write almost always holds. The losses hit the same registers every time,
not the ~150 X-codes evenly, so they are not flash read errors.

## 2. The checks
`harden_xcodes.py` decodes the X-code program from bank offset 0x80 to its END, inserts checks after writes, and moves
every goto/ifgoto to its original target, so Cromwell's branches are unchanged. The program grows from 228 to 312
X-codes and ends at 0xB78, below the pad calibration table at 0xC90.
1. **Waiting for the GPU** (the 1.0-1.5 value of 0x10B0 only): after the first write to 0x10B0, until it reads back
   the value written, write it again. While the GPU does not take writes, 0x10B0 reads its power-on value
   (0x76086673); while it does not even claim its registers, the cycles go to the LPC bus (the Modxo logs the
   writes) and reads return whatever the bus returns, so only the written value ends the wait on hardware. xemu
   reads 0x10B0 as 0: a 0 while PMC 0x0 (the GPU id) reads xemu's 0x02A000A3 also ends it. Then the single retry of
   step 2.
2. **One retry** after each write to the memory interface (0x10B0-0x10E8, including the three revision-dependent
   0x10B8 writes), the DRAM controller (0x100328, 0x10032C, 0x100330, 0x100338, 0x100410) and the refresh enable
   (0x100210 bit 31): read the register back and, if it differs, write it once more. Bounded: no loop.

Not checked: the GPU clock (NVPLL reads back differently right after its write; the 2BL repairs it) and the pad
registers (the 2BL's calibration rewrites them).

## 3. Results (1.4 test console, Modxo, netboot, FTP server title)
Boot loop: power off 8 s and on (cold), or `SITE REBOOT` (warm); success = the FTP server answering within 90 s.

| Build | Boots (cold / warm) | Failed | Settings lost at start.S entry |
|---|---|---|---|
| Before (Cromwell's X-codes) | 40 / 20 | 1 cold (X-codes ended), 2 warm (all lost) | about half of the cold boots |
| Read-back, one retry, 9 registers | 40 / 20 | 2 warm (all lost) | |
| Wait for the GPU + two retries, memory interface and refresh only | 30 / 30 | 1 cold (2BL ran on bad RAM) | 0x10B0/0x10BC/refresh never; 0x10B8 and the DRAM controller still |
| Wait + one retry, all of §2 | 30 / 30 | none | 0x10BC, 0x10B8, 0x100338 once each; GPU clock 4 (repaired) |
| As above, but the wait ends on any value other than the power-on one | 77 / 23 | 3 warm, 5 cold (all lost; GPU registers on the LPC bus) | |
| Wait until the written value holds (§2 as now) | 75 / 25 | none | 10E8 twice, refresh once (repaired); leaked LPC cycles in many boots, all waited out |

## 4. Entry snapshot (serial builds)
start.S reads NV2A 0x10B0, 0x10BC, 0x10E8, NVPLL (0x680500), the PLL select (0x68050C), 0x100210, 0x100200 and
0x100204 before anything else. It keeps them in MMX registers until the memory is sized, then stores them at
physical 0x2340 ('XC0S' and the eight values). The kernel prints `xcodes at entry: ...`. Build switch
`-DXK_XCODE_ENTRY_SNAPSHOT=$(SERIAL)`.

## 5. Open
- Whether other modchips or consoles show the same lost writes. Only the Modxo 1.4 console was measured. Cerbios was
  never seen to fail on this console, but it was not measured the same way.
- The 1.6 console's X-codes (MCPXREVD5) get the retries but not the wait: its 0x10B0 value differs and its power-on
  value was not measured.
- The wait has no limit (the X-codes have no counter). A GPU that never takes the write keeps the boot there; on the
  test console it always took it within the wait.
