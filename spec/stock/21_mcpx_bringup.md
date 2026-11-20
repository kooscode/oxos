# Spec 21 — MCPX chipset bring-up by the 2BL (before the kernel)

Reference behaviour of the second-stage boot loader (2BL) between the end of the X-codes and the jump into the kernel, as found in the Complex 4627 image
(consoles 1.0-1.5, MCPX 1.0 boot ROM) and the 5838 image (1.6, MCPX 1.1 boot ROM with a first-stage loader), with Cromwell for comparison; for the pad
calibration (§3) also the retail 4817, 5101, 5530 and 5713 images (consoles 1.1-1.5, MCPX 1.1); §3.7a says what OXOS implements. Behaviour only; no code. PCI
addresses are written bus:device:function, register offset; "dev1" is 0:1:0 (the MCPX LPC/ISA bridge), "host" is 0:0:0. **Status: reference only.**

## 1. Load address and execution environment
- Complex 4627: the decrypted 2BL is executed at physical 0x00090000 (its first action copies 0x6000 bytes from 0x00090000 to 0x00400000) (Complex 4627 2BL).
- 5838: the first-stage loader passes the 2BL's current location in a register; the 2BL copies 0x6000 bytes from there to 0x00400000 (skipped if it is already
  there). The image is linked to run at 0x00400000 (5838 2BL).
- Both then run from 0x00400000 (later also mapped at 0x80400000), so all statements below happen after the copy.
- Processor set-up before any chipset access, in order: caches off, MTRRs cleared; default type uncached (enabled); variable range 0 = RAM write-back (64 MB on
  Complex 4627, **128 MB** on 5838); range 1 = 0xFFF80000-0xFFFFFFFF (512 KB) write-protected; caches on; page directory at 0xF000 (layout in spec 01 §4); PAT
  MSR 0x277 = 0x0007010600070106; CR4 |= 0x610; paging on (Complex 4627 2BL) (5838 2BL).
- Neither 2BL reprograms the Super-I/O debug port at run time (a Super-I/O set-up routine exists in both images but nothing calls it) (Complex 4627 2BL) (5838
  2BL).

## 2. Chipset sequence (answers questions 1, 2, 3 and 6)
Execution order. "Byte" = an 8-bit cycle at 0xCFC; every other PCI access is a 32-bit cycle at 0xCF8/0xCFC.

| # | Action | 4627 | 5838 | Cromwell |
|---|---|---|---|---|
| 1 | dev1 0x80: **byte** write 0x02 to byte 0 (bytes 1-3 untouched; not read first) | yes | yes | dword write 2 (later, in its PCI set-up) |
| 2 | SMC challenge (SMBus slave 0x20): read registers 0x1C, 0x1D, 0x1E, 0x1F (byte reads) | yes | yes | (own code) |
| 3 | If all four bytes are 0: halt path (§2.4) | yes | yes | — |
| 4 | SMC 0x20 ← response byte 1, SMC 0x21 ← response byte 2, SMC 0x01 ← 0x00 (byte writes) | yes | yes | — (OXOS: yes, since 2026-10-26) |
| 5 | **dev1 0x8C ← 0x40000000** (plain dword write, not read-modify-write; only bit 30 set) | yes | yes | yes, same value, same point (before its PCI set-up) |
| 6 | Fill 0x80090000..+0x6000 with 0xCC (erase the old copy of the 2BL) | yes | yes | — |
| 7 | NV2A pad/drive calibration (§3) | yes | yes (other data source) | no |
| 8 | dev1 0x54: read, OR 0x88000000, write | yes | yes | yes |
| 9 | host 0x64: read, OR 0x88000000, write | yes | yes | yes |
| 10 | host 0x6C: read v; write v & ~1; write v (two data writes, one address write) | yes | yes | yes |
| 11 | host 0x80 ← 0x00000100 | yes | yes | yes |
| 12 | Read dev1 0x08 as a **byte** (revision); if ≥ 0xD1: **dev1 0xC8 ← 0x00008F00** (dword) | **no** | yes | yes (dword write, at the start of its PCI set-up) |
| 13 | Decrypt/verify the kernel, locate its PE entry, call it with the key block | yes | yes | — |

- Sources: (Complex 4627 2BL) (5838 2BL) (Cromwell).
- Steps 8-11 and 12 are separate routines; in 5838 step 12 runs **after** steps 8-11.
- The Complex 4627 2BL never reads the MCPX revision and never writes dev1 0xC8 on any console.
- Step 5 runs **after** the SMC challenge is answered, and before any other chipset write except step 1.
- In the image layout, the routine that ends with the 0x8C write sits before the main routine that starts with the 0x80 write; in execution order 0x80 (step 1)
  comes first.
- No 2BL step reads or writes the configuration space of 0:4:0 (NIC) or 0:9:0 (IDE), or any ACPI/GPIO port (0x80xx) (Complex 4627 2BL) (5838 2BL).
- No 2BL step writes NV2A 0x100200 / 0x100204 or the CRTC start 0x600800; those are left as the X-codes set them (the X-codes write 0x100200 and 0x100204)
  (Complex 4627 2BL) (5838 2BL).
- 1.6 (5838 2BL, re-checked 2026-11-07): the sequence above holds without change. Step 12 is the 5838 2BL's **only** hardware-revision test; it reads no NV2A
  revision, strap, top of memory or PCI ID, and writes no memory-interface, PFB, PLL or MTRR register beyond its CPU set-up (spec 24 §5.2-5.3).

### 2.1 Device 1 register 0x8C
- The only write is step 5: the whole register becomes 0x40000000 (bit 30 set, all others cleared).
- Cromwell writes the identical value at the same stage and labels it "enable IDE and NIC" (Cromwell).
- **Inferred:** bit 30 is the function-enable for the IDE controller (0:9:0) and/or the NIC (0:4:0); until it is set those functions do not decode configuration
  cycles (read 0xFFFFFFFF). This matches the measurement in which both read 0xFFFFFFFF at 2BL entry while USB, APU and AC'97 answer.
- The fully booted value 0x48000000 has bit 27 set as well; nothing in the 2BLs sets bit 27 (set later, by the kernel or hardware; not determined).

### 2.2 Device 1 register 0x80
- Only step 1 touches it: byte 0 ← 0x02 (bit 1 = boot ROM area off, per Cromwell's label "kill ROM area").
- It is not a read-modify-write; bits 0 and 2-7 of byte 0 become 0; bytes 1-3 keep their reset/X-code values.
- The booted value 0x04001800 has byte 0 = 0x00 and bits 11, 12, 26 set; none of those bits are written by any 2BL (Complex 4627 2BL) (5838 2BL).

### 2.3 Device 1 registers 0xC8, 0x54, 0x08
- 0x08: read once, as a byte, only by 5838, only for the threshold test (≥ 0xD1).
- 0xC8: written 0x00008F00 (whole dword) only when revision ≥ 0xD1; never read (5838 2BL).
- 0x54: OR 0x88000000 (bits 31 and 27) on all revisions (Complex 4627 2BL) (5838 2BL); host 0x64 gets the same OR.

### 2.4 Halt path
- Used when the SMC challenge reads all zeros, when the kernel digest does not match, and when the kernel has no valid PE header.
- Writes PCI 0:0:3 register 0x6C ← 0x01000000, then halts with interrupts off.
- The X-code tables write the same register 0x00230801 then 0x01230801 during memory set-up, and 0x01000000 on their own failure path; the meaning of bit 24
  there is not determined (likely boot ROM hide / processor stop).

## 3. NV2A pad calibration (step 7)
Re-derived instruction by instruction from the Complex 4627 2BL (routine at 0x400980, called from the main routine at 0x40064B with the NV2A base 0xFD000000)
and the 5838 2BL (0x400A5F, called at 0x4006B7); the retail 4817, 5101, 5530 and 5713 2BLs were studied the same way (2026-11-20) and compared step by step.
EvoX M8's loader contains the 4627 routine byte for byte (0x560 bytes). NV2A offsets below are from 0xFD000000. All arithmetic is 32-bit unsigned unless
stated.

| Image (sha256 of the BIOS image) | 2BL (decrypted, sha256) | Calibration routine; call site | Wait helper | Strap read | `wbinvd`; store loop | Table (header 0x7C) |
|---|---|---|---|---|---|---|
| Complex 4627 (34f1c8de…) | | 0x400980; 0x40064B | 0x400919 | | 0x400DE0; 0x400EEA-0x400F08 | 0xC30 |
| 4817 (db0587bc…) | 873dd396… | 0x4009AD-0x400F3F; 0x40069E | 0x400946 | 0x4009BC | 0x400E0C; 0x400F16-0x400F34 | 0xC90 |
| 5101 (f8b77144…) | f8f78088… | 0x400A46-0x400F17; 0x40069E | 0x400946 | 0x400A54 | 0x400DE1; 0x400EEB-0x400F0C | 0xD20 |
| 5530 (d4873903…), 5713 (fcde15fd…) | 34395d66…, e31c19b7… | 0x400A5F-0x400F30; 0x4006B7 | 0x40095F | 0x400A6D | 0x400DFA; 0x400F04-0x400F25 | 0xD30 |
| 5838 (148d279b…) | | 0x400A5F; 0x4006B7 | 0x40095F | | 0x400E57; 0x400F61-0x400F82 | 0xDC0 |

- All of them run the 2BL at 0x400000 (entry 0x4001BC after the copy, the FBL's images loaded at 0x90000 first). The 5530 and 5713 2BL code is byte for byte
  the same; 5101's calibration routine and helpers are 5713's shifted down by 0x19; 5530/5713's helpers (0x40095F-0x400A5E) are byte for byte 5838's.
- In short: 4627 and 4817 share one algorithm (§3.4-3.6 as written for 4627); 5101, 5530, 5713 and 5838 share another (the "later" variant in §3.4-3.6). The
  row source is the flash table in all but 5838 (EEPROM rows, §3.7). The row set is chosen differently by 4627, 4817 and 5101-5713 (§3.3).

### 3.1 Where it runs
- Main routine order (both 2BLs): device 1 0x80 byte ← 2; SMC challenge (§2 steps 2-5, which ends with SMC 0x20, 0x21, **0x01 ← 0** and device 1 0x8C ←
  0x40000000); fill 0x80090000..+0x6000 with 0xCC; **this calibration**; steps 8-11 (device 1 0x54, host 0x64, 0x6C, 0x80); 5838 only: step 12 (revision ≥ 0xD1
  → 0xC8); then the kernel is decrypted and decompressed.
- The 2BL runs from RAM at 0x400000 with caches on: MTRR default UC, range 0 RAM WB (4627, 4817, 5101 64 MB; 5530, 5713, 5838 128 MB; fixed, the 2BLs
  do not detect memory size), range 1 flash 0xFFF80000 512 KB WP; paging on
  with NV2A mapped uncached (PDE 0xFB). The X-codes have already set the memory clock and written the placeholder pad values (Cromwell-equivalent: 0x1214
  0x48480848 or 0x09090909, 0x122C 0x88888888 or 0xAAAAAAAA, 0x1230 0xFFFFFFFF, 0x1234/0x1238 0xAAAAAAAA, 0x123C/0x1244/0x1248 0x8B8B8B8B, 0x1240 0xFFFFFFFF).
  5838 (1.6): 0x1214 and 0x122C from EEPROM bytes 0xC0 and 0xC1 (each byte repeated in all four bytes), the others as Cromwell (spec 24 §2.6, §3.3).
  4817-5713: the X-codes choose 0x1214 / 0x122C from the strap field F of §3.3 (F = 0: 0x48480848 / 0x88888888, else 0x09090909 / 0xAAAAAAAA; 5713 X-codes
  0x5AB-0x605), and also write 0x1220, 0x1228, 0x1264 ← 0, 0x1210 ← 0x10, 0x124C ← 0xAA8BAA8B, 0x1250 ← 0x0000AA8B, 0x1218 ← 0x00010000 (5713 0x587-0x6DD).
  The calibration routine is the only 2BL code that touches the NV2A.

### 3.2 Wait loop (addresses in the table above; the same in every version)
`wait(n)`: rdtsc → (lo, hi); end_lo = lo + n, end_hi = hi + carry. Loop: rdtsc; stop if hi > end_hi; otherwise continue while lo < end_lo. Every wait below is n
= 10 000 (0x2710) TSC ticks (about 14 µs at 733 MHz).

### 3.3 Table and row-set selection
1. T = [0xFFC0007C] − 0x400000 (the flash header field 0x7C read through the 4 MB flash window; wraps to 0xFFC00000 + offset).
2. The strap NV2A 0x101000 is read once, **before** any calibration register is written. The row set S:
   - **4627:** bit 18 or 19 set → set 1 (T + 0x0C), else set 2 (T + 0x6B).
   - **4817:** always set 1; the strap value read is discarded. Sets 2 and 3 of its table are never used.
   - **5101, 5530, 5713:** the field F = bits 26:25 (bit 25 = F bit 0): F = 0 → set 2 (T + 0x6B); F = 1 → set 3 (T + 0xCA); F = 2 or 3 → set 1 (T + 0x0C).
     Nothing else takes part: no PCI read, no MCPX or NV2A revision, no memory size, no EEPROM, no test of bits 18-19 in the 2BL.
   - **5838:** no table rows (§3.7).
3. Rows: row r (r = 0..4) is the 19 bytes at S + 0x13·r. Breakpoints: A uses T[2], T[4], T[6], T[8], T[10]; B uses T[3], T[5], T[7], T[9], T[11].
4. Where F comes from (4817-5713): the X-codes read the board's strap bits 19:18 and write the strap register back, bits 18, 19 and 25-28 replaced and bit
   31 set (mask 0xE1F3FFFF, then an OR value); the 2BL reads what they wrote (4817/5101 X-codes 0x122-0x1BB, 5530/5713 0x146-0x227):

| Board strap bits 19:18 | OR value | F | 5101-5713 set | 4627 set (bits 18-19 directly) |
|---|---|---|---|---|
| 00 | 0x80000000 | 0 | set 2 | set 2 |
| 01 or 10 | 0x820C0000 | 1 | set 3 (the same bytes as set 1) | set 1 |
| 11 | 0x860C0000 (5530/5713: 0x86040000, bit 19 cleared, when I/O port 0x80D0 bit 5 reads 1 after 0x41 is written to it) | 3 | set 1 | set 1 |

   Side writes in the same branches: NV2A 0x10B8 ← 0xEEEE0000 (00), 0xFFFF0000 (11); for 01/10 0xFFFF0000 (4817/5101) or 0x11110000 (5530/5713). The 4627
   X-codes do not rewrite the strap register (they test bits 18-19 for their 0x1214 placeholder).
- Net effect: on every console the 5101-5713 2BLs use the same rows as the 4627 rule (straps 00 → set 2, else set 1's bytes); only the bytes of set 2 differ
  between 4627 and the later tables (§3.9). Set 3 is not for upgraded memory or a revision: it follows the straps alone.

### 3.4 Measurement (4627, 4817)
1. 0x1220 ← 0x000F0F0F.
2. For sel = 0x40004, 0x40005, 0x40006, 0x40007: 0x1228 ← sel; wait(10 000); read 0x121C. A = (sum of the four) >> 2.
3. For sel = 0x40000 .. 0x40003 the same; B = (sum) >> 2.
4. 0x1220 ← 0 (immediately after the eighth read).

The later variant (5101, 5530, 5713, 5838): four groups of four readings, in the order A-set, B-set, A-set, B-set (each reading: 0x1228 ← sel, wait(10 000),
read 0x121C); 0x1220 ← 0 after the last (5713: 0x1220 ← 0x000F0F0F at 0x400AA7, groups by the helper 0x4009C6, 0x1220 ← 0 at 0x400AE8). For each measurement
it keeps the group whose Σ|r[i] − r[i+1]| (i = 0..2, absolute signed differences) is smaller: the first group if its sum is strictly smaller, else the second;
then averages that group (sum >> 2) (5713 0x4009F4: A from groups 1 and 3, B from 2 and 4).

### 3.5 Interpolation (4627 and 4817; M = A with breakpoints b0..b4 = T[2,4,6,8,10], or M = B with T[3,5,7,9,11])
| Case (tested in this order) | Weight f (20-bit fixed point) | hi row (× f) | lo row (× (0x100000 − f)) |
|---|---|---|---|
| M ≤ b1 | ((M − b0) << 20) / (b1 − b0), unsigned; if bit 31 of the quotient is set, f = 0 | row 3 | row 4 |
| M ≥ b3 | ((M − b3) << 20) / (b4 − b3), unsigned; if f > 0x100000, **f = 1** (4627 quirk) | row 0 | row 1 |
| M < b2 | ((M − b1) << 20) / (b2 − b1) | row 2 | row 3 |
| otherwise | ((M − b2) << 20) / (b3 − b2) | row 1 | row 2 |
- Each output nibble: round(x) = ((x + 0x80000) >> 20) & 0xF with x = hi[k]·f + lo[k]·(0x100000 − f), 32-bit wrap.
- A gives o0..o15 from row bytes k = 0x00..0x0F. B gives c10, c11, c12 from row bytes 0x10, 0x11, 0x12.
- The later variant (5101-5838) differs: in the first case the division is signed (a negative quotient → f = 0; 5713 A 0x400B2A, B 0x400D34); in the
  second case f is clamped to 0x100000 (1.0; 5713 A 0x400B58, B 0x400D6B). The case order, breakpoints, rows, rounding and outputs are the same.
- A table whose breakpoints are not increasing (e.g. all zero) divides by zero: the reference has no check.

### 3.6 Values and writes
- Pattern(e, o) = the 32-bit value whose four bytes are each (o << 4 | e) (built as o, e, o, e, ... nibbles).
- 0x1214 value: v = c12; then three times (v = v << 2 | c11; v = v << 3 | c10; v = v << 3 | c12), then v = v << 2 | c11; v = v << 3 | c10 (32-bit; fields
  overlap and are OR-ed).
- List, in order: 0x122C Pattern(o0, o1), 0x123C Pattern(o2, o3), 0x1230 Pattern(o4, o5), 0x1240 Pattern(o6, o7), 0x1234 Pattern(o8, o9), 0x1244 Pattern(o10,
  o11), 0x1238 Pattern(o12, o13), 0x1248 Pattern(o14, o15), 0x1214 v.
- `wbinvd`, then the nine 32-bit stores in list order.
- 4627 and 4817: an entry whose value is 0 has its register replaced by 0, so that 0 is stored at NV2A offset 0 (PMC_BOOT_0, read-only) instead. The later
  variant (5101-5838) has the same loop but its test needs both the register and the value to be 0 (the register never is), so it writes all nine (5713
  0x400EEC).

### 3.6a The write window: no memory traffic while the pads change (requirement)
The nine stores change the drive, slew and delay of the NV2A's own DRAM interface. The reference arranges its code so that **from the first pad store to the
last, the CPU makes no memory (DRAM) access at all**: every instruction fetch and every data access hits the caches, and nothing dirty is written back. Getting
the register values right is not enough; an OXOS implementation must keep this invariant too. Read from the instructions of the Complex 4627 2BL (addresses at
its 0x400000 run address; the decompiler hides the second point):
1. Order. All arithmetic first: the 16 A nibbles and c10, c11, c12 (each a round-to-nibble of the §3.5 product) are computed into registers and stack slots.
   Then **`wbinvd`** (0x400DE0): caches written back and emptied. Only after it are the nine 32-bit values built (the pattern of §3.6, a helper called eight
   times, then the 0x1214 value) and stored, each with its register offset, into a local array of nine {offset, value} pairs on the stack. Those stores
   (write-allocate on the P6) bring the array's lines into the cache. A second loop over the array then replaces the offset of every entry whose offset or value
   is 0 by 0 (the "zero store" of §3.6), which reads and writes the same, now cached, lines.
2. Store loop (0x400EEA-0x400F08): an outer counter 0..1 around an inner loop over the nine entries; the inner body loads value and offset and stores the value
   to NV2A base + offset **only when the outer counter is 1**. Pass 0 does no load and no store: it only executes the loop's instructions, bringing its two
   32-byte code lines into the instruction cache. Pass 1 then runs entirely from the caches: its code lines were fetched by pass 0, the array lines are cached
   (and dirty) since step 1, there is no call, push or other stack access, and the only bus cycles are the nine uncached MMIO stores to 0xFD00xxxx. The few
   dirty lines are not evicted (a handful of lines in an empty cache), so no write-back happens either. Interrupts are off in the 2BL.
3. Invariant (what the next step relies on): between the first and the last pad store the memory controller sees no CPU read, write or write-back; the DRAM
   interface is reprogrammed while it is idle. Before the first store, everything the store loop will need (code, the value array) is already in the caches;
   after `wbinvd` nothing else is touched until the last store.
Requirements for OXOS (kernel/boot/bl.cpp BlPadCalibrate and any later re-calibration):
- Compute all nine values and their offsets **before** the `wbinvd`; after it, only fill the {offset, value} array (or registers) and run the store loop.
- Write the store loop so the compiler cannot move arithmetic, spills or other stack traffic into the window: an inline-assembly loop over the array (or with
  all nine values and offsets in registers), with **a dry pass first** that executes the same loop code without storing (warming its code lines), then the
  storing pass; no calls, no stack accesses and no other memory references between the first and the last store; interrupts off.
- Keep the store order (§3.6: 0x122C, 0x123C, 0x1230, 0x1240, 0x1234, 0x1244, 0x1238, 0x1248, 0x1214).
- The later 2BLs keep the same structure (5713: all nibbles before `wbinvd` at 0x400DFA, values built after it, the store loop with a dry pass). One
  difference: each storing iteration re-reads the NV2A base from the stack frame (0x400F15); only its first read can reach memory, and that is before the
  first pad store, so the invariant holds.
- Nothing that writes memory may follow in the window, e.g. logging the result (OXOS's record at 0x2300) comes after the last store.
- Status (2026-10-28): implemented in OXOS's 2BL (inline-assembly loop with a dry pass); it is kept as reference behaviour, but on the 1.4 console it did not
  change the rate of single-bit RAM changes (§3.8).

### 3.7 5838 row source (1.6)
- The 5838 2BL uses this source **unconditionally**: it has no table method and no revision test (5838 2BL 0x400A5F; spec 24 §5.2). The 1.0-1.5 retail
  images, for their part, have no EEPROM method: which source is used follows from the image, not from a test.
- 25 SMBus word reads of the EEPROM (slave 0xA8), commands 0xC4, 0xC6, ..., 0xF4 (50 bytes, 0xC4-0xF5).
- Row r (r = 0..4) from EEPROM bytes 0xC4 + 10r .. 0xCD + 10r: each of the first nine bytes gives two nibbles (high nibble first) for row bytes 0x00-0x11; the
  tenth byte is row byte 0x12. Only the breakpoints come from the flash table (5838: header 0x7C = 0xDC0). No strap test.
- The rest is the 5838 variant of §3.4-3.6: the measurement in four groups (A, B, A, B; the steadier group of each pair kept), the signed division in the lowest
  segment, the weight clamped to 1.0 above the top breakpoint, all nine registers written (no zero store); breakpoints from the 5838 table at 0xDC0 (§3.9).
- SMBus word read (5838 0x400430): host address 0xA9, command, status cleared by writing back what it reads, control 0x0B (start, word); then wait while status
  bit 3 (busy) is set; bit 1 (collision) starts again, bit 5 or bit 2 fails, bit 4 gives the word at 0xC006. Waiting for a done bit instead of for busy to clear
  hangs on the 1.6 console.
- OXOS (kernel/boot/bl.cpp BlPadCalibrateFromEeprom) makes the same NV2A writes as the 5838 routine 0x400A5F run in an emulator with a random EEPROM, over 2000
  cases (tools/padcal_check.py 5838). On the 1.6 console it reads all 25 words and calibrates (POST 0x32); the console's RAM is still not reliable, see
  spec/oxos_internal/28.
- **Write window (5838):** the same structure as §3.6a. The interpolated nibbles are computed first, then `wbinvd` (5838 2BL 0x400E57), then the nine values are
  built into the {offset, value} array (0x400E59-0x400F46), then the store loop 0x400F61-0x400F82 runs twice: an outer counter 0..1, the store done only when it
  is 1 (a dry pass first). Requirement for OXOS: as §3.6a.
- **Other users of the pad registers on a 1.6** (checked 2026-11-07, spec 24): the 5838 X-codes write placeholder values before the DRAM is initialised (0x1214
  ← EEPROM 0xC0 repeated, 0x122C ← EEPROM 0xC1 repeated, 0x1230/0x1240 0xFFFFFFFF, 0x1234/0x1238 0xAAAAAAAA, 0x123C/0x1244/0x1248 0x8B8B8B8B; 0x1248 also serves
  as a scratch register for the EEPROM bytes before that), and 0x124C 0xAA8BAA8B, 0x1250 0x00005474 after it. The X-codes do **not** measure (no read of 0x121C;
  0x1220 and 0x1228 are only set to 0). The kernel never writes 0x1200-0x1264 (spec 23 §4). So on a 1.6 the EEPROM method in the 2BL is the only calibration;
  0x124C and 0x1250 keep the X-codes' values.
- The EEPROM bytes the 1.6 uses for memory: **0xC0-0xC3** (X-codes, byte reads: 0x1214, 0x122C, 0x10B8, strap bits 18/19; spec 24 §3) and **0xC4-0xF5** (2BL,
  word reads: the five rows).

### 3.7a What OXOS implements (kernel/boot/bl.cpp)
One method per image, as the retail images have it:
- **1.0-1.5 images:** the 5713 2BL's table method (§3.3-3.6, the later variant): the retail 4817-5713 table (§3.9), the four-group measurement, the signed
  division and the clamp, all nine registers written, the write window of §3.6a. The row set follows the board strap bits 19:18 as the retail X-codes map
  them (§3.3 step 4: 00 → set 2, one bit → set 3, both → set 1), read directly: OXOS's X-codes (Cromwell's) do not write the field F.
- **1.6 images (MCPXREVD5):** the 5838 2BL's EEPROM method (§3.7), with the 5838 breakpoints as the image's table.
- Verified (2026-11-20): OXOS's 2BL makes the same NV2A writes as the 5713 routine 0x400A5F (random readings and straps with F as the X-codes leave it) and as
  the 5838 routine 0x400A5F (random readings and EEPROM), each over 2000 cases, run in a CPU emulator (tools/padcal_check.py 5713 / 5838).
- Not done: the 0xCC fill of 0x80090000 before the calibration.

### 3.8 Measured on the no-DVD 1.4 console (NV2A 0x02A000A3, MCPX 1.1), OXOS 2BL with the 4627 algorithm (2026-10)
A = 0x5E-0x63, B = 0x5D-0x61 over six boots, strap 0x861D4401 (board bits 19:18 = 11, F = 3: set 1 under every rule of §3.3, the same bytes in every
table); written: 0x122C 0x99999999, 0x123C 0x8E8E8E8E, 0x1230 0xFFFFFFFF, 0x1240
0xFFFFFFFF, 0x1234 0x99999999, 0x1244 0x8E8E8E8E, 0x1238 0x99999999, 0x1248 0x8E8E8E8E, 0x1214 0x09090909. 0x1240 stays 0xFFFFFFFF with this table.

**Drift with time powered (2026-10-28, ~430 cold boots, 8 s off between boots).** A and B are not constants of the console. After a long rest the first boot
reads A ≈ 0x65; in back-to-back boots (~35 s apart) A falls steadily (0x62, 0x61, 0x60 over the first ~5 min, 0x5E-0x5F by ~8 min, 0x5C-0x5D from ~12 min) and B
follows ~1-2 below it. Over the whole day A ranged 0x5A-0x66 and B 0x5A-0x63. The strap read 0x861D4401 on almost every boot and 0x1E1D4401 on a few (both
select row set 1).

**What the retail arithmetic writes for each A** (the Complex 4627 2BL's routine at 0x400980 run in an emulator with the console's strap, readings A ×4 then B =
A − 1 ×4; the same nine values from OXOS's implementation at the time), and what the later variant (the 5713 routine, the same inputs) writes:
| A | 0x122C, 0x1234, 0x1238 | 0x123C, 0x1244, 0x1248 | 0x1230, 0x1240 | 0x1214 (4627) | 0x1214 (5101-5713) |
|---|---|---|---|---|---|
| 0x56 | 0xBABABABA | 0x8E8E8E8E | 0xFFFFFFFF | 0x01010101 | 0x01010101 |
| 0x57 | 0xBABABABA | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 | 0x01010101 |
| 0x58-0x59 | 0xAAAAAAAA | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 | 0x01010101 |
| 0x5A-0x5B | 0xA9A9A9A9 | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 | 0x01010101 |
| 0x5C-0x6A | 0x99999999 | 0x8E8E8E8E | 0xFFFFFFFF | 0x09090909 | 0x01010101 |
- The eight A registers are the same under both. 0x1214 differs: this console's B (0x56 and up) is above the top B breakpoint (T[11] = 0x55), where the
  4627 weight becomes 1/2^20 (row 1's B bytes, 01 01 00) and the later one 1.0 (row 0's, 01 00 00). 0x01010101 is what the 5101-5713 retail BIOSes write on
  this console.
So for every A from 0x5C up the retail 2BL writes exactly what OXOS writes on this console; the step to a stronger setting is at A = 0x5B / 0x5C (the A
breakpoint T[6] = 0x5D of the table, with rounding).

**Single-bit RAM changes on this console (OXOS, spec/oxos_internal/21).** Boots whose A had drifted to 0x5C-0x5E showed single bits of RAM set within a few
seconds of the kernel starting (about 1 boot in 5-7 in that band; none at A ≥ 0x5F in 36 boots, 1 in 53 at A ≤ 0x5B). The write window of §3.6a was implemented
and did not change the rate (4 of 30 boots, same cells). The cause was not the pads: in exactly those boots DRAM refresh (PFB 0x100210) was off after the
X-codes (spec/oxos_internal/20). A, the console's warmth and the lost refresh go together; the pad values themselves are the reference ones.

### 3.9 The calibration tables
Functional hardware constants (as the encoder tables of specs 18-20): the breakpoints the measurements A and B are compared with, and the pad values
interpolated between. Rows: bytes 0x00-0x0F give o0-o15 (A), 0x10-0x12 give c10, c11, c12 (B).
- **4817, 5101, 5530, 5713:** byte for byte the same table, 0x129 bytes (12-byte breakpoints + three sets of 0x5F), at 0xC90 / 0xD20 / 0xD30 / 0xD30.
  OXOS builds it into its 1.0-1.5 images (kernel/boot/padcal_table.inc).
- **Complex 4627** (also 3944, 4034): 0xCA bytes, the same breakpoints and set 1, and an older set 2 (below); no set 3.
- **5838:** the 1.6 breakpoints only (12 bytes at 0xDC0); its rows come from the EEPROM (§3.7). OXOS builds them into its 1.6 images.

| Offset | Content | Bytes |
|---|---|---|
| +0x00 | breakpoints (4627-5713) | 20 4E 4E 3E 56 43 5D 4A 64 50 6A 55 |
| +0x0C | set 1, row 0 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 00 00 |
| +0x1F | set 1, row 1 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 01 00 |
| +0x32 | set 1, row 2 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 02 00 |
| +0x45 | set 1, row 3 | 0A 0B 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 01 02 00 |
| +0x58 | set 1, row 4 | 0B 0E 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 01 03 00 |
| +0x6B | set 2, row 0 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0x7E | set 2, row 1 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0x91 | set 2, row 2 | 08 08 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 01 |
| +0xA4 | set 2, row 3 | 08 08 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 00 01 01 |
| +0xB7 | set 2, row 4 | 08 08 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 00 01 01 |
| +0xCA | set 3 (= set 1), row 0 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 00 00 |
| +0xDD | set 3 (= set 1), row 1 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 01 00 |
| +0xF0 | set 3 (= set 1), row 2 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 01 02 00 |
| +0x103 | set 3 (= set 1), row 3 | 0A 0B 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 01 02 00 |
| +0x116 | set 3 (= set 1), row 4 | 0B 0E 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 01 03 00 |
| 4627 +0x6B | 4627 set 2, row 0 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 00 00 |
| 4627 +0x7E | 4627 set 2, row 1 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 01 00 |
| 4627 +0x91 | 4627 set 2, row 2 | 09 09 0E 08 0F 0F 0F 0F 09 09 0E 08 09 09 0E 08 00 02 00 |
| 4627 +0xA4 | 4627 set 2, row 3 | 0A 0B 0E 08 0F 0F 0F 0F 0A 0B 0E 08 0A 0B 0E 08 00 02 00 |
| 4627 +0xB7 | 4627 set 2, row 4 | 0B 0E 0E 08 0F 0F 0F 0F 0B 0E 0E 08 0B 0E 0E 08 00 03 00 |
| 5838 +0x00 | 1.6 breakpoints | 20 4E 57 2C 5F 30 66 36 6B 3D 74 40 |

- Set 2 of 4817-5713 against 4627: bytes 0-1 of every row are 08 08 (4627: 09 09, 09 09, 09 09, 0A 0B, 0B 0E) and bytes 0x11-0x12 are 01 01; bytes
  2-0x10 are the same. EvoX M8+ carries the 4817-5713 sets.

## 4. Flash header 0x6C-0x7F
| Offset | Complex 4627 | 5838 | Cromwell |
|---|---|---|---|
| 0x6C | 0x00000103 | 0x00000107 | 0x00000107 |
| 0x70 | 0x0000000F | 0x0000000F | 0x0000000F |
| 0x74 | 0x40004400 | 0x40004400 | 0x40004400 |
| 0x78 | 0x00000060 | 0x16CE0090 | 0x12D10070 (pre-1.6) / 0x16CE0090 (1.6) |
| 0x7C | 0x00000C30 | 0x00000DC0 | 0x00000C90 (pre-1.6) / 0x00000DC0 (1.6) |
- Sources: (flash header) (Cromwell).
- **0x7C**: read by the 2BL as the flash offset (from 0xFFC00000) of the NV2A calibration table (§3). In each retail image it points just past the X-code table;
  the 4627 table is 0xCA bytes (12-byte header + two 0x5F-byte sets); 4817 0xC90, 5101 0xD20, 5530 and 5713 0xD30, each 0x129 bytes (three sets) (§3.9).
- 0x6C, 0x70, 0x74, 0x78 have no direct references in the 2BLs, the 5838 first-stage loader or the kernels; the CPU-side boot ROM starts interpreting at 0x80.
  **Inferred:** they are consumed by the MCPX hardware at reset.
- Differences: 0x6C bit 2 (clear only in Complex 4627); 0x78 upper half (0 in Complex 4627, non-zero elsewhere) and lower half (0x60 / 0x70 / 0x90). What they
  control is not determined (§7).

## 6. Configuration cycle width (question 7)
- The 2BLs use dword cycles at 0xCFC everywhere except: the dev1 0x80 byte write (step 1) and the dev1 0x08 byte read (step 12), both at 0xCFC itself (register
  offsets that are multiples of 4) (Complex 4627 2BL) (5838 2BL).
- The 2BLs never use 0xCFD-0xCFF.
- The retail kernel's configuration accessors have byte, word and dword forms. Byte and word forms use port 0xCFC + (register & 3), so the kernel of the Complex
  4627 image does issue cycles at 0xCFD-0xCFF. Which registers use them is not catalogued here.

## 7. Open
- Exact meaning of dev1 0x8C bit 30 (IDE only, NIC only, or both); only Cromwell's label and the measurement support "both".
- Who sets dev1 0x8C bit 27 and dev1 0x80 bits 11, 12, 26 on a booted console (the kernel is the likely candidate; not searched).
- Whether dev1 0x8C ← 0x40000000 must follow the SMC challenge (the retail order) or only precede the IDE/NIC accesses.
- Meaning of dev1 0xC8 = 0x8F00 on MCPX ≥ D1, and of 0x54 / host 0x64 bits 31 and 27 (AGP-related per Cromwell's routine name; inferred).
- Meaning of header 0x6C, 0x70, 0x74, 0x78 fields and of their differences.
- Meaning of PCI 0:0:3 0x6C bit 24 (halt path).
- Whether the EEPROM bytes 0xC0-0xF5 used by 5838 exist on pre-1.6 consoles.
- The 1.6 X-code program, its EEPROM parameters and memory test: spec 24.
- Whether byte-wide cycles at 0xCFD-0xCFF behave on hardware; the retail kernel uses them, which suggests they do.
