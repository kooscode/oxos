# EvoX M8 spec 02: launching titles

What EvoX changes in how the kernel chooses, checks and starts a title (spec 00 for sources, status marks and where
the code lives).

## 1. Dashboard launcher
Replaces the retail "launch the dashboard" step (spec/stock/05 §3); the XBE loader's tail (0x8002F704) goes to the
resident module's launcher (0x8003E7C0; INIT copy 0x800B4930 before the install). The retail XBE launch routine
(0x8002F26D, path, 1, 0) is tried for, in order (paths at 0x8003ED44…): **[C]**
1. `\Device\Harddisk0\Partition2\evoXdash.xbe`
2. `\Device\Harddisk0\Partition1\Dash\UnleashX\unleashx.xbe`
3. `\Device\Harddisk0\Partition1\Dashboard\default.xbe`
4. `\Device\Harddisk0\Partition2\xboxdash.xbe`
The first that loads wins. If none does and this is a cold boot: SMC LED sequence 0xE1, then the retail fatal error
(13, 13 + launch-data type, or 20 when a title disc is present; spec/stock/05 §6), then reboot (0x8001542D(4)). **[C]**

## 2. Boot-time disc path; flag 0x8003C2F4
- The string the boot-time "launch the disc" path uses, `\Device\CdRom0\default.xbe` at 0x800127D3, is cut to
  `\Device\CdRom0\`: that path no longer names a file **[C]** (effect on the title choice **[I]**).
- INIT 0x8005531E: a `jne` is removed, so the flag byte 0x8003C2F4 is set to 1 regardless of bit 0 of a value read
  just before; the XBE loader tests 0x8003C2F4 **[C]**; meaning **[O]**.

## 3. Unsigned XBEs
- XBE header signature: the RSA check (`XePublicKeyData`, SHA-1 of 0x10104…) and the loader code around it
  (0x8002F057–0x8002F192, 0x8002F272–0x8002F70E, several runs) are rewritten; the check is gone and the freed space
  holds EvoX code (the dashboard launcher of §1, partition 6 / "F:" sizing, spec 03 §2). **[C]** (Remaining loader
  detail **[O]**.)
- Section digests: at 0x8002EF6D retail compares each loaded section's SHA-1 with the header's digest (`repe cmpsd`,
  failure → 0xC0000190); EvoX copies instead (`rep movsd`) and branches unconditionally. **[C]**

## 4. Flat code segment
Retail 0x8001FF28 writes a code-segment descriptor (0x8003C2A0) and reloads CS; it is used once (0x8001EC2A) to
**shrink the CS limit** after start-up. EvoX makes it return at once (`ret 8`): CS stays flat 4 GB, so code may run
above the old limit (the resident module, code a title loads). **[C]**

## 5. Macrovision off
`AvSendTVEncoderOption` cases 1 (Macrovision level, 0x800308E5) and 0x0A (Macrovision commit, 0x800308FA) jump to
the function's end: no copy protection is programmed into the encoder. **[C]**

## Open questions
- The meaning of flag 0x8003C2F4 (§2); the remaining XBE loader detail (§3).
