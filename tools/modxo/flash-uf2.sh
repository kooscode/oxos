#!/bin/sh
# Flash a .uf2 to the Modxo's Pico without touching the board (README.md in this folder).
#   flash-uf2.sh FILE.uf2 [SERIAL-PORT]      (default /dev/ttyACM0)
# A 1200-baud "touch" of the serial port restarts the Pico into its USB bootloader (BOOTSEL); the RPI-RP2 drive that
# appears (it must be auto-mounted, as desktop systems do) gets the file, and the Pico restarts with it. Repeated if
# the Pico comes back still in BOOTSEL. A running `console.py log` reconnects by itself.
# Firmware and BIOS are separate .uf2 files (a merged one lost the BIOS half): flash them one at a time.
set -u
uf2="${1:-}"; dev="${2:-/dev/ttyACM0}"
if [ -z "$uf2" ] || [ ! -f "$uf2" ]; then
    echo "usage: $0 FILE.uf2 [SERIAL-PORT]" >&2
    exit 2
fi
python3 -c "
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200); time.sleep(0.2); s.baudrate = 1200; time.sleep(0.5); s.close()" "$dev" 2>/dev/null
for attempt in 1 2 3; do
    m=
    for i in $(seq 1 40); do
        m=$(findmnt -rn -o TARGET -S LABEL=RPI-RP2 2>/dev/null)
        [ -n "$m" ] && break
        sleep 0.5
    done
    if [ -z "$m" ]; then
        echo "the Pico did not show up as an RPI-RP2 drive (is $dev the Pico? is the drive auto-mounted?)" >&2
        exit 1
    fi
    sleep 2
    cp "$uf2" "$m"/ && sync
    for i in $(seq 1 30); do [ -e "$dev" ] && break; sleep 0.5; done
    if [ -e "$dev" ] && ! lsusb | grep -q "2e8a:0003"; then
        sleep 3
        echo "flashed $uf2"
        exit 0
    fi
    echo "still in BOOTSEL, copying again"
done
echo "failed to flash $uf2" >&2
exit 1
