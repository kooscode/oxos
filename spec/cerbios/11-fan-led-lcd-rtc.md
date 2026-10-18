# Cerbios spec 11: fan, front LED, LCD, external RTC

Studied in Cerbios 3.1.0 Beta: the extension's fan and LED set-up and LCD start, the kernel's LCD drivers and RTC
read. Behaviour and formats only. SMC = SMBus device 0x20 (write address).

## 1. Fan (OVERRIDEFAN +0xD6, FANSPEED +0xD7)
At boot, if OVERRIDEFAN is set, FANSPEED (signed byte) is applied:
- FANSPEED > 0: SMC register **0x05 = 1** (manual fan mode) then register **0x06 = FANSPEED / 2**, clamped to 1–50
  (so FANSPEED is a percentage), 1 ms pause after each write; the pair is written twice.
- FANSPEED ≤ 0: register 0x05 = 0 and 0x06 = 0 (automatic), the same way; skipped on a quick reboot of a console
  with the Xcalibur video encoder.

## 2. Front LED (FRONTLED +0xD1, 4 characters)
Each of the 4 characters is one step of the LED's 4-step cycle: `G` green, `R` red, `A` amber (both), anything else
off. Step i sets bit i (green) and/or bit i + 4 (red) of the pattern byte. The pattern is written to SMC register
**0x08**, then register **0x07 = 1** (use the pattern). Default `GGGG` = 0x0F; safe mode `RRRR` = 0xF0. The dashboard
fallback failure uses pattern 0xE1 (spec 09 §2).

## 3. LCD (INAPPLCDENABLE +0xDD, LCDI2CADDR +0xDE, LCDPROTO +0xDF, LCDBUS +0xE0)
- LCDI2CADDR is the 7-bit I²C address (default 0x3C), doubled at boot to the SMBus write address.
- **LCDPROTO 0** — character LCD of the HD44780 type, driven 4 bits at a time:
  - LCDBUS 1: through an LPC-bus register interface (with short waits between nibbles); LCDBUS 0: through the SMBus
    to an I²C port expander at LCDI2CADDR.
  - Initialisation: nibble 0x3 three times, 0x2, then commands 0x28, 0x0C, 0x01 (with a pause), 0x06, 0x02.
  - Cursor positioning: command 0x80 | (row start + column), row starts 0x00, 0x40, 0x14, 0x54 (20 × 4 display).
- **LCDPROTO 1** — I²C character OLED/LCD controller at LCDI2CADDR; each byte is sent as an SMBus write with control
  byte 0x80 (command) or 0x40 (data). It is initialised with 31 command and data bytes: function set with the extended
  and OLED command sets (0x2A/0x79/0x78/0x28 toggles), clock divider, display mode, internal regulator, contrast,
  pre-charge and VCOM levels, then clear display and display on (0x01, 0x0C). Take the sequence from the controller's
  data sheet (the command bytes are those of the US2066/SSD1311 family). Row starts 0x00, 0x20, 0x40, 0x60.
- The display is probed once (a failed first write means no display, and the LCD feature stays off).
- **In-app display** (INAPPLCDENABLE, not in safe mode): the extension links `\??\LCD-E:` → Partition1 and
  `\??\LCD-F:` → Partition6, opens both (to query free space), writes a fixed layout and starts a kernel thread that
  refreshes the values while titles run:

  | Row | Column 0 | Column 8 / 10 | Column 14 |
  |---|---|---|---|
  | 0 | `-= Cerbios v3.1.0 =-` | | |
  | 1 | `CPU:` | `F:` (col 8) | `U:` |
  | 2 | `E:` | `F:` (col 10) | |
  | 3 | `MEM:` | `FPS:` (col 10) | |

  The frame count behind FPS is kept by the graphics hook (spec 13 §5).

## 4. External RTC (RTCENABLE +0xE1)
On a cold boot, if set: read registers 0–6 of the SMBus device **0xD0** (a DS1307-style clock, BCD): seconds,
minutes, hours (6 bits, 24-hour), weekday (3 bits), date, month, year (+2000). If every read succeeds, the values set
the system time and the HAL clock. A failed read leaves the time as it is.

## Open
- The LPC register addresses of LCDBUS 1 (one write is to port 0xF503).
- Which values the refresh thread writes after each label (CPU, F, U, E, F, MEM).
