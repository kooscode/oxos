# 15. NV2A pad calibration in the OXOS 2BL

Implemented 2026-10-26; since 2026-11-20 it follows the last retail 2BLs and their tables: the 5713 2BL on consoles 1.0-1.5, the 5838 2BL on the 1.6.
Behaviour: spec/stock/21 §3 (§3.7a says what OXOS implements). Code: `BlPadCalibrate` and `BlPadCalibrateFromEeprom` in kernel/boot/bl.cpp.

## 1. Where and which method
- `BlMain` follows the retail 2BL's order (spec/stock/21 §2, §3.1): device 1 0x80 byte ← 2, SMC challenge (with SMC 0x01 ← 0), device 1 0x8C ← 0x40000000,
  the pad calibration, `BlSetUpChipset` (device 1 0x54, host 0x64, the host 0x6C LDT toggle, host 0x80, then the revision ≥ D1 write), front LED, network
  download (netboot builds), Inflate. Not done: the retail 0xCC fill of 0x80090000.
- **One method per image**, as the retail images: the 1.0-1.5 images use the table method (`BlPadCalibrate`, as the 5713 2BL), the 1.6 images (MCPXREVD5)
  the EEPROM method (`BlPadCalibrateFromEeprom`, as the 5838 2BL). No revision, memory size or PCI test chooses between them; an image built for one console
  family does not boot on the other anyway (its X-codes differ).
- Both use the later retail arithmetic (spec/stock/21 §3.4-3.6): four groups of readings (A, B, A, B) with the steadier of each pair kept, a signed division
  below the second breakpoint, a weight clamped to 1.0 above the top one, all nine registers written in the write window of §3.6a (inline assembly: `wbinvd`,
  a dry pass, the storing pass, interrupts off).
- **Row set (table method):** from the board strap bits 19:18 in NV2A 0x101000, mapped as the retail X-codes map them (spec/stock/21 §3.3 step 4): 00 → set 2,
  one bit → set 3, both → set 1. The retail 2BL reads the field the retail X-codes write into bits 26:25; OXOS's 1.0-1.5 X-codes (Cromwell's) do not write it,
  so the bits are read directly. The outcome is the same on every console.

## 2. Tables
- **1.0-1.5 images:** the retail 4817-5713 table (0x129 bytes: breakpoints, three row sets; spec/stock/21 §3.9), byte for byte as the four retail images
  carry it, at flash offset 0xC90 (header 0x7C).
- **1.6 images:** the 5838 breakpoints (12 bytes, spec/stock/21 §3.9) at 0xECB, after the longer 1.6 X-code program; the rows come from the EEPROM.
- Both from kernel/boot/padcal_table.inc, placed by kernel/boot/xcodes.S and read through the 16 MB flash window. Like the video encoder tables, they are
  functional hardware constants (dmca_compliance.md §5).
- A table whose breakpoints do not increase, or an EEPROM that does not answer, makes the 2BL return before touching a register, so the X-codes' values stay
  (POST 0x33 / 0x34).
- Before 2026-11-20 the 1.0-1.5 table had a different set 3 (for upgraded memory, chosen by the top of memory) and the 4627 arithmetic. No retail BIOS
  has such a set: the retail third set is a copy of set 1 and follows the straps, and a 128 MB-capable BIOS (iND-BiOS 5003) carries the retail table
  unchanged.

## 3. Diagnostics and checks
- The 2BL leaves a record at physical 0x2300: 'PADC' (table method), 'PADE' (EEPROM method) or 'PADN' (not calibrated), A, B, the strap, the nine values. The
  kernel copies it before the memory manager starts; the debug log prints it ("nv pads: 2BL calibration ...") and the nine registers after the memory manager
  and at the title launcher.
- tools/padcal_check.py runs a retail routine and OXOS's in a CPU emulator with the same inputs and compares their NV2A writes: `5713` (the table method,
  random readings and straps as the retail X-codes leave them) and `5838` (the EEPROM method, random readings and EEPROM). 2026-11-20: 2000 cases each, no
  difference.

## 4. Results
- **No-DVD 1.4 console (2026-10, the 4627 arithmetic):** calibration ran on every boot (A 0x5E-0x63, B 0x5D-0x61; values in spec/stock/21 §3.8; the first
  row set, the same bytes in every table). The netlog build loaded the dashboard 3/3.
- **With the retail 5713 arithmetic** the eight A registers stay the same on that console; 0x1214 becomes 0x01010101 instead of 0x09090909, because its B is
  above the top B breakpoint, where the 4627 weight is 1/2^20 and the later one 1.0 (spec/stock/21 §3.8). 0x01010101 is what the 5101-5713 retail BIOSes
  write there. Booted 2026-11-20 (serial build in the Modxo's flash): 3 of 3 cold boots to the dashboard, the table method each time (A 0x61-0x62,
  B 0x62-0x63, strap 0x861D4401, set 1), 0x122C/0x1234/0x1238 0x99999999, 0x123C/0x1244/0x1248 0x8E8E8E8E, 0x1230/0x1240 0xFFFFFFFF, 0x1214 0x01010101,
  read back unchanged after the memory manager and at the title launcher.
- **1.6 console:** the EEPROM method reads all 25 words and calibrates (POST 0x32; spec 28). Unchanged by the 2026-11-20 switch apart from the method now
  being chosen by the image.
