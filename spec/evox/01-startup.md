# EvoX M8 spec 01: start-up

What EvoX changes between kernel entry and the first title (spec 00 for sources, status marks and where the code
lives). The new code here is in the boot module (new INIT bytes, freed after start-up).

## 1. Kernel entry and keys
- The entry routine (INIT 0x800548F2) no longer copies the two 2BL keys (EEPROM key to 0x8003C140 =
  `XboxEEPROMKey`, second key to 0x8003BFF8); it only clears the 2BL's 0x6000 bytes at 0x80400000 and continues to
  start-up. Its arguments are ignored (the loader passes two zeros, spec 00 §1). **[C]**
- Certificate key: preset in the file at STICKY +0x118 (VA 0x8003BFF8, the slot retail fills with the second key;
  spec 00 §2). **[C]**

## 2. EEPROM key autodetection (0x800B1466)
- Called in phase 1 where retail calls the EEPROM security-section routine 0x800549C8 (patch at 0x8001598D). For
  each of three built-in 16-byte keys — a third key **[O]**, the retail EEPROM key, and the key the retail 5838 BIOS
  carries (inferred: the 1.6 console's) — copy it to `XboxEEPROMKey` and run the retail routine; stop at the first
  that returns 0. If none does: SMC/LED fatal sequence (0x80014F9B with 0xA0), then 0x80021DC0(0). **[C]**
- The retail routine is patched so that a failed security-section check **returns −1** instead of the fatal path,
  and success returns 0 (INIT 0x80054A93). **[C]**

## 3. Quick-reboot `.data` check removed
Retail (0x8001C748) hashes `.data` with SHA-1 at cold boot (digest at 0x8003C2D0) and compares it on a quick reboot,
halting on a mismatch. EvoX makes the routine return at once: the check is gone. **[C]**

## 4. Booting without a DVD drive
The retail DVD start-up treats a missing or failing drive as fatal (spec/stock/04). EvoX makes four failure branches
unconditional or removes them (INIT 0x800556A1, 0x800556E2, 0x800556F8, 0x80055714): **no fatal error without a DVD
drive**. **[C]** (Per-branch detail **[O]**.)

## 5. Video encoder detection (0x800B1B76)
Replaces the retail 5838 detection, which reads flash offset 0x78 (the 1.6 BIOS's marker, spec/stock/17 §1a): EvoX
probes the SMBus instead (hooked at INIT 0x80054922 → 0x800B145A) and sets the kernel's hardware flags
(`XboxHardwareInfo.Flags`, 0x8003C150), then the retail routine continues after its flash read: **[C]**
1. read from 0xE0 (Xcalibur) succeeds → flags |= 0x400, "Found XCalibur Encoder";
2. else read from 0xD4 (Focus) succeeds → flags |= 0x10 | 0x20, "Found Focus Encoder";
3. else read from 0x8A (Conexant) succeeds → "Found Conexant Encoder"; else "No TV Chip Found".
Messages go through `DbgPrint`. A byte at 0x8003C15C is set to 1 first (meaning **[O]**).
Consequence: one image runs on every revision with the 5838 kernel's per-encoder code.

## 6. Boot-animation logo
Wrappers that run new code, then the retail routine: 0x800B14FB (→ 0x800B1C46, then 0x80057242), 0x800B1507
(→ 0x800B1D86, then 0x800566FB), 0x800B1513 (→ 0x800B24C6, then 0x8003DDF0); hooked at INIT 0x800572CE,
0x800572D4 and 0x800566F1. Strings "-> App Init", "CreateVertexBuffer_Logo", "<- App Init", "App Shotdown" and a
block of vertex-like data at 0x800A98CA–0x800A9BFF point to a **custom logo drawn into the boot animation** with the
retail GPU code. One 3-byte constant in the animation changes, 0xA0FF60 → 0xCADE00 at INIT 0x800597AE (a colour,
**[I]**). Details **[O]**.

## Open questions
- The third built-in EEPROM key (§2); byte 0x8003C15C (§5); the logo code (§6); per-branch DVD detail (§4).
