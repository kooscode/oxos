# 17. Boot jingle

OXOS addition. On cold boot the splash (spec/oxos-internal/00-boot-splash) comes with a short, original piece of
music for cellos, bass and drums, as the retail boot animation comes with its sound. Implemented 2026-10-27. Code: `kernel/src/hal/jingle.cpp`
(the synthesizer and the score), `kernel/src/hal/bootsound.cpp` (the player); host side `tools/jingle`.

## 1. The music
5.9 seconds, timed to the splash: it starts with the fade-in, which reaches full intensity at the climax. One lead-in
and one climax in D, with no beat:
1. The lead-in, 0–2.6 s: a swoosh (noise through a band-pass rising from 250 Hz to 7 kHz) rises the whole way, and
   cellos swell in under it one after another, each fading in over 1.7 s: the low D at 0.15 s, its fifth at 0.72 s,
   its octave at 1.23 s.
2. The climax at 2.6 s (`jingle::ClimaxMs`): the cello section (root, fifth and octave, and the D above) and the bass
   an octave below on one D chord, with a single taiko, kick and crash, holding to the end.
3. The last 1.2 s fade out on a squared curve, so the chord eases out.

The notes are written for OXOS. Nothing is taken from another piece or from the retail boot sound.

## 2. The synthesizer
Integer arithmetic only: the kernel is built without x87 or SSE code, and the synthesis runs in an ordinary thread
(no floating point in an interrupt). 48 kHz, 16-bit stereo. The working state (delay lines, filters, voices) is one
70 KB block that the caller provides. The same source builds into the kernel and into `tools/jingle`, which renders
it to a WAV file and prints its levels. The render on the PC and the samples the kernel synthesizes in xemu (saved
from guest memory with `boot_test.py --dash --jingle-pcm`) are byte-identical.

| Voice | How it is made |
|---|---|
| Cello | Three sawtooth waves about 0.4% apart, with vibrato (5.5 Hz, coming in over 0.4 s) and a bow attack and release. The rasp: bow noise that scratches once a cycle (loudest as the sawtooth jumps, as the string slips under the hair) over a steady hiss. Then a resonant low-pass at 3.2 kHz and a clipper. |
| Bass | A sawtooth and a sine an octave below it, through a low-pass at 520 Hz. |
| Drums | Kick, taiko and toms: a sine whose pitch falls, plus filtered noise. Snare: a short tone and high-passed noise. Cymbals: six square waves at unrelated pitches, high-passed. Swoosh: see §1. |
| Chime | A square wave with an exponential decay (not in the current score). |
| Reverb | Per side, four comb filters with damping in the feedback and two all-pass filters. |

The mix goes through a soft limiter and fades over its last 1.2 s. Peaks are about −5 dBFS, at the climax.

## 3. Playing it
The MCPX AC'97 controller's memory-mapped registers (PCI BAR2, 0xFEC00000) are used directly. The APU is not.
1. `HalpStartSplash` starts the player thread at priority 7, below the splash thread. So the splash's PNG decode
   is not slowed down, and the picture comes up as soon as it would without the jingle.
2. The thread allocates the sample buffer (32 buffers of 0.225 s, the controller's limit of 32 descriptors, write-combined, below 16 MB as the splash) and its
   state. It cold-resets the codec (global control 0x12C bit 1, then waits for codec-ready, global status 0x130
   bit 8) and unmutes the master and PCM volumes only if they are muted. Then it resets the PCM-out (0x110) and
   S/PDIF-out (0x170) channels.
3. Both channels get the same descriptor list, with every descriptor valid from the start, over a zeroed buffer.
   The thread synthesizes in 50 ms slices until the splash's fade-in begins (`HalpBootSoundGo`), then starts both
   channels. From then on it runs at priority 9, keeping at least 0.6 s of sound ready ahead of the controller and
   sleeping when it is further ahead. On the console, synthesis finishes about 5.3 s after it starts, sharing the
   CPU with the splash; in xemu it takes about 0.35 s.
4. On the console, the MCPX does not stop at the last valid index: it carries on round the descriptor list. So the
   player keeps time itself, 48 frames a millisecond from the start, and stops both channels when the jingle's
   length has passed. A buffer not yet synthesized would play the silence it was cleared to.
5. `HalpEndSplash` waits for the player to finish. It resets both channels, clears their list addresses, frees its
   memory and exits before the title starts. Titles find the controller idle.

Quick reboots (no splash) have no jingle. `HalpBootSoundOn = 0` turns it off.

## 4. Tests
- `make test-dash` also checks that the splash reached full intensity within 100 ms after the climax
  (`HalpSplashFullMs` against `HalpSplashFadeMs`; 2 601 ms against 2 600 ms in xemu).
- `make test-dash`: the boot test reads `HalpBootSoundStatus` (3 = played to the end with no underruns), the
  controller's buffer at the end, the lead at the start and the synthesis time.
- `boot_test.py --dash --jingle-pcm FILE` saves the synthesized samples from guest memory while the jingle plays, to
  compare with `tools/jingle/bin/jingle OUT.wav`.
- Checked on the 1.4 console (Modxo rig): the HDMI capture card's audio input recorded the jingle at the right pitch
  and length (the level envelope correlates 0.75 with the render; HDMI adapters drop about the first 0.7 s while
  they lock), no underruns, the splash's timing unchanged, and the dashboard and titles start as before.
- The lead-in and climax version (2026-10-27), on the 1.4 console: the capture card's picture reaches its final
  brightness at the audio's loudest point (within the 0.1 s of the measurement), the serial log reports the fade
  complete 2.61 s after the jingle started, and no underruns.
