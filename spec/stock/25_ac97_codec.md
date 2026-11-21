# Spec 25 — The AC'97 controller and codec: what the stock kernel does to them (xboxkrnl 1.0.5838.1; 5713, 4627 and EvoX M8 compared)

Clean-room behavioural spec. Sources: Ghidra 12.1.4 analysis (REA) and a whole-image objdump listing of the shipped binaries below. Addresses are 5838 unless
marked. No code is reproduced: behaviour, order, values and the reference's addresses as citations only. **Status: reference only** (what OXOS does instead:
spec/oxos_internal/46).

| Image | sha256 | Role |
|---|---|---|
| `5838-unpacked/xboxkrnl.exe` | 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde | primary |
| `5713-unpacked/xboxkrnl.exe` | d39084e06ca57340750a68d83b5cefc5c291455dc9639b352ddf410882f02db1 | compared |
| `xboxkrnl-4627.exe` | 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f | compared |
| `evoxm8_unpacked/xboxkrnl.exe` (EvoX M8, a patched 5838) | 402da98541f522bdb4b9436dba984f525023af617ed16924db2e3b55d1afdb94 | compared |

## 0. What was searched
- Every instruction with an immediate address or displacement in 0xFEC00000–0xFEC001FF (the controller's memory BAR), including the table-based forms
  (0xFEC00100 + channel offset, 0xFEC00000 + register); every I/O-port access in 0xD000–0xD0FF and 0xD200–0xD2FF (the I/O BARs): none in any image, the BARs
  are only programmed (spec 23 §3.1 row 9); every APU address 0xFE800000–0xFE85FFFF in resident `.text` (0x800102C0–0x800367BF): none.
- Byte-pattern hits for 0xFEC0xxxx outside those (0x80022B29, 0x8002A160, 0x8003F69A, 0x80046E09, 0x80047762, 0x800678AE, 0x80073C3B) are other instructions or
  data; 0x80052C80/0x80052C84 are the base-address table below.
- **Result:** every AC'97 access lies in INIT 0x8004D7F3–0x80052640, the kernel's copy of the title sound library (pool tag 'DSND'), which only the boot
  animation uses. INIT is freed before the first title (spec 23 §2.8): **no resident code touches the AC'97 at any time**.

## 1. Registers used
| Offset from 0xFEC00000 | Register | Use |
|---|---|---|
| 0x000–0x07F | codec (mixer) registers, 16-bit | written only: 0x26 power-down control, 0x02 master volume, 0x18 PCM-out volume. **Never read** |
| 0x110–0x11B | PCM out bus master: BDBAR +0, CIV +4, LVI +5, SR +6, PICB +8, CR +0xB | channel 0 |
| 0x170–0x17B | S/PDIF out bus master, same layout | channel 1 |
| 0x17C | S/PDIF control word | 0x02000000 (meaning not determined) |
| 0x12C | GLOB_CNT | bit 1 cold reset released; bits 2/3 (warm reset, link off) cleared once if bit 3 was set; nothing else written |
| 0x130 | GLOB_STA | bit 8 primary codec ready (polled); bits 4 / 6 the S/PDIF / PCM-out interrupts (cleared by the interrupt service); bit 9 never tested |
| 0x134 | codec access semaphore | bit 0 clear: free (reading it claims it) |
PCM in (0x100) and microphone (0x120) are never accessed.

## 2. When the kernel touches it
| Case | AC'97 accesses |
|---|---|
| Cold boot | the boot animation thread's first device action (phase 1, after the EEPROM step, before the disk set-up; 0x800159CD → 0x800558C3 → thread 0x8005588A → 0x8005727E): SMC register 0x0B ← 1 (0x800563A3), create #1 (§3), scene set-up, release #1 (§4), SMC 0x0B ← 0; then, with the full animation, the boot sound's create #2 (§3 again, 0x80061118) and release #2 at its end (0x8006137A). Short animation (boot flag 0x10): create #1 / release #1 only |
| Before the first title | 0x800557EF waits for the animation thread (no timeout); INIT is then freed |
| Quick reboot (`HalReturnToFirmware(2)`, every title-to-title launch) | **none**: the animation does not run, and the controller and codec keep what the previous title left. The shutdown notifications run first (spec 12), which is where a title's own sound library stops its channels |
| Full reset or power cycle | the hardware resets; the next cold boot runs §3 again |

## 3. Bring-up (create #1; create #2 identical)
Object create 0x80052BA2 → device 0x80052A97 → device init 0x80052996 → core init 0x800528C8 → AC'97 set-up 0x800525A1 → AC'97 init 0x800523CA.
| # | Address | Register | Value / behaviour |
|---|---|---|---|
| 1 | 0x8004D81F | GLOB_CNT | read once; bit 1 clear → written with bit 1 set (**the cold reset released**). The kernel never asserts the cold reset: it is held from power-on until this write |
| 2 | 0x8004D81F | GLOB_CNT | only if bit 3 is set: written with bits 2 and 3 cleared |
| 3 | 0x8004D81F | GLOB_STA | bit 8 polled: 20 µs apart, at most 1000 times (≈ 20 ms). Timeout: AC'97 init fails with 0x88780078, no retry, no warm reset; create #1's caller ignores it (the reset stays released, so the codec keeps coming up for create #2) |
| 4 | 0x8004D89F | codec 0x26 | ← 0x0000 (every section powered), through the semaphore (§5); semaphore timeout → E_FAIL, set-up stops |
| 5 | 0x80052409 | CR 0x11B, 0x17B | ← 0x02 each (reset registers), no wait |
| 6 | 0x800523CA | — | interrupt: bus IRQ 6, level-sensitive, shareable (ISR 0x8005162B → 0x8005102B) |
| 7 | 0x80052465 | codec 0x02 | ← 0x0000 (0 dB, not muted) |
| 8 | 0x8005247B | codec 0x18 | ← 0x0808 (0 dB, not muted) |
| 9 | 0x80052490 | — | a shutdown notification registered (its teardown) |
| 10 | 0x8004F3E1, 0x8005070A | PCM out | 256-byte descriptor list; reset (CR ← 0x02, then until CR bit 1 reads 0, **unbounded**), BDBAR ← list, CIV ← 0, LVI ← 0 |
| 11 | 0x8004F446 | PCM out | one descriptor: the 8 KB output buffer (length 0x1000 samples, interrupt on completion, underrun policy bit); LVI ← 0 |
| 12 | 0x8004F3E1, 0x8005070A | S/PDIF out | as row 10, and 0x17C ← 0x02000000 during the reset |
| 13 | 0x8004F446 | S/PDIF out | as row 11, the same buffer |
| 14 | 0x80052632 | 0x17C | ← 0x02000000 |
| 15 | 0x80052957 | S/PDIF out | SR bit 0 (halted) set → reset again as row 12; CR ← 0x1D (run, last-valid, FIFO-error and completion interrupts) |
| 16 | 0x80052964 | PCM out | likewise; CR ← 0x1D |
Not done: codec register 0x00 (reset), 0x28/0x2A, the sample-rate registers, any codec read, any GLOB_CNT interrupt enable or warm reset, the I/O BARs.

## 4. Teardown (release #1 and #2) and the state a title finds
Object destructor 0x80050860 → device destructor 0x800527FB: interrupt disconnected (0x800197E0), shutdown notification removed; each channel (0x80051F71 →
0x800507AD → 0x8004F54D): CR ← 0, reset (as §3 row 10, and 0x17C ← 0x02000000 for S/PDIF), CIV ← 0, LVI ← 0; descriptor list and buffers freed; the APU
stopped (spec 23 §2.4).
| Register | Before the first title (cold boot) |
|---|---|
| GLOB_CNT | bit 1 set (released) |
| GLOB_STA | bit 8 set (if the codec came up) |
| Codec | out of reset, 0x26 = 0x0000, 0x02 = 0x0000, 0x18 = 0x0808, the rest at reset defaults |
| PCM out, S/PDIF out | CR = 0, registers reset, CIV = LVI = 0, BDBAR = the freed list's address; 0x17C = 0x02000000 |
| Interrupt | disconnected; no channel interrupt enabled |
| SMC 0x0B | 0 |
A title's sound library (the same code) then finds the codec released and ready at its first poll, resets and reprograms both channels and 0x17C, and
writes 0x26, 0x02 and 0x18 again.

## 5. Helpers
| Helper | Behaviour |
|---|---|
| 0x8004D7F3 semaphore | 0x134 bit 0 clear → free; set → 20 µs stall and again, at most 1000 times; then failure |
| 0x8004D882 codec write | semaphore, then a 16-bit store at 0xFEC00000 + register; not acquired → nothing written |
| 0x8005102B interrupt service | GLOB_STA & 0x51 = 0 → not ours; else per channel with its bit set: SR read, bit 3 (buffer complete) advances its index, SR written back; GLOB_STA ← original & 0x50 |

## 6. Versions
| Image | Behaviour |
|---|---|
| 5713 | INIT 0x8004D574–0x80052D7F byte-identical to 5838 (same addresses) |
| 4627 | the same values and order at other addresses: semaphore 0x8004DF53, reset/ready 0x8004DF7F, codec write 0x8004DFE2, 0x26 at 0x8004DFFF, init 0x80052B2A (0x02 and 0x18 at 0x80052BC5 / 0x80052BDB, IRQ 6), 0x17C at 0x80052D8D, run 0x8004FD43; animation 0x8001583D → 0x80055FBA → 0x800579F4 with the same SMC 0x0B pattern |
| EvoX M8 | the sound library range byte-identical to 5838; EvoX's own code (0x800A8C92–0x800BA530) has no AC'97, APU or SMC 0x0B reference. Behaviour = 5838 |

## 7. Unknown
- The meaning of 0x17C = 0x02000000, of GLOB_CNT bit 3 on the MCPX, and of SMC register 0x0B (written 1 around the bring-up and in the shutdown sequence).
- How long a codec takes to become ready after the release on each console revision.
