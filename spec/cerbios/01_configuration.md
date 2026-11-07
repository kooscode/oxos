# Cerbios spec 01: configuration (built-in record, cerbios.ini, safe mode)

Studied in Cerbios 3.1.0 Beta (kernel .data, the extension's ini reader and the safe-mode check). Behaviour and
formats only.

## 1. The configuration record
One record holds every setting. It lives at the **start of the kernel's .data section** (0x800420C0 in 3.1.0), so the
values flashed in the BIOS image are the built-in configuration;
`cerbios.ini` then overrides individual fields at boot.

| Offset | Size | Field (ini key) | Type | Built-in default (3.1.0 release image) |
|---|---|---|---|---|
| 0x00 | 4 | magic `CERB` | | |
| 0x04 | 1 | record format | | 2 |
| 0x05 | 1 | LoadConfig (no ini key: read cerbios.ini at all) | flag | 1 |
| 0x06 | 1 | AVCHECK | flag | 1 |
| 0x07 | 1 | DEBUG | flag | 0 |
| 0x08 | 1 | DRIVESETUP | number | 1 |
| 0x09 | 100 | DASHPATH (alias DASHPATH1) | path, ≤ 99 chars + NUL | `C:\evoxdash.xbe` |
| 0x6D | 100 | BOOTANIMPATH | path, ≤ 99 chars + NUL | `E:\Cerbios\BootAnims\Xbox\BootAnim.xbe` |
| 0xD1 | 4 | FRONTLED | exactly 4 chars | `GGGG` |
| 0xD6 | 1 | OVERRIDEFAN | flag | 1 |
| 0xD7 | 1 | FANSPEED | number | 0 |
| 0xD8 | 1 | UDMAMODEMASTER | number | 2 |
| 0xD9 | 1 | UDMAMODESLAVE | number | 2 |
| 0xDA | 1 | FORCE480P | flag | 0 |
| 0xDB | 1 | FORCEVGA | flag | 0 |
| 0xDC | 1 | FORCEFFILTER | number | 6 |
| 0xDD | 1 | INAPPLCDENABLE | flag | 0 |
| 0xDE | 1 | LCDI2CADDR | number | 0x3C |
| 0xDF | 1 | LCDPROTO | number | 0 |
| 0xE0 | 1 | LCDBUS | number | 0 |
| 0xE1 | 1 | RTCENABLE | flag | 0 |
| 0xE2 | 1 | BLOCKDASHUPDATE | flag | 0 |
| 0xE3 | 1 | XONLINEDASHREDIR | flag | 0 |
| 0xE4 | 1 | RESETONEJECT | flag | 0 |
| 0xE5 | 1 | ADVCPUSUPPORT | flag | 1 |
| 0xE6 | 1 | DISABLELIMITMEM | flag | 0 |
| 0xE7 | 1 | APPLYTITLEPATCHES | flag | 1 |
| 0xE8 | 1 | READONLYC | flag | 0 |
| 0xE9 | 1 | ENABLESCREENSHOTS | flag | 0 |
| 0xEA | 1 | SCREENSHOTHDD | number | 0 |
| 0xEB | 1 | SCREENSHOTPART | number | 1 |
| 0xEC | 1 | OVERCLOCKING | flag | 0 |
| 0xED | 4 | CPUMPLLCOEFF | number (32-bit) | 0 |
| 0xF1 | 4 | NVPLLCOEFF | number (32-bit) | 0 |
| 0xF5 | 1 | TUDATAREDIR | flag | 0 |
| 0xF6 | 1 | TUDATAREDIRHDD | number | 0 |
| 0xF7 | 1 | TUDATAREDIRPART | number | 1 |
| 0xF8 | 1 | IGRMASTERPORT | number | 0 |
| 0xF9 | 2 | IGRDASH | button mask | 0xC030 (LT RT START BACK) |
| 0xFB | 2 | IGRGAME | button mask | 0xD010 |
| 0xFD | 2 | IGRFULL | button mask | 0xD020 |
| 0xFF | 2 | IGRSHUTDOWN | button mask | 0xC021 |
| 0x101 | 2 | IGRCYCLE | button mask | 0xD001 |
| 0x103 | 2 | IGRSCREEN | button mask | 0x00C0 (LS RS) |
| 0x105 | 1 | splash-screen factor (no ini key; read as a number by the splash code, safe mode uses 1) | number | 1 |
| 0x10A | 1 | XTALFREQ | number | 0 |
| 0x10B | 1 | run-time: set once start-up has finished (not a setting) | | 0 |
| 0x10C | 1 | run-time: safe mode active (§4) | | 0 |

Unaligned fields are deliberate (byte-packed record).

## 2. cerbios.ini
- **Path:** `\Device\Harddisk0\Partition1\Cerbios\cerbios.ini` (E:\Cerbios\cerbios.ini on the master disk).
- **When:** once at boot, only if record LoadConfig (0x05) = 1 and the console is **not in safe mode** (§4).
  At most 64 KB of the file is read.
- **Lines:** a line ends at CR or LF; TAB characters are dropped; `;` starts a comment to the end of the line; empty
  lines are skipped; a line is cut at 512 characters; the last line counts without a final newline.
- **Key = value:** split at the first `=`; anything without `=` is ignored. Key and value are each cut to 255
  characters, then trimmed of whitespace (both ends). The key is upper-cased (a–z only) and compared exactly with the
  key names (so keys are case-insensitive); unknown keys are ignored. Each key overwrites only its own field.
- **Value types:**
  - *flag*: `TRUE` or `FALSE` (case-insensitive); any other text leaves the field unchanged.
  - *number*: decimal, or hexadecimal with a `0x`/`0X` prefix; if anything follows the digits, the field is
    unchanged. Byte fields keep the low 8 bits; CPUMPLLCOEFF/NVPLLCOEFF keep 32 bits.
  - *path* (DASHPATH, DASHPATH1, BOOTANIMPATH): copied as text (up to 99 characters), interpreted when used (spec 09 §1).
  - FRONTLED: used only if exactly 4 characters long.
  - *button mask* (IGR keys): used only if at least 2 characters long; each character is a hex digit k
    (upper-cased first) and sets bit (k XOR 8) of a 16-bit mask. The mask is compared against the controller's
    button state, in which bits 0–7 are the digital buttons (D-pad up, down, left, right, START, BACK, LS, RS) and
    bits 8–15 the analog buttons pressed (A, B, X, Y, BLACK, WHITE, LT, RT). So codes 0–7 select A … RT and codes
    8–F select D-pad up … RS (how the IGR code builds that state is confirmed in spec 08). A combo fires when all
    its buttons are held.
- DASHPATH1 is an older name for DASHPATH (both write the same field).

## 3. Checks after reading the ini
After the ini (or without it) these fields are clamped: XTALFREQ ≤ 2; IGRMASTERPORT ≤ 4; TUDATAREDIRPART and
SCREENSHOTPART 1–9; TUDATAREDIRHDD, SCREENSHOTHDD, LCDBUS and LCDPROTO 0 or 1; FORCEFFILTER ≤ 6; DRIVESETUP ≤ 3;
LCDI2CADDR is doubled (7-bit address to the 8-bit SMBus form); FRONTLED is upper-cased. If FORCEVGA is set or the
AV pack is VGA (pack code 5), FORCE480P is set too.

The feature specs (02 onward) define the behaviour each field controls, from the code that reads it.

## 4. Safe mode
- **Entered at boot when** any of these holds (checked once, cold boot):
  - the SMC tray state read at boot reports the Eject button being held (tray state 0x20, or a non-zero change
    count) and the EEPROM "reset-on-eject happened" marker (spec 13 §6) is clear;
  - otherwise, the EEPROM's last-error bytes (EEPROM offsets 0xFA = error code, 0xFB = flag) hold error **9 or 13**
    with flag 1 (both bytes are then cleared and the EEPROM written back);
  - or boot flag 0x800 is set, which comes from **bit 5 (0x20) of SMC register 0x1B** (the scratch register) read at
    start-up (spec 06 §1): a menu or OS writes that bit and reboots to request safe mode.
- **Effect:** field 0x10C is set (so cerbios.ini is not read), and these fields are forced: DRIVESETUP 1,
  UDMAMODEMASTER 2, UDMAMODESLAVE 2, FRONTLED `RRRR` (red), AVCHECK 0, DEBUG 0, INAPPLCDENABLE 0, ADVCPUSUPPORT 0,
  DISABLELIMITMEM 0, APPLYTITLEPATCHES 0, READONLYC 0, ENABLESCREENSHOTS 0, OVERCLOCKING 0, CPUMPLLCOEFF 0,
  NVPLLCOEFF 0, TUDATAREDIR 0, RESETONEJECT 0. The splash shows a "safe mode" notice, and the recovery XBE
  `E:\Cerbios\Recovery\default.xbe` is tried before the dashboard (spec 14).

## Open
- Exact meaning of field 0x105 (a float factor in the splash animation) and bytes 0x106–0x109.
- The EEPROM error bytes are described in spec 14 §3.
