# Spec 46 — The sound codec at a title switch: left running, and recovered when it freezes

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

## 2. What OXOS does at a title switch (revised 2026-11-23)
- **At every quick reboot** (`KiQuickRebootQuiesce`, with the bus-master stops of spec 25 §5 and §7): PCM out CR 0xFEC0011B ← 0, S/PDIF out CR 0xFEC0017B ← 0
  (no more DMA). **The codec's cold reset is left as the title left it**, as the stock kernel leaves it (spec/stock/25 §2: after its one release at the cold
  boot it never asserts the reset again).
- **At a cold boot**: nothing; the codec is in its power-on state (held in reset) until the first title releases it.
- No codec register is written or read by the kernel: on the MCPX a codec access with no codec answering hangs the console (§3.1).

### 2.1 Why the hold of 2026-11-21 was dropped
The first version of this section held the codec in cold reset (GLOB_CNT ← 0) at every quick reboot, so each title restarted it from a clean reset (it cured
§1's crash). An automated loop on the Conexant debug kit (2026-11-23: a test title that starts the sound through nxdk's `XAudioInit`, plays a tone for 10 s,
checks that the PCM DMA moves and relaunches itself through `oxos_title_launch`, while the capture card's sound level was measured for every round) showed
the hold freezes the codec:
| Kernel at the quick reboot | Title starting the sound as the fixed nxdk (no reset of a running codec) | Title pulsing the cold reset of the running codec at start and exit (older nxdk) |
|---|---|---|
| holds the cold reset (2026-11-21 to 23) | frozen at round 13 | frozen at round 16 |
| leaves it (as stock) | 113 rounds, none frozen (stopped by hand) | frozen at round 9 |
- **Frozen:** the PCM DMA runs but its position never moves (the codec gives the AC-link no bit clock); HDMI sound at the capture card's noise floor
  (-70 dBFS against -18 for the tone). Every later round stays frozen, through any number of holds and releases; only removing the power (an SMC power
  cycle) brings the codec back.
- **The common factor:** the codec's cold reset asserted after the codec had been running, by the kernel's hold or by the title's pulse. A likely mechanism
  (inferred, not proven): at the release of the cold reset an AC'97 codec samples SYNC and SDATA_OUT, and either high puts it in a vendor or ATE test mode,
  outputs and bit clock off, until power is removed; with no bit clock the controller's SYNC stays at the level it had, so each reset of a running codec is a
  gamble and later resets cannot help.
- §2.1 of 2026-11-21 (the stale ready bit after a held reset, "harmless") is superseded: the stale bit let titles talk to the codec before it was up, and
  with a frozen codec it is all the controller ever shows.
- Titles that pulse the reset themselves still freeze it, as on the stock kernel (§1); §3 recovers from that.

## 3. Recovering a frozen codec (2026-11-23)
User, 2026-11-23: power-cycle and make the interrupted launch again, with a guard and a toast ("Audio Codec Frozen - Restarting"). Code: the kernel module
`kernel/modules/sound_recovery.cpp` (spec 43: nothing resident beyond the hooks), called by the title launcher.
1. **At every quick reboot**, after the kernel modules are mapped and before the next title is chosen: when the codec is out of reset (GLOB_CNT bit 1), the
   PCM output's DMA is run over a silent buffer (one descriptor, 1024 samples, underrun policy set) for up to 20 ms. Its position moves within a fraction of
   a millisecond on a working codec; it does not move on a frozen one. The channel is reset afterwards (as the stock kernel's teardown leaves it).
2. **Frozen:** the launch the quick reboot was about to make (the launch data page, when there is one, and the disc images mounted, `SavedVirtualDiscs`) is
   written to `C:\OXOS\cache\oxos_sound_recovery.bin` (state "pending"), "Audio Codec Frozen - Restarting" is drawn on the video overlay at once (spec 36,
   `toasts::ShowNotice`, without the queue), and 2 s later the console is powered off and on through the SMC (spec 22 §6).
3. **The cold boot** that follows finds the pending file: the launch data page and the disc images are put back, the file is marked "relaunched", and the
   launch is made as the quick reboot would have made it, without the boot video. Any other cold boot deletes a file it finds.
4. **The guard:** the codec frozen again at the first quick reboot after a relaunch (before any working test) means the title freezes it every time: no
   second power cycle; the file is deleted, and a kernel toast says "Audio Codec Frozen" / "Power off and on to restore sound" once the next title has set its
   video mode (spec 36 §3.4; it needs the extensions' run loop, so it shows when an extension is loaded, as the shipped in-game shortcuts are). A working
   test deletes the file.

### 3.1 What the test never does
It never reads or writes a codec register: a codec read with no codec answering hangs the console on the MCPX (seen twice on 2026-11-23: a watcher thread's
read while a title reset the codec, and a read of a frozen codec), and reading the codec-access semaphore (0xFEC00134) takes it (a title's sound start then
timed out on it). Only the controller's bus-master registers are used.

## 4. The boot video player's sound start
`shared/video_player/sound_output.cpp` (spec 38) starts the codec as the XDK sound library does (spec/stock/25 §3), without a reset pulse:
1. GLOB_CNT bit 1 clear → set it (release). Bit 1 set and bit 8 clear (released, not ready) → a warm reset: bit 2 set for 10 µs, bits 2 and 3 cleared.
2. Wait for bit 8, at most 500 ms (1 ms steps); not ready → no sound.
3. Through the semaphore: codec 0x26 ← 0x0000, 0x02 ← 0x0000, 0x18 ← 0x0808, always (they were read first and written only when muted: right after a reset of
   a running codec the read gave the values of before the reset, the writes were skipped and the codec came up muted).

## 5. nxdk
The fix of §1's `XAudioInit` (release only a held reset, a warm reset for a released codec that is not ready, bounded waits, the power and volume writes only
once the codec is ready) is proposed upstream as XboxDev/nxdk#806, together with a second fix in the same file: the descriptor lists live in the global
`ac97Device`, which could straddle a page boundary, and the bus-master engines read them by physical address, so past the boundary they read unrelated
memory and ran past the last valid descriptor (no sound, or a looping buffer, on some boots only); `ac97Device` is now page-aligned. Titles built with an
nxdk without the first fix start correctly on OXOS because of §2; the second is not something the kernel can make up for. OXOS's console apps build with
kooscode/nxdk (branch oxos), which has both.

## 6. Checked
- 2026-11-21 (the hold, since dropped): debug kit (Conexant, MCPX X2), netboot: five runs of boot video → nxdk dashboard → ISO launcher → trainer menu → the
  title that crashed before: the title's sound library released the held reset and started, no crash.
- 2026-11-23: the loop of §2.1 (its table), and the recovery of §3 under the pulsing test title: see the commit.
- nxdk's xaudio sample on the debug kit: sound with the codec held at the start (§2) and, with the fixed nxdk, with the codec left running and unmuted before
  `XAudioInit`.
- The full regression: see the commit.

## 7. Code
- `kernel/src/hardware/chipset/smbus.cpp` `KiQuickRebootQuiesce`: §2.
- `kernel/modules/sound_recovery.cpp`, its hooks in `kernel/src/titles/title_loader.cpp` (`TitleThread`, `LaunchAtColdBoot`), `toasts::ShowNotice` and
  `toasts::QueueKernelToast` (`kernel/src/toasts/`), `storage::CopyVirtualDiscMounts` / `RestoreVirtualDiscMountsFrom`: §3.
- `shared/video_player/sound_output.cpp` `StartCodec`: §4.
