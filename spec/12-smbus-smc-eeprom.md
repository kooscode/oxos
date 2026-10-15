# Spec 12 — SMBus, SMC, EEPROM, firmware return

Evidence: batch_decompile `ev_4cc1cd36…` (AvGetSavedDataAddress, AvSendTVEncoderOption, HalReadSMBusValue,
HalWriteSMBusValue, HalReturnToFirmware), `ev_6316410b…` (SMBus transaction start 0x8001460c / 0x800146df),
`ev_1ae40f50…` (ExQueryNonVolatileSetting, ExSaveNonVolatileSetting, HalReadSMCTrayState), `ev_1f71a036…` (setting
lookup, checksum, EEPROM shadow read/write), objdump of the SMBus DPC (0x80014bfa) and the SMC state machine
(0x800147a8…), and the setting tables at 0x80010918 / 0x80010930. **Status: implemented (milestones 4b-1/4b-2), except the
SMC interrupt state machine and the per-mode AV tables.**

## 1. SMBus host (MCPX, I/O 0xC000)
- Registers: +0 status (written back to clear), +2 control, +4 address (slave << 1 | read), +6 data (byte/word),
  +8 command.
- A transaction writes the address, command (and data for writes), clears the status, then control
  **0x1A** (byte) / **0x1B** (word). This starts the cycle with the interrupt enabled.
- **Completion** (IRQ 11 → DPC), by status:
  - 0x02/0x04 (collision/protocol): retry after 2 ms, up to 20 attempts
  - 0x10: success; the data comes from +6
  - 0x20: STATUS_IO_TIMEOUT
  - anything else: STATUS_IO_DEVICE_ERROR
- `HalReadSMBusValue(slave, command, word, *value)` / `HalWriteSMBusValue(slave, command, word, value)` are
  serialised by a kernel lock inside a critical region. The caller waits on a per-transaction event.
- Ours polls the status register instead of using the interrupt (same retry policy).

## 2. SMC (slave 0x20)
- Register 0x03 tray state: `HalReadSMCTrayState(&state, &changeCount)`. A cached value is used if the SMC interrupt
  path has one. Otherwise read 0x03: state = value & 0x70, kept only if 0x40 (no media) or 0x60 (media), else 0x10.
- `HalReturnToFirmware(routine)`:
  - routine 2 (quick reboot): re-enter the kernel image after the shutdown notifications
  - routine 4 (fatal error): first SMC 0x1B ← 2
  - otherwise: run the shutdown notification list, then reset through the SMC
  - ours: SMC 0x02 ← 0x01 (reset) or 0x80 (halt = power off); quick reboot is not done yet
- The SMC event state machine (power button, tray, AV pack; registers 0x11, 0x0B, 0x19…) is not implemented yet.

## 3. EEPROM (slave 0xA8, 256 bytes)
- Read once into a 256-byte shadow (byte reads). Writes go word by word for the changed words, and update the shadow.
- **Sections:**
  - security 0x00–0x2F, encrypted
  - factory 0x30–0x5F: checksum, serial[12], MAC[6], online key[16], video standard
  - user 0x60–0xBF: checksum, time zone, language, video/audio flags, …
- **Checksum:** 32-bit sum of the section's dwords with end-around carry. A section is valid when the sum is
  0xFFFFFFFF; the first dword stores ~sum of the rest.
- **ExQueryNonVolatileSetting(index, &type, buffer, length, &resultLength):**
  - 0x104 → game region (REG_DWORD)
  - 0xFFFF → the whole EEPROM; 0xFFFE → the security section (REG_BINARY)
  - 0x100 + n → factory descriptor n; n < 0x100 → user descriptor n. Descriptors are {type, index, size,
    offset}; the full tables are in `src/ex/eeprom.cpp`.
  - unknown index → OBJECT_NAME_NOT_FOUND; short buffer → BUFFER_TOO_SMALL; bad section checksum →
    STATUS_DEVICE_DATA_ERROR
- **ExSaveNonVolatileSetting:**
  - 0xFFFF writes all 256 bytes; user settings rewrite the user section with a new checksum
  - a corrupt section is rebuilt from zeros
  - a longer value → INVALID_PARAMETER
- **Security section:** {HMAC-SHA1[20], confounder[8], HD key[16], game region[4]}.
  RC4 key = HMAC-SHA1(XboxEEPROMKey, digest); plaintext authenticated by HMAC-SHA1(XboxEEPROMKey, plaintext) ==
  digest. This gives XboxHDKey and the console game region used by the XBE loader.

## 4. AV (milestone 4b-2: partial)
Evidence: `ev_179f3a58…` (AV capabilities 0x8002fc8f, AvSetSavedDataAddress, encoder option write-back 0x8002fdd2),
`ev_e1e1a834…` (AV pack mapping 0x8002fc57, macrovision table writer 0x8002fa11), `ev_82a2118e…` (AvSetDisplayMode).
- **HalBootSMCVideoMode** = the SMC AV pack code (register 0x04). It maps to AV_PACK as 0→SCART(3), 1→HDTV(4),
  2→VGA(5), 3→RFU(2), 4→S-Video(6), 6→standard(1), else none(0).
- **AV_QUERY_AV_CAPABILITIES (option 6)** = (user setting 8 & 0xFFFF0000) | AV pack | (factory setting 0x103 &
  0xC0FF00, default 0x400100 = NTSC-M, 60 Hz).
- **Other options:**
  - 9 blanks/unblanks via VGA sequencer register 1 (0x21 off / 0x01 on, plus port 0x80D3 = 4 when on)
  - 0xC forgets the current mode
  - 0x10 → encoder is Focus
  - the rest program encoder registers over SMBus
- **AvSetDisplayMode(regs, step, mode, format, pitch, framebuffer):**
  - mode 0 = 0x0F010101
  - format 0x10/0x1C → CRTC 0x28 depth code 2; 0x11 → code 2 and RAMDAC general control 0x101030; else code 3 with
    control 0x100030
  - same mode: only RAMDAC general control (0x680600), CRTC 0x13/0x19/0x25 (pitch / 8), CRTC 0x28 (depth | 0x80) and
    the scanout start (0x600800) are rewritten, then the encoder flicker/size options
  - new mode: wait for vblank, unlock CRTC (0x1F = 0x57), disable (0x21 = 0xFF, 0x28 = 0), program the encoder table
    for the mode (mode & 0x7F), the RAMDAC/PCRTC register list ((mode >> 16) & 0xFF), the VGA attribute/graphics/
    sequencer defaults and CRTC table ((mode >> 8) & 0xFF), then pitch/format, wait 3 vblanks, set the palette/scanout
- **Ours:**
  - AV capabilities, saved-data address, blank, null-mode and encoder-type options
  - for every mode: VGA 640x480 timings from the public VGA register model, plus the stock pitch/format/start
    programming. The per-mode encoder tables (needed on real hardware) still have to be derived.

- AvGetSavedDataAddress returns the persisted framebuffer pointer. AvSendTVEncoderOption is a large option switch
  (encoder at 0x8A Conexant / 0xD4 Focus / 0xE0 Xcalibur).
- AvSetDisplayMode programs NV2A PRAMDAC/PCRTC (via the register file at 0xFD000000) and the encoder, from per-mode
  tables. The tables must be re-derived from hardware documentation, not copied.
