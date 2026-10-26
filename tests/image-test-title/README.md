# Image test title

The simplest check that a console (or xemu) runs the OXOS kernel and a title: an nxdk title that sets a 640x480
video mode, loads `D:\picture.jpg` from its disc, and decodes it on the console with libjpeg (file I/O, memory).
It shows the picture with a status panel in Xbox green below it. The picture keeps its shape on the TV: when the
console's video setting is widescreen, it is drawn 3/4 as wide, because a 16:9 TV stretches the 640-pixel width. Once a second the panel updates and the front LED
changes pattern: a frozen console is obvious at a glance.

```sh
make -C kernel image-test              # -> tests/image-test-title/image-test.iso (fetches nxdk the first time)
make -C kernel run-image-test          # boots it in xemu with the release BIOS
make -C tests/image-test-title PICTURE=other.jpg   # another picture on the disc (nxdk environment as above)
```

The panel shows:
- the kernel version;
- seconds alive and frames per second (vertical blanks waited for);
- free memory;
- the picture's size, how long loading and decoding it took (or why it failed), and widescreen or 4:3.

For automated runs the same state is in RAM as `XkImageTest` (signature `XKIMGTS1`): seconds, frames, picture
status (1 = shown), size, load time, free pages, widescreen (1) or not.

`picture.jpg` is a picture contributed for this title, stored at the size it is shown (361x330, 39 KB).
