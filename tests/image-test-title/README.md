# Image test title

The simplest check that a console (or xemu) runs the OXOS kernel and a title: an nxdk title that sets a 640x480
video mode, loads `D:\picture.png` from its disc, and decodes it on the console with libpng (file I/O, zlib, memory).
It shows the picture with a status panel in Xbox green below it. Once a second the panel updates and the front LED
changes pattern: a frozen console is obvious at a glance.

```sh
make -C kernel image-test              # -> tests/image-test-title/image-test.iso (fetches nxdk the first time)
make -C kernel run-image-test          # boots it in xemu with the release BIOS
make -C tests/image-test-title PICTURE=other.png   # another picture on the disc (nxdk environment as above)
```

The panel shows:
- the kernel version;
- seconds alive and frames per second (vertical blanks waited for);
- free memory;
- the picture's size and how long loading and decoding it took (or why it failed).

For automated runs the same state is in RAM as `XkImageTest` (signature `XKIMGTS1`): seconds, frames, picture
status (1 = shown), size, load time, free pages.

`picture.png` is a picture contributed for this title.
