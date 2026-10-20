# Tests

`make test` (in `kernel/`) runs every phase below in xemu, on a copy of the default hard disk (the free xemu-dashboard image, fetched on first use; `HDD_IMAGE=...` for another). The tests make no assumptions about the disk's contents beyond a formatted drive with a dashboard; each prints a `[PASS]`/`[FAIL]` line per check and
`RESULT: PASS` when all pass. Most checks come from the kernel's own boot self-tests (`kernel/src/ke/selftest*.cpp`,
compiled into the self-test build only), which leave their results in kernel variables that `boot_test.py` reads.

## Test phases (make targets)
| Target | What it checks |
|---|---|
| `make test` | The main phase: boots the self-test BIOS with a generated test disc and hard disk, and checks every milestone (CPU and HAL set-up, memory manager, scheduler, I/O, FATX, DVD and XDVDFS, EEPROM, crypto, the title loader through the kernel test title, the debug serial log). Then runs the four phases below. |
| `make test-dash` | The release BIOS with no disc boots the dashboard from the hard disk. |
| `make test-relaunch` | The release BIOS runs the kernel test title, which restarts itself through a launch page and a quick reboot (as games do with `XLaunchNewImage`). |
| `make test-encoders` | Separate: the release build on each video encoder xemu can present (`-machine video-encoder=conexant`, `focus`, `xcalibur`): the bus probe finds it at 0x8A / 0xD4 / 0xE0, `XboxHardwareInfo.Flags` reports it (0, 0x20, 0x400), on Conexant an SD mode set leaves the generated registers (`tools/gen_tv_encoders.py`) in the encoder, and on Xcalibur a 4-byte SMBus block transfer completes. Then the self-test build on xemu's Focus model, whose boot self-test sets every Focus HD mode word and reads the registers back. It first runs the generator's self-check (the CX25871 datasheet's autoconfiguration modes reproduced field by field) and fails if `kernel/src/hal/av_modes.inc` is stale. xemu's encoder models store registers only (no picture), and its Xcalibur model is byte-wide, so a 32-bit read cannot return data there. |
| `make test-fatal` | With no hard disk, the kernel reports a fatal error the way the original does (LED, error history in the EEPROM, code on screen). |
| `make test-udf` | A UDF (DVD-video style) disc instead of an Xbox one: mounting and reading files from it. |
| `make test-exfat` | Separate (not part of `make test`): the self-tests on a GPT disk with one exFAT volume (256 GB, sparse, in `xemu-state/oxos-xemu-exfat/`), the test disk's C: and E: copied into folders. The FATX suite runs on E: as a folder of the volume; afterwards `exfat.py` checks the whole volume (spec/oxos-internal/05-exfat-driver.md). |
| `make test-lba48` | Separate (not part of `make test`): the self-tests on a 2 TB disk (a sparse copy of the test disk in `xemu-state/oxos-xemu-lba48/`). The kernel writes test sectors either side of the 28-bit limit and at the end of the disk; the test then finds each one at its exact place in the image and checks that the start of the disk is untouched (spec/oxos-internal/02-lba48.md). |
| `make test-mcpx` | Only with `MCPX_ROM=...`: the dashboard phase again, booted through a real MCPX boot ROM. |
| `make test-controller` | Separate (not part of `make test`): every input of a virtual gamepad reaches an nxdk title as the right Xbox controller input. Needs cmake, flex, bison and access to `/dev/uinput`. |
| `make test-memory` | Separate (not part of `make test`): how much memory a title is given, as built (the XBE flag "limit to 64 MB", as retail games have it) and with the flag cleared; `MEMORY=128` for a 128 MB console, `MEMORY_BIOS=...` (with `MCPX_ROM=...`) to measure another BIOS. Needs nxdk, like the controller test. |

## Files
| File | What it is |
|---|---|
| `boot_test.py` | The test runner behind every phase except `test-controller`: makes the test disc and a copy of the hard disk, boots the BIOS in xemu, reads the results through xemu's QMP monitor and the kernel's map file, and prints the checks. |
| `kernel-test-title/` | The kernel test title: a minimal freestanding Xbox "game" run from the test disc. It checks that the kernel loads and starts titles and that the kernel functions games rely on work (clock, memory pool, disc reads, threads, drive letters, on-demand sections, SHA-1), and does the relaunch test. The comment at the top of the file lists each test. |
| `controller-test-title/` | The controller test title, built with nxdk: shows every controller input live and records which inputs arrived, for `controller_test.py`. |
| `memory-test-title/` | The memory test title, built with nxdk: asks the kernel how much memory there is (`MmQueryStatistics`), asks for a contiguous page above 64 MB, then allocates 1 MB blocks until the kernel refuses, noting the physical address of every page; the results are on screen and in a block `memory_test.py` reads. |
| `memory_test.py` | The `make test-memory` runner: puts the title on a disc (once as built, once with the 64 MB flag cleared), boots the BIOS at 64 or 128 MB and prints the measurements. |
| `controller_test.py` | The `make test-controller` runner: boots the controller test title's disc, presses each input of a virtual gamepad and checks what the title received. |
| `virtual_gamepad.py` | A virtual gamepad through Linux uinput, which xemu sees as an ordinary USB pad. Used by `controller_test.py`. |
| `mkxiso.py` | Builds the Xbox (XDVDFS) test disc: test files plus the kernel test title as `default.xbe`. Can also pack any folder as a disc (used by `tools/debug-app.py`). |
| `mkudf.py` | Builds the UDF test disc for `make test-udf`, covering the different ways UDF stores files and names. |
| `fatx.py` | A FATX reader for the Xbox hard-disk partitions, to check files the kernel wrote, and a small writer (`FatxWriter`: folders, files, copying a host folder) for preparing disks: `python3 tests/fatx.py SRC.qcow2 --add "E:\\Folder=host/folder" --out NEW.qcow2`. |
| `exfat.py` | Builds GPT disks with an exFAT volume (`--from-fatx SRC DST` copies a FATX disk's C: and E:) and checks a volume completely: boot and entry-set checksums, name hashes, FAT chains, no cluster used twice, the allocation bitmap (`--check IMG`, `--ls IMG /path`); `--add /F/dir=HOST` also copies a host file or folder onto a new disk. |
| `qcow2.py` | A reader for xemu's qcow2 disk images (plain and compressed), used by `fatx.py`; writes go to memory and `save()` writes a new, clean image, so the source disk is never changed. `qcow2.py --grow SRC DST 2T` makes a bigger copy of a disk (the added space is sparse). |
| `test_lzx_frames.py` | A standalone check (not part of `make test`: `python3 tests/test_lzx_frames.py`) that the BIOS packer's LZX compression (`tools/lzx.py`) decodes correctly across several frames, with `tools/lzx.py`'s own decoder and, independently, with the system's libarchive (a CAB file the test builds) when it is installed. |
