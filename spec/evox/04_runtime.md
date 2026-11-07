# EvoX M8 spec 04: while a title runs

Fan / LED control and the in-game reset (spec 00 for sources, status marks and where the code lives). Both are in the
resident module.

## 1. Fan and LED at title launch
In the phase-1 thread, right after the XBE's entry point has returned (it starts the title's threads), and before
the thread ends itself (`PsTerminateSystemThread`), retail calls 0x80021DC0; EvoX calls 0x8003E6C0 instead (patch at
0x80015A71): **[C]**
- if the LED byte (0x8003E640) ≠ 0x0F: SMC command 0x08 (LED sequence) = the byte, then 0x07 (LED mode) = 1;
- if the fan byte (0x8003E641) ≠ 0x0A: SMC command 0x06 (fan speed) = the byte, then 0x05 (fan mode) = 1 (manual).
Each write goes straight to the MCPX SMBus host (I/O 0xC000–0xC008: address 0x20, command, data, start 0x1A) and is
followed by a ~1 000 000-iteration busy wait; no status is checked. (0x0F / 0x0A mean "leave alone".)

## 2. In-game reset (IGR)
Installed as a hook on `KeRaiseIrqlToDpcLevel`: every call goes through 0x8002454A (patch at 0x80013D04), which
jumps to 0x8003F07A once the module is installed. The code (0x8003EFC0–0x8003F1BF) is stored bit-inverted in the
image and restored by the installer (spec 00 §3). **[C]**
- It acts only when the caller's code at the return address begins with the bytes `8B 54 24 0C` (identifies the
  gamepad input path, **[I]**), reading the caller's report structure (pointer at the caller's stack +0x18, report at
  +0x14).
- The report is the standard gamepad state: a 16-bit button word (0x01 up, 0x02 down, 0x04 left, 0x08 right,
  0x10 START, 0x20 BACK, 0x40 left thumb, 0x80 right thumb) followed by 8 analog bytes (A, B, X, Y, BLACK, WHITE,
  left trigger, right trigger).
- **Combo**: both triggers fully pressed (analog bytes 6 and 7 == 0xFF) together with BACK + START (and A, B
  released), or with BACK + BLACK (BLACK non-zero) → reset.
- **Sequence**: a 14-step table of button masks at 0x8003F042 (up, up, down, down, left, right, left, right, left
  thumb, right thumb, BACK, START, left, left); each change of the button word that matches the current step's mask
  advances the count (a word above 0xFFFF in the combined dword restarts it); after the 14th step the reset happens
  only while the running XBE's header holds `EvoX` at +0x180 (the EvoX dashboard) **[I]** for the exact matching rule.
- **Reset**: if the flag at 0x8003F1BC == 1, `HalReturnToFirmware`-style reboot 0x8001542D(2) (back to the
  dashboard); otherwise SMC command 0x02 = 1 and halt.
- Then (or when nothing matched) the original `KeRaiseIrqlToDpcLevel` body runs: old IRQL returned, IRQL = 2.

## Open questions
- Where the LED / fan bytes and the IGR mode flag are set (they are fixed bytes in this image; the EvoX
  configuration file is handled by the dashboard, the kernel holds only the bytes) **[O]**.
