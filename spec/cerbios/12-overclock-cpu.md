# Cerbios spec 12: clocks, crystal, CPU speed, CPU upgrades

Studied in Cerbios 3.1.0 Beta: the extension's PLL, crystal and CPU-speed routines, the kernel entry's microcode load
and the section-load hook. Behaviour and formats only.

## 1. Overclocking (OVERCLOCKING +0xEC, CPUMPLLCOEFF +0xED, NVPLLCOEFF +0xF1)
Only when OVERCLOCKING is set and not in safe mode:
- A coefficient of 0 means "keep the current value": NVPLLCOEFF 0 → the current GPU PLL register (low 24 bits of
  NV2A register 0xFD680500); CPUMPLLCOEFF 0 → the current CPU PLL value (low 24 bits of PCI bus 0, device 0,
  function 3, register 0x6C).
- **GPU:** the low 24 bits are compared with the register; if they differ, the value is written to 0xFD680500 and
  kept there through a busy loop of 12 500 000 iterations (the PLL settling time).
- **CPU:** the value is formed as (coefficient & 0xFFFFFF) | 0x01000000 and compared with the current register; if it
  differs, a counter in the current thread's kernel block (+0x68) is decremented, the value is written through the PCI configuration ports (address
  0x8000036C to 0xCF8, value to 0xCFC), a 12 500 000-iteration busy loop follows, then the counter is restored
  (with a software interrupt if work is pending: the same pattern as leaving a critical region).
- Order: GPU first, then CPU.

## 2. Crystal (XTALFREQ +0x10A, 0–2)
The base clock used for the speed calculations: 2 → **13.5 MHz**; 1 → **16.666 666 666 MHz**; 0 (automatic) →
13.5 MHz when bits 18–19 of MSR 0x2A equal 2, else 16.667 MHz. (Kept internally in millihertz: 13 500 000 000 and
16 666 666 666.)

## 3. CPU speed measurement
- Run at start-up (forced on a cold boot) and whenever the CPU PLL register is non-zero:
  - multiplier from MSR 0x2A bits 22–27 (masked with 0x2F), the processor's bus-ratio encoding, as ×10 values: 0 → 50, 1 → 30, 2 → 40, 4 → 55, 5 → 35,
    6 → 45, 9 → 70, 10 → 80, 11 → 60, 13 → 75, 15 → 65, 0x20 → 90, 0x24 → 95, 0x26 → 85, 0x2A → 130, 0x2B → 100,
    0x2C → 140, 0x2F → 105; others 0;
  - with N = PLL bits 0–7 and M = bits 8–15: CPU Hz = crystal / N × M × multiplier / 10 / 1000 (crystal in mHz);
  - if the multiplier is unknown or N or M is 0: **733 333 333 Hz**.
- The result is kept as the kernel's CPU frequency (read by §5), and frequency / 2 000 000 is stored next to it
  (its reader is open).
- The GPU clock is computed the same way from 0xFD680500 (N bits 0–7, M bits 8–15, post-divider 2^P with P from
  bits 16–23).

## 4. CPU microcode
Early in the cold-boot start-up (before the interrupt table is loaded; not on a quick reboot), the CPUID signature is read; for **0x6B1, 0x6B4, 0x686 and 0x68A** the
kernel loads the matching 2 KB microcode update stored in its INIT section (WRMSR 0x79 with the update's address).
Other CPUs get none.

## 5. ADVCPUSUPPORT (+0xE5): title fixes for faster CPUs
When an XBE section is loaded, if the extension is present, the measured CPU speed is **above 734 000 000 Hz** and
ADVCPUSUPPORT is set:
- the title is eligible unless its title ID is 0x09999990–0x09999991 or 0x0FACE007–0x0FACE008 (not retail IDs:
  absent from the title-ID database), or its certificate title name is `LithiumX`;
- for sections named `WMADEC`, `XMVDEC`, `.text`, `XNET`, `D3D` or `BINK`, the section is scanned for specific
  instruction sequences (their constants are stored encoded relative to a key value set at start-up) and those are
  rewritten, so library code that assumes a 733 MHz CPU keeps working.

## Open
- The exact instruction sequences and replacements of §5 (table-driven; to be derived again from the library code
  they target rather than copied).
- Whether the GPU clock value of §3 is used anywhere (only computed in the code studied), and the reader of the
  frequency / 2 000 000 value.
