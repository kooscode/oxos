# Boot splash image (OXOS addition)

Not part of the reference: the retail kernel shows its own boot animation from its INIT section (what it does with
the animation's memory is in stock spec 17 §6). OXOS shows a picture during cold boot instead. First implemented
2026-10-15 (drawn emblem, then the PNG from flash the same day). Code: `kernel/src/hal/splash.cpp`,
`kernel/src/hal/png.cpp`; host side `tools/mkbios.py`. The music that comes with it: spec/oxos-internal/17.

## 1. The image in flash
- `tools/mkbios.py --splash IMAGE` (the Makefile passes `kernel/splash/splash.png` when present) keeps a
  640×480 palette PNG as it is (no Pillow needed; the committed splash is one, and `--save-splash PNG` writes such a
  file).
- Any other image needs Pillow:
  - it is scaled to fit 640×480 keeping its aspect ratio (black bars);
  - pixels with every channel ≤ 6 are set to black (background noise that costs bytes but is invisible);
  - it is reduced to 256 colours without dithering: octree by default (`SPLASH_QUANT=octree`; smallest, suits
    textured art: 93 KB for the current splash against 241 KB with median cut), or `SPLASH_QUANT=median` for images
    with smooth gradients, where octree shows banding.
- The PNG is re-chunked to a single IDAT and stored in flash bank 1 (offset 0x40000, physical 0xFFF40000) behind a
  20-byte header: `"XKSPLASH"`, u32 version 1, u32 PNG length, u32 CRC-32 of the PNG.
- Banks 0 and 3 must stay as they are: the MCPX boot ROM reads its init table at 0xFF000000 (bank 0 of a 1 MiB chip)
  and the 2BL/kernel from the top bank.
- **256 KiB images** (since 2026-10-21): the open image also puts a copy of the same header and PNG in each boot
  bank's spare space, between the kernel's deflate stream and its `.data` initializer, when it fits, and records it
  in the payload header (bank offset 0x6000, words 6 and 7: offset and size; 0, 0 for none; stock spec 01 §5). So
  `make bios-256k` (`mkbios.py --size 256K`), or the first 256 KiB of a 1 MiB image, shows the splash too. If the
  kernel grows past the room, the 1 MiB build warns and keeps only bank 1, and the 256 KiB build fails.

## 2. Showing it
- The splash thread maps bank 1 with `MmMapIoSpace(…, PAGE_READONLY | PAGE_NOCACHE)`; without a header there (on a
  256 KiB chip that address mirrors the boot bank) it maps the boot bank at 0xFFFC0000 and uses the copy the payload
  header points to. It decodes the image with
  `src/hal/png.cpp`: inflate (RFC 1951), Adler-32 check, PNG scanline un-filtering. Supported input is exactly what
  mkbios writes, plus 8-bit RGB/RGBA: not interlaced, one IDAT. The inflated scanlines (≤ 1.2 MB) go to a temporary
  buffer below 16 MB.
- When there is no valid header or decoding fails, the drawn emblem and caption are shown instead.
  `HalpSplashSource` (1 = drawn, 2 = flash image) and `HalpSplashStatus` (0 = shown; 0x101 no header, 0x102 bad
  header, 0x103 out of memory, 1–10 decoder step) record what happened.
- It fades in while boot continues, with the boot jingle (spec/oxos-internal/17-boot-jingle.md): the fade follows
  the clock from the jingle's start, a frame about every 40 ms, and reaches full intensity as the jingle's build-up
  lands on its final chord (`jingle::ClimaxMs`, 2.6 s; `HalpSplashFullMs` records when it did). It stays until
  5.5 s after the fade began (`SPLASH_SHOWN_MS`), as the retail boot animation shows for a few seconds; the first
  title is loaded meanwhile but starts only after that and after the jingle has ended. HDMI adapters and capture
  cards take about 2 s to lock onto the boot display (measured on the 1.4 console), so an HDMI screen shows the end of
  the fade. Quick reboots show no splash.

## 3. Memory
- The launcher waits for the fade, then frees both the decode buffer and the screen buffer and leaves the scanout on
  the screen buffer, so the title starts with no splash pages held, as on the reference (stock spec 17 §6).
- Until 2026-10-17 the screen buffer was kept as the persisted display (`AvSetSavedDataAddress`), which held 301
  pages (1.2 MB) at the title's entry point that the reference does not.
