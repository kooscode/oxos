# Cerbios spec 14: debug mode, recovery, fatal-error screens, AV check

Studied in Cerbios 3.1.0 Beta: the extension's debug-monitor loader and dashboard fallback, the kernel's fatal-error
routine and AV-pack check. Behaviour and formats only.

## 1. DEBUG (+0x07): development-kit mode
- At start-up (spec 06 step 16), if DEBUG is set and the kernel debugger is enabled (KdDebuggerNotPresent false;
  the kernel carries the debugger code, section KDCODE), the **debug monitor** is loaded:
  - path: `\Device\CdRom0\xbdm.dll` when booted with `/CDBOOT` (boot flag 0x04), else
    `\Device\Harddisk0\Partition1\xbdm.dll` (E:\xbdm.dll);
  - the file is read whole into memory from the debug-monitor allocator (export 374, MmDbgAllocateMemory; spec 04),
    loaded as a PE image named `xbdm`, its entry point called with a block of kernel pointers (loader list, debugger
    data, three kernel callbacks);
  - if the load returns a failure status, the boot stops with KeBugCheckEx(0x32, status).
- The dashboard becomes the path stored in `\Device\Harddisk0\Partition1\dashboard.xbx` (spec 09 §2).
- The TDATA/UDATA redirect is skipped while `\Device\Harddisk0\Partition2\xshell.xbe` runs (spec 13 §3).

## 2. Recovery XBE
`\Device\Harddisk0\Partition1\Cerbios\Recovery\default.xbe` (E:\Cerbios\Recovery\default.xbe) is the second choice
for the dashboard, and the **first** in safe mode (spec 09 §2).

## 3. Fatal errors
Called with an error code (for example 13 when no dashboard loads):
- The 16-byte key area the title loader uses for key derivation (spec 09 §3) is cleared.
- The whole EEPROM is read. **UDMA safety net:** for codes **9 and 13**, if the EEPROM's pending-error bytes
  (offset 0xFA code, 0xFB flag) are both 0, the code is stored at 0xFA with flag 1 at 0xFB, the EEPROM is written,
  and the screen shows a message giving the error code, saying that bad UDMA settings may be the cause, that the
  console reboots into safe mode, and that the BIOS should be flashed with another UDMA mode; after 15 seconds SMC register 0x02 = 0x01 (reset)
  reboots into safe mode (spec 01 §4 reads the pending bytes back).
- Otherwise the pending flag is cleared; for codes ≥ 5 bit (code − 5) of the 16-bit error history at EEPROM offset
  0xFC is set; the EEPROM is written. (On manufacturing-region consoles, region bit 0x80000000, the console is then
  power-cycled: SMC register 0x02 = 0x40.)
- The splash is drawn with the code and an explanation chosen by code:

  | Code | Subject of the explanation |
  |---|---|
  | 2 | EEPROM check failed |
  | 4 | RAM check failed |
  | 5 | Hard disk drive (HDD) not locked |
  | 6 | Incorrect hard drive password |
  | 7 | Hard drive timeout |
  | 8 | No hard drive found |
  | 9 | Hard drive parameters are missing or incorrect |
  | 10 | DVD drive timeout |
  | 11 | No DVD drive found |
  | 12 | DVD drive parameters are missing or incorrect |
  | 13 | Dashboard failed to launch due to missing/bad key … |
  | 14 | Dashboard failed to launch (generic error) |
  | 16 | Internal clock cannot be set |
  | 20 | Dashboard failed to launch (cold boot) |
  | 21 | Unspecific/generic error |
  | other | Unknown |

  Each text is a "Problem / Cause/Solution" paragraph, wrapped at the screen width; the wording is Cerbios's own
  and is not reproduced here.
- The thread then ends; the console stays on the error screen.

## 4. AVCHECK (+0x06)
On a cold boot, if AVCHECK is set and **no AV pack** is connected (internal code 0, spec 10 §1) and the console is not
a manufacturing-region console: SMC register 0x1B is cleared, and after **300 seconds** the console is powered off
(SMC register 0x02 = 0x80), repeating if it is still on.

## For OXOS (notes, not Cerbios behaviour)
- The UDMA safety net generalises: record a "pending" error in EEPROM before rebooting into a conservative mode, and
  clear it once the next boot gets past the point that failed.
- The xbdm load block should be specified from a stock debug kernel, which passes the same kind of block.

## Open
- The exact contents of the pointer block passed to xbdm's entry point.
- Whether code 13's "+ reason" variant (spec 09 §2) can reach codes above 21.
