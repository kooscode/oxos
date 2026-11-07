# Boot splash (OXOS addition)

Not part of the reference: the retail kernel shows its own boot animation from its INIT section (what it does with
the animation's memory is in stock spec 17 §6). OXOS shows its logo during cold boot instead, drawn from vector
shapes and animated (since 2026-11-06; from 2026-10-15 a 640x480 PNG, removed 2026-11-06). Code:
`kernel/src/hal/splash.cpp`, `kernel/src/hal/vsplash.cpp`; host side `tools/svg_splash.py`, `tools/mkbios.py`. The music that comes with it: spec/oxos-internal/17.

## 1. The image in flash
- `tools/mkbios.py --splash SVG` (the Makefile passes `kernel/splash/oxos-splash.svg`, `SPLASH=` to choose another
  SVG) turns the SVG into a vector scene (§1.1) and stores it in flash bank 1 (offset 0x40000, physical 0xFFF40000)
  behind a 20-byte header: `"XKSPLASH"`, u32 version 2, u32 length, u32 CRC-32 of the scene. (Version 1 was a PNG; the
  2BL and the kernel no longer accept it.)
- Banks 0 and 3 must stay as they are: the MCPX boot ROM reads its init table at 0xFF000000 (bank 0 of a 1 MiB chip)
  and the 2BL/kernel from the top bank.
- **256 KiB images** (since 2026-10-21): the open image also puts a copy of the same header and scene in each boot
  bank's spare space, between the kernel's deflate stream and its `.data` initializer, when it fits, and records it
  in the payload header (bank offset 0x6000, words 6 and 7: offset and size; 0, 0 for none; stock spec 01 §5). So
  `make bios-256k` (`mkbios.py --size 256K`), or the first 256 KiB of a 1 MiB image, shows the splash too. If the
  kernel grows past the room, the 1 MiB build warns and keeps only bank 1, and the 256 KiB build fails.

### 1.1 The vector scene (version 2)
`tools/svg_splash.py` turns an SVG into shapes the kernel can fill (24 KB for the OXOS logo and tagline;
the 640x480 PNG used until 2026-11-06 took 93 KB):
- **Placement**: the SVG's viewBox is scaled to the full 640-pixel width (or the full height if it is taller than
  4:3) and centred; what lies outside it is not drawn (black bars above and below a wide picture). A widescreen console
  shows the same 640 x 480 picture.
- **Shapes**: curves are flattened to polygons (about one point per 3 output pixels), strokes become outlines (a quad
  per segment and a bevel triangle each side of every joint, all wound the same way, so their non-zero union is the
  stroke). Supported SVG: `g`, `rect` (square corners), `circle`, `ellipse`, `polygon`, `polyline`, `path` (no arcs),
  transforms (blur strengths scale with them), inherited fill / stroke / opacities / fill-rule, linear and radial gradients (both units,
  `gradientTransform`), `filter` with a `feGaussianBlur`, alone (blur) or under the `SourceGraphic` in an `feMerge`
  (glow). Anything else is refused at build time.
- **Animation**: `data-oxos-phase="start"` on an element or group draws it only in the start picture, `"end"` only in
  the end picture; everything else is in both. `kernel/splash/oxos-splash.svg`: the letters in both; a metal X
  (the letters' gradient and outline on the X's shape) at the start; the green X with its glow and the halo at the end;
  the tagline in both.
- **Format** (little-endian; coordinates in 1/16 pixel): clip rectangle (4 x u16, pixels); u16 paint count, each paint:
  u8 type (0 colour, 1 linear, 2 radial), u8 stop count (1-16), linear: i32 a, b, c with t x 65536 = a X + b Y + c at
  pixel (X, Y), radial: i32 centre x, y and radius; then the stops: u16 offset (0-256), u8 red, green, blue, alpha; u16
  layer count, each layer: u8 phases (bit 0 start, bit 1 end), u8 filter (0 none, 1 blur, 2 glow), u16 blur sigma, u8
  opacity, u8 0, u16 draw count, each draw: u16 paint, u8 alpha, u8 flags (bit 0 even-odd), u16 contour count, each
  contour: u16 point count and the points (2 x i16).
- **Drawing** (`vsplash.cpp`, integer only, shared with the host preview `tools/vsplash` and the unit tests): each
  shape filled with its rule, 8 sample rows per pixel row and exact coverage along the row; gradients through a
  257-entry table; a filtered layer drawn into its own buffer, blurred (three box blurs each way, together close to the
  Gaussian), and composited at its opacity, its sharp copy over the blur for a glow. `vsplash::Check` walks the whole
  scene first (counts, offsets, coordinate and gradient ranges, exact length) and a scene that fails shows the drawn
  emblem (status 0x200 + the check's number).
- The kernel draws both pictures once (into two 1.2 MB buffers below 16 MB, with 2.4 MB of working memory freed right
  after); on the 1.4 console both took under 0.6 s from kernel start, during the drives' start-up.

## 2. Showing it
- The 2BL copies the image into RAM before it unpacks the kernel (`boot/bl.cpp` `BlCopySplash`, since 2026-10-28):
  the "XKSPLASH" blob from bank 1 (0xFFF40000), or, without a header there (on a 256 KiB chip that address mirrors the
  boot bank), the copy the payload header of the boot bank (or of a netboot download) points to. It goes to physical
  0xC00000 (at most 256 KiB), and a record at physical 0x2380 (next to the pad calibration record at 0x2300) gives
  'OXSP' (0x5053584F), the physical address and the size (0: none).
- The kernel takes the record before the memory manager starts (`HalpTakeSplashRecord`, cold boot only); the memory
  manager keeps those pages in use (`HalpIsSplashPage`, like a netboot title's pages) and the splash thread frees them
  once the scene is drawn. The kernel does not read the flash: decoding the earlier PNG byte by byte from uncached
  flash while the kernel ran coincided with bits of RAM changing on a Modxo console (a crash in about one cold boot in
  ten), and long flash reads hung that console. Without a record (an older 2BL) the drawn emblem is shown.
- When there is no valid header or the scene fails its checks, the drawn emblem is shown instead: the logo's X as two
  crossed green bars with a glow, and "OXOS" under it. `HalpSplashSource` (1 = emblem, 2 = scene) and
  `HalpSplashStatus` (0 = drawn; 0x100 no copy from the 2BL, 0x101 no header, 0x102 bad header or an old version 1
  PNG, 0x103 out of memory, 0x200 + the scene check that failed) record what happened.
- It starts before the IDE set-up (since 2026-11-06; until then after it): a hard disk or DVD drive can take seconds
  to spin up after power-on, and the screen stayed dark meanwhile. The drives' ready waits run at the boot thread's
  IRQL (the reference raises to the IDE interrupt's first) and follow the clock, so the splash and jingle threads run
  during them; the boot thread, which may not block, polls a lock the splash thread holds (SMBus, EEPROM) instead.
- It fades in while boot continues, with the boot jingle (spec/oxos-internal/17-boot-jingle.md): the fade follows
  the clock from the jingle's start, a frame about every 40 ms, and reaches full intensity as the jingle's build-up
  lands on its final chord (`jingle::ClimaxMs`, 2.6 s; `HalpSplashFullMs` records when it did). Since 2026-11-06
  (user's timeline): a 0.5 s fade from black to the start picture (the metal logo), which stays until 1 s (the fade included); then the
  jingle starts and the picture cross-fades (smoothstep) to the end picture (the green X and its glow), reaching it
  at the climax, 2.6 s later (3.6 s); the end picture is held while the chord lingers, until the jingle has ended
  (`jingle::LengthMs`, 1.4 s after the climax: 5 s in all). The
  emblem is one picture: it fades in and stays, with the same timing. The first
  title is loaded meanwhile but starts only after the hold (5 s after the picture came on) and after the jingle has
  ended. HDMI adapters and capture cards take about 2 s to lock onto the boot display (measured on the 1.4 console),
  so an HDMI screen may miss the logo and the start of the cross-fade. Quick reboots show no splash.

## 3. Memory
- The launcher waits for the fade, turns the screen off (`AvSendTVEncoderOption` option 9 with 1), then frees the
  two pictures and the screen buffer, so the title starts with no splash pages held, as on the reference (stock
  spec 17 §6). The title's display-mode change leaves the screen off and its first frame turns it on (option 9 with
  0, stock spec 22). Until 2026-10-28 the scanout stayed on the freed buffer, and the title's graphics set-up
  garbled the last splash frame on screen (and the network-log builds' text panel) before its first frame.
- The splash's code and its scene renderer are boot-only memory, freed at the same point
  (spec/oxos-internal/18-boot-memory.md).
- Until 2026-10-17 the screen buffer was kept as the persisted display (`AvSetSavedDataAddress`), which held 301
  pages (1.2 MB) at the title's entry point that the reference does not.
