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
| `make test-fatal` | With no hard disk, the kernel reports a fatal error the way the original does (LED, error history in the EEPROM, code on screen). |
| `make test-udf` | A UDF (DVD-video style) disc instead of an Xbox one: mounting and reading files from it. |
| `make test-mcpx` | Only with `MCPX_ROM=...`: the dashboard phase again, booted through a real MCPX boot ROM. |
| `make test-controller` | Separate (not part of `make test`): every input of a virtual gamepad reaches an nxdk title as the right Xbox controller input. Needs cmake, flex, bison and access to `/dev/uinput`. |

## Files
| File | What it is |
|---|---|
| `boot_test.py` | The test runner behind every phase except `test-controller`: makes the test disc and a copy of the hard disk, boots the BIOS in xemu, reads the results through xemu's QMP monitor and the kernel's map file, and prints the checks. |
| `kernel-test-title/` | The kernel test title: a minimal freestanding Xbox "game" run from the test disc. It checks that the kernel loads and starts titles and that the kernel functions games rely on work (clock, memory pool, disc reads, threads, drive letters, on-demand sections, SHA-1), and does the relaunch test. The comment at the top of the file lists each test. |
| `controller-test-title/` | The controller test title, built with nxdk: shows every controller input live and records which inputs arrived, for `controller_test.py`. |
| `controller_test.py` | The `make test-controller` runner: boots the controller test title's disc, presses each input of a virtual gamepad and checks what the title received. |
| `virtual_gamepad.py` | A virtual gamepad through Linux uinput, which xemu sees as an ordinary USB pad. Used by `controller_test.py`. |
| `mkxiso.py` | Builds the Xbox (XDVDFS) test disc: test files plus the kernel test title as `default.xbe`. Can also pack any folder as a disc (used by `tools/debug-app.py`). |
| `mkudf.py` | Builds the UDF test disc for `make test-udf`, covering the different ways UDF stores files and names. |
| `fatx.py` | A read-only FATX reader for the Xbox hard-disk partitions, to check files the kernel wrote. |
| `qcow2.py` | A read-only reader for xemu's qcow2 disk images, used by `fatx.py`. |
| `test_lzx_frames.py` | A standalone check (not part of `make test`: `python3 tests/test_lzx_frames.py`) that the BIOS packer's LZX compression (`tools/lzx.py`) decodes correctly across several frames, with `tools/lzx.py`'s own decoder and, independently, with the system's libarchive (a CAB file the test builds) when it is installed. |
