# OXOS — an open Xbox kernel

A clean-room, ABI-compatible reimplementation of the original Xbox kernel (`xboxkrnl.exe` 1.0.4627.1) in C++,
built on Linux with clang and lld. It packs into a standard 1 MB BIOS image and boots retail software in
[xemu](https://xemu.app); console revisions 1.0–1.5 are the target.

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
| `xbox-files/bios/Complex_4627.bin` | a 1 MB MCPX 1.0 BIOS image; its init tables and 2BL are reused, the kernel is replaced |
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

## License
GPL-2.0 (see [`LICENSE`](LICENSE)). `tests/padtest` builds against [nxdk](https://github.com/XboxDev/nxdk), which
is fetched at build time and keeps its own licenses.

## Legal
This is an independent project, not affiliated with or endorsed by Microsoft. "Xbox" is a trademark of
Microsoft Corporation. Use it only with BIOS, boot ROM and game images you have dumped from hardware and discs you
own.
