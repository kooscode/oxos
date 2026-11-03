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
Just before the title starts (spec 06 step 22), the extension compares the certificate's title ID with eleven entries.
For each one it checks a dword of the loaded image at a fixed address, often trying two addresses (two game
versions), and patches only when the check matches. All addresses below are title virtual addresses. Names come
from the title-ID database (titleIDs.db, mobcat.zip/XboxIDs).

Each fix was followed into the game's own code (2026-11-03, nine of the games), to find what the patched code does.

| Title ID | Game | Condition |
|---|---|---|
| 0x41560010 | Return to Castle Wolfenstein: Tides of War | always |
| 0x434D0050 | TOCA / DTM / V8 Supercars Race Driver 3 | always; one extra change only when not started from the DVD |
| 0x45410042 | 007: Everything or Nothing | always; the address depends on the certificate region |
| 0x4541038B | The Sims 2 | 64 MB console, HDTV pack, 720p and widescreen enabled, AV standard 1 or 2 |
| 0x48450001 | Rugby League | always |
| 0x4D4A000C | Ultra Bust-A-Move / Ultra Puzzle Bobble | CPU faster than 734 MHz (spec 12 §3) |
| 0x4D530031 | Kung Fu Chaos | always |
| 0x53430007 | Rolling | always |
| 0x56550013 | Total Immersion Racing | always |
| 0x56550039 | The Incredible Hulk: Ultimate Destruction | always |
| 0x56550041 | Crash Tag Team Racing | always |

The tables below give each check as "dword at address = value" and each change as the bytes written.

### 7.1 Ultra Bust-A-Move: the intro waits forever on a fast CPU
| Check | Change |
|---|---|
| 0x506D7 = 0x0064D4E8, else 0x507A7 = 0x0065D4E8 | EB 08 at the matched address |

- **The game:** the intro is a task on the game's update list. Two logos fade in, hold for 120 frames and fade out.
- **When the second logo has faded out,** the task does two things:
  - it removes the logo's draw task;
  - it calls a routine that starts the "now loading" animation. That routine first **sets the loading-finished
    flag to 0**.
- **A separate task polls the loader** and sets that flag to 1, once, when loading has finished. The loading animation
  runs until it sees the flag, then frees the logo textures.
- **The race:** on a fast CPU the loading ends before the logos. The flag is set to 1 and then cleared by the
  animation's set-up, so the animation waits forever.
- **The patch** is a short jump over both calls: the flag keeps its value and the game goes on. The logo draw task
  stays on its list, drawing nothing (its alpha is 0).

### 7.2 Total Immersion Racing: the intro movies are skipped
| Check | Change |
|---|---|
| 0x1FC80 = 0x83EC8B55 | C3 at 0x1FC80 |

- 0x1FC80 plays a movie. It takes a path, replaces its extension with `xmv`, opens it with the XMV decoder (with
  audio), and shows frames until the movie ends or a controller button is pressed.
- The game calls it twice at start-up, for `graphics\anims\logo_30a.xmv` and `graphics\anims\intro_30a.xmv`, and
  ignores the result.
- The patch makes it return at once, so neither movie plays. The code does not show why the movies fail. A test
  should find it (OXOS: XMV playback from an image).

### 7.3 The Incredible Hulk: Ultimate Destruction: the disc's sector size
| Check | Change |
|---|---|
| 0x20A53E = 0x83EC8B55, else 0x209A8E = 0x83EC8B55 | B8 00 08 00 00 C2 04 00 at the matched address (`mov eax, 0x800; ret 4`) |

- The routine returns a drive's sector size: it opens the volume of the path it is given (NtOpenFile), asks for
  FileFsSizeInformation (NtQueryVolumeInformationFile) and returns BytesPerSector, or 0 on an error.
- The patch makes it always return 2048, the DVD's sector size.
- A game run from the hard disk sees 512 (FATX) and goes wrong. A disc or a disc image gives 2048 already.

### 7.4 007: Everything or Nothing: the DVD check
| Check | Change |
|---|---|
| region 1: 0x5BA3F = 0x026A0B75; other regions: 0x5BD0F = 0x026A0B75 | 90 90 at the matched address |

- At start-up the game asks the sector size of `D:\`, with the same routine as Hulk's (§7.3).
- If the answer is 2048 it calls its asset set-up with mode 2 (the DVD), otherwise with mode 0 (a development or
  hard-disk layout).
- The patch removes the conditional jump, so mode 2 is always used.
- The address is chosen by the certificate's region (header +0xA0 = 1 or not). A copy whose region field was changed
  is checked at the wrong address and left unpatched.

### 7.5 The Sims 2: a failed allocation in 720p on 64 MB
| Check | Change |
|---|---|
| 0x1F7AFC = 0x0D8B1775 (and the condition in the table) | EB at 0x1F7AFC |

- **The routine** creates a resource: it calls the graphics manager to allocate it (vtable +0x80), then calls its
  initialiser.
- **If the initialiser fails,** the routine frees the resource and calls the game's fatal-error routine, which shows
  the "disc cannot be read" style error screen.
- **In 720p widescreen** the buffers do not fit in 64 MB. The patch makes the conditional jump unconditional, so the
  failure is ignored and the game goes on.
- The condition reads the video flags (ExQueryNonVolatileSetting): HDTV pack, 720p and widescreen set, NTSC-M or
  NTSC-J. It also requires that MmHighestPhysicalPage is 0x3FFF or less, i.e. 64 MB.

### 7.6 Kung Fu Chaos: field rendering
| Check | Change |
|---|---|
| 0x15DF0F = 0xDA6C358B | the opcode bytes at 0x15DF0F and 0x15DF69 become FF; the bytes at 0x15DF17 and 0x15DF71 become 8D and 8C |

- **Two set-up routines in the game:**
  - one (0xC6E30) switches the display to **field rendering**: 640 × 240 (288 for PAL) with present flag 0x80
    (field);
  - the other (0xC6C30) sets the same screen at 640 × 480 (576), with flag 0x80 cleared.
- **The game calls the field version** in two game states, as `mov esi, [object]; call 0xC6E30`.
- **The patch makes both places** `push [object]; call 0xC6C30`. The opcode becomes a push, and the call's
  displacement is 0x200 lower. So the game stays in full-frame rendering.
- **Why:** field rendering does not work with progressive output (480p, HDTV, or Cerbios's forced 480p/VGA).

### 7.7 Return to Castle Wolfenstein: CPUID clobbers EBX
| Check | Change |
|---|---|
| 0x140C30 = 0x83EC8B55, else 0x143110 = 0x83EC8B55 | at the matched address A: A ← 60 (pushad), A+8 ← 24, A+0x19, A+0x21, A+0x2A, A+0x33 ← 28, A+0x3C ← 61 (popad) |

- The routine is a CPUID wrapper: CPUID with the leaf in its first argument, then EAX, EBX, ECX and EDX stored
  through its second.
- It changes EBX (CPUID's result) and does not restore it, although EBX belongs to the caller.
- The patch adds `pushad` and `popad` around the body and moves the argument offsets past the 32 saved bytes, so
  every register is kept.
- What EBX holds after CPUID depends on the processor, so the damage changes with upgraded CPUs and emulators.

### 7.8 Crash Tag Team Racing: two views kept at 4:3
| Check | Change |
|---|---|
| 0x15788B = 0x8B4052FF, else 0x157B4B; and 0x3D97E = 0x8B4052FF, else 0x3D62E | 48 at check address + 2, at each |

- **Two render paths** set a projection with near 0.1 and far 10.0. They get its aspect ratio from a camera object's
  virtual call +0x40, which returns 16/9 when the display mode is widescreen and 4/3 otherwise.
- **The patch changes the call to +0x48,** which always returns 4/3. So these two views stay 4:3 in widescreen; the
  rest of the game keeps its widescreen aspect.
- **Probably a visual fix.** Which views these are was not seen.

### 7.9 TOCA Race Driver 3: the archive's disc position
| Check | Change |
|---|---|
| 0x1C930D = 0xE856F08B | 0x1C930D ← B8 7C D0 07 00 90 90 90; 0x20831B ← BE 56 34 12 00 8B C6 5E C9 C2 04 00 90 90; 0x1C9CC3 ← 10 × 90; 0xECD8B ← 8 × 90; when not started from the DVD also 0xF6EB7 ← 05 |

1. **Where the archive starts:** the game opens `d:\default.xbe` and takes its physical sort key, i.e. its position
   on the disc (0x208315; see the note below). From it, it works out where its archive `default.big` starts. It logs
   "default.big is at %u".
   - The patch replaces that call with the fixed value 0x7D07C: the position on the original disc.
   - It makes the sort-key routine return 0x123456 straight away.
2. **The disc check:** a check of disc data (a value XORed with 0x538F5B39, compared with a stored one) has its
   failure path removed.
3. **The "D:\" check:** an error path after a check on `D:\` is removed.
4. **When not started from the DVD,** a counter at the start of a string-hash routine is set to 5. Its purpose was not
   found.
5. **Why:** on a rebuilt image or the hard disk, the files are not at their original disc positions.

**The physical sort key**, as the game's library reads it:
- It asks the file's volume for FileFsAttributeInformation (IoQueryVolumeInformation).
- **"FATX"** (name length 4): the key is the dword at FsContext + 0x1C (the first cluster). It is 0 if bit 0 of the
  first byte at FsContext is set.
- **"GDFX"**: the key is the dword at FsContext + 0 (the start sector).
- Any other name is an error (STATUS_INVALID_PARAMETER).

### 7.10 Rugby League and Rolling
| Game | Check | Change |
|---|---|---|
| Rugby League | 0xA2DA0 = 0x0108EC81 (a routine starting `sub esp, 0x108`) | C3 at 0xA2DA0 |
| Rolling | 0xBC550 = 0xA5D43D83 (a routine starting with a compare of a global) | C3 at 0xBC550 |

Both routines are made to return at once. What the routines do is not known.

### 7.11 The four kinds of fix
- **Disc checks** (Hulk, Everything or Nothing, TOCA Race Driver 3): the game expects a DVD's sector size or its
  original file positions. Needed when the game runs from the hard disk or a rebuilt image.
- **Display** (Kung Fu Chaos, Crash Tag Team Racing, The Sims 2): progressive and HDTV modes, widescreen, and memory
  at 720p.
- **CPU** (Ultra Bust-A-Move, Wolfenstein): only on a faster or different processor.
- **Not known** (Total Immersion Racing's movies, Rugby League, Rolling).

## 8. Region and parental control
Region-free reporting is in spec 10 §6; the parental-control reset in spec 06 §2 step 3.

## Open
- The exact routines the D3D hooks replace, per XDK build.
- Which IRP field identifies the partition in §2 (the check is device extension +0x28 = 0 and partition number 2).
