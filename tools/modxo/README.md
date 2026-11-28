# modxo: a boot-debugging rig built from a Modxo modchip

An RP2040 [Modxo](https://github.com/Shalx/Modxo) modchip on the console's LPC header, with the **oxos_debug** firmware module, turns one USB cable into a
console debug rig:
- **COM1:** the emulated serial port (an OXOS kernel's boot log; `SERIAL=1` builds).
- **POST codes:** every write to I/O port 0x80, as it happens (the 2BL's and kernel's progress codes).
- **Status lines:** LPC power and reset pins, LPC cycle counts and last addresses, the first memory reads after power-up (the boot ROM fetching X-codes).
- **The power button:** pressed from the PC, so test boots run unattended.
- **Reflashing over USB:** BIOS or firmware, without touching the board.

These scripts are the PC side.

## Hardware
- An RP2040 Modxo board on the LPC header, flashed with Modxo built from the **`oxos` branch** (the default) of
  [kooscode/modxo](https://github.com/kooscode/modxo). The module is `modxo_modules/oxos_debug`; its README has the stream format. Build as Modxo (`cmake`
  for your board, then `ninja`), and flash the firmware .uf2 with `flash_uf2.sh`. The firmware serves USB on the Pico's second core: status lines go out
  whole, never cut by COM1 bytes, and the power-button command is taken wherever it is in the PC's stream, so a console stopped in the GDB debugger cannot
  hold it back.
- Power button: GPIO 28 (`OXOS_POWER_PIN`) → 1 kΩ → base of an NPN transistor (e.g. 2N2222). Emitter to Xbox ground, collector to the power switch's signal
  side. On the RP2040-Tiny GPIO 28 is SERIRQ, so that branch leaves the SERIRQ module unregistered.
- **1.6 board without the transistor** (e.g. an RP2040-Zero on the ModXo-RP2040-Zero PCB): build Modxo with `-DMODXO_PINOUT=rp2040_zero_1_6` and wire GPIO 9
  straight to the power switch line; D0 stays on GPIO 6 as on the standard pinout. The pin stays high and is pulled low for a press. The switch line has its own
  pull-up on the console, so the pin not being driven while the Pico restarts does not press it (checked: reflashing the firmware with the console off left it
  off).
- **1.6 console:** it has no LPC header, so the LPC lines are wired to the board (a rebuilt header). LFRAME goes to the Pico's LFRAME pin (GPIO 5 on the
  RP2040-Tiny), not to ground. Use two ground wires and keep every wire short: with long wires the LPC cycles were corrupted and the console showed FRAG (red
  and green).
- The BIOS goes into Modxo's flash bank as its own .uf2: `flashbin_to_uf2.py` from Modxo's `tools/` (default base 0x10040000).
- Python 3 with pyserial (`pip install pyserial`).

## Commands
Each Modxo is named by its Pico's USB serial number; `console.py` with no arguments lists the connected ones. One logger runs per Pico: it owns the Pico's
serial port, and the other commands talk to it (each logger takes the first free UDP port from 47800). A command for a Pico without a logger starts one in
the background (log in `/data/sources/tmp/console_SERIAL.txt`), or start it yourself:

```sh
python3 tools/modxo/console.py                                   # 5303284748C4721C   /dev/ttyACM0   logger on 47800, off
python3 tools/modxo/console.py log 5303284748C4721C --out console.txt
python3 tools/modxo/console.py on 5303284748C4721C
```

| Command | Does |
|---|---|
| `console.py` (or `list`) | the connected Modxos: serial number, port, logger and power state; any command below without SERIAL lists them too |
| `console.py log SERIAL [--out FILE] [--raw FILE] [--no-dtr] [--gdb PORT]` | log everything; `--raw` also keeps COM1's bytes unmodified (binary protocols); `--no-dtr` hides COM1 from the console (it raises DTR only for a power press); `--gdb PORT`: also a bridge for gdb to OXOS's GDB debugger on localhost:PORT (`tools/gdb_bridge.py`); `--dev PORT` instead of SERIAL |
| `console.py state SERIAL` | `on`, `off`, `unknown` (no status line yet) or `no pico` (from the status lines' 3v3 level) |
| `console.py on` / `off SERIAL` | switch the console on / off; no press if it already is; a missed press is retried |
| `console.py power SERIAL` | press the power button once (a toggle) |
| `console.py cycle SERIAL [--off S]` | off, wait S seconds (default 8), on |
| `bootrun.py --log FILE [--runs N] ...` | boot N times and classify each boot: PASS / FREEZE / NOBOOT (see `--help`) |
| `flash_uf2.sh FILE.uf2 [PORT]` | flash a firmware or BIOS .uf2 over USB |

Log lines:

```
20:01:10.989 post 20
20:01:13.149 uart OXOS kernel (Oct 25 2026 19:59:11): cold boot
20:01:59.471 pico 3v3=1 lreset=1 d0=0 ior=24525/03fd iow=28572/03f8 memw=4696/04000ff3 memr=235889/ffffd533 ...
```

## With netboot
Flash an OXOS netboot image once (`make -C kernel netboot`; see `tools/netboot/README.md`), then:

```sh
python3 tools/netboot/netboot.py serve.bin --to 192.168.1.255 &       # serve.bin: a symlink to the image
python3 tools/modxo/bootrun.py --log console.txt --runs 5 --image build/kernel/serial/bios_256k.bin --serve serve.bin
```

## Notes
- The console's Super I/O probe and COM1 work only while the logger holds DTR (stock Modxo behaviour); without a logger running, an OXOS console fails to boot
  (red/green FRAG).
- 0xFE and 0xFC mark POST codes and status lines in the stream. Binary COM1 output (a kernel debugger's packets) can contain them; read such output from
  `--raw`.
- A press is ignored while some kernels run (e.g. Microsoft's debug kernel). `on` / `off` retry once and report a failure.

## Troubleshooting
- **`state` says `no pico`, and `lsusb` shows `2e8a:0003 Raspberry Pi RP2 Boot`:** the Pico is in its USB bootloader (BOOTSEL), with its flash intact. Copy the
  firmware .uf2 to the RPI-RP2 drive (or run `flash_uf2.sh FIRMWARE.uf2`), and it restarts. The BIOS bank is not touched.
- **`no logger answers`:** start `console.py log` first. Only one logger can own the port.
