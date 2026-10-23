# Debug serial port (OXOS addition)

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
- Everything passed to DbgPrint (titles' and the kernel's own), as well as the in-memory log (spec/stock/13).
- Title loads: the image path and the loader's status.
- Fatal paths: unexpected CPU exceptions (vector, error code, EIP), bug checks (code and four parameters), and
  fatal errors reported through the fatal-error screen (code).

## 5. Builds
- `SERIAL=1` compiles it in (`XK_DEBUG_SERIAL`). On by default in the self-test build (`make`, `make test`) and
  the troubleshooting builds (`make run-trace-basic`, `make run-trace-full`); off in release builds, where it would cost resident memory.
- `make run-trace-basic` and `make run-trace-full` start xemu with the SuperIO and prints the serial output in the terminal.
- `make test` checks the serial log of the self-test phase.

## 6. Boot status on the front LED (every build)
First implemented 2026-10-22, for consoles without a serial adapter or network. The LED is the SMC's (SMBus 0x20):
register 0x08 is the sequence (bits 7–4 red, 3–0 green, one bit per quarter of the cycle), 0x07 = 1 custom mode.

| Stage | Pattern | Set by |
|---|---|---|
| the 2BL, after the SMC challenge | 0xAA, blinking orange | `kernel/boot/bl.cpp` |
| the kernel, from the end of HAL phase 0 to the title launch (cold and quick boots) | 0x5F, alternating green / orange | `KiSystemStartup` |
| the title (dashboard or game) entered | 0x0F, solid green | the title launcher |
| fatal error | 0xE1, red / green (the reference's) | `IdexFatalError` |
| NETLOG builds, each second after the title launch | 0x0F, then 0xFF (green / orange, slow), or 0xF0 (green / red, slow) once the log goes to the disk file (§7) | the heartbeat thread |

- Flashing red / green is the console's error signal (the SMC's own and the reference's fatal error), so no boot stage
  uses it.
- A stage that never ends shows where a boot stops: the 2BL pattern means the kernel never started (inflate or entry),
  green / orange that it stopped before the title launch (the network log, §7, tells where). In NETLOG builds a colour
  that stops changing once a second means the system itself stopped (interrupts off or the heartbeat blocked).

## 7. Network log (NETLOG=1)
First implemented 2026-10-22 (the first console runs had no serial port). Code: `kernel/src/hal/netlog.cpp`; PC side
`tools/netlog.py`; xemu: `tools/run.py --net-udp PORT`.
- Everything `XK_SERIAL` prints (NETLOG=1 implies SERIAL=1) is also sent as UDP from `NETLOG_IP`:5140 (default
  192.168.1.222) to `NETLOG_DEST`:5140 (default 255.255.255.255, broadcast; a PC's address makes it point-to-point),
  always in an Ethernet broadcast frame (the log never receives, so it cannot resolve a MAC address with ARP; a PC
  accepts a broadcast frame carrying its own IP), source MAC 02:58:42:4F:58:31 (locally administered), up to 1400
  bytes per packet. Broadcasts stay on the local segment (routers do not forward them; some Wi-Fi access points drop
  them).
- **On by default in trace builds** (TRACE_IMPORTS=1 or 2: `make run-trace-full`, `run-trace-xbe`), whose only output on
  a console is the network; `NETLOG=0` turns it off, `NETLOG=1` adds it to any build.
- **No link** (no cable, or xemu without a network backend): if the first packet is not sent within 50 ms the log
  stops for the rest of the boot, so it costs nothing.
- Messages before the memory manager maps the NIC (0xFEF00000) are kept in an 8 KB buffer and sent when the NIC is up
  (`HalpNetLogStart`, right after `MmInitSystem`).
- NIC set-up: the nForce sequence of XboxDev/cromwell's etherboot `forcedeth.c` (GPL-2.0): reset, MAC address, set-up
  registers, two transmit descriptors, transmitter on; the link (PHY 1) through `PhyInitialize`. Transmit only.
- A packet is queued by filling a descriptor (buffer, length − 1, flags last packet | valid) and writing KICK to the
  TxRx control register. **Bit 2 of that register must stay clear** (xemu treats it as "DMA off"; cromwell kicks with
  KICK alone).
- Polled and interrupt-free (the caller's interrupts are disabled while a packet is built), so the bug check and fatal
  error messages go out too; a write re-entered from an interrupt is dropped (`HalpNetDropped`), as is a packet whose
  slot is still busy after 20 ms.
- `tools/netlog.py` prints the lines with arrival times (`--out FILE` keeps them); `--xemu PORT` reads the raw frames
  of xemu's `udp` network backend instead of the LAN.
- **The log ends when a title takes the NIC:** a title with a network stack (the stock dashboard's XNet) resets the NIC
  and its rings when it starts, so nothing after that is sent (seen in xemu and on a console: the heartbeat stops at the
  title start while the title keeps running).
- **Heartbeat:** once a second a priority-30 system thread logs the tick count, the interrupts taken per IRQ line since
  the last beat and the IDE channel state (`hb N: tick … irq L:+n …; ide busy …`), so a stalled boot shows whether the
  system still runs. Serial and network-log builds also detect an **interrupt storm** (more than 5000 interrupts of one
  line in one clock tick), log `*** interrupt storm on IRQ n` and mask the line.
- Checked on a 1.4 console (2026-10-21): link 100 Mbit/s full duplex, the whole boot log through the dashboard load.

## 8. Open questions (hardware)
- Whether the MCPX forwards LPC I/O cycles for 0x2E and 0x3F8 to the LPC header without any set-up on a retail
  board (modded BIOSes that use the adapter report that it works; confirm on our first hardware boot).
