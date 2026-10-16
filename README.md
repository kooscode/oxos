# OXOS — an open Xbox kernel

A clean-room, ABI-compatible reimplementation of the original Xbox kernel (`xboxkrnl.exe` 1.0.4627.1) in C++,
built on Linux with clang and lld, inside a **fully open 1 MB BIOS image**: no Microsoft code, keys or boot-ROM
dump is needed to build or run it. It boots retail software in [xemu](https://xemu.app); console revisions
1.0–1.5 are the target.

![OXOS boot splash in xemu](docs/splash-screenshot.png)

*The OXOS boot splash, captured in xemu.*

**Status:**
- Every function and variable the 4627 kernel exports is implemented.
- In xemu it boots the dashboard, **Halo** and **Halo 2** (both playable), reads UDF (DVD-video) discs and has its
  own boot splash.
- The BIOS image is ours from the first instruction, and xemu runs it without an MCPX boot ROM file.
- Real hardware has not been validated yet (the TV-encoder mode tables are still missing).

## Download
**[Download the latest BIOS image (releases/latest)](https://github.com/kooscode/oxos/releases/latest)**: a ready
1 MB `bios_vX.Y.bin`. To use it in xemu:
1. Open *Machine → Settings → System*.
2. Set *Flash (BIOS) File* to the image and leave *MCPX Boot ROM File* empty.
3. Use the copyright-free [xemu HDD image](https://github.com/xemu-project/xemu-hdd-image) as the hard disk.

It has only been tested in xemu so far, not on a real console.

## How it was made
- **The kernel** (`kernel/src`): the reference kernel was studied with Ghidra to write behavioural specifications
  (`spec/`), and the kernel is written from those specs only. No decompiled code, Microsoft keys or data tables are
  included.
- **The boot chain** (`kernel/boot`, spec 01 §5):
  - our own reset code and 2BL;
  - the hardware set-up table (X-codes) and the EEPROM key handling of
    [XboxDev/cromwell](https://github.com/XboxDev/cromwell) (GPL-2.0), which the build fetches.

  On a console the MCPX boot ROM runs Cromwell's X-codes, whose "overflow trick" hands control to our 2BL without
  any Microsoft key. Without a boot ROM (xemu), our reset code runs the same X-codes. The hard disk unlocks with
  Cromwell's EEPROM key states, as Cromwell has done for twenty years.

## What you need
- **To build:** clang/lld (18 tested), Python 3 with Pillow and git. Cromwell (and, for the controller test, nxdk)
  is fetched into `third_party/` automatically.
- **To run in xemu:**
  - xemu, on `PATH` or at `~/.local/bin/xemu`;
  - a hard-disk image at `xbox-files/hdd/xbox_hdd.qcow2`. The copyright-free
    [xemu HDD image](https://github.com/xemu-project/xemu-hdd-image) with its open dashboard works; tests and
    `make run` only ever use a temporary copy;
  - your own game discs (XISO) in `xiso/`.

  **No BIOS and no MCPX boot ROM are needed.**

Optional, for comparing against the original:
- your own BIOS dump in `xbox-files/bios/`;
- the MCPX boot ROM: `STOCK_MCPX`, or `MCPX_ROM=...` to boot OXOS through the real ROM as a console does.

`xbox-files/` and `xiso/` are git-ignored.

## Build and run
```
cd kernel
make bios                        # build/bios.bin, the open BIOS image
make test                        # boot it in xemu and check every milestone
make run ISO=../xiso/game.iso    # play: the release BIOS in an interactive xemu window
make test-mcpx MCPX_ROM=...      # optional: the dashboard again, through a real MCPX boot ROM
make run-stock ISO=...           # optional: your original BIOS, for comparison
```
`make test-pad` (controller test) also needs cmake, flex, bison, the LLVM 18 tools and write access to
`/dev/uinput`; it fetches [nxdk](https://github.com/XboxDev/nxdk) into `third_party/`.

See [`kernel/README.md`](kernel/README.md) for the layout, milestones and known deviations, and `spec/` for the
behavioural specifications.

## About the BIOS image you build
`make bios` contains our code plus parts of XboxDev/cromwell (X-codes and the EEPROM key states), both GPL-2.0;
there is nothing of Microsoft's in it. One known gap: there is no certificate key. Games therefore derive
different save-signature and System Link keys than a stock console does: saves made on a stock console do not
verify under OXOS, and System Link with stock consoles does not work. Saves made under OXOS work among themselves.

`IMAGE=base` still packs the kernel into your own Microsoft BIOS instead, for comparisons. Such an image contains
Microsoft's 2BL and keys: do not distribute that one.

## Next steps
- **Real hardware:** video encoder mode tables (Conexant, Focus) for TVs, then validation on revisions 1.0–1.5
  (the open image's 1.6 values, `MCPX16=1`, are untested).
- **Certificate key:** a bring-your-own-key option for stock-compatible saves and System Link.

## License
GPL-2.0 (see [`LICENSE`](LICENSE)). The build fetches [XboxDev/cromwell](https://github.com/XboxDev/cromwell)
(GPL-2.0) for the X-codes and EEPROM key states, and `tests/padtest` builds against
[nxdk](https://github.com/XboxDev/nxdk); both keep their own licenses and copyrights.

## Legal
This is an independent project, not affiliated with or endorsed by Microsoft. "Xbox" is a trademark of
Microsoft Corporation. Use it with game discs you own.
