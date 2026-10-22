# Consoles without a DVD drive (OXOS deviation)

Not the reference's behaviour: the retail kernel stops the boot with fatal error 10, 11 or 12 when the DVD drive
(IDE device 1) does not come up (stock spec 04 §3). OXOS boots without one, for consoles whose drive was removed or
disconnected. First implemented 2026-10-21, after the first run on a real console (a 1.4 with no drive). Code:
`kernel/src/idex/cdrom.cpp` (`IdexCdRomStart`, `IdexCdRomInit`).

## 1. Detection
At IDE initialisation the drive is brought up as the reference does (select device 1, wait while busy, SET FEATURES
PIO 4 and UDMA 2, IDENTIFY PACKET DEVICE). It counts as absent when:
1. the status register reads 0xFF (nothing drives the bus) or 0x7F (only the host's pull-down on DD7) right after
   device 1 is selected;
2. it stays busy for 30 s (600 polls of 50 ms, the reference's limit);
3. a SET FEATURES fails;
4. IDENTIFY PACKET DEVICE gives no data within 5 s.
`IdexCdRomAbsentReason` records the step (0 when a drive was found). Device 0 is selected again afterwards.

## 2. Behaviour without a drive
- `\Device\CdRom0` and `\??\CdRom0:` are still created: every title's start-up code links `D:` to
  `\Device\CdRom0`, and the stock dashboard asks for a fatal-error reboot (`HalReturnToFirmware(4)`, then fatal
  error 21) when that link cannot be made.
- The device acts as a drive with an empty tray: reads and every device control (CHECK_VERIFY, geometry, TOC, pass-
  through, DVD structure, disc authentication) complete at once with STATUS_NO_MEDIA_IN_DEVICE (0xC0000013), and the
  bus is never touched.
- The title launcher's disc attempt (`\Device\CdRom0\default.xbe`) therefore fails as with an empty drive and the
  dashboard starts. The tray wait after a boot with the eject flag skips its dismount when there is no device object.

## 3. Checks
- In xemu (which always has a drive) a test build that forces step 1 boots to the stock dashboard and stays there
  (no bug check, no reboot) with `IdexCdRomAbsentReason` = 1.
- With a drive present: `make test` (DVD checks), `make test-dash` and `make test-udf` pass unchanged.
