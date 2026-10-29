# 21 — DRAM margin experiments (single-bit RAM changes on the 1.4 console)

Status: experiment plan, 2026-10-28. **Resolved the same day by E3** (§9): the bad boots are the ones that start with
DRAM refresh off; fixed by spec/oxos-internal/20. E1, E2, E5 and E6 were built but not run (the code is kept outside
the tree, scratchpad wip-backup2 of the session). The implementation agent builds exactly what is described here. Every
experiment is a build switch, **off by default**; none is part of a release unless a later decision says so.
Background: experimental/bitflip-crash-symptoms.md (symptoms and runs), spec/stock/21 §3.6a and §3.8 (pad
calibration, its write window, the drift of A), spec/stock/22 §6.3 and §6.3a (memory set-up by the X-codes and by
the stock kernels).

## 0. What is known
- On the no-DVD 1.4 console (Modxo, NV2A 0xA3) single bits of RAM change from 0 to 1 a few seconds after the OXOS
  kernel starts, always the same cells first and more of them the longer it runs; the kernel then crashes. It
  happens only when the 2BL's pad measurement A (spec 21 §3.4) reads 0x5C-0x5E, which it does once the console has
  been powered for ~10 minutes (A falls from ~0x65 when cold). The machine-check banks record nothing during it.
- In that band the 2BL writes the retail values (0x122C/0x1234/0x1238 = 0x99999999 for every A ≥ 0x5C, spec 21
  §3.8), with the retail write window (§3.6a). The memory set-up before the 2BL (refresh, timing, mode registers,
  PLLs) is identical in every reference image for this NV2A (spec 22 §6.3), and the stock kernels never change it
  afterwards (§6.3a).
- Not yet known: whether a stock kernel on the same console in the same band shows the same changes (E6), whether
  the cells lose the stored value or are read wrongly (E4), and which setting would give margin (E1, E2).

## 1. Common protocol
1. Hardware: the no-DVD 1.4 console with the Modxo and its logger (tools/modxo), a netboot server
   (tools/netboot) serving the experiment kernel and the title launcher (as the hunt loops of 2026-10-28:
   `console.py off`, 8 s, `on`, 25 s, repeat).
2. Warm-up: run boots back to back until the 2BL's logged A ("nv pads: 2BL calibration A ...") is ≤ 0x5E; this
   takes about 12 minutes from a cold console. Keep going without pauses longer than a cycle, so A stays there.
3. Only **band boots** count: A in 0x5C-0x5E. Others are logged but left out of the result.
4. Baseline in the band (OXOS serial kernel with the retail calibration, all runs so far): 21 bad boots of 144
   (14.6 %). Bad = a textguard report or a free-list report (spec/oxos-internal debug checks; kernel/src/ke/textguard.cpp,
   kernel/src/mm/init.cpp MiCheckFreeLists).
5. Each arm runs until it has **40 band boots**. Decision: 0 bad of 40 = **fixed** (by chance with a 14.6 % rate:
   0.854^40 ≈ 0.2 %); 3 or more bad = **no effect**; 1-2 bad = run 40 more and decide on 80 (0-2 of 80 = fixed).
6. Log for every boot (serial, already partly there): A, B, strap and the nine written values (the 2BL record at
   0x2300, "nv pads: 2BL calibration"), the experiment's own values, the textguard and free-list result with the
   first checkpoint that saw it, the register watch of E3, and the SMC temperatures (CPU / board, logged at the
   title launcher since 2026-10-28).
7. Results go to WORKLOG.md and a table in this file (§9) per arm: band boots, bad boots, decision.

## 2. E3 — register watch (serial builds, always on; do first)
- Where: kernel/src/ke/textguard.cpp, called from KiTextGuardCheck (every checkpoint), plus once right after
  MmInitSystem (the first snapshot).
- What: read and remember, then at each later checkpoint log only the ones that changed, as
  `regwatch (CHECKPOINT): NAME old -> new`:
  PFB 0x100200, 0x100204, 0x100210, 0x100214, 0x100228; PBUS 0x1210, 0x1214, 0x1218, 0x122C-0x1250;
  PMC 0x000200; NVPLL 0x680500, MPLL 0x680504, VPLL 0x680508, 0x68050C; host bus 0 device 0 function 3 register
  0x6C (configuration address 0x8000036C; read through 0xCF8/0xCFC with interrupts off).
- Also once per boot at the first snapshot: the SMC version bytes (SMBus 0x20, command 0x01, read three times after
  writing 0 to command 0x01), to know whether the retail X-codes' early host 0x6C write (spec 22 §6.3, taken unless
  the first byte is 0x50) would apply to this console.
- PFB 0x100210: log its value at every checkpoint (not only on change) for the first 10 band boots, to see whether
  any field of it moves while the console runs (its fields are not documented in the binaries; spec 22 §6.3a).
- Pass/fail: none; it is information. A change in a bad boot (any of these registers) is the finding to report.

## 3. E4 — persistence of a changed cell (serial builds, always on; do with E3)
- Where: KiTextGuardCheck, when it finds the first changed byte of a boot (the live code differs from the copy).
- What, for that byte (live address L, copy value C, live value V):
  1. Read L three more times with `wbinvd` between the reads (each read from RAM); log the three values.
  2. Write C back to L (lift CR0.WP for the store, as the cell test does), `wbinvd`, then wait 1 s with the
     interrupts on (KeStallExecutionProcessor in 1 ms steps), `wbinvd`, read L; log it.
  3. Wait 1 s more and read again; log it.
- Interpretation (log line `persist: L C V reads r1 r2 r3, rewrite then t1 t2`): r1-r3 all V and t1/t2 = C, then
  V again later = the cell loses its charge again (retention / refresh); r1-r3 flicker between C and V = a read-path
  fault; t1/t2 stay C = a one-time event.
- Pass/fail: none; information.

## 4. E6 — stock baseline (the most important control; after E3/E4)
- Goal: the stock Complex 4627 chain (its X-codes, its 2BL with its calibration, its kernel) on the same console in
  the same band. Does RAM change the same way?
- Build: experimental/retail-netboot (retail4627-256k.bin and its build scripts) with the statedump probe of
  experimental/statedump injected into the stock 4627 kernel at the end of its start-up, as evox_entry.cpp does for
  EvoX. The probe runs twice: at the end of start-up and again about 10 s later (a timer or a delay loop in the
  probe; the stock kernel keeps running between).
- The probe records (to its existing output path, serial or the statedump area):
  1. PBUS 0x1214-0x1250 (what the stock 2BL wrote);
  2. a measurement of A and B the way spec 21 §3.4 does it (0x1220 ← 0x000F0F0F, the eight selectors through
     0x1228 with the 10 000-tick waits, read 0x121C, 0x1220 ← 0), without writing any pad register;
  3. a comparison of the stock kernel's read-only code section in RAM with the decompressed kernel image: the host
     generates the expected bytes (or a CRC-32 per 4 KB page) from xbox-files/xboxkrnl-4627.exe (the
     decompressed 4627 kernel; its .text bounds come from the PE section table, checked with REA), the probe compares and reports each changed byte as
     textguard does (address, expected, found), at most 40.
- Warm the console into the band with OXOS boots first (A is logged only by OXOS's 2BL), then switch the netboot
  server to the stock image for the counted boots; alternate one OXOS boot every 5 stock boots to confirm A is still
  in the band.
- Outcomes: the stock kernel shows changed bytes too = this console's DRAM interface is marginal when warm and the
  OXOS deviation is chosen by E1/E2; stock stays clean in 40 band boots with 0x99999999 written = the cause is in
  what OXOS does after the 2BL, and E3/E5 point at it.

## 5. E1 — more pad drive in the band (intentional deviation from the reference; one arm per value)
- Switch: `XK_EXPERIMENT_PADS_DRV=0xA9A9A9A9` (arm 1) and `=0xAAAAAAAA` (arm 2) for boot/bl.cpp.
- Where: BlPadCalibrate, after the nine values are computed and before the {offset, value} array is built for the
  write window (§3.6a unchanged): replace the values for 0x122C, 0x1234 and 0x1238 with the switch's value when the
  computed value is 0x99999999 (i.e. A ≥ 0x5C); everything else as computed. Record the forced value and a flag in
  the 0x2300 record (add a field) and log it.
- Why these values: they are the reference table's own next rows (spec 21 §3.8: A 0x5A-0x5B gives 0xA9A9A9A9, A
  0x58-0x59 gives 0xAAAAAAAA), and the only boots with that setting so far were nearly always clean (1 of 53).
- Reflash the Modxo with the arm's netboot image (tools/modxo, the stty / udisksctl route if flash-uf2.sh does not
  reset the Pico); keep the previous image.
- Pass: fixed per §1.5. If arm 1 fixes it, arm 2 is not needed.

## 6. E2 — more frequent refresh (intentional deviation; hypothesis)
- Switch: `XK_EXPERIMENT_REFRESH_X2` for boot/start.S.
- Where: start.S after the memory-size probe (label 13), still with the caches off: PFB 0x100228 ← the X-codes'
  value with bits 15:5 halved, i.e. 0x081205FF → 0x081202FF; read it back into a free word of page 2 (next to the
  0x2380 splash record) for the kernel to log.
- Hypothesis, not established from the binaries: bits 15:5 of 0x100228 hold a refresh interval in some clock
  unit (the X-codes write the register once, after the mode-register sequence and 0x100210, and nothing reads or
  changes it; spec 22 §6.3/§6.3a). If the console does not boot, or the memory probe / kernel misbehaves, stop the
  arm and record that.
- Interpretation: fixed = charge loss with refresh running too slowly for this console when warm; no effect =
  refresh missing for the failing cells, or the read path (see E4).

## 7. E5 — retention with no activity (serial builds)
- Switch: `XK_EXPERIMENT_CELLTEST` (kernel/src/ke/textguard.cpp KiCellTest, exists), changed to run only in band
  boots (A from the 0x2300 record in 0x5C-0x5E; otherwise log "celltest: skipped, A = ...").
- What: two arms, fill pattern 0x00 and 0xFF, over the free frames (not only the code frames: every frame on the
  free lists that is below the top 64 KB, taken off the lists for the test and put back after), interrupts off,
  10 s idle in a loop that runs from the cache (no memory access), compare, report up to 64 changed bytes and the
  total. Three rounds per boot.
- Interpretation: changes with no activity = retention / refresh; none = something the running kernel does (then
  E3 is the lead).

## 8. Order
1. E3 and E4 (one build; information on every bad boot from then on).
2. E6 (needs experimental/statedump and retail-netboot work; it decides whether OXOS deviates at all).
3. E1 arm 1, then (only if needed) arm 2.
4. E2.
5. E5.

## 9. Results
| Arm | Band boots | Bad | Decision | Date / notes |
|---|---|---|---|---|
| baseline (retail calibration, OXOS serial kernel) | 144 | 21 | - | 2026-10-28 |
| E3 + E4 (information) | 40 | 8 | - | PFB 0x100210 = 0 at kernel entry in all 8 bad boots, set in all 49 good ones (refresh off); E4: the changed byte reads back the same 3x and stays good once rewritten (stored charge loss, one-time) |
| refresh marks through the boot chain | 44 | 1 | - | the bad boot had 0x100210 = 0 already at start.S entry: lost in the X-code stage |
| refresh check (spec/oxos-internal/20) | 56 | 0 | fixed | start.S found refresh off on 6 boots and set it; those boots were clean |
