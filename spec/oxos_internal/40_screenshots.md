# 40. Screenshots: a kernel extension that saves what is on screen (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "a screenshot kernel extension"). Nothing here is decided until the user says so.

While any title runs, a button combination (or a command from a PC, spec 41) saves the picture on screen as an image file on the hard disk, without the
title's help and without changing it. Cerbios has screenshots too, by hooking each title's graphics library (spec/cerbios/13 §5); OXOS would read the frame
the display is showing instead, which needs nothing from the title.

## 1. What the user sees
- **Take one:** hold a combination on any controller (to choose; it must not clash with the in-game reset's LT + RT + BACK + BLACK, spec 22), for example
  BACK + BLACK + the right thumbstick click. A toast says "Screenshot saved" (spec 36, when toasts exist), or the front LED blinks once.
- **Where:** `C:\OXOS\screenshots\<title ID>\<date time>.png` (on an exFAT disk `/C/OXOS/screenshots/...`): OXOS's files are only ever in `C:\OXOS`.
- **Settings** in the extension's own file, `C:\OXOS\extensions\oxos_screenshots.ini` (spec 34 §3.1):
```ini
; the combination
buttons = back black right_thumb
; png (smaller) or bmp (faster to write)
format = png
```

## 2. How it works
- **An extension, `oxos_screenshots.xos`:** its run loop (spec 34 §4.3) reads the controllers' buttons the way the in-game reset sees them, and on the
  combination takes the picture.
- **Which picture:** the frame the display is scanning out: the CRTC's start address (NV2A `0x600800`), its width, height and pitch from the CRTC, its pixel
  format (16- or 32-bit) from PRAMDAC; the same registers the toasts proposal reads (spec 36 §3).
- **Copy, then write:** the frame is copied at vertical blank, a band of lines at a time, so a double-buffered title's next frame does not tear the picture,
  then compressed and written from the run loop. The title keeps running; writing takes a moment of disk time.
- **Memory:** a 640x480 picture is 1.2 MB at 32 bits, 720p 3.6 MB. The extension cannot take that from the title. It copies and writes in bands of a few KB, so
  it needs only a small buffer; the picture may then mix two frames in a title that redraws mid-copy (to measure).

## 3. Things to settle before building it
- **The combination**, and whether it is also a command from a PC (spec 41: `oxos screenshot`).
- **What the SDK must give:** reading the controllers from an extension (the in-game reset's view, spec 22), the display registers, and PNG compression
  (a small encoder in the extension, no C library beyond the SDK's).
- **HD and field modes:** 1080i shows one field at a time; the picture is the whole frame, both fields.
- **Tiled or swizzled frame buffers:** the display reads linear memory, so what it scans out is linear; to confirm on all three encoders.

## 4. Tests
- xemu: a test title draws a known pattern; the combination on a virtual pad (as `make test_upload` presses X) saves a file; the file decodes to the pattern
  (16- and 32-bit modes, 480 and 720 lines).
- Console: the HDMI capture and the saved file side by side.
