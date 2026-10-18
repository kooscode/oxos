# Cerbios spec 13: smaller features

Studied in Cerbios 3.1.0 Beta: the kernel's file-open, read, write and symbolic-link paths, the SMC eject handler, the
graphics hooks and screenshot writer, the extension's title-patch and hook installers. Behaviour and formats only.

## 1. BLOCKDASHUPDATE (+0xE2)
- When a file named `y:\xboxdash.xbe` (compared case-insensitively) is opened with read access, the kernel remembers
  the handle.
- A read of that handle at offset 0 records the certificate's file offset (header +0x118 − header +0x104). A later
  read at exactly that offset has the dword at **+0xAC of the returned data** (the certificate's version field)
  replaced by **0x185EAD00**, and the handle is forgotten.
- Only the first such read pair per open is changed; the file itself is never written.

## 2. READONLYC (+0xE8)
A write request (IRP major function 3) to **disk 0, partition 2** (C:) fails with STATUS_MEDIA_WRITE_PROTECTED
(0xC00000A2). Reads and other partitions are unaffected.

## 3. TDATA/UDATA redirect (TUDATAREDIR +0xF5, TUDATAREDIRHDD +0xF6, TUDATAREDIRPART +0xF7)
- Applied to every file open (on the object name) and every symbolic-link creation (on the target), when
  TUDATAREDIR is set, the running title is not one of 0x41560010 (Return to Castle Wolfenstein: Tides of War),
  0x41560051 (Call of Duty 2: Big Red One), 0x4156005D (Call of Duty 3) or 0x4D53006E (Forza Motorsport), and the running XBE is not
  `\Device\Harddisk0\Partition2\xshell.xbe`.
- If the name begins with `\Device\Harddisk0\partition1\UDATA` or `…\TDATA` (34 characters, compared
  case-insensitively), or if the link is `\??\C:` with target `\Device\Harddisk0\partition1`, the name is
  changed **in place**: the disk digit (character 16) becomes '0' + TUDATAREDIRHDD and the partition digit
  (character 27) '0' + TUDATAREDIRPART (1–9).

## 4. LIMITMEM
See spec 02 §4 (DISABLELIMITMEM).

## 5. Graphics hooks and screenshots (ENABLESCREENSHOTS +0xE9, SCREENSHOTHDD +0xEA, SCREENSHOTPART +0xEB)
- **Hooks** (installed at title start when screenshots or the in-app LCD are enabled): the extension scans the
  title's `D3D` section for four routines and its `.text` for two GPU-register write sequences (all identified by
  byte patterns), and overwrites each with a 5-byte JMP to a kernel handler. The handlers count frames (FPS for the
  LCD) and run the screenshot capture at present time. The layout of the title's graphics device object differs
  by XDK build, so the D3D8 library build from the XBE (spec 09 §3) selects one of several sets of five field
  offsets (front buffer, its surface description, gamma ramp, …).
- **Request:** the IGRSCREEN combo (spec 08) sets a flag; the next present runs the capture and clears it.
- **Capture:**
  - the other threads of the title are suspended for the duration;
  - the front buffer's surface description gives width ((bits 0–11) + 1), height ((bits 12–23) + 1), pitch
    (((bits 24–31) + 1) × 64) and format; two titles (0x56550026 The Chronicles of Riddick: Escape from Butcher Bay, 0x4D53006E Forza
    Motorsport) take the address from the GPU's scan-out register (0xFD600800) instead;
  - formats: 0x11 (16-bit 5-6-5), 0x1C (16-bit 1-5-5-5), 0x12 and 0x1E (32-bit; alpha ignored); channels are
    expanded to 8 bits; if the device has a gamma ramp, each channel goes through it;
  - 1920 × 540 and 640 × 240 buffers (field rendering) are written as 1920 × 1080 and 640 × 480 by doubling each
    line;
  - output: an uncompressed **24-bit BMP** (54-byte header, rows bottom-up) at
    `\Device\Harddisk<SCREENSHOTHDD>\Partition<SCREENSHOTPART>\Cerbios\Screenshots\<title ID, 8 hex digits>\<YYYY-MM-DD HH-MM-SS>.bmp`
    (directory created first), written in 1020-byte chunks; then the threads resume.

## 6. RESETONEJECT (+0xE4)
- At each title load the SMC's reset-on-eject control (SMC register **0x19**) is set: **0 (reset on eject)** for a
  non-system title started from a disc (not the dashboard or boot animation, not a CDBOOT debug boot, certificate
  allowed-media bit 31 clear, not a debug-region title), else **1**. Boot flag 0x08 mirrors "register is 1"; with
  RESETONEJECT off the register is simply set to 1 once after a cold boot.
- When Eject is pressed while a disc title runs with this enabled, the SMC eject handler sets bit 0 of **EEPROM byte 0xFF**
  (a marker, also kept in RAM) and then continues with the normal eject handling. At the next boot the marker is cleared, and it stops
  the "Eject held" check from entering safe mode (spec 01 §4).

## 7. APPLYTITLEPATCHES (+0xE7)
Before the title starts, the extension checks the title ID against eleven entries. For each entry it checks that the
expected bytes are at a fixed address of the loaded image (often trying two addresses, one per game version), and
only then patches. Names come from the title-ID database (titleIDs.db, mobcat.zip/XboxIDs); the fixes are described
from the code:

| Title ID | Game | Condition | Kind of fix |
|---|---|---|---|
| 0x41560010 | Return to Castle Wolfenstein: Tides of War | always | changes part of one routine (two game versions) |
| 0x434D0050 | TOCA / DTM / V8 Supercars Race Driver 3 | always; one extra change only when not started from the DVD | replaces a call with a fixed value and removes three code runs |
| 0x45410042 | 007: Everything or Nothing | always (location depends on the certificate region) | removes one conditional jump |
| 0x4541038B | The Sims 2 | 64 MB console, HDTV pack, 720p and widescreen enabled, AV standard 1 or 2 | turns a conditional jump into an unconditional one |
| 0x48450001 | Rugby League | always | makes one routine return at once |
| 0x4D4A000C | Ultra Bust-A-Move / Ultra Puzzle Bobble | CPU faster than 734 MHz (spec 12 §3) | skips one call (two game versions) |
| 0x4D530031 | Kung Fu Chaos | always | changes two comparisons |
| 0x53430007 | Rolling | always | makes one routine return at once |
| 0x56550013 | Total Immersion Racing | always | makes one routine return at once |
| 0x56550039 | The Incredible Hulk: Ultimate Destruction | always | replaces one routine with one returning a fixed value (two game versions) |
| 0x56550041 | Crash Tag Team Racing | always | changes one instruction in two places (two game versions) |

The patch bytes and addresses are not reproduced: they are compatibility data, to be derived again by testing each
game (the condition column says when the fix is needed).

## 8. Region and parental control
Region-free reporting is in spec 10 §6; the parental-control reset in spec 06 §2 step 3.

## Open
- The exact routines the D3D hooks replace, per XDK build.
- Which IRP field identifies the partition in §2 (the check is device extension +0x28 = 0 and partition number 2).
