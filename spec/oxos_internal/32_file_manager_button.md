# 32. The X button: the OXOS File Manager (OXOS's own)

Status: implemented 2026-11-08. Code: `kernel/src/gamepad/` (the USB controller and the pads), `kernel/src/bootscreen/splash.cpp`
(when the pads are read), `kernel/src/titles/loader.cpp` (what happens then), `kernel/src/core/fatal.cpp` (the
screen), `kernel/src/uploader/` (the File Manager, spec/oxos_internal/31).

X pressed on any controller while the boot splash and jingle run, or on the "No Dashboard Found" screen, opens
the OXOS File Manager: a web page for uploading, organizing and launching files from a browser. X, not WHITE: the
Xbox Live dashboard already uses WHITE held at its start (2026-11-08).

## 1. When
- Cold boot only, with an AV pack (the splash runs); a quick reboot never reads the pads.
- X at any time from the start picture to the end of the jingle (about 4 s). The title thread waits for the
  splash to end (`HalpWaitForSplash`) before it chooses a title, then:
  - X seen: the File Manager (spec/oxos_internal/31) until a browser launches an XBE that loads
    (`XeTitleLaunched` 4). Without a network card it cannot run: the title is chosen as usual.
  - Otherwise: the usual title (spec/stock/03 §4).
- No dashboard on a cold boot (13–20): `XkNoDashboardError` shows the fatal error's screen (the dashboards tried and
  "[ Press X to launch Web File Manager ]", spec/oxos_internal/29), with its LED sequence and error history, but
  keeps the boot-only pages (`HalpStopSplash`, not `HalpEndSplash`) and the keys (a title launched from the File
  Manager needs them). It reads the controllers (`GamepadWaitForX`: set up, read every 10 ms; with none found,
  set up again every 3 s for one plugged in later) until X, then closes the screen, sets the LED green and runs the
  File Manager. Without a network card: the fatal error (`IdexFatalError`). A quick reboot without a dashboard still
  reboots cold.

## 2. In the splash thread
No thread of its own: the splash thread mostly sleeps between frames.
- After the fade-in, while the start picture stands still (0.5 s before the jingle): `GamepadWatchStart` finds and
  sets up the pads (§3, §4). It blocks; a few hundred milliseconds with one controller.
- Every sleep of the splash after that reads the pads about every 10 ms (`GamepadWatchPoll`), without waiting.
- Before the splash signals its end: `GamepadWatchStop` (§3).

## 3. The USB controller
The first OHCI controller (USB 0, 0xFED00000: the four controller ports), polled, following OpenHCI 1.0a:
- Reset (HCR), the HCCA, the control list and the frame interval set, then running (USBOPERATIONAL, control and
  periodic lists on, no interrupts). The root hub's ports powered, then the power-good time and 100 ms to settle.
- Everything the controller reads or writes in one uncached page: the HCCA, an ED for control transfers, one
  interrupt-IN ED per pad, their TDs, and the buffers.
- Control transfers: SETUP, data, status TDs on the control ED, waited for (at most 100 ms each).
- Interrupt-IN pipes: each pad's ED is on the periodic list (every HCCA interrupt-table entry: read each frame), one
  TD queued at a time, checked without waiting and queued again.
- Stop: a reset (no more DMA into the page), then USBRESET, which resets the bus as at power-on: the title's own USB
  driver finds the controller as after a cold boot (as `hardware/chipset.cpp` leaves it). Then the page is freed.

## 4. The pads
- Each connected root port is reset and its device set up: its address, its configuration descriptor, its
  configuration set.
- A hub (class 9) has its ports powered and each connected one reset and set up, up to three hubs deep: every Xbox
  controller is a hub with the pad and its memory-unit slots behind it, and xemu puts a hub of its own in front.
- A pad is an interface of class 0x58 (Xbox input) with an interrupt-IN endpoint: it gets a pipe (at most four).
  Its state is asked for once at once (XID GET_REPORT: 0xA1, 1, 0x0100), for X already held; a pad without that
  request is read through its pipe only.
- The report: 0, its length (20), the digital buttons, then the analog buttons A, B, X, Y, BLACK, WHITE, LT, RT.
  X (byte 6) at 0x80 or more (half way) sets `GamepadXPressed`.

## 5. The File Manager's screen
Not a fatal error (no number, no LED sequence, no EEPROM history), drawn in the layout of the error screens
(spec/oxos_internal/29 §2):

| Part | Where | Look |
|---|---|---|
| `OXOS v0.4.1/CONEXANT` | y 36 | grey |
| a folder with an arrow up, in place of the digits | y 76–182 | two greens |
| `OXOS File Manager` | y 204 | scale 3, green |
| `Open this address in a browser:` | y 244 | light grey |
| `http://192.168.1.91/` | y 272 | scale 3, green |
| `[ Upload files, manage folders, launch XBEs ]` | y 316 | light grey |
| the status (network, the last upload, delete or launch) | y 380 | grey |
| `Launch an XBE, or turn the console off.` | y 430 | grey |

## 6. Memory
All of it is boot-only (spec/oxos_internal/18): `gamepad/` and the screen's code are freed when the first title
starts, the page with the controller's memory before that. The resident kernel does not grow: the old upload
screen, resident, is gone.

## 7. Checks
- `make test_upload` (xemu): a virtual pad (`tests/virtual_gamepad.py`, its X is X in xemu) holds X from
  power-on; the kernel sees it, and the File Manager's checks of spec/oxos_internal/31 §8 follow.
- `make test_format`: no pad pressed: after the format, error 13, no File Manager; and a fourth run whose pad presses
  X once error 13 is up: the File Manager starts.
- `make test_dash`: the normal boot, with the pads read during the splash, reaches the dashboard.
