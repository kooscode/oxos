# EvoX M8 spec 02: the boot module

Clean-room behaviour description (spec 00 for sources and status marks). Start-up-only code and data in new INIT
bytes 0x800A8CA0–0x800B252F; freed with INIT after start-up.

## 1. EEPROM key autodetection (0x800B1466) — see spec 00 §4 **[C]**

## 2. Video encoder detection (0x800B1B76)
Replaces the retail 5838 detection, which reads flash offset 0x78 (the 1.6 BIOS's marker, spec/stock/17 §1a): EvoX
probes the SMBus instead and sets the kernel's hardware flags (`XboxHardwareInfo.Flags`, 0x8003C150), then the retail
routine continues after its flash read: **[C]**
1. read from 0xE0 (Xcalibur) succeeds → flags |= 0x400, "Found XCalibur Encoder";
2. else read from 0xD4 (Focus) succeeds → flags |= 0x10 | 0x20, "Found Focus Encoder";
3. else read from 0x8A (Conexant) succeeds → "Found Conexant Encoder"; else "No TV Chip Found".
Messages go through `DbgPrint`. A byte at 0x8003C15C is set to 1 first (meaning **[O]**).
Consequence: one image runs on every revision with the 5838 kernel's per-encoder code.

## 3. Boot animation hooks
Wrappers that run new code, then the retail routine: 0x800B14FB (→ 0x800B1C46, then 0x80057242), 0x800B1507
(→ 0x800B1D86, then 0x800566FB), 0x800B1513 (→ 0x800B24C6, then 0x8003DDF0). Strings "-> App Init",
"CreateVertexBuffer_Logo", "<- App Init", "App Shotdown" and a block of vertex-like data at 0x800A98CA–0x800A9BFF
point to a **custom logo drawn into the boot animation** with the retail GPU code; plus one 3-byte constant changed
in the animation (spec 00 §5.2, a colour **[I]**). Details **[O]**.

## 4. Disk set-up entry points
0x800B2770 / 0x800B2784 are the INIT copies of resident-module table entries 0 and 4 (spec 01 §2–§3), used because
the disk is set up before the module is installed. **[C]**
