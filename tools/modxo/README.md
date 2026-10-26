# modxo: a boot-debugging rig built from a Modxo modchip

An RP2040 [Modxo](https://github.com/Shalx/Modxo) modchip on the console's LPC header, with the **oxos_debug**
firmware module, turns one USB cable into a console debug rig:
- **COM1:** the emulated serial port (an OXOS kernel's boot log; `SERIAL=1` / network-log builds).
- **POST codes:** every write to I/O port 0x80, as it happens (the 2BL's and kernel's progress codes).
- **Status lines:** LPC power and reset pins, LPC cycle counts and last addresses, the first memory reads after
  power-up (the boot ROM fetching X-codes).
- **The power button:** pressed from the PC, so test boots run unattended.
- **Reflashing over USB:** BIOS or firmware, without touching the board.

These scripts are the PC side.

## Hardware
- An RP2040 Modxo board on the LPC header, flashed with Modxo built from the **`oxos-debug` branch** of
  [kooscode/modxo](https://github.com/kooscode/modxo). The module is `modxo_modules/oxos_debug`; its README has the
  stream format. Build as Modxo (`cmake` for your board, then `ninja`), and flash the firmware .uf2 with
  `flash-uf2.sh`.
- Power button: GPIO 28 (`OXOS_POWER_PIN`) → 1 kΩ → base of an NPN transistor (e.g. 2N2222). Emitter to Xbox ground,
  collector to the power switch's signal side. On the RP2040-Tiny GPIO 28 is SERIRQ, so that branch leaves the
  SERIRQ module unregistered.
- The BIOS goes into Modxo's flash bank as its own .uf2: `flashbin_to_uf2.py` from Modxo's `tools/` (default base
  0x10040000).
- Python 3 with pyserial (`pip install pyserial`).

## Commands
Start the logger once. It owns the Pico's serial port, and the other commands talk to it:

```sh
python3 tools/modxo/console.py log --out console.txt
```

| Command | Does |
|---|---|
| `console.py log [--out FILE] [--raw FILE] [--no-dtr] [--dev PORT]` | log everything; `--raw` also keeps COM1's bytes unmodified (binary protocols); `--no-dtr` hides COM1 from the console (it raises DTR only for a power press) |
| `console.py state` | `on`, `off`, `unknown` (no status line yet) or `no pico` (from the status lines' 3v3 level) |
| `console.py on` / `off` | switch the console on / off; no press if it already is; a missed press is retried |
| `console.py power` | press the power button once (a toggle) |
| `console.py cycle [--off S]` | off, wait S seconds (default 8), on |
| `bootrun.py --log FILE [--runs N] ...` | boot N times and classify each boot: PASS / FREEZE / NOBOOT (see `--help`) |
| `flash-uf2.sh FILE.uf2 [PORT]` | flash a firmware or BIOS .uf2 over USB |

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
python3 tools/modxo/bootrun.py --log console.txt --runs 5 --image kernel/build/netlog/bios-256k.bin --serve serve.bin
```

## Notes
- The console's Super I/O probe and COM1 work only while the logger holds DTR (stock Modxo behaviour).
- 0xFE and 0xFC mark POST codes and status lines in the stream. Binary COM1 output (a kernel debugger's packets)
  can contain them; read such output from `--raw`.
- A press is ignored while some kernels run (e.g. Microsoft's debug kernel). `on` / `off` retry once and report a
  failure.

## Troubleshooting
- **`state` says `no pico`, and `lsusb` shows `2e8a:0003 Raspberry Pi RP2 Boot`:** the Pico is in its USB
  bootloader (BOOTSEL), with its flash intact. Copy the firmware .uf2 to the RPI-RP2 drive (or run `flash-uf2.sh
  FIRMWARE.uf2`), and it restarts. The BIOS bank is not touched.
- **`no logger answers`:** start `console.py log` first. Only one logger can own the port.
