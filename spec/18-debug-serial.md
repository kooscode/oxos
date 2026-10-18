# Spec 18 — Debug serial port (OXOS addition)

Not part of the reference: a retail 4627 kernel never programs a SuperIO, and has no serial output. Debug kernels
and some modded BIOSes do. This spec describes our own, opt-in debug output for bringing OXOS up on hardware.
Sources: the public SMSC LPC47M15x/LPC47M192 configuration-port conventions (as implemented by xemu's `lpc47m157`
device and Team-Resurgent/Modxo's `superio` module), and the standard 16550 UART register set.

## 1. Hardware it works with
- **XboxDev serial-usb-adapter:** an LPC47M192 SuperIO plus a USB-serial chip on the LPC header.
- **Modxo (RP2040 on the LPC header):** emulates an LPC47M152 at 0x2E and a 16550 at 0x3F8, bridged to the Pico's
  USB serial port (no baud rate). The SuperIO reads as absent (0xFF) while no terminal has the USB port open, so the
  terminal must be open before power-on.
- **xemu:** `-device lpc47m157 -serial file:PATH` (or any QEMU chardev). Not present unless asked for.
- Without any of these, LPC I/O reads return 0xFF: the port is reported absent and nothing is written.

## 2. Bring-up (HalpSerialInit, first thing in KiSystemStartup, cold and quick boots)
1. SuperIO configuration port 0x2E (index) / 0x2F (data): write 0x55 to enter configuration mode.
2. Presence: global register 0x26 (configuration port address, low byte) must read 0x2E. Otherwise write 0xAA and
   stop: no serial port.
3. Logical device 4 (serial port 1): register 0x07 = 4; base address 0x60/0x61 = 0x03F8; interrupt 0x70 = 4
   (COM1's usual line; interrupts stay disabled in the UART); activate 0x30 = 1.
4. Write 0xAA to leave configuration mode.
5. UART at 0x3F8: LCR = 0x80 (divisor latch), DLL = 1, DLM = 0 (115200 baud), LCR = 0x03 (8N1), FCR = 0x07 (FIFOs
   on and cleared), MCR = 0x03 (DTR, RTS), IER = 0 (polled).
6. LSR (0x3FD) must not read 0xFF; otherwise the port is reported absent.

## 3. Output
- Polled: wait for LSR bit 5 (transmit holding register empty), then write the byte to 0x3F8. `\n` is sent as
  `\r\n`.
- Each line is written with interrupts disabled, so lines from different threads and interrupt levels never mix.
  At 115200 baud a 100-character line keeps interrupts off for about 9 ms (the PIT tick is latched, so no tick is
  lost, only delayed). Modxo and xemu have no baud-rate delay.
- A byte that is not accepted within about 100 ms disables the port for the rest of the boot (a stuck or removed
  adapter must not hang the kernel).

## 4. What is sent
- A banner at each boot (cold or quick), then each start-up step of KiSystemStartup.
- Everything passed to DbgPrint (titles' and the kernel's own), as well as the in-memory log (spec/13).
- Title loads: the image path and the loader's status.
- Fatal paths: unexpected CPU exceptions (vector, error code, EIP), bug checks (code and four parameters), and
  fatal errors reported through the fatal-error screen (code).

## 5. Builds
- `SERIAL=1` compiles it in (`XK_DEBUG_SERIAL`). On by default in the self-test build (`make`, `make test`) and
  the troubleshooting build (`make run-trace`); off in release builds, where it would cost resident memory.
- `make run-trace` starts xemu with the SuperIO and prints the serial output in the terminal.
- `make test` checks the serial log of the self-test phase.

## 6. Open questions (hardware)
- Whether the MCPX forwards LPC I/O cycles for 0x2E and 0x3F8 to the LPC header without any set-up on a retail
  board (modded BIOSes that use the adapter report that it works; confirm on our first hardware boot).
