# 21 — Single-bit RAM changes on the 1.4 console (investigation, resolved)

Status: resolved 2026-10-28. The bad boots were the ones that started with DRAM refresh off; the fix is
spec/oxos-internal/20. This file records what was seen and how the cause was found. Background: spec/stock/21 §3.6a
and §3.8 (pad calibration, its write window, the drift of A), spec/stock/22 §6.3 and §6.3a (memory set-up by the
X-codes and by the stock kernels).

## 1. Symptom
- On the no-DVD 1.4 console (Modxo, NV2A 0xA3), single bits of RAM changed from 0 to 1 a few seconds after the OXOS
  kernel started. The same cells went first, and more changed the longer the kernel ran. The kernel then crashed:
  changed instructions, damaged free lists, or a jump to a bad address as the first title started. The
  machine-check banks recorded nothing.
- It happened only once the console had been powered for about 10 minutes. Then the 2BL's pad measurement A
  (spec/stock/21 §3.4) had drifted from about 0x65 to 0x5C-0x5E. In that band, 21 of 144 boots were bad. None were
  bad in 36 boots at A ≥ 0x5F, and 1 of 53 at A ≤ 0x5B.
- In that band the 2BL writes the retail pad values (spec/stock/21 §3.8) with the retail write window (§3.6a). The
  memory set-up before the 2BL is identical in every reference image for this NV2A (spec/stock/22 §6.3), and the
  stock kernels never change it afterwards (§6.3a). So the pads were not the difference.

## 2. Method
Warm boots back to back on the Modxo console (netboot, power off 8 s, on 25 s). Only boots with A in 0x5C-0x5E
counted. A debug kernel compared its code section with a copy and checked the free lists at checkpoints. It also
logged two things:
- **Register watch:** the memory-interface registers (PFB 0x100200-0x100228, PBUS pads 0x1210-0x1250, PMC, the
  PLLs, host bridge register 0x6C) at each checkpoint, and any change.
- **Persistence:** for the first changed byte, three more reads from RAM (after `wbinvd`), then the original value
  written back and read again after 1 s and 2 s.

Further arms were prepared but not needed once the cause was found: more pad drive in the band, a shorter refresh
interval, a retention test on idle memory, and the stock 4627 chain on the same console.

## 3. Results
| Run | Band boots | Bad | Finding |
|---|---|---|---|
| Baseline (retail calibration, OXOS debug kernel) | 144 | 21 | - |
| Register watch + persistence | 40 | 8 | PFB 0x100210 = 0 at kernel entry in all 8 bad boots, bit 31 set in all 49 good ones. The changed byte read back the same 3 times and stayed good once rewritten, so stored charge was lost once, not misread. |
| Refresh recorded at each boot stage | 44 | 1 | The bad boot already had 0x100210 = 0 at the reset code's entry, so refresh was lost in the X-code stage. |
| Refresh check (spec/oxos-internal/20) | 56 | 0 | The reset code found refresh off on 6 boots and switched it on; those boots were clean. |

## 4. Conclusion
On a warm console, refresh (PFB 0x100210 bit 31) is sometimes off after the X-codes, and nothing later in a
reference boot chain checks it. Without refresh, the weakest DRAM cells lose their charge within seconds. Low A and
lost refresh both go with how warm the console is; the pad values themselves are correct. Why the X-code stage
loses the enable is open (spec/oxos-internal/20).
