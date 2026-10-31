# Hardware-in-the-loop testing

Most of the kernel was built and tested in xemu. Real consoles are then tested with a rig that boots a fresh kernel
on a real Xbox in about a minute, with no chip to flash and nobody at the console. An AI coding agent (Claude Code)
drives it from the PC: it builds, boots, watches the console and powers it off.

![The hardware-in-the-loop rig: an RP2040 Modxo on the LPC header of a 1.4 Xbox](hwitl-rig.jpg)

*An RP2040-Zero running [Modxo](https://github.com/Shalx/Modxo) on a perfboard, wired to the console's LPC header
(the ribbon cable) and to its power switch (the white wire). The Pico presses the button through a 2N2222: GPIO →
1 kΩ → base, collector to the Xbox power-switch line, emitter to ground. One USB cable to the PC.*

## The modchip is the debug port
The Pico runs Modxo with an OXOS debug module ([`tools/modxo`](../tools/modxo/README.md), firmware on the
`oxos-debug` branch of [kooscode/modxo](https://github.com/kooscode/modxo)). Over its one USB cable it:
- holds the BIOS image, reflashed over USB;
- emulates the console's SuperIO serial port, so the kernel's boot log and crash reports reach the PC (COM1);
- streams every POST code (port 0x80 writes) and the LPC power and reset state;
- presses the power button: `console.py on`, `off`, `cycle`.

## Network boot
The BIOS flashed on the Modxo can be a netboot image. Its 2BL asks the PC for a kernel at every power-on, so a new
build needs no flashing. [`tools/netboot`](../tools/netboot/README.md) serves the kernel over UDP, and can add a title
package: an XBE and its files (the ISO launcher and its `iso.txt`, a test title), installed on E: and started instead
of the dashboard. The kernel's log is the serial log (`SERIAL=1` builds), which the Modxo passes to the PC.

## Eyes on the screen
The console's video goes through an HDMI adapter into a USB capture card. Screenshots and videos of the real screen
are taken with `ffmpeg`, so the agent can check what the console shows (the splash, the dashboard, a title's start
screen). The serial log and the frames are matched by their timestamps.

## One iteration
Build (`make -C kernel BUILD=... SERIAL=1`) → `netboot.py` serves it → `console.py on` → read the serial log, POST
codes and capture frames → `console.py off`. `bootrun.py` repeats a boot N times and classifies each one (PASS /
FREEZE / NOBOOT), which found the layout-dependent boot freeze that only showed on hardware
([`spec/oxos-internal/13-flash-mapping.md`](../spec/oxos-internal/13-flash-mapping.md)).

## Results so far
The boot freezes on the 1.4 console were found and fixed this way, along with the CPU set-up, interrupt start-up and
NV2A pad calibration (specs 13–15), and the DRAM refresh loss behind rare crashes on warm boots (spec 20). So was the
first retail title on real hardware: Halo 2 from an XISO on an exFAT SSD, and later Brute Force the same way.
