# OXOS — an open Xbox kernel

A clean-room, ABI-compatible reimplementation of the original Xbox kernel (`xboxkrnl.exe` 1.0.4627.1) in C++,
built on Linux with clang and lld. It packs into a standard 1 MB BIOS image and boots retail software in
[xemu](https://xemu.app); console revisions 1.0–1.5 are the target.

![OXOS boot splash in xemu](docs/splash-screenshot.png)

*The OXOS boot splash, captured in xemu.*

**Status:** every function and variable the 4627 kernel exports is implemented. In xemu it boots the dashboard,
**Halo** and **Halo 2** (both playable), reads UDF (DVD-video) discs, and has its own boot splash. Real hardware has not
been validated yet (the TV-encoder mode tables are still missing).

## How it was made
The reference kernel was studied with Ghidra to write behavioural specifications (`spec/`). The kernel in
`kernel/` is written from those specs only: no decompiled code, Microsoft keys or data tables are included.
Keys the console needs (EEPROM, certificate) are taken at run time from the boot loader of the BIOS image you
supply.

## Files you must supply (not included)
Nothing copyrighted is in this repository. Put your own dumps here (both folders are git-ignored):

| path | what |
|---|---|
| `xbox-files/bios/*.bin` | a 1 MB MCPX 1.0 BIOS image (any name); its init tables and 2BL are reused, the kernel is replaced |
| `xbox-files/mcpx/mcpx_1.0.bin` | the MCPX 1.0 boot ROM (for xemu, and to decrypt the 2BL) |
| `xbox-files/hdd/xbox_hdd.qcow2` | an xemu hard-disk image (tests and `make run` only ever use a temporary copy) |
| `xbox-files/xboxkrnl.exe` | optional: the reference kernel, for reverse-engineering work |
| `xiso/*.iso` | game discs (XISO) for `make run ISO=...` |

## Build and run
Requirements: clang/lld (18 tested), Python 3 with Pillow, and xemu (on `PATH` or `~/.local/bin/xemu`).

```
cd kernel
make bios                        # build/bios.bin
make test                        # boot it in xemu and check every milestone
make run ISO=../xiso/game.iso    # play: the release BIOS in an interactive xemu window
make run-stock ISO=...           # the same with your original BIOS, for comparison
```
`make test-pad` (controller test) also needs cmake, flex, bison, the LLVM 18 tools and write access to
`/dev/uinput`; it fetches [nxdk](https://github.com/XboxDev/nxdk) into `third_party/`.

See [`kernel/README.md`](kernel/README.md) for the layout, milestones and known deviations, and `spec/` for the
behavioural specifications.

## About the BIOS image you build
`make bios` produces a BIOS image that still contains parts of the BIOS you supplied: the X-code init table, the
second-stage boot loader (2BL) and the keys inside it are Microsoft's; only the kernel (and the splash) are ours.
**Do not distribute a built `bios.bin`.** Share the source and let everyone build from their own dump.

## Next steps
- **A fully open, shippable BIOS image**, based on [XboxDev/cromwell](https://github.com/XboxDev/cromwell) (GPL-2.0,
  fetched at build time and credited, not copied in):
  - Cromwell's X-code init table, ending with its "overflow trick": the last X-code writes a jump into RAM at
    address 0, the boot ROM's 2BL check then fails and switches the ROM off, and the CPU runs on into that jump.
    No MCPX key and no TEA-hash weakness needed, so one image covers revisions 1.0–1.5 (1.6 with Cromwell's
    MCPX-D5 values);
  - our own reset stub in the top 512 bytes of flash that runs the same X-codes when no boot ROM is present
    (xemu without an MCPX ROM file; on hardware the chip's ROM hides that area), so **no MCPX dump is needed**;
  - our own 2BL that sets up paging and starts the kernel per `spec/01-boot.md`;
  - EEPROM / hard-disk key handling from Cromwell, so stock locked drives unlock.
- **Video encoder mode tables** (Conexant, Focus) for real TVs, then validation on real hardware.
- **MCPX 1.1 support in `tools/mkbios.py`** (it only packs MCPX 1.0 images today).

## License
GPL-2.0 (see [`LICENSE`](LICENSE)). `tests/padtest` builds against [nxdk](https://github.com/XboxDev/nxdk), which
is fetched at build time and keeps its own licenses.

## Legal
This is an independent project, not affiliated with or endorsed by Microsoft. "Xbox" is a trademark of
Microsoft Corporation. Use it only with BIOS, boot ROM and game images you have dumped from hardware and discs you
own.
