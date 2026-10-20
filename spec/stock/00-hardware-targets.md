# Spec 00 — Real-hardware targets and constraints

## Revisions supported by the reference kernel (1.0.4627.1)
Observed: the video code (0x8002f…–0x80030…) talks over SMBus to only two TV encoders:
- **0x8A Conexant CX25871**: calls at 0x8003035c, 0x800303be, 0x800305c4, 0x8003060c, 0x80030921
- **0xD4 Focus FS454**: calls at 0x8002fb21, 0x8002fbf2, 0x8002fe5c
- No Xcalibur (1.6) encoder code was found. The only `push 0xE0` in the image is the IDE device/head
  value at 0x80024621, not an encoder.
=> This kernel targets console revisions **1.0–1.5**. Supporting **1.6** means also reversing a
later kernel (5713/5838) for its Xcalibur and 1.6-specific code (done for the encoder driver: spec 19; OXOS's own
encoder support: spec/oxos-internal/tv-encoders.md, Conexant and Focus, hardware testing TBD).
User-supplied MCPX ROM = **MCPX 1.0** (the version used in 1.0 consoles).

## Boot chain on hardware (what our image must satisfy)
flash (256 KB bank, mirrored x4 in a 1 MB image)
 -> MCPX hidden ROM: runs the X-codes, then RC4-decrypts the 2BL (MCPX 1.0)
 -> 2BL: checks the kernel digest, decrypts and LZX-decompresses the kernel, then jumps to the PE entry
 -> kernel entry (spec 01)
Project decisions (user, 2026-10-14):
- Target revisions **1.0–1.5** now; add **1.6** later.
- **Image size is not a hard limit.** Modchips with 512 KB / 1 MB banks are fine, so we are not held to
  the stock 256 KB layout. (Flashing the onboard TSOP of a 1.2+ console, which has a 256 KB flash, would
  bring the limit back. Not a goal for now.) Reference sizes: kernel compressed 0x33C5E (~207 KB),
  data 0xA00.
- Single-image "runs on any revision" BIOSes exist in the community. Supporting the different MCPX
  handoffs in one image is a later goal, alongside 1.6.
- **Testing is xemu-only** until a sizeable, testable chunk exists; hardware testing comes after that.
- When reusing the stock 2BL, the repacker must recompute the kernel digest (the unpacker already
  verifies it: "2BL kernel digest: MATCH").
- A fully open boot chain also needs our own 2BL. The MCPX 1.0 and 1.1+ ROMs hand off differently
  (1.1+ uses a different check on the next loader), so supporting every revision is extra work.

## Hardware behaviour that xemu does not enforce
- **Microcode load** (spec 01 §2.3) and **MTRR/cache setup**. Wrong cache settings boot but run
  very slowly on hardware.
- **Timer calibration** for `KeStallExecutionProcessor` and the performance counter (PIT and the ACPI PM timer).
- **Video encoder init**: wrong settings mean no TV picture on hardware even when the kernel is fine.
- **SMC protocol** (SMBus 0x20): LED, tray, fan, reset/shutdown. A missing reply can trigger the error ("FRAG") LED.
- **IDE PIO/UDMA timing** and drive quirks, USB OHCI, NIC PHY.

## Debugging on hardware
- **SMC LED as POST codes** via SMBus 0x20 LED registers. This works before video is up.
- **Port 0x80 POST codes**: visible with an LPC debug or POST board, or a modchip that has a POST display.
- Always flash with recovery available: a dual-bank modchip, or a split TSOP with a stock bank that
  is never overwritten. Every image is tested in xemu first, then on hardware.
