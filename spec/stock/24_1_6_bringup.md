# Spec 24 — The 1.6 console from power-on to the kernel's first title (retail 5838 BIOS)

Reference behaviour of the retail BIOS for the 1.6 console (NV2A revision D3 or later, MCPX with the 1.1 boot ROM,
Xcalibur video encoder): the boot ROM's X-code interpreter, the 5838 X-code program, the EEPROM memory parameters,
the memory test, the first-stage loader (FBL), the 2BL's 1.6 paths and the kernel's 1.6 paths. Behaviour only; no
code. Status: reference only. OXOS comparisons and plans: spec/oxos_internal/28.

Evidence conventions:
- "MCPX 1.1 ROM 0xFFFFxxxx": the 512-byte boot ROM (xbox_files/mcpx/mcpx_1.1.bin) at its run address 0xFFFFFE00.
  "MCPX 1.0 ROM": mcpx_1.0.bin, same addressing.
- "5838 X-code 0x0412": flash offset of a 9-byte X-code in xbox_files/bios/5838.bin. Same for "4627 X-code"
  (Complex_4627-256k.bin) and "Cerbios X-code" (cerbios-hybrid-3.1.0.bin).
- "5838 FBL 0xFFFFD5B1": the first-stage loader at its run address (flash 0x3D400 = 0xFFFFD400).
- "5838 2BL 0x400A5F": the decrypted 2BL at 0x400000 + image offset (where it runs; spec 21 uses the same addresses).
- "5838 kernel 0x80054921": xboxkrnl 1.0.5838.1 at base 0x80010000.
- "Cromwell": third_party/cromwell (GPL v2), file and line.
- PCI addresses are bus:device:function register. "0:30:0" is the AGP bridge (configuration address 0x8000F0xx),
  "1:0:0" the NV2A, "0:1:1" the SMBus function, "0:0:3" the host function at 0x800003xx. NV2A registers are given as
  offsets; during the X-codes the NV2A is at 0x0F000000, afterwards at 0xFD000000.

## 1. The X-code interpreter (MCPX 1.1 boot ROM)

### 1.1 Entry
- Reset vector 0xFFFFFFF0 jumps to 0xFFFFFFB8: GDT and IDT loaded from the descriptor at 0xFFFFFFF4, protected mode
  on, far jump to 0x08:0xFFFFFE00 (MCPX 1.1 ROM 0xFFFFFFB8-0xFFFFFFCE).
- 0xFFFFFE00: DS, ES, SS ← 0x10 (flat). The program pointer starts at **0xFF000080** (flash offset 0x80 through the
  16 MB flash window) (MCPX 1.1 ROM 0xFFFFFE0A).
- Caches are as the processor leaves reset (CR0.CD and NW set) and the MTRRs are off, so every X-code memory access
  goes to the bus uncached.

### 1.2 Op format and state
- Every op is 9 bytes: op byte, operand 1 (32-bit, little-endian), operand 2 (32-bit) (MCPX 1.1 ROM 0xFFFFFE0F-0xFFFFFE14).
- One 32-bit **accumulator** (the result register). Ops 0x02, 0x05, 0x06, 0x10 and 0x12 set it; no other op changes it.
- A second register used only by op 0x10. The ROM never initialises it (its value at reset is the processor's).
- After every op except the exit, the pointer moves on by 9 bytes.

### 1.3 Ops
| Op | Name | Behaviour (MCPX 1.1 ROM address) |
|---|---|---|
| 0x02 | memory read | accumulator ← 32-bit read of (operand 1 **AND 0x0FFFFFFF**) (0xFFFFFE23). An address at or above 256 MB is folded into the low 256 MB (a read of 0xFF000080 reads 0x0F000080): the flash and the ROM cannot be read, and the NV2A must sit below 256 MB (0x0F000000) while the X-codes read its registers. |
| 0x03 | memory write | 32-bit write of operand 2 to operand 1 (no address mask) (0xFFFFFE34) |
| 0x04 | PCI write | 0xCF8 ← operand 1, 0xCFC ← operand 2 (32-bit cycles). If operand 1 is 0x80000880 (0:1:0 0x80), bit 1 of the value is forced to 0: the X-codes cannot hide the ROM (0xFFFFFE46-0xFFFFFE62) |
| 0x05 | PCI read | 0xCF8 ← operand 1; accumulator ← 32-bit read of 0xCFC (0xFFFFFE64) |
| 0x06 | AND-OR | accumulator ← (accumulator AND operand 1) OR operand 2 (0xFFFFFE3C) |
| 0x07 | use result | the low byte of operand 1 is taken as the op to run; that op gets operand 2 as its operand 1 and the **accumulator** as its operand 2. So "07, 03, A" writes the accumulator to memory A; "07, 04, A" writes it to PCI address A; "07, 11, P" writes its low byte to port P (0xFFFFFE17-0xFFFFFE21) |
| 0x08 | jump if not equal | if accumulator ≠ operand 1: pointer += operand 2 (signed, relative to the **next** op) (0xFFFFFE77) |
| 0x09 | jump | pointer += operand 2 (relative to the next op) (0xFFFFFE83) |
| 0x10 | AND-OR, second register | second register ← (second register AND operand 1) OR operand 2; accumulator ← second register (0xFFFFFE8B). Not used by 4627, 5838, Cerbios or Cromwell. |
| 0x11 | I/O write | 8-bit write of the low byte of operand 2 to port operand 1 (0xFFFFFE97) |
| 0x12 | I/O read | accumulator ← 8-bit read of port operand 1, zero-extended (0xFFFFFEA2) |
| 0xEE | exit | §1.4 (0xFFFFFEAE) |
| any other | — | **no operation**: the pointer moves on by 9. This covers **0xF5 and 0x80**, the first two ops of the 5838 and 4627 programs (their operands, 0x00081000 / 1 and 0x00000114 / 0x00000228, are not read by the ROM). |
- A jump of 0 is a no-op (5838 X-codes 0x07EB and 0x07F4; Cromwell `xcode_goto(1)` twice, boot_rom/Xcodes.h:261-262).
- There is no counter, no delay op and no time-out: every poll loop in a program waits for ever if its condition never
  comes true.

### 1.4 Exit (op 0xEE)
1. MSRs 0x200-0x20F (all variable MTRRs) ← 0 (MCPX 1.1 ROM 0xFFFFFEBC-0xFFFFFECA).
2. MSR 0x2FF (MTRR default type) ← operand 1, high half 0 (0xFFFFFECC-0xFFFFFED0). Every retail program and Cromwell pass
   **0x806**: MTRRs enabled, default type **write-back for the whole address space** (RAM, MMIO and flash).
3. CR0 CD and NW cleared: caches on (0xFFFFFED2-0xFFFFFEDA).
4. MCPX 1.1: a hash of the flash range 0xFFFFD400-0xFFFFFC80 (the FBL) is computed, using a stack at **0x8F000 in RAM**,
   and compared with 16 bytes at 0xFFFFFFA8. Equal: jump to **0xFFFFD400** (the FBL) with the ROM still visible
   (0xFFFFFEDD-0xFFFFFF86).
5. Not equal: PCI address 0x80000880 is selected, a far jump goes to 0xFFFFFFFA, where a byte 0x02 is written to 0xCFC
   (0:1:0 0x80 byte 0: the ROM is hidden) and execution runs off the top of the address space to **physical 0**, which is
   now RAM (0xFFFFFF8B-0xFFFFFF9A, 0xFFFFFFFA-0xFFFFFFFF). This is the "overflow trick": a program that wants its own
   code to run writes a jump there before it exits (Cromwell boot_rom/Xcodes.h:290-295: 0 ← 0xFC1000B8, 4 ← 0x90E0FFFF,
   "mov eax, 0xFFFC1000; jmp eax").

### 1.5 MCPX 1.0 compared
- The interpreter bytes (0xFFFFFE00-0xFFFFFEBB) and the exit's steps 1-3 are identical in both ROMs.
- After step 3, the 1.0 ROM decrypts the 2BL from flash to 0x90000 (work area at 0x8F000 in RAM), checks a dword at
  0x95FE4, and jumps to the address stored at 0x90000; on failure it takes the same step 5 (MCPX 1.0 ROM 0xFFFFFEDD-0xFFFFFF9F).
- So an X-code program behaves the same on both ROMs; only what runs after a successful check differs.
- Both ROMs use **RAM at 0x8F000** after the exit and fetch the overflow-trick jump from RAM at 0: the program must leave
  the DRAM working.

### 1.6 The decoy loader (flash 0x3FE00-0x3FFFF)
The top 512 bytes of the retail 4627 and 5838 images hold a loader (the same bytes in both; flash offsets below are in
5838.bin). A retail console never runs it: the MCPX ROM covers 0xFFFFFE00-0xFFFFFFFF. A console without a boot ROM
does (the MCPX X2 of debug kits, spec 01 §5). What it does if run:
1. **Entry**, laid out as the ROM's (§1.1): the reset vector jumps to 0xFFFFFFB8; GDT and IDT are both loaded from the
   descriptor at 0xFFFFFFF4 (limit 0x18, base 0xFFFFFFD8: null, flat code 0x08, flat data 0x10); protected mode on;
   far jump to 0x08:0xFFFFFE00 (0x3FFB8-0x3FFD5, 0x3FFF0).
2. DS, ES, SS ← 0x10; no stack is set. The program pointer starts at **0xFFFC0000**: bank offset 0, which in a retail
   image is the init table, not the X-code program at 0x80 (0x3FE00-0x3FE0A).
3. **Ops**: the ROM's 9-byte format and accumulator (§1.2), the ROM's operations under other op numbers, plus one more.
   Unknown ops are no-ops (0x3FE0F-0x3FEAF):

   | Operation | Decoy op | ROM op (§1.3) |
   |---|---|---|
   | memory read (no address mask) | 0x01 | 0x02 |
   | memory write | 0x09 | 0x03 |
   | PCI write (no 0x80000880 guard) | 0x11 | 0x04 |
   | PCI read | 0x04 | 0x05 |
   | AND-OR | 0x12 | 0x06 |
   | use result | 0x03 | 0x07 |
   | jump if not equal | 0x10 | 0x08 |
   | jump | 0x02 | 0x09 |
   | AND-OR, second register | 0x06 | 0x10 |
   | I/O write | 0x05 | 0x11 |
   | I/O read | 0x07 | 0x12 |
   | exit | 0xEE | 0xEE |
   | read port 0x80 operand-2 times into memory at the accumulator's address | 0x08 | — |
4. **Exit** (op 0xEE): MSRs 0x200-0x20F ← 0, MSR 0x2FF ← operand 1, CR0 CD and NW cleared: the ROM's exit steps 1-3
   (§1.4) (0x3FEB4-0x3FED2).
5. **Key schedule**: 256 bytes of state S at RAM 0x10000 ← 0..255, then mixed with 256 key bytes K from flash
   0x3FC80-0x3FD7F (the bytes just below the loader): for i = 0..255, j ← j + S[j] + K[i] (8-bit), swap S[i] and
   S[j]. RC4's key schedule adds S[i], not S[j] (0x3FED5-0x3FF04).
6. **Decryption**: 0x4000 bytes from flash 0x3A000 (0xFFFFA000) to RAM **0x400000**: for byte n, with i = n mod 256,
   out = in XOR S[i]; then S[i] and S[out] are swapped. This is not RC4's output step (0x3FF06-0x3FF29).
7. Jump to 0x400000, with no check (0x3FF2B-0x3FF30). 0x3FF32-0x3FFB7 is filler (NOPs).
8. 0x3FFFA-0x3FFFF hold the bytes of the ROM's final hide write (PCI 0x80000880 byte ← 2, ending `B0 02 EE`), as at
   the end of the real ROM; nothing jumps there.

Compared with the real ROM (§1.5): table at offset 0 instead of 0x80, renumbered ops, a key from flash instead of
16 bytes inside the ROM, an RC4-like cipher instead of RC4, 0x4000 bytes from 0x3A000 to 0x400000 instead of 0x6000
from 0x39E00 to 0x90000, and no check or hiding before the jump.

On an X2 console a retail image therefore does not boot: the loader runs the init table and the X-code program under
its own op numbers (seen 2026-11-10 on the green debug kit: the 4627 image ran into wild reads, the 5838 image hung
reading 0xFFFFFE3F).

## 2. The 5838 X-code program, step by step
The program runs from 0x80 to the exit at 0x0DAF (376 ops; the next free byte is 0x0DB8). The pad-calibration
breakpoints follow at 0x0DC0 (flash header 0x7C = 0xDC0, spec 21 §3.7). It is a 1.6-only program: it has no NV2A
revision or encoder test.

### 2.1 Control flow
The program is not executed in file order. Order of execution (5838 X-codes):
1. 0x0080-0x013D: bridge, chipset, SMBus set-up and the first two memory-interface writes (§2.2-§2.4).
2. 0x0146 jumps to 0x02A5: the EEPROM loop starts, counter = 0xC3 (§3).
3. EEPROM 0xC3 → strap; EEPROM 0xC2 → 0x10B8; then a jump **back** to 0x014F.
4. 0x014F-0x0293: the rest of the memory interface, the DRAM controller, the SMC version check, the GPU clock, PBUS
   0x1210-0x1264 (§2.4-§2.7).
5. 0x029C jumps to 0x02AE: EEPROM 0xC1 → 0x122C; EEPROM 0xC0 → 0x1214; then a jump to 0x069E.
6. 0x069E-0x0DAF in file order: pad defaults, DRAM initialisation, chipset, memory test, size decision, SMC report,
   final BARs, exit (§2.5-§2.10, §4).

### 2.2 Bridge, chipset and bus set-up (in order)
| # | Register | Value | Evidence |
|---|---|---|---|
| 1 | (no-op ops 0xF5, 0x80) | — | 5838 X-codes 0x0080, 0x0089 |
| 2 | PCI 0:1:0 0x84 (ACPI/PM I/O base) | 0x00008001 (I/O at 0x8000) | 0x0092 |
| 3 | PCI 0:1:0 0x04 (command) | 0x00000003 | 0x009B |
| 4 | I/O 0x8049 (byte) | 0x08 | 0x00A4 |
| 5 | I/O 0x80D9 (byte) | 0x00 | 0x00AD |
| 6 | I/O 0x8026 (byte) | 0x01 | 0x00B6 |
| 7 | PCI 0:30:0 0x4C | 0x00000001 | 0x00BF |
| 8 | PCI 0:30:0 0x18 (bus numbers) | 0x00010100 | 0x00C8 |
| 9 | PCI 0:0:0 0x84 (top of memory) | 0x07FFFFFF (128 MB, provisional) | 0x00D1 |
| 10 | PCI 0:30:0 0x20 (memory window) | 0x0FF00F00 | 0x00DA |
| 11 | PCI 0:30:0 0x24 (prefetch window) | 0xF7F0F000 | 0x00E3 |
| 12 | PCI 1:0:0 0x10 (NV2A register BAR) | 0x0F000000 | 0x00EC |
| 13 | PCI 1:0:0 0x14 | 0xF0000000 | 0x00F5 |
| 14 | PCI 1:0:0 0x04 | 0x00000007 | 0x00FE |
| 15 | PCI 0:30:0 0x04 | 0x00000007 | 0x0107 |
| 16 | PCI 0:1:1 0x04 (SMBus function command) | 0x00000001 | 0x0110 |
| 17 | PCI 0:1:1 0x14 (SMBus I/O base) | 0x0000C001 (I/O at 0xC000) | 0x0119 |
| 18 | PCI 0:1:1 0x18 | 0x0000C201 | 0x0122 |
| 19 | I/O 0xC200 (byte) | 0x70 | 0x012B |
- The SMBus is enabled **before** any memory-interface write, because the EEPROM loop (§3) runs between the first two
  memory-interface writes and the rest.
- 5838 writes the PM base only at 0:1:0 0x84. The 4627 program writes it only at 0:1:0 0x10 (4627 X-code 0x0092);
  Cerbios and Cromwell write both (§7).

### 2.3 NV2A memory interface (0x10B0-0x10E8) and strap
| # | Register | Value | Condition | Evidence |
|---|---|---|---|---|
| 20 | 0x10B0 | 0x01000010 | always | 5838 X-code 0x0134 |
| 21 | 0x10CC | 0x66660000 | always | 0x013D |
| 22 | 0x101000 (strap) | read-modify-write from EEPROM 0xC3 (§3.3) | always | 0x0359-0x03E0 |
| 23 | 0x10B8 | EEPROM 0xC2 in bytes 2 and 3, bytes 0-1 zero (§3.3) | always | 0x04EE-0x0500 |
| 24 | 0x10D4 | 0x0000000F | always (runs after #22-23) | 0x014F |
| 25 | 0x10B4 | 0x00000000 | always | 0x0158 |
| 26 | 0x10BC | 0x00005860 | always | 0x0161 |
| 27 | 0x10C4 | 0xAAAA0000 | always | 0x016A |
| 28 | 0x10C8 | 0x00007D67 | always | 0x0173 |
| 29 | 0x10D8 | 0x00000000 | always | 0x017C |
| 30 | 0x10DC | 0x00000000 | always | 0x0185 |
| 31 | 0x10E8 | 0x04000000 | always | 0x018E |
- Note: **0x10D4 is written** (0x0F). It sits in the file before the EEPROM loop and is jumped over on the first pass,
  so a straight read of the file looks as if it were skipped; it runs when the loop returns to 0x014F.

### 2.4 DRAM controller set-up (PFB)
| # | Register | Value | Evidence |
|---|---|---|---|
| 32 | 0x100200 | 0x03070103 | 5838 X-code 0x0197 |
| 33 | 0x100410 | 0x11000016 | 0x01A0 |
| 34 | 0x100330 | 0x84848888 | 0x01A9 |
| 35 | 0x10032C | 0xFFFFCFFF | 0x01B2 |
| 36 | 0x100328 | 0x00000001 | 0x01BB |
| 37 | 0x100338 | 0x000000DF | 0x01C4 |
- 0x100204 (the DRAM geometry) is not written here; it keeps its power-on value until the memory test (§4).

### 2.5 SMC version check (SMBus slave 0x10: address byte 0x20 write, 0x21 read)
| # | Action | Evidence |
|---|---|---|
| 38 | SMBus byte write: 0xC004 ← 0x20, 0xC008 ← 0x01 (command), 0xC006 ← 0x00 (data), 0xC002 ← 0x0A (start, byte data); poll 0xC000 until it reads exactly 0x10; 0xC000 ← 0x10 (clear) | 0x01CD-0x0203 |
| 39 | SMBus byte read: 0xC004 ← 0x21, 0xC008 ← 0x01, 0xC002 ← 0x0A; poll 0xC000 until 0x10; 0xC000 ← 0x10; read 0xC006 | 0x020C-0x0242 |
| 40 | If the byte read is **not 0x50** ('P'): PCI 0:0:3 0x6C ← 0x01000000. Either way the program goes on. | 0x024B-0x025D |
- Writing 0 to SMC register 1 restarts the SMC's version string, so the byte read is its first character. The same
  check is in 4627 (4627 X-codes 0x03BC-0x044C) and Cerbios; Cromwell reads the byte and ignores it (boot_rom/Xcodes.h:199-200).

### 2.6 GPU clock and PBUS defaults
| # | Register | Value | Evidence |
|---|---|---|---|
| 41 | 0x680500 (NVPLL) | 0x00011C01 | 5838 X-code 0x0266 |
| 42 | 0x68050C (PLL select) | 0x000A0400 | 0x026F |
| 43 | 0x1220 | 0 | 0x0278 |
| 44 | 0x1228 | 0 | 0x0281 |
| 45 | 0x1264 | 0 | 0x028A |
| 46 | 0x1210 | **0** | 0x0293 |
| 47-48 | 0x122C, 0x1214 | from EEPROM 0xC1 and 0xC0, each byte repeated in all four bytes (§3.3) | 0x0536-0x053F, 0x055A-0x0563 |
| 49 | 0x1248 | 0 (end of its use as scratch, §3.2) | 0x069E |
| 50 | 0x1230 | 0xFFFFFFFF | 0x06A7 |
| 51 | 0x1234 | 0xAAAAAAAA | 0x06B0 |
| 52 | 0x1238 | 0xAAAAAAAA | 0x06B9 |
| 53 | 0x123C | 0x8B8B8B8B | 0x06C2 |
| 54 | 0x1240 | 0xFFFFFFFF | 0x06CB |
| 55 | 0x1244 | 0x8B8B8B8B | 0x06D4 |
| 56 | 0x1248 | 0x8B8B8B8B | 0x06DD |
- The memory PLL (0x680504) is **not written** by 5838 (nor by 4627, Cerbios or Cromwell): the memory clock is the
  hardware's own.
- 0x122C, 0x1214 and 0x1230-0x1248 are the pad registers. These are the values in force while the DRAM is initialised
  and tested; the 2BL's calibration replaces nine of them (spec 21 §3.7). 0x124C and 0x1250 (#62-63) are not touched
  by the 2BL.

### 2.7 DRAM initialisation and refresh
| # | Register | Value | Evidence |
|---|---|---|---|
| 57 | 0x1002D4 | 0x00000001 | 5838 X-code 0x06E6 |
| 58 | 0x1002C4 | **0x00100000** | 0x06EF |
| 59 | 0x1002CC | **0x00100000** | 0x06F8 |
| 60 | 0x1002C0, then 0x1002C8 | 0x11, 0x11; then 0x32, 0x32; then 0x132, 0x132 (six writes, alternating) | 0x0701-0x072E |
| 61 | 0x1002D0 | 1, twice | 0x0737, 0x0740 |
| 62 | 0x100210 (refresh) | 0x80000000 | 0x0749 |
| 63 | 0x124C | 0xAA8BAA8B | 0x0752 |
| 64 | 0x1250 | **0x00005474** | 0x075B |
| 65 | 0x100228 | 0x081205FF | 0x0764 |
| 66 | 0x1218 | **0** | 0x076D |
- No delay and no poll between any of these writes.
- Cerbios's 1.6 path issues the 0x132 pair later, after #61-62 and after #67-73 (Cerbios X-codes 0x0DC1, 0x0DCA); the
  rest is the same. Both boot the 1.6 console.

### 2.8 Chipset (bridge and host function 3)
| # | Register | Value | Evidence |
|---|---|---|---|
| 67 | PCI 0:1:0 0x60 | read, OR 0x00000400, write | 5838 X-codes 0x0776-0x0788 |
| 68 | PCI 0:1:0 0x4C | 0x0000FDDE | 0x0791 |
| 69 | PCI 0:1:0 0x9C | 0x871CC707 | 0x079A |
| 70 | PCI 0:1:0 0xB4 | read, bits 8-11 set (AND 0xFFFFF0FF, OR 0x00000F00), write | 0x07A3-0x07B5 |
| 71 | PCI 0:0:3 0x340 | 0xF0F0C0C0 | 0x07BE |
| 72 | PCI 0:0:3 0x344 | **0x00000000** | 0x07C7 |
| 73 | PCI 0:0:3 0x35C | **0x00000000** | 0x07D0 |
| 74 | PCI 0:0:3 0x36C | 0x00230801, then 0x01230801 | 0x07D9, 0x07E2 |
| 75 | (two no-op jumps) | — | 0x07EB, 0x07F4 |

### 2.9 Memory test and size decision
§4. Writes: 0x100200, 0x100204, PCI 0:0:0 0x84, PCI 0:2:0 0x3C and 0:3:0 0x3C, test patterns in RAM, and the SMC
registers 0x13 and 0x12 (5838 X-codes 0x07FD-0x0D94).

### 2.10 Final BARs and exit
| # | Register | Value | Evidence |
|---|---|---|---|
| 90 | PCI 0:30:0 0x20 | 0xFDF0FD00 (window 0xFD000000-0xFDFFFFFF) | 5838 X-code 0x0D9D |
| 91 | PCI 1:0:0 0x10 | 0xFD000000 (NV2A registers move to 0xFD000000) | 0x0DA6 |
| 92 | exit | operand 1 = 0x806 | 0x0DAF |

### 2.11 What the program leaves for the next stage (A5)
- NV2A at 0xFD000000; the AGP window as #90; NV2A command 7.
- The memory configuration of §4.3 in 0x100200 / 0x100204 and PCI 0:0:0 0x84.
- **PCI 0:2:0 0x3C = the "large" mask, PCI 0:3:0 0x3C = the "faulty" mask** (low byte; §4). These are the USB
  controllers' interrupt-line registers used as scratch. Neither the FBL nor the 5838 2BL reads them; the kernel
  overwrites them (spec 23 §3.1 rows 7-8). Cerbios's loader reads them (spec 21 §3.7a step 5).
- SMBus 0xC006 holds the last data byte (the faulty mask); the SMC has registers 0x13 and 0x12 set (§4.4).
- Test patterns in RAM at the §4.1 addresses. Nothing at physical 0: the retail program does not use the overflow trick.
- From the exit: MTRRs all off except the default type 0x806 (all memory write-back), caches on.
- **The overflow-trick jump** (for a non-retail 2BL): only the exit makes the ROM jump to physical 0, so the two writes
  can be anywhere before the exit, provided the DRAM then keeps them: **after the DRAM initialisation (#57-62) and after
  the last write to 0x100200 / 0x100204 (§4.3)**, because a change of the DRAM geometry changes which cells an address
  reaches. Cromwell puts them after the final BAR writes, just before the exit (boot_rom/Xcodes.h:286-298); the ROM's
  own stack at 0x8F000 is used under the same final geometry.

## 3. The EEPROM memory parameters (A3)

### 3.1 The loop
- A counter lives in **PCI 0:3:0 0x3C** (configuration address 0x8000183C), the second USB controller's interrupt-line
  register, used as scratch: 0xC3 at the start (5838 X-code 0x02A5), then 0xC2, 0xC1, 0xC0. Each pass reads the EEPROM
  byte whose offset is the counter, then branches on the counter.
- The counter is reset to 0xC3 at the end (0x056C) and cleared to 0 by the memory test (0x0818).

### 3.2 One EEPROM read (5838 X-codes 0x02AE-0x0335)
1. 0xC004 ← 0xA9 (EEPROM, slave 0x54, read).
2. 0xC008 ← the counter (read back from PCI 0:3:0 0x3C; its low byte is the EEPROM offset).
3. 0xC000 ← 0x00. (This does not clear the status bits: the SMC transfers clear them by writing 0x10.)
4. 0xC002 ← 0x0A (start, byte-data protocol).
5. Read 0xC000 and park the value in **PCI 0:2:0 0x3C** (scratch). While bit 3 (busy) is set, read again.
6. If the parked status (low byte) is not exactly **0x10**, go back to step 1 (the whole read is repeated). No limit:
   a missing or failing EEPROM keeps the program in this loop for ever.
7. The data byte is 0xC006 (low byte). It is written to NV2A **0x1248** (a pad register, used as scratch).
- Byte reads only (protocol 0x0A); the 2BL uses word reads for its own EEPROM area (spec 21 §3.7).

### 3.3 What each byte becomes
| EEPROM offset | Order | Register written | Rule | Evidence |
|---|---|---|---|---|
| **0xC3** | 1st | NV2A **0x101000** (strap) | read the strap; AND **0xE1F3FFFF** (clears bits 18, 19 and 25-28); OR **0x80000000** plus: byte 0x42 → bits 18 and 19 (0x800C0000); byte 0x40 → bit 19 (0x80080000); byte 0x02 → bit 18 (0x80040000); any other value → neither; write back | 5838 X-codes 0x0359-0x03E0 |
| **0xC2** | 2nd | NV2A **0x10B8** | the byte repeated into all four bytes, then AND 0xFFFF0000: 0x10B8 = (b << 24) \| (b << 16) | 0x03FB-0x0500 |
| **0xC1** | 3rd | NV2A **0x122C** | b × 0x01010101 | 0x0536-0x053F |
| **0xC0** | 4th | NV2A **0x1214** | b × 0x01010101 | 0x055A-0x0563 |
- **Byte repetition** is done with the ops available: for each bit k (0-7) of the byte (tested in the value read back
  from 0x1248), 0x1248 is ORed with 0x01010100 << k (5838 X-codes 0x03FB-0x04CA tests, 0x057E-0x0695 ORs). The result is
  the byte in all four bytes of 0x1248.
- The strap's bit 31 set makes the written value override the hardware straps (inferred from all four programs setting
  it whenever they rewrite the strap). Bits 25-28 always end at 0 on a 1.6; the kernel then sets XboxHardwareInfo flag
  0x40 (bits 25-26 zero, spec 02 §5; §6.1).
- **0x10D4 and the rest of the memory interface (§2.3 #24-31) are written after 0xC3 and 0xC2 are applied; the DRAM
  controller (§2.4) after that; 0x122C and 0x1214 (from 0xC1, 0xC0) after the GPU clock and before the DRAM
  initialisation.**

### 3.4 Range, blank and failure
- No range check: every byte value is used as it is. A blank EEPROM (0xFF) gives: strap without bits 18/19, 0x10B8 =
  0xFFFF0000 (the 1.0-1.5 value), 0x122C = 0x1214 = 0xFFFFFFFF.
- A read that does not end with status 0x10 is repeated without limit (§3.2 step 6).

### 3.5 The EEPROM bytes
- The X-codes read **0xC0-0xC3** (byte reads); the 2BL reads **0xC4-0xF5** (25 word reads, spec 21 §3.7). Both lie past
  the user section (0x60-0xBF) and outside every checksummed section of spec 12 §3.
- Nothing in the 5838 FBL, 2BL or kernel writes them, except the kernel's whole-EEPROM setting (ExSaveNonVolatileSetting
  with index 0xFFFF writes all 256 bytes, spec 12 §3): restoring another console's EEPROM image that way overwrites them.
- What writes them at the factory, and their physical meaning (memory vendor or chip type for 0xC3, drive strengths for
  the others), is not in the code (§8).

## 4. Memory test and size decision (A4)
Same logic as the 4627 program (4627 X-codes 0x0671-0x0BE4) and Cerbios; spec 22 §6.3 describes it for 1.0-1.5.

### 4.1 Set-up (5838 X-codes 0x07FD-0x0818)
| # | Register | Value |
|---|---|---|
| 76 | 0x100200 | 0x03070103 |
| 77 | 0x100204 | **0x11448000** (the larger geometry, for the test) |
| 78 | PCI 0:2:0 0x3C ("large" mask) | 0 |
| 79 | PCI 0:3:0 0x3C ("faulty" mask, also the EEPROM counter) | 0 |
- PCI 0:0:0 0x84 is still 0x07FFFFFF (#9).

### 4.2 The eight lanes (5838 X-codes 0x0821-0x0C98)
Lane n (bit n of the masks) uses address A = 0x00555548, 0x00555558, 0x00555568, 0x00555578 (n = 0-3) or 0x04555548,
0x04555558, 0x04555568, 0x04555578 (n = 4-7), and its partner A + 0x02000000 (32 MB higher). For each lane in order:
1. A ← 0xAAAAAAAA; A + 32 MB ← 0x5A5A5A5A.
2. Read A + 32 MB. Not 0x5A5A5A5A → faulty bit n set (read-modify-write of 0:3:0 0x3C); next lane.
3. Read A. Still 0xAAAAAAAA (the partner write did not alias onto A) → large bit n set (0:2:0 0x3C).
4. A ← 0x55555555 and read back. Different → faulty bit n set.
- All accesses are uncached 32-bit cycles (caches are off during the X-codes).

### 4.3 Decision (5838 X-codes 0x0CA1-0x0D16)
1. 0x100200 ← **0x03070003**; PCI 0:0:0 0x84 ← **0x03FFFFFF** (64 MB) — always, first.
2. The large mask's low byte is written to 0xC006 and read back from there (0xC006 is used as scratch).
3. Then:
| Large mask | 0x100200 | 0x100204 | PCI 0:0:0 0x84 | Memory |
|---|---|---|---|---|
| 0x00 | 0x03070103 | **0x11338000** | 0x03FFFFFF | 64 MB |
| 0xFF | 0x03070103 | 0x11448000 (kept) | 0x07FFFFFF | 128 MB |
| anything else | 0x03070003 (kept) | 0x11448000 (kept) | 0x03FFFFFF | 64 MB |
- The faulty mask does not take part in the decision; it is only reported.
- The kernel takes its memory size from PCI 0:0:0 register 0x87 (the top byte of 0x84) (spec 23 §2.2 step 4); its
  graphics library reads 0x100200 / 0x100204 (spec 22 §6.3a).
- On the 1.0-1.5 consoles the result is the third row (large 0x0F, faulty 0xF0; spec 22 §6.3). Which row a 1.6 console
  takes depends on its chips; it is not shown by the code (§8). (OXOS's earlier note of 0x11338000 for the 1.6 is the
  first row.)

### 4.4 Report to the SMC (5838 X-codes 0x0D1F-0x0D94)
- SMBus byte write to the SMC: command **0x13** ← the large mask (the value left in 0xC006 by §4.3 step 2); poll 0xC000
  until 0x10; clear with 0x10.
- Then command **0x12** ← the faulty mask (0xC006 ← mask, 0xC008 ← 0x12, start); poll; clear.
- Measured on a 1.6 console (2026-11-07, spec/oxos_internal/28 §2): a program without these two writes is reset by
  the console about 0.4 s after power-on; with them it boots.
- No retail code reads SMC 0x12 or 0x13 back (none of the 5838 kernel's SMBus calls uses them). Cromwell sends fixed
  values 0x0F and 0xF0 (boot_rom/Xcodes.h:279-282, "We emulate Good memory result to PIC").

## 5. The FBL and the 2BL on a 1.6 (B)

### 5.1 The FBL (5838 FBL, flash 0x3D400, run at 0xFFFFD400)
- It writes **no hardware register** on its success path. Entry (0xFFFFD588): segments 0x10, stack 0x8F000, call of the
  loader routine 0xFFFFD6D7 with destination 0x3FA000; on success EBP ← 0x3FA000 and a jump to the 2BL's entry.
- On failure (0xFFFFD5B1): PCI 0:0:3 0x6C ← 0x01000000, interrupts off, halt. (The same write as the 2BL's halt path,
  spec 21 §2.4.)
- What it checks (0xFFFFD6D7): a digest over the flash header at 0xFFFFFD80 (0x80 bytes), over **flash 0x0000-0x0E04**
  (the MCPX header, the whole X-code program and the 0xDC0 breakpoints; start and length from header fields 0xFFFFFD80
  and 0xFFFFFD84) and over 0xFFFF9E00-0xFFFFFC80 (the encrypted 2BL and the FBL), checked against the signature at
  0xFFFFFC80. Then it derives the 2BL key from 16 bytes of the still-visible boot ROM (0xFFFFFF9C) and 16 bytes of
  flash (0xFFFFFDF0), decrypts 0x6000 bytes from 0xFFFF9E00 to 0x3FA000, and takes the entry from the dword at
  0x3FA000 + 0x35F8 (0x1BC in 5838).
- So the retail chain does not run with changed X-codes: the FBL would halt. A changed program can only boot through
  the overflow trick (§1.4 step 5) with a non-retail 2BL.
- It does not hide the boot ROM; the 2BL's first chipset write does (spec 21 §2 step 1).
- What the 2BL relies on from it: EBP = its current location; the MTRR/cache state of the exit (all write-back).

### 5.2 Hardware-revision tests in the 5838 2BL
| Test | Where | Effect |
|---|---|---|
| MCPX revision: PCI 0:1:0 0x08 read as a byte, ≥ 0xD1 | 5838 2BL 0x400661-0x400687 | PCI 0:1:0 0xC8 ← 0x00008F00 (spec 21 §2 step 12) |
- That is the only one. The 5838 2BL does **not** read the NV2A revision, the strap, the top of memory, PCI IDs or the
  memory-test masks. Its pad calibration (0x400A5F) **always** uses the EEPROM rows (0xC4-0xF5) and the 5838 breakpoints:
  it has no table method. The NV2A ≥ D3 test of spec 21 §3.7 is Cerbios's choice between two methods, not a retail test.
- Spec 21 §2 (chipset sequence) is confirmed for the 1.6 path without change. Spec 21 §3.7 is confirmed; additions are
  made there (the write window, the X-codes' EEPROM bytes).

### 5.3 Memory-controller, PFB, clock and MTRR writes by the 5838 2BL
| Write | Value | Evidence |
|---|---|---|
| MTRR default (0x2FF) | 0, then 0x800 (UC, enabled) | 5838 2BL 0x4001CF, 0x40021C |
| MTRR range 0 (0x200/0x201) | base 0 type 6 (WB), mask 0xF_F8000800: **128 MB write-back** | 0x4001DA-0x4001F3 |
| MTRR range 1 (0x202/0x203) | 0xFFF80005 (WP), mask 0xF_FFF80800 (512 KB) | 0x4001F5-0x40020A |
| MTRRs 0x204-0x20F | 0 | 0x40020C-0x40021A |
| PAT (0x277) | 0x0007010600070106 | 0x4002E4-0x4002F0 |
| NV2A 0x1220, 0x1228, nine pad registers | spec 21 §3.4-3.7 | 0x400A5F-0x400F8B |
- Nothing else: no write to 0x10B0-0x10E8, 0x100200-0x100410, 0x1002C0-0x1002D4, 0x100210, 0x680500-0x68050C, 0x1210,
  0x1218, 0x124C or 0x1250, and no repeat of an X-code value. The retail 2BL has no memory-interface repair.
- For an OXOS repair on the 1.6 (spec/oxos_internal/28): the values the 5838 X-codes leave, in their order, are
  0x10B0 0x01000010, 0x10CC 0x66660000, 0x10B8 (from EEPROM 0xC2, §3.3), 0x10D4 0x0000000F, 0x10B4 0, 0x10BC 0x00005860,
  0x10C4 0xAAAA0000, 0x10C8 0x00007D67, 0x10D8 0, 0x10DC 0, 0x10E8 0x04000000.

### 5.4 What the 2BL passes to the kernel
- The same as on 1.0-1.5 (spec 21, spec 01): the kernel is entered with the key block and the 2BL parameter area
  (0x80400004); nothing depends on the revision. The memory size is not passed: the kernel reads PCI 0:0:0 0x87 itself.

## 6. The kernel on a 1.6 (C)

### 6.1 Hardware-revision tests in the 5838 kernel
| Test | Where | Effect | 1.6 only? |
|---|---|---|---|
| Flash byte 0x78 (read at physical 0xFFF00078) ≥ 0x80 | 0x80054945 | Flags \|= 0x10 | the 1.6 image has 0x90 there, so the 1.6 gets 0x10; 4627 tests == 0x80 |
| Same byte == **0x90** | 0x80054950 | Flags \|= **0x400** (Xcalibur) | **1.6 only** (not in 4627) |
| Otherwise port 0x80CE bit 5 | 0x80054960 | Flags \|= 0x20 (Focus) | also 4627 |
| Strap 0x101000 bits 25-26 == 0 | 0x80054978 | Flags \|= 0x40 | also 4627; on a 1.6 always true, because the X-codes clear bits 25-28 (§3.3) |
| McpRevision (PCI 0:1:0 0x08) ≤ 0xB3 | 0x800549AC | Flags \|= 0x01 | also 4627; not set on a 1.6 (MCPX ≥ 0xD1, §5.2) |
| GpuRevision (NV2A 0 low byte) | 0x800549B3 | stored only; no code reads it back | — |
| Flags 0x400 | 0x800315F8 | encoder address 0xE0 and the Xcalibur tables of the AV blob (spec 17 §1.2) | **1.6 only** |
| Flags 0x400 | 0x80014899, 0x80014960, 0x80014B6A | AV-pack events: Xcalibur register 4 (4-byte write) instead of Focus / Conexant (spec 20 §3) | **1.6 only** |
| SMBus address (A & 0xF8) == 0xE0 | 0x80014743 | 4-byte transfers (spec 20 §1) | **1.6 only** |
| McpRevision == 0xA1 | 0x8002DC80 | NIC PHY: register 0x18 bit 8 cleared unless A1 | also 4627 (0x8002D7E5); not a 1.6 test |
| Flags & 0x09 == 0 | 0x8001984F | an extra set-up when interrupt vector 0x31 is connected | 5838 only (not in 4627); true on any MCPX ≥ 0xB4, so 1.1-1.6, not 1.6 only |
| Flags & 0x08 | 0x800630DF | AGP bridge 0x26 bit 10 (spec 23 §3.1 row 13) | bit 8 is never set by the detection |
- On a 1.6 with the retail image the flags are therefore **0x450** (0x10 | 0x40 | 0x400), plus nothing else from the
  detection.
- No other code reads the NV2A revision, the MCPX revision, the strap or an SMC version. The SMC call sites of the kernel
  are the same as 4627's (constant commands 0x02, 0x03, 0x04, 0x07, 0x08, 0x0C, 0x0D, 0x0E, 0x11, 0x19, 0x1A, 0x1B,
  plus the generic SMC write export); no 1.6-specific SMC traffic (fan, LED, challenge) exists in the kernel.

### 6.2 HAL phase 0 and memory-manager start-up
- Nothing in HAL phase 0, the PCI set-up or MmInitSystem depends on the revision (5838 kernel 0x80061D38-0x80061D90,
  0x80062E24, 0x8001EA91). The writes are spec 23 §2.1-2.2 and §3.1, the same on every console.
- The window between the PIT set-up and the PCI set-up (5838 kernel 0x80061D70-0x80061D86), in order:
  1. PIT (0x8006319C): interrupts masked around it; 0x43 ← 0x34; 0x40 ← 0x65, then 0x04 (count 0x0465).
  2. The pulse (0x8003183D): port 0x80CF ← 5, stall 5 µs, ← 4, stall 5 µs (spec 23 §2.0); the stall is a calibrated spin.
  3. CMOS (0x800319CC): interrupts masked; index 0x70 ← 0x0B, read 0x71, keep bit 0, set bit 1, write 0x71; index 0x0C,
     read 0x71 (clears pending RTC flags); the flag at 0x80036EED ← 1; interrupts restored.
  4. The pulse again.
  5. The PCI set-up (spec 23 §3.1).
  The 8259s were programmed with every line masked (spec 23 §2.1 step 3), so no device interrupt is delivered in this
  window. It uses only port I/O, the stack and the kernel's code and data in RAM. A stop there on a 1.6 points at RAM
  (code, data or stack read back wrong) or at the stall loop, not at a missing hardware step.
- No PFB, 0x10xx, 0x12xx or PLL write anywhere in the kernel's start-up (spec 22 §6.3a, spec 23 §4).

### 6.3 Video encoder at boot (Xcalibur)
- **Detection:** no bus probe and no time-out. The encoder is chosen from the flash byte 0x78 (§6.1): 0x90 → Xcalibur,
  SMBus address 0xE0. The choice is made once at cold boot (flags) and the table loader (0x800315D4) picks the AV-blob
  descriptor at every boot (spec 17 §1.2).
- **Before the first AvSetDisplayMode:** nothing is sent to the encoder — no reset, no power-up, no register write. The
  5838 X-codes do not touch the encoder either (they send nothing to SMBus 0x8A, 0xD4 or 0xE0). The first Xcalibur
  traffic is the boot animation's first `AvSetDisplayMode` (spec 22 §8.1), whose step 0 reads registers 4 and 0 and
  switches the outputs off (spec 20 §3). An AV-pack event before that writes register 4 ← 0x0F (spec 20 §3).
- All Xcalibur accesses use 32-bit (4-byte block) SMBus transfers (spec 20 §1). No Xcalibur register is polled.
- **First mode:** the boot animation's mode word from its mode list (spec 22 §8.2-8.3, e.g. 0x04010101 for NTSC
  composite); the Xcalibur row via the row maps (spec 20 §6.1); the NV2A and CRTC rows of spec 22 §3.10-3.11; clocking
  of spec 22 §5.2. Picture on with option 9 (register 4 ← mode bits 24-27, spec 22 §14).
- **Clocks:** the kernel never writes NVPLL (0x680500) or MPLL (0x680504). Its graphics library reads NVPLL, MPLL and
  VPLL (0x680508) to compute clock rates (5838 kernel 0x80047A53-0x80047B7D); `AvSetDisplayMode` ORs 0x10020000 into
  0x68050C (0x800313D2). The GPU core clock on a 1.6 is the X-codes' NVPLL 0x00011C01 with select 0x000A0400 (§2.6),
  the same as on 1.0-1.5.

### 6.4 SMC, USB, IDE, audio
- SMC: no revision-dependent code (§6.1). The SMC challenge is the 2BL's (spec 21 §2), the same on 1.6.
- USB, IDE, NIC, audio: the PCI set-up is the same on every console (spec 23 §3.1); the only revision tests near them
  are the NIC PHY's MCPX == 0xA1 test and the vector-0x31 set-up (§6.1), neither specific to the 1.6.

## 7. Comparison of the X-code programs (A6)
Final value of every register a program writes. 4627 = Complex 4627 for an NV2A that is not A1 (the A1 branch: spec 22
§6.3). Cerbios = its 1.6 path (taken when the SMBus read of address 0xE1 answers). Cromwell = boot_rom/Xcodes.h with
MCPXREVD5. "EE" = from the EEPROM (§3.3). Values in hex. Registers equal in all four are listed at the end.

| Register | 4627 | 5838 (1.6) | Cerbios 1.6 path | Cromwell MCPXREVD5 | Cromwell source |
|---|---|---|---|---|---|
| ops 0xF5, 0x80 (no-ops) | yes | yes | no | no | — |
| PCI 0:1:0 0x84 (PM base) | not written | 8001 | 8001 | 8001 | Xcodes.h:27 |
| PCI 0:1:0 0x10 (PM base) | 8001 | not written | 8001 | 8001 | Xcodes.h:28 |
| SMBus enable (0:1:1 0x04/0x14/0x18, I/O 0xC200) | after the memory interface | **before** the memory interface | before | after | Xcodes.h:96-99 |
| Encoder SMBus traffic in the X-codes | Conexant writes | **none** | a read of 0xE1 (Xcalibur probe) | Conexant, Focus, Xcalibur writes | Xcodes.h:110-178 |
| 0x10B0 | 07633461 | **01000010** | 01000010 | 01000010 | Xcodes.h:47 |
| 0x101000 (strap) | not written (A1 only) | **from EE 0xC3**, mask E1F3FFFF | from EE 0xC3 | from the hardware strap bits 18/19: 00 → 80000000 set; 11 → 860C0000; mixed → 820C0000 (mask E1F3FFFF) | Xcodes.h:52-78 |
| 0x10B8 | FFFF0000 | **from EE 0xC2** (b·0x01010000) | from EE 0xC2 | EEEE0000 / FFFF0000 / 11110000 by the same strap bits | Xcodes.h:64-79 |
| 0x10D4 | 9 | **F** | F | 9 | Xcodes.h:80 |
| 0x10BC | 5866 | **5860** | 5860 | 5866 | Xcodes.h:82 |
| 0x10C4 | 0351C858 | **AAAA0000** | AAAA0000 | 0351C858 | Xcodes.h:83 |
| 0x10C8 | 30007D67 | **00007D67** | 00007D67 | 30007D67 | Xcodes.h:84 |
| 0x10DC | A0423635 | **0** | 0 | A0423635 | Xcodes.h:86 |
| 0x10E8 | 0C6558C6 | **04000000** | 04000000 | 0C6558C6 | Xcodes.h:87 |
| SMC version check (0:0:3 0x6C ← 01000000 unless 'P') | yes | yes | yes | no (byte read, ignored) | Xcodes.h:199-200 |
| 0x1210 | 10 | **0** | 0 | 10 | Xcodes.h:207 |
| 0x1214 (before calibration) | 10101010 or 12121212 (strap 18/19) | **from EE 0xC0** | from EE 0xC0 | 48480848 or 09090909 (strap 25/26) | Xcodes.h:208-220 |
| 0x122C (before calibration) | AAAAAAAA | **from EE 0xC1** | from EE 0xC1 | 88888888 or AAAAAAAA | Xcodes.h:208-220 |
| 0x1230 | AAAAAAAA | FFFFFFFF | FFFFFFFF | FFFFFFFF | Xcodes.h:221 |
| 0x1240 | 8B8B8B8B | FFFFFFFF | FFFFFFFF | FFFFFFFF | Xcodes.h:225 |
| 0x1002C4, 0x1002CC | 00100042 | **00100000** | 00100000 | 00100042 | Xcodes.h:229-230 |
| order of the 0x132 mode-register pair | before 0x1002D0 | before 0x1002D0 | after 0x1002D0, 0x100210 and 0:0:3 0x340-0x35C | before 0x1002D0 | Xcodes.h:235-238 |
| 0x1250 | AA8B | **5474** | 5474 | AA8B | Xcodes.h:241 |
| 0x1218 | 00010000 | **0** | 0 | 00010000 | Xcodes.h:244 |
| PCI 0:0:3 0x344 | 00C00000 | **0** | 0 | 00C00000 | Xcodes.h:257 |
| PCI 0:0:3 0x35C | 04070000 | **0** | 0 | 04070000 | Xcodes.h:258 |
| Memory test, size decision (§4) | yes | yes | yes | **no**: 0x100200 03070103, 0x100204 11448000, 0:0:0 0x84 07FFFFFF kept | Xcodes.h:263-265 |
| PCI 0:2:0 / 0:3:0 0x3C after the X-codes | masks | masks (§2.11) | masks | 0:2:0 ← 0 only | Xcodes.h:265 |
| SMC 0x13 / 0x12 | masks | masks | masks | fixed 0F / F0 | Xcodes.h:279-282 |
| Overflow-trick jump | no | no | yes (own code) | yes | Xcodes.h:294-295 |
| Exit operand | 806 | 806 | 806 | 806 | Xcodes.h:298 |
- 4627 on an A1 NV2A also differs in 0x10B0 (07633451), 0x10CC (0), 0x10D4 (5), the strap (803D4401) and NVPLL
  (00011701) (spec 22 §6.3).
- **Equal in all four:** PCI 0:1:0 0x04, I/O 0x8049/0x80D9/0x8026, 0:30:0 0x4C/0x18/0x20/0x24/0x04, 0:0:0 0x84
  (provisional 07FFFFFF), 1:0:0 0x10/0x14/0x04, 0:1:1 0x04/0x14/0x18, I/O 0xC200, 0x10CC 66660000, 0x10B4 0,
  0x10D8 0, PFB 0x100200 03070103 (before the test), 0x100410 11000016, 0x100330 84848888, 0x10032C FFFFCFFF,
  0x100328 1, 0x100338 DF, NVPLL 00011C01, 0x68050C 000A0400, 0x1220 0, 0x1228 0, 0x1264 0, 0x1234/0x1238 AAAAAAAA,
  0x123C/0x1244/0x1248 8B8B8B8B, 0x1002D4 1, the 0x1002C0/0x1002C8 values 11, 32, 132, 0x1002D0 1 twice, 0x100210
  80000000, 0x124C AA8BAA8B, 0x100228 081205FF, 0:1:0 0x60 \|= 400, 0x4C FDDE, 0x9C 871CC707, 0xB4 bits 8-11, 0:0:3
  0x340 F0F0C0C0, 0x36C 00230801 then 01230801, final 0:30:0 0x20 FDF0FD00 and 1:0:0 0x10 FD000000.
- **Flash header 0x00-0x7F:** 5838 and Cromwell MCPXREVD5 are identical byte for byte (Cromwell
  boot_rom/2bBootStartup.S:63-105, 0x1C-0x2B = C8B4588A 00100000 00050AA7 F0000000, 0x78 = 0x16CE0090, 0x7C = 0xDC0).
  Cerbios uses the 1.0-1.5 values at 0x1C-0x2B on every console and 0x78 = 0x1F480060 (spec 21 §4).
- **What Cromwell gets right for the 1.6:** the header, 0x10B0, everything in the "equal" list, the exit, the overflow
  trick. **What it lacks or has wrong:** the EEPROM parameters (strap, 0x10B8, 0x122C, 0x1214), 0x10D4, 0x10BC, 0x10C4,
  0x10C8, 0x10DC, 0x10E8, 0x1210, 0x1218, 0x1250, 0x1002C4/0x1002CC, 0:0:3 0x344/0x35C, the SMC version check, the
  memory test and size decision (its 2BL forces 128 MB again: boot_rom/2bBootStartBios.c:120-123), the real masks to the
  SMC; and its encoder writes and late SMBus enable are not what the 1.6 retail program does.

## 8. Open
- The 1.6 console's own EEPROM bytes 0xC0-0xC3, and which row of §4.3 it takes (its masks). Both can be read on the
  console (EEPROM dump; PCI 0:2:0 / 0:3:0 0x3C at 2BL entry after the 5838 X-codes).
- Physical meaning of EEPROM 0xC3 (values 0x42, 0x40, 0x02) and of strap bits 18/19; of 0x10B8's two bytes; of
  0x1002C4/0x1002CC (0x00100042 against 0x00100000); of 0x10D4 0xF against 9.
- The ops 0xF5 and 0x80 at 0x80/0x89 of the retail programs: no-ops in both ROMs; their purpose (and operands 0x00081000,
  1, 0x114, 0x228) is not shown by the code. 0x114 equals the value the kernel writes to PCI 0:0:0 0x48 and 1:0:0 0x4C
  (spec 23 §3.1); this may be a coincidence.
- What the SMC does with registers 0x12 / 0x13 (nothing in the BIOS or kernel reads them back; the console resets
  without the writes, §4.4).
- The meaning of PCI 0:0:3 0x6C bit 24 (written when the SMC's version does not start with 'P', and by the halt paths).
- The X-code EEPROM read starts a transfer without clearing the status register (§3.2 step 3); whether the MCPX clears
  status bit 4 itself at the start, or the program relies on the busy bit being set at once, is not shown.
- What the extra set-up at 5838 kernel 0x80019848 (vector 0x31, MCPX ≥ 0xB4) does.
- Who writes EEPROM 0xC0-0xF5 at the factory.
