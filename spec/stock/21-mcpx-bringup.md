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
| 4 | SMC 0x20 ← response byte 1, SMC 0x21 ← response byte 2, SMC 0x01 ← 0x00 (byte writes) | yes | yes | yes | — (OXOS: yes, since 2026-10-26) |
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
Re-derived instruction by instruction from the Complex 4627 2BL (routine at 0x400980, called from the main routine
at 0x40064B with the NV2A base 0xFD000000) and the 5838 2BL (0x400A5F, called at 0x4006B7). EvoX M8's loader
contains the 4627 routine byte for byte (0x560 bytes). NV2A offsets below are from 0xFD000000. All arithmetic is
32-bit unsigned unless stated. Verified: OXOS's implementation (kernel/boot/bl.cpp) makes the same NV2A writes in
the same order as the 4627 routine run in a CPU emulator, over 3000 random measurement and strap inputs.

### 3.1 Where it runs
- Main routine order (both 2BLs): device 1 0x80 byte ← 2; SMC challenge (§2 steps 2-5, which ends with SMC 0x20,
  0x21, **0x01 ← 0** and device 1 0x8C ← 0x40000000); fill 0x80090000..+0x6000 with 0xCC; **this calibration**;
  steps 8-11 (device 1 0x54, host 0x64, 0x6C, 0x80); 5838 only: step 12 (revision ≥ 0xD1 → 0xC8); then the kernel
  is decrypted and decompressed.
- The 2BL runs from RAM at 0x400000 with caches on: MTRR default UC, range 0 RAM WB (4627 64 MB, 5838 128 MB), range
  1 flash 0xFFF80000 512 KB WP; paging on with NV2A mapped uncached (PDE 0xFB). The X-codes have already set the
  memory clock and written the placeholder pad values (Cromwell-equivalent: 0x1214 0x48480848 or 0x09090909, 0x122C
  0x88888888 or 0xAAAAAAAA, 0x1230 0xFFFFFFFF, 0x1234/0x1238 0xAAAAAAAA, 0x123C/0x1244/0x1248 0x8B8B8B8B, 0x1240
  0xFFFFFFFF).

### 3.2 Wait loop (4627 0x400919, 5838 0x40095F)
`wait(n)`: rdtsc → (lo, hi); end_lo = lo + n, end_hi = hi + carry. Loop: rdtsc; stop if hi > end_hi; otherwise
continue while lo < end_lo. Every wait below is n = 10 000 (0x2710) TSC ticks (about 14 µs at 733 MHz).

### 3.3 Table and row-set selection (4627; Cerbios on NV2A < D3; inferred for the other 1.0-1.5 retail 2BLs, not examined)
1. T = [0xFFC0007C] − 0x400000 (the flash header field 0x7C read through the 4 MB flash window; wraps to
   0xFFC00000 + offset).
2. If NV2A 0x101000 (strap) has bit 18 or 19 set: row set S = T + 0x0C, else S = T + 0x6B. The strap is read
   **before** any calibration register is written.
3. Rows: row r (r = 0..4) is the 19 bytes at S + 0x13·r. Breakpoints: A uses T[2], T[4], T[6], T[8], T[10]; B uses
   T[3], T[5], T[7], T[9], T[11].

### 3.4 Measurement (4627)
1. 0x1220 ← 0x000F0F0F.
2. For sel = 0x40004, 0x40005, 0x40006, 0x40007: 0x1228 ← sel; wait(10 000); read 0x121C. A = (sum of the four) >> 2.
3. For sel = 0x40000 .. 0x40003 the same; B = (sum) >> 2.
4. 0x1220 ← 0 (immediately after the eighth read).

5838 differs: four groups of four readings, in the order A-set, B-set, A-set, B-set (each reading: 0x1228 ← sel,
wait(10 000), read 0x121C); 0x1220 ← 0 after the last. For each measurement it keeps the group whose
Σ|r[i] − r[i+1]| (i = 0..2, absolute signed differences) is smaller: the first group if its sum is strictly smaller,
else the second; then averages that group (sum >> 2).

### 3.5 Interpolation (4627; M = A with breakpoints b0..b4 = T[2,4,6,8,10], or M = B with T[3,5,7,9,11])
| Case (tested in this order) | Weight f (20-bit fixed point) | hi row (× f) | lo row (× (0x100000 − f)) |
|---|---|---|---|
| M ≤ b1 | ((M − b0) << 20) / (b1 − b0), unsigned; if bit 31 of the quotient is set, f = 0 | row 3 | row 4 |
| M ≥ b3 | ((M − b3) << 20) / (b4 − b3), unsigned; if f > 0x100000, **f = 1** (4627 quirk) | row 0 | row 1 |
| M < b2 | ((M − b1) << 20) / (b2 − b1) | row 2 | row 3 |
| otherwise | ((M − b2) << 20) / (b3 − b2) | row 1 | row 2 |
- Each output nibble: round(x) = ((x + 0x80000) >> 20) & 0xF with x = hi[k]·f + lo[k]·(0x100000 − f), 32-bit wrap.
- A gives o0..o15 from row bytes k = 0x00..0x0F. B gives c10, c11, c12 from row bytes 0x10, 0x11, 0x12.
- 5838 differs: in the first case the division is signed (idiv); in the second case f is clamped to 0x100000 (1.0).
- A table whose breakpoints are not increasing (e.g. all zero) divides by zero: the reference has no check.

### 3.6 Values and writes
- Pattern(e, o) = the 32-bit value whose four bytes are each (o << 4 | e) (built as o, e, o, e, ... nibbles).
- 0x1214 value: v = c12; then three times (v = v << 2 | c11; v = v << 3 | c10; v = v << 3 | c12), then
  v = v << 2 | c11; v = v << 3 | c10 (32-bit; fields overlap and are OR-ed).
- List, in order: 0x122C Pattern(o0, o1), 0x123C Pattern(o2, o3), 0x1230 Pattern(o4, o5), 0x1240 Pattern(o6, o7),
  0x1234 Pattern(o8, o9), 0x1244 Pattern(o10, o11), 0x1238 Pattern(o12, o13), 0x1248 Pattern(o14, o15),
  0x1214 v.
- `wbinvd`, then the nine 32-bit stores in list order.
- 4627: an entry whose value is 0 has its register replaced by 0, so that 0 is stored at NV2A offset 0 (PMC_BOOT_0,
  read-only) instead. 5838 has the same loop but its test compares the register (never 0), so it writes all nine.

### 3.6a The write window: no memory traffic while the pads change (requirement)
The nine stores change the drive, slew and delay of the NV2A's own DRAM interface. The reference arranges its code so
that **from the first pad store to the last, the CPU makes no memory (DRAM) access at all**: every instruction fetch
and every data access hits the caches, and nothing dirty is written back. Getting the register values right is not
enough; an OXOS implementation must keep this invariant too. Read from the instructions of the Complex 4627 2BL
(addresses at its 0x400000 run address; the decompiler hides the second point):
1. Order. All arithmetic first: the 16 A nibbles and c10, c11, c12 (each a round-to-nibble of the §3.5 product)
   are computed into registers and stack slots. Then **`wbinvd`** (0x400DE0): caches written back and emptied. Only
   after it are the nine 32-bit values built (the pattern of §3.6, a helper called eight times, then the 0x1214
   value) and stored, each with its register offset, into a local array of nine {offset, value} pairs on the stack.
   Those stores (write-allocate on the P6) bring the array's lines into the cache. A second loop over the array
   then replaces the offset of every entry whose offset or value is 0 by 0 (the "zero store" of §3.7a step 4), which
   reads and writes the same, now cached, lines.
2. Store loop (0x400EEA-0x400F08): an outer counter 0..1 around an inner loop over the nine entries; the inner
   body loads value and offset and stores the value to NV2A base + offset **only when the outer counter is 1**.
   Pass 0 does no load and no store: it only executes the loop's instructions, bringing its two 32-byte code lines
   into the instruction cache. Pass 1 then runs entirely from the caches: its code lines were fetched by pass 0, the
   array lines are cached (and dirty) since step 1, there is no call, push or other stack access, and the only bus
   cycles are the nine uncached MMIO stores to 0xFD00xxxx. The few dirty lines are not evicted (a handful of lines in
   an empty cache), so no write-back happens either. Interrupts are off in the 2BL.
3. Invariant (what the next step relies on): between the first and the last pad store the memory controller sees
   no CPU read, write or write-back; the DRAM interface is reprogrammed while it is idle. Before the first store,
   everything the store loop will need (code, the value array) is already in the caches; after `wbinvd` nothing
   else is touched until the last store.
Requirements for OXOS (kernel/boot/bl.cpp BlPadCalibrate and any later re-calibration):
- Compute all nine values and their offsets **before** the `wbinvd`; after it, only fill the {offset, value} array
  (or registers) and run the store loop.
- Write the store loop so the compiler cannot move arithmetic, spills or other stack traffic into the window: an
  inline-assembly loop over the array (or with all nine values and offsets in registers), with **a dry pass first**
  that executes the same loop code without storing (warming its code lines), then the storing pass; no calls, no
  stack accesses and no other memory references between the first and the last store; interrupts off.
- Keep the store order (§3.6: 0x122C, 0x123C, 0x1230, 0x1240, 0x1234, 0x1244, 0x1238, 0x1248, 0x1214) and the
  zero-store behaviour.
- Nothing that writes memory may follow in the window, e.g. logging the result (OXOS's record at 0x2300) comes
  after the last store.
- Status (2026-10-28): implemented in OXOS's 2BL (inline-assembly loop with a dry pass); it is kept as reference
  behaviour, but on the 1.4 console it did not change the rate of single-bit RAM changes (§3.8).

### 3.7 5838 row source (NV2A ≥ D3; 1.6)
- 25 SMBus word reads of the EEPROM (slave 0xA8), commands 0xC4, 0xC6, ..., 0xF4 (50 bytes, 0xC4-0xF5).
- Row r (r = 0..4) from EEPROM bytes 0xC4 + 10r .. 0xCD + 10r: each of the first nine bytes gives two nibbles (high
  nibble first) for row bytes 0x00-0x11; the tenth byte is row byte 0x12. Only the breakpoints come from the flash
  table (5838: header 0x7C = 0xDC0). No strap test.
- Cerbios takes this path when NV2A PMC 0 low byte ≥ 0xD3 (and a PCI condition holds), the table path otherwise.

### 3.7a Cerbios 3.1.0 on NV2A < D3 (consoles 1.0-1.5): the complete sequence (what OXOS implements)
From Cerbios's bootloader (plaintext at flash 0x3CDD8, loaded at 0x90000, entry 0x90208; main routine 0x90738).
Addresses below are those load addresses. Every step was read from the code; values are exact.
1. CPU set-up (0x90208): IDT/GDT loaded; CR0 |= CD|NW, `wbinvd`; MTRR default (0x2FF) ← 0; range 0 base 0 type 6
   (WB), mask 0xF_F0000800 (**256 MB**); range 1 base 0xFFF80005 (WP), mask 512 KB; others cleared; default ← 0x800
   (UC, enabled); caches on. The code is copied to and runs from RAM at 0x400000.
2. Device 1 register 0x80: byte write 0x02 at 0xCFC (0x90738).
3. SMC challenge (0x90646): byte reads of SMC 0x1C, 0x1D, 0x1E, 0x1F; all four 0 → halt path; else the answer bytes
   to SMC 0x20 and 0x21, then **SMC 0x01 ← 0x00**; then device 1 0x8C ← 0x40000000 (dword).
4. Pre-check (0x911E4 = strstr): if the string "SHADOW" occurs in the 2BL parameter area at 0x80400004, steps 5-9
   are skipped (a shadow restart). On a cold boot it does not occur.
5. Method selection (0x9076A-0x907E5), in this order: read host 0x84 (top of memory, dword) = top; read PCI 0:2:0
   0x3C (dword, low byte b2) and 0:3:0 0x3C (dword, low byte b3); read the NV2A byte at 0xFD000000 (revision).
   upgraded128 = (b2 = 0x0F and b3 = 0xF0 and top = 0x07FFFFFF).
   - revision ≥ 0xD3 and not upgraded128 → the EEPROM-row method (0x90F40; §3.7, 1.6 consoles).
   - otherwise → the table method (0x9103C) with flag = (top = 0x0FFFFFFF) or upgraded128.
6. Table method (0x9103C, flag):
   1. T = [0xFF00007C] − 0x1000000 (header field 0x7C, 0x1400 in Cerbios, through the 16 MB flash window).
   2. strap = NV2A 0x101000 (read first). Row set S = T + 0xD6 if flag; else T + 0x18 if strap bit 18 or 19 is set;
      else T + 0x77. Row r = S + 0x13·r (r = 0..4).
   3. Measurement exactly as §3.4 (4627): 0x1220 ← 0x000F0F0F; selectors 0x40004-0x40007, each 0x1228 ← sel,
      wait(10 000) (§3.2, same loop at 0x909F1), read 0x121C; A = sum >> 2; selectors 0x40000-0x40003 the same → B;
      0x1220 ← 0.
   4. Values and writes (shared routine 0x90B08): exactly §3.5 and §3.6 for the 4627 variant (unsigned divisions,
      the 1/2^20 weight above the top breakpoint, breakpoints T[2,4,6,8,10] for A and T[3,5,7,9,11] for B,
      `wbinvd`, the nine stores in order, a zero value stored at NV2A offset 0 instead of its register), with the
      write window of §3.6a: no memory access between the first and the last pad store.
7. AGP/host (0x91187): device 1 0x54 |= 0x88000000; host 0x64 |= 0x88000000; host 0x6C: read v, write v & ~1,
   write v (the LDT reset); host 0x80 ← 0x00000100.
8. Device 1 0x08 read as a byte; if ≥ 0xD1: device 1 0xC8 ← 0x00008F00 (0x90710).
9. Then the kernel is decompressed (0x92D66) and entered.
- Table layout (0x135 bytes, Cerbios's flash 0x1400-0x1534): bytes 0x00-0x0B the 4627 breakpoint header (byte for
  byte), 0x0C-0x17 the 5838 one (used by the EEPROM method), row sets at +0x18 (strap bit), +0x77 (no strap bit),
  +0xD6 (upgraded memory). The +0x18 set equals the 4627 table's first set byte for byte; the +0x77 set differs from
  4627's second set only in the B bytes (0x10-0x12 of each row, which give 0x1214). So on a console whose strap
  selects the first set (the no-DVD 1.4 console), Cerbios and 4627 write the same nine values.
- The MCPX revision is not part of the selection; it only gates step 8.
- Verified: OXOS's 2BL with this table (§3.9) makes the same NV2A writes as Cerbios's 0x9103C run in an emulator,
  both flag values, over 2000 random inputs (tools/padcal_check.py).

### 3.8 Measured on the no-DVD 1.4 console (NV2A 0x02A000A3, MCPX 1.1), OXOS 2BL with the 4627 table
A = 0x5E-0x63, B = 0x5D-0x61 over six boots, strap 0x861D4401 (first row set); written: 0x122C 0x99999999,
0x123C 0x8E8E8E8E, 0x1230 0xFFFFFFFF, 0x1240 0xFFFFFFFF, 0x1234 0x99999999, 0x1244 0x8E8E8E8E, 0x1238 0x99999999,
0x1248 0x8E8E8E8E, 0x1214 0x09090909. 0x1240 stays 0xFFFFFFFF with this table.

**Drift with time powered (2026-10-28, ~430 cold boots, 8 s off between boots).** A and B are not constants of the
console. After a long rest the first boot reads A ≈ 0x65; in back-to-back boots (~35 s apart) A falls steadily
(0x62, 0x61, 0x60 over the first ~5 min, 0x5E-0x5F by ~8 min, 0x5C-0x5D from ~12 min) and B follows ~1-2 below it.
Over the whole day A ranged 0x5A-0x66 and B 0x5A-0x63. The strap read 0x861D4401 on almost every boot and
0x1E1D4401 on a few (both select row set 1).

**What the retail arithmetic writes for each A** (the Complex 4627 2BL's routine at 0x400980 run in an emulator with
the console's strap, readings A ×4 then B = A − 1 ×4; the same nine values from OXOS's implementation, which
matches it write for write, §3.7a):
| A | 0x122C, 0x1234, 0x1238 | 0x123C, 0x1244, 0x1248 | 0x1230, 0x1240 | 0x1214 |
|---|---|---|---|---|
| 0x56 | 0xBABABABA | 0x8E8E8E8E | 0xFFFFFFFF | 0x01010101 |
| 0x57 | 0xBABABABA | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 |
| 0x58-0x59 | 0xAAAAAAAA | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 |
| 0x5A-0x5B | 0xA9A9A9A9 | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 |
| 0x5C-0x6A | 0x99999999 | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 |
So for every A from 0x5C up the retail 2BL writes exactly what OXOS writes on this console; the step to a stronger
setting is at A = 0x5B / 0x5C (the A breakpoint T[6] = 0x5D of the table, with rounding).

**Single-bit RAM changes on this console (OXOS, spec/oxos-internal/21).** Boots whose A had drifted to 0x5C-0x5E
showed single bits of RAM set within a few seconds of the kernel starting (about 1 boot in 5-7 in that band; none at
A ≥ 0x5F in 36 boots, 1 in 53 at A ≤ 0x5B). The write window of §3.6a was implemented and did not change the rate
(4 of 30 boots, same cells). The cause was not the pads: in exactly those boots DRAM refresh (PFB 0x100210) was off
after the X-codes (spec/oxos-internal/20). A, the console's warmth and the lost refresh go together; the pad values
themselves are the reference ones.

### 3.9 The calibration table (Cerbios layout, 0x135 bytes)
Functional hardware constants (as the encoder tables of specs 18-20): the breakpoints the measurements A and B are
compared with, and the pad values interpolated between. Rows: bytes 0x00-0x0F give o0-o15 (A), 0x10-0x12 give c10,
c11, c12 (B). Set 1 equals the 4627 table's first set; set 2 differs from 4627's second set in bytes 0x10-0x12 only.
OXOS builds it from kernel/boot/padcal_table.inc.

| Offset | Content | Bytes |
|---|---|---|
| +0x00 | breakpoints | 20 4E 4E 3E 56 43 5D 4A 64 50 6A 55 |
| +0x0C | 1.6 breakpoints (EEPROM method) | 20 4E 57 2C 5F 30 66 36 6B 3D 74 40 |
| +0x18 | set 1 (strap bit 18/19), row 0 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 00 00 |
| +0x2B | set 1 (strap bit 18/19), row 1 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 01 00 |
| +0x3E | set 1 (strap bit 18/19), row 2 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 02 00 |
| +0x51 | set 1 (strap bit 18/19), row 3 | 0A 0B 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 01 02 00 |
| +0x64 | set 1 (strap bit 18/19), row 4 | 0B 0E 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 01 03 00 |
| +0x77 | set 2 (no strap bit), row 0 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0x8A | set 2 (no strap bit), row 1 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0x9D | set 2 (no strap bit), row 2 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0xB0 | set 2 (no strap bit), row 3 | 08 08 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 00 01 01 |
| +0xC3 | set 2 (no strap bit), row 4 | 08 08 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 00 01 01 |
| +0xD6 | set 3 (upgraded memory), row 0 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 0E 0E 0E 0E 01 00 00 |
| +0xE9 | set 3 (upgraded memory), row 1 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 0E 0E 0E 0E 01 01 00 |
| +0xFC | set 3 (upgraded memory), row 2 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 0E 0E 0E 0E 01 02 00 |
| +0x10F | set 3 (upgraded memory), row 3 | 0A 0B 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0E 0E 0E 0E 01 02 00 |
| +0x122 | set 3 (upgraded memory), row 4 | 0B 0E 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0E 0E 0E 0E 01 03 00 |

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
