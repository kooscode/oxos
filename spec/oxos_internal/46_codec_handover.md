# Spec 46 — The sound codec at a title switch: held in cold reset for every title

OXOS's own design, from observing the AC'97 controller on a console. **Status: implemented** (2026-11-21). What the stock kernel does to the controller and
codec: spec/stock/25.

## 1. The failure
A retail title crashed at its start, now and then, when started from a homebrew dashboard built with nxdk (bug check 0x1E, an access violation in the title:
its sound library's DirectSound start had failed with 0x88780078, the codec not ready within its 20 ms poll, spec/stock/25 §3 row 3, and the title went on with
the null object). Watching the controller every 1 ms on the Conexant debug kit (GLOB_CNT 0xFEC0012C, GLOB_STA 0xFEC00130, both output channels' CR) showed:
| What | Observed |
|---|---|
| The dashboard's sound start and its exit | each pulses the cold reset of a codec that is already running (GLOB_CNT 2 → 0 → 2 within about 2 ms) and waits for GLOB_STA bit 8 |
| GLOB_STA bit 8 after such a pulse | still set for about 25 ms: the wait ends at once on the ready bit of before the reset |
| Usually | the codec comes back, and the dashboard plays sound |
| In the crash runs | bit 8 dropped 25 ms after the dashboard's exit pulse and never came back; every following title found the codec released but not ready |
| GLOB_STA bit 8 while the cold reset is held | stays set; only a power-on clears it |
| A codec held in cold reset since power-on, then released | ready 20 µs later (the 20 ms poll has room to spare) |
The dashboard's code is nxdk's `XAudioInit` (`lib/hal/audio.c`): it pulses the reset unconditionally, polls the stale ready bit without a limit, and never
writes the codec's power or volume registers. The stock kernel does not touch the controller at a title switch (spec/stock/25 §2), so a codec left this way
reaches the next title as it is.

## 2. What OXOS does
- **At every quick reboot** (`KiQuickRebootQuiesce`, with the bus-master stops of spec 25 §5 and §7): PCM out CR 0xFEC0011B ← 0, S/PDIF out CR 0xFEC0017B ← 0
  (no more DMA), then GLOB_CNT ← 0: the cold reset held and the controller's interrupt enable off, as after power-on. The next title releases it:
  | Next title | What its start then does |
  |---|---|
  | an XDK title (its sound library) | finds bit 1 clear, releases the reset and polls bit 8: the codec is ready within its 20 ms |
  | an nxdk title | its reset pulse becomes a plain release; the codec restarts from a clean reset |
  | the boot video player (§3) | releases the reset and waits for the codec |
- **At a cold boot**: nothing; the codec is in its power-on state (held in reset) until the first title releases it.
- **Not done, though the stock kernel does it** (spec/stock/25 §2–§3): the cold-boot release with the codec's power and volume writes, and SMC register 0x0B ←
  1 … ← 0 around it. Tried on the debug kit: with the SMC 0x0B writes every sound stayed silent until the next power cycle (boot video and dashboard; peak
  level −58 dB, the capture card's noise floor, against −8 dB without them); the S/PDIF control word 0x17C ← 0x02000000 alone made no difference. A codec
  released at the cold boot also turned the nxdk dashboard's reset pulse into the failure of §1.
- No codec register is written by the kernel.

## 3. The boot video player's sound start
`shared/video_player/sound_output.cpp` (spec 38) starts the codec as the XDK sound library does (spec/stock/25 §3), without a reset pulse:
1. GLOB_CNT bit 1 clear → set it (release). Bit 1 set and bit 8 clear (released, not ready) → a warm reset: bit 2 set for 10 µs, bits 2 and 3 cleared.
2. Wait for bit 8, at most 500 ms (1 ms steps); not ready → no sound.
3. Through the semaphore: codec 0x26 ← 0x0000, 0x02 ← 0x0000, 0x18 ← 0x0808, always (they were read first and written only when muted: right after a reset of
   a running codec the read gave the values of before the reset, the writes were skipped and the codec came up muted).

## 4. nxdk
The fix of §1's `XAudioInit` (release only a held reset, a warm reset for a released codec that is not ready, bounded waits, the power and volume writes) is
proposed upstream as XboxDev/nxdk#806. Titles built with an nxdk without it start correctly on OXOS because of §2.

## 5. Checked
- Debug kit (Conexant, MCPX X2), netboot: five runs of boot video → nxdk dashboard → ISO launcher → trainer menu → the title that crashed before: the title's
  sound library released the held reset and started (both channels CR 0x1D), sound in the boot video, the dashboard and the title, no crash.
- nxdk's xaudio sample on the debug kit: sound with the codec held at the start (§2) and, with the fixed nxdk, with the codec left running and unmuted before
  `XAudioInit`.
- The full regression: see the commit.

## 6. Code
- `kernel/src/hardware/chipset/smbus.cpp` `KiQuickRebootQuiesce`: §2.
- `shared/video_player/sound_output.cpp` `StartCodec`: §3.
