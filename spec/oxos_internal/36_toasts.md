# 36. Toasts: short messages on screen while a title runs (proposal)

Status: **proposal**, not implemented (user, 2026-11-14: "write it up as a proposal ... would be nice if extensions could also popup a toast"). Nothing here is
decided until the user says so.

Today OXOS draws only while no title runs: the splash, the error screens and the file manager (`kernel/src/graphics/`). Once a title starts, the screen is the
title's. A **toast** is one short line of text the kernel shows over the running title for a few seconds, then removes: "Saved", "Extension example_run_loop
stopped", "Hold to reset", a clock. Extensions can show one too.

## 1. What a toast is
- One line of plain ASCII text (the OXOS 5x8 font, `graphics::DrawText`: printable ASCII only), at most about 48 characters, in a box with a dark background in
  a corner of the screen (bottom right by default), for 3 seconds, then gone.
- One toast at a time; others wait in a short queue (8) and follow in order. A toast shown while one is up waits for it; when the queue is full, the oldest
  waiting one is dropped.
- The title keeps running and keeps the controllers: a toast is never interactive.

## 2. Who shows one
- **The kernel**, for its own events, each a decision of its own when it is built: the in-game reset's combination seen (spec/oxos_internal/22), an extension
  unloaded or a slow run loop (spec/oxos_internal/34 §4.3), a log file that could not be written (spec/oxos_internal/35).
- **Extensions**, through the SDK: `oxos_toast(Format, ...)` formats like `oxos_log` (oxos_sdk.h §2) and queues the text. It can be called from `ExtensionStart`
  (the toast waits until the title shows its first frame), from `ExtensionRunLoop`, from a hook or an event. The kernel adds nothing to the text: the extension
  says who it is if it wants to.
- **The user switches them off** with one setting (where it lives is open, §7); then `oxos_toast` does nothing and returns.

## 3. How the kernel draws over a title
The title owns the GPU: its graphics library programs the NV2A, connects the GPU interrupt and swaps its frame buffers at vertical blank. The kernel takes
nothing from it. It writes the toast's pixels into the frame the title is showing, just after the display has started scanning it:

1. **Which frame is shown:** the CRTC's start address (NV2A `0x600800`, which `hardware/video/video.cpp` sets for the kernel's own screens) points at the frame
   being scanned out. Its width, height and pitch come from the CRTC's timing and pitch registers, its pixel format (16- or 32-bit) from PRAMDAC's general
   control; the drawing code writes both formats.
2. **When:** the CRTC's raster position (the line being scanned; `0x600808` on NV hardware, not used by OXOS yet: to check) says when vertical blank has
   passed. The kernel draws then, so the toast is in the frame before the beam reaches it.
3. **Every frame:** a title that double-buffers draws a new frame each time and swaps, so the toast is drawn again into each frame it shows, for as long as it
   is up. When it ends, the kernel stops drawing: the title's next frames are untouched.
4. **Where it runs:** on the extensions' run loop thread (spec/oxos_internal/34 §4.3), which already wakes every millisecond while a title runs, or on a small
   kernel thread of its own when no extension is loaded. No interrupt of the title's is taken over.

The box covers about 300 x 20 pixels (8 KB to 24 KB of writes per frame): small next to what a title draws.

## 4. The SDK
```cpp
// oxos_sdk.h, §2 next to oxos_log: one short line on screen for 3 seconds, over the running title (ASCII; longer text is cut). FALSE when toasts are off.
BOOLEAN oxos_toast(const char *Format, ...);
```
A kernel export of OXOS's own (after 432, spec/oxos_internal/34 §9.1) takes the formatted text and queues it; the SDK's library formats it, as `oxos_log` does.

## 5. Risks to settle before building it
- **Tiled or compressed frame buffers:** NV2A frame buffers can sit in a tiled region (PFB tile registers). Whether CPU writes through the frame buffer's
  address land in the right pixels in a tiled region must be checked on hardware; if not, the toast must untile its writes or skip such frames.
- **Unusual modes:** HD modes (720p, 1080i), field rendering (interlaced titles that scan one field per vertical blank), 16-bit buffers with odd pitches, and
  titles that move the CRTC start mid-frame. Each needs a test title or a game on the test consoles.
- **Tearing:** a frame the title finishes drawing after the kernel wrote into it overwrites the toast for that frame: a flicker, not a crash. Drawing at the
  right raster line keeps it rare.
- **Memory parity:** the drawing code, the queue and the font are resident (the font already is: `graphics/text.cpp`). The release kernel's headroom under the
  stock-parity line is small (686 bytes on 2026-11-14), so the code must fit there or live in memory that is only taken while a toast is up.
- **The title's memory:** the kernel writes only inside the frame the CRTC shows, never elsewhere.

## 6. Tests
- **xemu:** a test title with a known double-buffered frame; boot_test.py reads the frame back (xemu's screendump) and finds the toast's box and its text,
  on 16- and 32-bit buffers and in 720p.
- **Hardware:** the capture card (16 ms per frame) on the test consoles: a toast over a game in each encoder (Conexant, Focus, Xcalibur), its 3 seconds, no
  flicker, and the game's frame rate unchanged.
- **Extensions:** extension_test calls `oxos_toast` at its first launch; boot_test.py finds the text on screen.

## 7. Open
- Where the on/off setting lives (an ini file in `C:\OXOS`, or the dashboard).
- Which kernel events show a toast (§2): each is its own decision.
- The position and colours, and whether a title can be told apart so a toast avoids its subtitles.
- Toasts with more than one line, or a second size for 720p and 1080i.
