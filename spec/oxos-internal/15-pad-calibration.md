# 15. NV2A pad calibration in the OXOS 2BL

Implemented 2026-10-26. Behaviour: spec/stock/21 §3.7a (Cerbios's sequence for NV2A < D3), matched write for write.

## 1. Where
`BlMain` (kernel/boot/bl.cpp) follows Cerbios's (and the retail 2BL's) order: device 1 0x80 byte ← 2, SMC challenge
(with SMC 0x01 ← 0), device 1 0x8C ← 0x40000000, method selection and `BlPadCalibrate`, `BlSetUpChipset` (device 1
0x54, host 0x64, the host 0x6C LDT toggle, host 0x80, then the revision ≥ D1 write), front LED, network download
(netboot builds), Inflate. Not done: the "SHADOW" pre-check (our 2BL runs only on a cold boot) and the retail 0xCC
fill of 0x80090000 (Cerbios does not do it either).

## 2. Table
- Cerbios's layout (0x135 bytes), recorded in spec/stock/21 §3.9 and built into the X-code area at the flash offset in
  header 0x7C (0xC90; 0xDC0 in MCPXREVD5 images) from kernel/boot/padcal_table.inc, read through the 16 MB flash
  window as Cerbios does. Like the video encoder tables, it is a functional hardware constant
  (dmca-compliance.md §5).
- A table whose breakpoints do not increase makes the 2BL return before touching a register (Cerbios would divide
  by zero), so the X-codes' values stay.
- NV2A D3 and later (1.6) without upgraded memory: Cerbios's EEPROM method, not implemented; values stay.

## 3. Diagnostics
- The 2BL leaves a record at physical 0x2300: 'PADC' (or 'PADN' = not calibrated), A, B, the strap, the nine values.
  The kernel copies it before the memory manager starts; the debug log prints it ("nv pads: 2BL calibration ...")
  and the nine registers after the memory manager and at the title launcher.
- tools/padcal_check.py runs Cerbios's routine and ours in an emulator and compares their NV2A writes.

## 4. Results (no-DVD 1.4 console)
Calibration ran on every boot (A 0x5E-0x63, B 0x5D-0x61; values in spec/stock/21 §3.8; first row set, identical for
the 4627 and Cerbios tables). The netlog build loaded the dashboard 3/3. The serial-only build still hangs at
"load: first page read" of xboxdash.xbe, as it did before calibration: a separate bug.
