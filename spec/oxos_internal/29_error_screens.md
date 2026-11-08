# 29. Error screens (OXOS's own)

Status: implemented 2026-11-07. Code: `kernel/src/core/fatal.cpp`, `kernel/src/core/fatal_help.cpp`,
`kernel/src/bootscreen/text.cpp`.

The reference kernel shows a fatal error as a two-digit number and nothing else, and a bug check shows nothing at
all (spec/stock/05 §6). OXOS keeps the codes and what goes with them (LED sequence, EEPROM error history, the halt)
and draws a screen that says what happened and what to try.

## 1. Text
- One font for every OXOS screen: `text.cpp`, the OXOS 5x8 font (drawn for OXOS), printable ASCII 0x20–0x7E, one
  byte per row. Rows 0–6 hold capitals and digits (row 6 is the baseline), row 7 descenders. A glyph is drawn as
  `Scale x Scale` squares, glyphs `6 * Scale` apart (`XkDrawText`, `XkTextWidth`).
- The boot splash's caption and its `[v0.3.16/CONEXANT]` label use the same code (`XkVersionLabel`).
- `text.cpp` is resident (not boot-only, not `-O2`): fatal errors 13, 20 and 21 and bug checks come after the
  boot-only pages are freed (spec/oxos_internal/18).

## 2. Fatal error screen
640x480 X8R8G8B8 in its own contiguous frame buffer, only with an AV pack connected (as before):

| Part | Where | Look |
|---|---|---|
| `OXOS v0.3.16/CONEXANT` | y 36, centred | scale 2, grey |
| the code, two seven-segment digits (our own layout) | y 72–182, x 250–390 | white |
| the title | y 204, centred | scale 3, white; scale 2 when wider than 544 px |
| what happened and what to try | y 244, x 48, 20 px apart | scale 2, light grey |
| details | 8 px below | scale 2, grey |

Lines are at most 45 characters (`XK_FATAL_LINE`, 45 x 12 px fits between 48 px margins); at most six help lines so
the details stay inside the part of the picture a TV shows. A unit test (`tests/unit/test_fatal.cpp`) checks every
code's title and lines.

| Code | Title | Raised when | Details |
|---|---|---|---|
| 6 | Could Not Unlock HDD | SECURITY UNLOCK with this console's key fails | disk model, status |
| 7 | HDD Timeout | the disk is still busy after 30 s | disk model, status |
| 8 | HDD IDENTIFY Failed | IDENTIFY, or the commands after an unlock, fail | disk model, status |
| 9 | No HDD Detected | SET FEATURES gets no answer, or the disk is under 8 GB (0xEE8AB0 sectors) | disk model and size when it answered, else status |
| 13–19 | No Dashboard Found (on a cold boot: the upload server's screen instead, spec/oxos_internal/31) | none of `C:\evoxdash.xbe`, `E:\evoxdash.xbe`, `C:\xboxdash.xbe`, `E:\XBLDashboard\default.xbe` loads (14–19: 13 + the launch data's dashboard reason) | the reason for 14–19 |
| 20 | No Dashboard Found (Disc Inserted) | the same, with an authenticated game disc in the drive | |
| 21 | Previous Boot Failed | a cold boot after `HalReturnToFirmware(4)` (fatal error reboot by a title or the dashboard; SMC scratch bit 1, cleared at every boot) | |
| other | Fatal Error | anything else (e.g. 0x10E5F, the DVD device could not be created) | the full code |

OXOS never raises 5 (non-secure disk, spec/stock/05 "Policy differences") or 10–12 (no DVD drive is allowed).

## 3. Bug check screen
`KeBugCheckEx` records the bug check (CMOS, serial, `KiBugCheckData`) and then calls `XkShowBugCheck`, with
interrupts off. Nothing can be allocated then, so it takes over the frame buffer being scanned out (the CRTC start,
NV2A 0x600800), which is a title's or the splash's:
1. No AV pack, no scan-out address, or 640x480x4 bytes from it would pass the end of RAM: nothing is drawn.
2. The GPU stops drawing into it: PFIFO caches (0x2500), push buffer fetch (0x3220) and puller (0x3250) off,
   PGRAPH FIFO (0x400720) off, the video overlay stopped (PVIDEO 0x8704 = 0x11).
3. The eight PFB tile regions (0x100240 + 16 i) are switched off, so the screen is linear as written.
4. The screen is drawn through the frame-buffer window at 0xF0000000 + address (mapped at all times, write-through)
   and shown as 640x480 X8R8G8B8 with `AvSetDisplayMode` and the screen-on option.

Titles' frame buffers hold at least 640x480 pixels; a 16-bit one is half the size, so the screen runs into the
title's next buffer, which no longer matters.

Content: `Kernel Stopped`; the bug check code; for 0x1E (unhandled exception) the exception code and address; the
four parameters; the running title's ID (its XBE header at 0x10000, certificate +8) when there is one; and "Take a
photo of this screen for a bug report, then turn the console off and on again."

## 4. Cost and checks
- Resident code and data: about 4 KB more (release `.text` 0x23840 → 0x247FC, one page); the boot-only section
  is 0x2A0 bytes smaller (the font moved out of it).
- `make test_fatal` (xemu, no disk): code 9, the digits, the title drawn, the EEPROM history bit.
- Bug check: checked in xemu with a temporary build that bug-checks at the title launcher (over the splash's frame
  buffer) and 25 s into the dashboard (over the dashboard's frame buffer, title FFFF0002 shown); and on a console
  (debug kit, Conexant, 2026-11-07): 25 s into the EvoX dashboard, the screen drawn cleanly over its frame buffer,
  title 9E115330 shown.
- Fatal error screen on the same console: a temporary build raising 13 at the title launcher showed the full screen.
- After a bug check the console stays on until its power button is pressed (the SMC then turns it off itself);
  powering it on again at once (8 s later) gave a reset loop before the kernel on the debug kit, held in reset with
  an orange LED; a power-on after it had been off for 10 s booted normally.
