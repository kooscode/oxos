# 28. The 1.6 console: bring-up status

OXOS addition. Started 2026-11-07 on a 1.6 console (NV2A revision D3, MCPX D5, Xcalibur encoder) with an RP2040
Modxo on a rebuilt LPC header (LFRAME on the Pico's GPIO 5, two grounds, short wires: longer wires gave corrupted
LPC cycles and FRAG even with Cerbios). Images: `make bios MCPX16=1` (separate 1.6 build; one hybrid image for
1.0-1.6 is the plan after it, §4.5 step 6).

## 1. Status (2026-11-07)
The 1.6 boots OXOS to the dashboard from OXOS's own code, 3 of 3 cold boots (POST 0x20 → 0x32 → 0x23 → 0x21 → 0x22,
then the kernel to idle 0x30):
- **X-codes:** `kernel/boot/xcodes_1_6.inc`, written from spec/stock/24 on the layout of Cromwell's table. The
  1.0-1.5 images keep Cromwell's `Xcodes.h` unchanged. The program, with its write checks, is 406 X-codes and ends at
  0xEC6. The pad table moved to 0xECB-0xFFF (header 0x7C).
- **2BL:** the EEPROM pad calibration (spec/stock/21 §3.7); the X-code check list and the memory interface repair
  hold the 1.6 values (0x10D4 0x0F included, 0x10B8 left alone).
- **Checks:**
  - A scratch X-code simulator gives the same final register values as the 5838 program for this console's EEPROM
    bytes and for 300 random ones, with the same register write order. The differences: the memory test (start.S
    makes it), the overflow-trick jump, and the lane masks.
  - On the console, the 2BL's snapshot of the memory set-up (serial builds, log lines `xcode snapshot:`) reads the
    same as with the 5838 X-codes, register for register, except the lane masks.

## 2. Measured on this console
- EEPROM 0xC0-0xC3: 0x68, 0xAA, 0xDD, 0x42. So 0x1214 = 0x68686868, 0x122C = 0xAAAAAAAA, 0x10B8 = 0xDDDD0000, strap
  0x801D4401 (bits 18 and 19 set).
- Memory test: large mask 0x0F, faulty mask 0xF0 → the third row of spec/stock/24 §4.3 (0x100200 0x03070003, 0x100204
  0x11448000, 64 MB), the same result as on 1.0-1.5. With the 5838 X-codes, OXOS's start.S then tests again (the
  geometry is still 0x11448000) and comes to the same result.
- PFB 0x1002C4 and 0x1002CC read back 0x00100042 after a write of 0x00100000, with the 5838 X-codes and with OXOS's.
- **The SMC report is required:** without the writes of SMC registers 0x13 and 0x12 (spec/stock/24 §4.4) the console
  resets about 0.4 s after power-on, every time (the 2BL had reached POST 0x23). OXOS sends Cromwell's fixed values
  0x0F and 0xF0 from the X-codes (the memory test comes later, in start.S); they equal this console's result.
- Before (Cromwell's 1.6 values): the RAM was unreliable (POST 0xE1, or the kernel stopping in HAL phase 0), because
  Cromwell's table sets the memory up as on 1.0-1.5. Writing the 1.6 interface values from the 2BL on top of it made it
  worse (the 2BL hung): they belong with the rest of the X-code set-up.

## 3. Open
- §4 rows not done yet: G7/B5 (real lane masks to the SMC instead of the fixed values), H3 (the 1.6 power-on value of
  0x10B0), K1 (kernel flag 0x10 on a 1.6), B3 (128 MB MTRR), the cold/warm boot loop of spec 27 §3 on the 1.6.
- The check list's masks for PCI 0:1:0 0x04 (reads 0x000F, written 3) and 1:0:0 0x14 (reads 0xF0000008, bit 3 is the
  prefetchable flag) report two settings as "differ" on every boot.

## 4. Gap list (2026-11-07)
Spec/stock/24 (the retail 1.6 path) compared with OXOS's code: `kernel/boot/xcodes.S` (MCPXREVD5 = Cromwell's
boot_rom/Xcodes.h), `tools/harden_xcodes.py`, `kernel/boot/start.S`, `kernel/boot/bl.cpp` (including the uncommitted
1.6 repair table), `kernel/src/hardware/`, `kernel/src/memory/init.cpp`. Each row: what OXOS must do, where, and how
the hardware side can check it. "Snapshot" = the start.S entry snapshot (spec 27 §4, log line `xcodes at entry`),
extended as listed in G0.

### 4.1 X-codes (`kernel/boot/xcodes.S`, MCPXREVD5)
| # | Gap | What OXOS must do | Check |
|---|---|---|---|
| G1 | The program is Cromwell's 1.0-1.5 set-up with one 1.6 value (0x10B0) | An own 1.6 program written from spec 24 §2-§4, in that order: bridge and SMBus set-up first (§2.2), 0x10B0/0x10CC, the EEPROM loop, the rest of the interface, DRAM controller, SMC version check, GPU clock, PBUS defaults, DRAM initialisation, chipset, memory test, size decision, SMC report, final BARs | Snapshot equals the reference snapshot taken with the 5838 bytes (G0) |
| G2 | No EEPROM parameters | Read EEPROM 0xC3, 0xC2, 0xC1, 0xC0 (byte reads, SMBus 0xA9) and apply spec 24 §3.3: strap 0x101000 (mask 0xE1F3FFFF, OR 0x80000000 and the 0xC3 bits), 0x10B8 = b·0x01010000, 0x122C and 0x1214 = b·0x01010101; strap and 0x10B8 before 0x10D4-0x10E8, 0x122C/0x1214 before the DRAM initialisation | Log EEPROM 0xC0-0xC3 (2BL or kernel) and 0x101000, 0x10B8; 0x122C/0x1214 read at 2BL entry, **before** the pad calibration |
| G3 | Interface and PBUS values differ | 0x10D4 0x0F, 0x10BC 0x5860, 0x10C4 0xAAAA0000, 0x10C8 0x7D67, 0x10DC 0, 0x10E8 0x04000000, 0x1210 0, 0x1218 0, 0x1250 0x5474, 0x1002C4/0x1002CC 0x00100000, host 0:0:3 0x344 0 and 0x35C 0 (spec 24 §7) | Snapshot / XCHK record (B2) |
| G4 | SMBus enabled after the memory interface; encoder writes for Conexant, Focus and Xcalibur | Enable the SMBus (0:1:1 0x04/0x14/0x18, I/O 0xC200) before 0x10B0; no encoder traffic in the 1.6 program (spec 24 §2.2, §6.3) | Modxo LPC log: no SMBus 0x8A/0xD4/0xE0 cycles before the kernel |
| G5 | No SMC version check | SMC 0x01 ← 0, read 0x01; not 'P' → 0:0:3 0x6C ← 0x01000000 (spec 24 §2.5) | Log the byte read (expected 'P') |
| G6 | PM base | Write 0:1:0 0x84 ← 0x8001 (retail 1.6). The extra 0x10 write Cromwell and Cerbios make is harmless (Cerbios boots this console) | — |
| G7 | No memory test in the X-codes (start.S does it) | Either the retail test and decision in the X-codes (spec 24 §4) with the SMC report (0x13 large, 0x12 faulty), or keep start.S's (same rule, spec 22 §6.3) and send the report from the 2BL. The X-codes must then leave 0x100204 = 0x11448000 so start.S runs it | Serial `ram:` line: 0x100200, 0x100204, host 0x84; log both masks |
| G8 | Overflow-trick jump | Keep the two writes (0 ← 0xFC1000B8, 4 ← 0x90E0FFFF) after the DRAM initialisation and after the last 0x100200/0x100204 write, i.e. after the size decision when the X-codes make it, then exit 0x806 (spec 24 §1.4, §2.11) | POST 0x20 (2BL reached) on every boot |
| G9 | Layout | The program plus the pad table must fit below 0x1000: header 0x7C names the table (0xDC0 in 5838; the temporary image moved it to 0xE00). Byte 0x78 (0x16CE0090) stays: the retail kernel reads it (spec 24 §6.1) | mkbios.py / build check that the program ends below the table |

### 4.2 Write checks (`tools/harden_xcodes.py`, spec 27)
| # | Gap | What OXOS must do | Check |
|---|---|---|---|
| H1 | LIMIT is 0xC90 for both banks | Use the table offset of the image (0x7C) as the limit | Build fails if the program would overlap the table |
| H2 | Space | A 5838-shaped program is 376 ops (+2 pokes, ends near 0xDCA); one retry on 17 registers adds 68 ops and passes 0x1000. Options: the memory test in start.S (saves about 130 ops), drop the retail no-ops (4 ops) | Build output: end offset |
| H3 | The GPU wait uses the 1.0-1.5 value of 0x10B0 | For MCPXREVD5 wait for 0x01000010 (the 1.6 power-on value of 0x10B0 is not measured; only the written value ends the wait, spec 27 §2) | XCHK record at 2BL entry |
| H4 | Only plain writes (op 0x03) are checked | The EEPROM-derived writes (op 0x07: strap, 0x10B8, 0x122C, 0x1214) need their own read-back check written into the program, or none. 0x1248 is scratch during the EEPROM loop: never check it there | Snapshot |
| H5 | Jumps | The 1.6 loop jumps backwards and into the middle of the program (spec 24 §2.1); the tool already re-targets every jump, keep it so | Emulator run of the hardened bank (same writes as before hardening) |

### 4.3 2BL (`kernel/boot/start.S`, `kernel/boot/bl.cpp`)
| # | Gap | What OXOS must do | Check |
|---|---|---|---|
| B1 | 1.6 repair table (uncommitted) lacks 0x10D4 and 0x10B8 | If a repair stays: 0x10B0, 0x10CC, 0x10B8 (the value read at 2BL entry, or EEPROM 0xC2), 0x10D4 0x0F, 0x10B4, 0x10BC, 0x10C4, 0x10C8, 0x10D8, 0x10DC, 0x10E8 in that order (spec 24 §5.3). The retail 2BL repairs nothing: once G1 and the write checks hold, the 1.6 repair can go | XCHK at 0x2600 empty |
| B2 | `BlXcodeValues` holds Cromwell's values | A 1.6 list (spec 24 §7, 5838 column; EEPROM-derived registers left out or compared with the logged EEPROM bytes) | XCHK record lists only real losses |
| B3 | MTRR range 0 is 64 MB | 5838's 2BL makes it 128 MB (spec 24 §5.3); matters only on 128 MB consoles | — |
| B4 | Pad method choice | Nothing to change: NV2A ≥ D3 and not "upgraded" → EEPROM method. With the retail decision "upgraded" cannot happen: host 0x84 is 128 MB only when the large mask is 0xFF, never with 0x0F/0xF0 | POST 0x32 |
| B5 | SMC 0x13/0x12 | If the memory test stays in start.S: send the two masks to the SMC from the 2BL (retail sends them from the X-codes; Cromwell sends 0x0F/0xF0) | — (nothing reads them back; spec 24 §8) |

### 4.4 Kernel (`kernel/src/hardware/init.cpp`, `video.cpp`, `memory/init.cpp`)
| # | Gap | What OXOS must do | Check |
|---|---|---|---|
| K1 | Flags on a 1.6 | The retail kernel sets 0x10 when flash byte 0x78 ≥ 0x80 and 0x400 when it is 0x90; OXOS tests == 0x80 and probes the encoder, so a 1.6 gets 0x440, not 0x450. Set 0x10 together with 0x400 (or apply the 5838 byte rule; the MCPX16 image has 0x90 at 0x78) | Log XboxHardwareInfo.Flags: 0x450 |
| K2 | Nothing else is 1.6-specific | HAL phase 0, PCI, MmInit and SMC need no 1.6 change (spec 24 §6.1-6.2). The stop between the PIT and the PCI set-up seen with Cromwell's X-codes is a RAM symptom, not a missing step | — |
| K3 | MmInitSystem fallback (0x100200 bit 8, host 0x84) | Keep; with retail sizing it changes nothing (0x11338000 is excluded; the mixed row has bit 8 clear) | Serial `ram:` line before/after equal |
| K4 | Encoder detection by probe | Keep (OXOS's image is not the retail one); the retail kernel sends nothing to the Xcalibur before the first mode set either (spec 24 §6.3) | Serial `av:` line: encoder 0xE0 |

### 4.5 Implementation plan (in order)
1. **Replace the 5838 X-code bytes with OXOS's own 1.6 program** (G1-G6, G8, G9), written in `kernel/boot/xcodes.S`
   for MCPXREVD5 from spec 24 (the Cromwell include stays for 1.0-1.5). Keep the memory test where it is easiest to
   fit (G7, H2) and the overflow-trick jump to 0xFFFC1000 before the exit. Before switching, take the reference
   snapshot with the current temporary image (G0 below); after, the same snapshot must match it register for
   register, and the console must reach the dashboard on 3 of 3 cold boots (POST 0x20 → 0x32 → 0x23 → 0x21 → 0x22).
   Then delete the scratch `swap_xcodes.py` from the build flow.
2. Write checks for the 1.6 program (H1-H5), then the cold/warm boot loop of spec 27 §3 on the 1.6.
3. 2BL: the 1.6 X-code check list (B2), then the repair table (B1: complete it, or drop it if the loop of step 2 shows
   no losses), and the SMC report if the test stays in start.S (B5).
4. Kernel flag 0x10 on a 1.6 (K1).
5. Optional: 128 MB MTRR (B3).
6. Future (decided 2026-11-07, after steps 1-4): one hybrid image for 1.0-1.6, the X-codes detecting a 1.6 at run
   time (as Cerbios does with an SMBus read of 0xE1, spec 24 §7) instead of the MCPX16 build switch.

G0 (reference snapshot, for step 1): extend the serial build's entry snapshot with 0x101000, 0x10B8, 0x10D4, 0x10C4,
0x10C8, 0x10DC, 0x1210, 0x1214, 0x1218, 0x122C, 0x1250, 0x1002C4, 0x100204, host 0x84, 0:0:3 0x344/0x35C, PCI 0:2:0 and
0:3:0 0x3C (the masks); and log EEPROM 0xC0-0xC3 and PCI 0:1:0 0x08 from the kernel. Record it with the 5838 bytes.
