# Spec 21 — MCPX chipset bring-up by the 2BL (before the kernel)

Reference behaviour of the second-stage boot loader (2BL) between the end of the X-codes and the jump into the kernel,
as found in the Complex 4627 image (consoles 1.0-1.5, MCPX 1.0 boot ROM) and the 5838 image (1.6, MCPX 1.1 boot ROM
with a first-stage loader), with Cerbios and Cromwell for comparison. Behaviour only; no code. PCI addresses are
written bus:device:function, register offset; "dev1" is 0:1:0 (the MCPX LPC/ISA bridge), "host" is 0:0:0.
**Status: reference only.**

## 1. Load address and execution environment
- Complex 4627: the decrypted 2BL is executed at physical 0x00090000 (its first action copies 0x6000 bytes from
  0x00090000 to 0x00400000) (Complex 4627 2BL).
- 5838: the first-stage loader passes the 2BL's current location in a register; the 2BL copies 0x6000 bytes from
  there to 0x00400000 (skipped if it is already there). The image is linked to run at 0x00400000 (5838 2BL).
- Both then run from 0x00400000 (later also mapped at 0x80400000), so all statements below happen after the copy.
- Processor set-up before any chipset access, in order: caches off, MTRRs cleared; default type uncached (enabled);
  variable range 0 = RAM write-back (64 MB on Complex 4627, **128 MB** on 5838); range 1 = 0xFFF80000-0xFFFFFFFF
  (512 KB) write-protected; caches on; page directory at 0xF000 (layout in spec 01 §4); PAT MSR 0x277 =
  0x0007010600070106; CR4 |= 0x610; paging on (Complex 4627 2BL) (5838 2BL).
- Neither 2BL reprograms the Super-I/O debug port at run time (a Super-I/O set-up routine exists in both images
  but nothing calls it) (Complex 4627 2BL) (5838 2BL).

## 2. Chipset sequence (answers questions 1, 2, 3 and 6)
Execution order. "Byte" = an 8-bit cycle at 0xCFC; every other PCI access is a 32-bit cycle at 0xCF8/0xCFC.

| # | Action | 4627 | 5838 | Cerbios | Cromwell |
|---|---|---|---|---|---|
| 1 | dev1 0x80: **byte** write 0x02 to byte 0 (bytes 1-3 untouched; not read first) | yes | yes | yes | dword write 2 (later, in its PCI set-up) |
| 2 | SMC challenge (SMBus slave 0x20): read registers 0x1C, 0x1D, 0x1E, 0x1F (byte reads) | yes | yes | yes | (own code) |
| 3 | If all four bytes are 0: halt path (§2.4) | yes | yes | yes | — |
| 4 | SMC 0x20 ← response byte 1, SMC 0x21 ← response byte 2, SMC 0x01 ← 0x00 (byte writes) | yes | yes | yes | — |
| 5 | **dev1 0x8C ← 0x40000000** (plain dword write, not read-modify-write; only bit 30 set) | yes | yes | yes | yes, same value, same point (before its PCI set-up) |
| 6 | Fill 0x80090000..+0x6000 with 0xCC (erase the old copy of the 2BL) | yes | yes | no | — |
| 7 | NV2A pad/drive calibration (§3) | yes | yes (other data source) | yes (two variants) | no |
| 8 | dev1 0x54: read, OR 0x88000000, write | yes | yes | yes | yes |
| 9 | host 0x64: read, OR 0x88000000, write | yes | yes | yes | yes |
| 10 | host 0x6C: read v; write v & ~1; write v (two data writes, one address write) | yes | yes | yes | yes |
| 11 | host 0x80 ← 0x00000100 | yes | yes | yes | yes |
| 12 | Read dev1 0x08 as a **byte** (revision); if ≥ 0xD1: **dev1 0xC8 ← 0x00008F00** (dword) | **no** | yes | yes | yes (dword write, at the start of its PCI set-up) |
| 13 | Decrypt/verify the kernel, locate its PE entry, call it with the key block | yes | yes | yes | — |

- Sources: (Complex 4627 2BL) (5838 2BL) (Cerbios) (Cromwell).
- Steps 8-11 and 12 are separate routines; in 5838 and Cerbios step 12 runs **after** steps 8-11.
- The Complex 4627 2BL never reads the MCPX revision and never writes dev1 0xC8 on any console.
- Step 5 runs **after** the SMC challenge is answered, and before any other chipset write except step 1.
- In the image layout, the routine that ends with the 0x8C write sits before the main routine that starts with the
  0x80 write; in execution order 0x80 (step 1) comes first.
- No 2BL step reads or writes the configuration space of 0:4:0 (NIC) or 0:9:0 (IDE), or any ACPI/GPIO port
  (0x80xx) (Complex 4627 2BL) (5838 2BL) (Cerbios).
- No 2BL step writes NV2A 0x100200 / 0x100204 or the CRTC start 0x600800; those are left as the X-codes set them
  (the X-codes write 0x100200 and 0x100204) (Complex 4627 2BL) (5838 2BL).

### 2.1 Device 1 register 0x8C
- The only write is step 5: the whole register becomes 0x40000000 (bit 30 set, all others cleared).
- Cromwell writes the identical value at the same stage and labels it "enable IDE and NIC" (Cromwell).
- **Inferred:** bit 30 is the function-enable for the IDE controller (0:9:0) and/or the NIC (0:4:0); until it is
  set those functions do not decode configuration cycles (read 0xFFFFFFFF). This matches the measurement in which
  both read 0xFFFFFFFF at 2BL entry while USB, APU and AC'97 answer.
- The fully booted value 0x48000000 has bit 27 set as well; nothing in the 2BLs sets bit 27 (set later, by the
  kernel or hardware; not determined).

### 2.2 Device 1 register 0x80
- Only step 1 touches it: byte 0 ← 0x02 (bit 1 = boot ROM area off, per Cromwell's label "kill ROM area").
- It is not a read-modify-write; bits 0 and 2-7 of byte 0 become 0; bytes 1-3 keep their reset/X-code values.
- The booted value 0x04001800 has byte 0 = 0x00 and bits 11, 12, 26 set; none of those bits are written by any 2BL
  (Complex 4627 2BL) (5838 2BL).

### 2.3 Device 1 registers 0xC8, 0x54, 0x08
- 0x08: read once, as a byte, only by 5838/Cerbios, only for the threshold test (≥ 0xD1).
- 0xC8: written 0x00008F00 (whole dword) only when revision ≥ 0xD1; never read (5838 2BL) (Cerbios).
- 0x54: OR 0x88000000 (bits 31 and 27) on all revisions (Complex 4627 2BL) (5838 2BL); host 0x64 gets the same OR.

### 2.4 Halt path
- Used when the SMC challenge reads all zeros, when the kernel digest does not match, and when the kernel has no
  valid PE header.
- Writes PCI 0:0:3 register 0x6C ← 0x01000000, then halts with interrupts off.
- The X-code tables write the same register 0x00230801 then 0x01230801 during memory set-up, and 0x01000000 on
  their own failure path; the meaning of bit 24 there is not determined (likely boot ROM hide / processor stop).

## 3. NV2A pad calibration (step 7)
NV2A registers below are offsets from 0xFD000000.
- Write 0x001220 ← 0x000F0F0F (calibration on; inferred).
- For each selector 0x40004-0x40007: write 0x001228 ← selector, wait 10 000 TSC ticks, read 0x00121C.
  The average of the four reads is measurement A.
- Likewise selectors 0x40000-0x40003 → measurement B.
- 5838 takes each set of four readings twice and keeps the set whose consecutive readings vary least (5838 2BL).
- Write 0x001220 ← 0.
- Each measurement selects a point on a five-row table by piecewise-linear interpolation (20-bit fixed point,
  rounded to a 4-bit value): A uses breakpoints at table bytes 2, 4, 6, 8, 10 and row bytes 0x00-0x0F; B uses
  bytes 3, 5, 7, 9, 11 and row bytes 0x10-0x12. Rows are 19 bytes; the lowest breakpoint selects row 4, the
  highest row 0.
- Outputs (o0..o15 from A, c0..c2 from B) are written as:

| Register | Value |
|---|---|
| 0x00122C | nibbles alternate o1 (odd nibbles) / o0 (even nibbles), i.e. 0x(o1)(o0)(o1)(o0)(o1)(o0)(o1)(o0) |
| 0x00123C | same pattern from o3 / o2 |
| 0x001230 | o5 / o4 |
| 0x001240 | o7 / o6 |
| 0x001234 | o9 / o8 |
| 0x001244 | o11 / o10 |
| 0x001238 | o13 / o12 |
| 0x001248 | o15 / o14 |
| 0x001214 | each byte = c0 << 5 \| c2 << 3 \| c1 (four identical bytes; values are OR-ed, not masked) |

- Write order: 0x00122C, 0x00123C, 0x001230, 0x001240, 0x001234, 0x001244, 0x001238, 0x001248, 0x001214.
- Complex 4627 skips a register whose computed value is 0; 5838 writes all nine (Complex 4627 2BL) (5838 2BL).
- Clamping differs: above the top breakpoint 5838 clamps the fraction to 1.0; Complex 4627 sets it to 1/2^20
  (effectively the lower row) (Complex 4627 2BL) (5838 2BL).
- **Table source:**
  - Complex 4627: the flash table at the offset in header 0x7C (§4). Bytes 0x0C-0x6A hold one set of five rows,
    0x6B-0xC9 a second set; the first set is used when NV2A strap register 0x101000 has either of bits 18-19 set,
    the second otherwise (Complex 4627 2BL).
  - 5838: breakpoints from the flash table; the five rows come from the **EEPROM** (SMBus 0xA8, word reads of
    0xC4-0xF5; each row = 9 bytes split into 18 nibbles plus one byte) (5838 2BL).
  - Cerbios: chooses between the two methods by the NV2A revision (PMC register 0x000000, low byte ≥ 0xD3 → the
    EEPROM method) and a memory-layout test (Cerbios).

## 4. Flash header 0x6C-0x7F
| Offset | Complex 4627 | 5838 | Cerbios | Cromwell |
|---|---|---|---|---|
| 0x6C | 0x00000103 | 0x00000107 | 0x00000107 | 0x00000107 |
| 0x70 | 0x0000000F | 0x0000000F | 0x0000000F | 0x0000000F |
| 0x74 | 0x40004400 | 0x40004400 | 0x40004400 | 0x40004400 |
| 0x78 | 0x00000060 | 0x16CE0090 | 0x1F480060 | 0x12D10070 (pre-1.6) / 0x16CE0090 (1.6) |
| 0x7C | 0x00000C30 | 0x00000DC0 | 0x00001400 | 0x00000C90 (pre-1.6) / 0x00000DC0 (1.6) |
- Sources: (flash header) (Cromwell).
- **0x7C**: read by the 2BL as the flash offset (from 0xFFC00000) of the NV2A calibration table (§3). In each retail
  image it points just past the X-code table; the 4627 table is 0xCA bytes (12-byte header + two 0x5F-byte sets).
- 0x6C, 0x70, 0x74, 0x78 have no direct references in the 2BLs, the 5838 first-stage loader or the kernels; the CPU-side boot ROM
  starts interpreting at 0x80. **Inferred:** they are consumed by the MCPX hardware at reset.
- Differences: 0x6C bit 2 (clear only in Complex 4627); 0x78 upper half (0 in Complex 4627, non-zero elsewhere) and
  lower half (0x60 / 0x70 / 0x90). What they control is not determined (§7).
- Cerbios uses the 4627-era values at 0x00-0x2F and 0x70-0x74, 0x6C = 0x107, and a 0x7C table whose breakpoint
  bytes equal Complex 4627's (Cerbios).

## 5. Cerbios versus the retail 2BLs (question 5)
- Same device 1 / host sequence as 5838, including the revision ≥ 0xD1 write of dev1 0xC8 (§2, step 12) (Cerbios).
- Same SMC challenge and the same dev1 0x8C ← 0x40000000 right after it (Cerbios).
- Differences:
  - no 0xCC erase;
  - a pre-check, after step 5, that can skip steps 7-12 entirely (purpose not determined);
  - two calibration variants (§3).
- Nothing in the Cerbios 2BL touches 0:4:0 or 0:9:0 either. **Inferred:** what makes Cerbios (and a retail 2BL)
  bring up IDE and NIC on a 1.4 console is step 5 (plus step 12 on MCPX ≥ D1), not anything extra.

## 6. Configuration cycle width (question 7)
- The 2BLs use dword cycles at 0xCFC everywhere except: the dev1 0x80 byte write (step 1) and the dev1 0x08 byte
  read (step 12), both at 0xCFC itself (register offsets that are multiples of 4) (Complex 4627 2BL) (5838 2BL).
- The 2BLs never use 0xCFD-0xCFF.
- The retail kernel's configuration accessors have byte, word and dword forms. Byte and word forms use port
  0xCFC + (register & 3), so the kernel of the Complex 4627 image does issue cycles at 0xCFD-0xCFF. Which registers use them
  is not catalogued here.

## 7. Open
- Exact meaning of dev1 0x8C bit 30 (IDE only, NIC only, or both); only Cromwell's label and the measurement support
  "both".
- Who sets dev1 0x8C bit 27 and dev1 0x80 bits 11, 12, 26 on a booted console (the kernel is the likely candidate;
  not searched).
- Whether dev1 0x8C ← 0x40000000 must follow the SMC challenge (the retail order) or only precede the IDE/NIC
  accesses.
- Meaning of dev1 0xC8 = 0x8F00 on MCPX ≥ D1, and of 0x54 / host 0x64 bits 31 and 27 (AGP-related per Cromwell's
  routine name; inferred).
- Meaning of header 0x6C, 0x70, 0x74, 0x78 fields and of their differences.
- Meaning of PCI 0:0:3 0x6C bit 24 (halt path).
- Purpose of the Cerbios pre-check that skips steps 7-12.
- Whether the EEPROM bytes 0xC4-0xF5 used by 5838 exist on pre-1.6 consoles.
- Whether byte-wide cycles at 0xCFD-0xCFF behave on hardware; the retail kernel uses them, which suggests they do.
