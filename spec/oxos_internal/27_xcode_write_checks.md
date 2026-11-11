# 27 — The X-codes: write checks and our interpreter

OXOS addition. Write checks (§1-§4) implemented 2026-11-06; our X-code interpreter for consoles without a boot ROM
(§5) implemented 2026-10-15 with the open BIOS image (spec/stock/01 §5).
- **Code:** `tools/harden_xcodes.py` (run by `kernel/Makefile` on the assembled X-code bank), `kernel/boot/start.S`
  (entry snapshot, serial builds), `kernel/src/core/startup.cpp` (its log line), `kernel/boot/reset.S` (§5).
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

## 5. Our X-code interpreter (reset code)
`kernel/boot/reset.S`, the top 512 bytes of each 256 KB bank (bank offset 0x3FE00, run address 0xFFFFFE00).

### 5.1 When it runs
The processor fetches its first instruction from 0xFFFFFFF0. Which device answers decides who runs the X-codes:
- **Retail console (MCPX 1.0, 1.1, 1.6):** the MCPX boot ROM covers 0xFFFFFE00-0xFFFFFFFF. The ROM runs the X-codes
  (spec/stock/24 §1) and reaches the 2BL through the overflow trick. The reset code is in the flash but never read.
- **Debug kit with the MCPX X2:** there is no boot ROM; the fetch reaches the flash over LPC (seen 2026-11-10), and
  the reset code runs.
- **xemu without a boot-ROM file** (`bootrom_path` empty): as the X2.

### 5.2 Entry
1. Reset vector, bank offset 0x3FFF0: a 16-bit near jump to 0xFFFFFE00.
2. Still in real mode (code segment base 0xFFFF0000): interrupts off, direction flag clear; the GDT is loaded from a
   descriptor inside the block, so its address is taken relative to the code segment.
3. The GDT, also inside the block: null; 0x08 flat 32-bit code (base 0, limit 4 GB); 0x10 flat data.
4. CR0.PE set; a far jump to 0x08 continues in 32-bit code; DS, ES, SS ← 0x10, FS, GS ← 0.
5. Not set: no IDT, no stack (nothing uses one), caches and MTRRs as the processor leaves reset (CR0.CD and NW set,
   MTRRs off): every access goes to the bus uncached.

### 5.3 The interpreter
- Program pointer: **0xFFFC0080**, bank offset 0x80: the same table the ROM reads at 0xFF000080. Accumulator ← 0.
- Each op is 9 bytes: op byte, operand 1, operand 2 (32-bit, little-endian). The pointer moves on by 9 before the op
  runs, so jumps are relative to the next op.
- Op 0x07 ("use result") first rewrites itself: the op becomes operand 1 (all 32 bits), operand 1 becomes operand 2,
  operand 2 becomes the accumulator. Then that op runs.

| Op | Does |
|---|---|
| 0x02 | accumulator ← 32-bit read of operand 1 |
| 0x03 | 32-bit write of operand 2 to operand 1 |
| 0x04 | 0xCF8 ← operand 1, 0xCFC ← operand 2 (32-bit) |
| 0x05 | 0xCF8 ← operand 1; accumulator ← 32-bit read of 0xCFC |
| 0x06 | accumulator ← (accumulator AND operand 1) OR operand 2 |
| 0x07 | use result (above) |
| 0x08 | if accumulator ≠ operand 1: pointer += operand 2 |
| 0x09 | pointer += operand 2 |
| 0x11 | 8-bit write of the low byte of operand 2 to port operand 1 |
| 0x12 | accumulator ← 8-bit read of port operand 1, zero-extended |
| 0xEE | exit (§5.4) |
| any other | no operation |

The op numbers and operand layout are the X-code encoding of XboxDev/cromwell's `boot_rom/2bBootStartup.S` macros,
which `kernel/boot/xcodes.S` also uses to write the table.

### 5.4 Exit (op 0xEE)
A jump to **0xFFFC1000** (bank offset 0x1000, the 2BL's entry in start.S), in flat 32-bit mode with the caches still
off. Operand 1 is not used. Nothing is checked or hidden. The table's last two writes still put the overflow trick's
jump in RAM at 0; it is not run, and start.S clears it as on a retail console.

### 5.5 Compared with the ROM's interpreter (spec/stock/24 §1)
| | MCPX ROM | Ours |
|---|---|---|
| Table start | 0xFF000080 | 0xFFFC0080 (the same bytes through the flash window) |
| 0x02 read | address AND 0x0FFFFFFF | address as given |
| 0x04 PCI write | 0x80000880 bit 1 forced to 0 (the ROM cannot be hidden) | no guard |
| 0x07 use result | op = low byte of operand 1 | op = all of operand 1 |
| 0x10 (AND-OR, second register) | supported | no operation |
| Exit | MTRRs cleared, default type ← operand 1, caches on; then the loader check (1.0 RC4, 1.1 TEA) and on failure hide + overflow trick | jump to 0xFFFC1000 |

None of the differences reaches our tables: the 1.0-1.5 program (Cromwell's, with the §2 checks; 312 ops) and the
1.6 program (`boot/xcodes_1_6.inc`, 406 ops) have no read at or above 256 MB, no write to 0x80000880, no op 0x10 and
only exact op numbers in op 0x07's operand 1 (checked on the built release images, 2026-11-10). start.S sets the
MTRRs and caches itself (spec/stock/01 §5), so the skipped exit steps are not needed. A table change that uses any of
these would behave differently on the two paths.

### 5.6 Tested
- xemu without a boot ROM: every `make test` boot.
- Green debug kit (MCPX X2, Conexant, 128 MB), Modxo: every boot of the kit since 2026-11-07 (netboot, release and
  serial images, 1.0-1.5 X-codes).

## 6. Open
- Whether other modchips or consoles show the same lost writes. Only the Modxo 1.4 console was measured. Cerbios was
  never seen to fail on this console, but it was not measured the same way.
- The 1.6 console's program (boot/xcodes_1_6.inc, 2026-11-07) gets the retries and the wait, on its own 0x10B0
  value 0x01000010; the end of the program comes from header 0x7C (0xECB on the 1.6). The 1.6's power-on value of
  0x10B0 was not measured: if it already equals 0x01000010, the wait ends at once. The writes the 1.6 program makes
  from EEPROM bytes (strap, 0x10B8, 0x122C, 0x1214) are not checked.
- The wait has no limit (the X-codes have no counter). A GPU that never takes the write keeps the boot there; on the
  test console it always took it within the wait.
