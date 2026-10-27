# 13. Flash mapping and the hardware freeze

OXOS choice, implemented 2026-10-26. It matches the reference kernel (spec/stock/09, spec/stock/23 §5.1).

## 1. Behaviour
- The 2BL maps PDE 0x3FF (0xFFC00000–0xFFFFFFFF, the flash) as a 4 MB write-back page (0xFFC000E3); the kernel
  runs on that mapping from entry until the memory manager starts.
- `MmInitSystem` clears PDE 0x3FF, as the reference does. Nothing in the kernel reads flash through it afterwards:
  the boot splash maps the flash bank with `MmMapIoSpace(..., PAGE_NOCACHE)`, and quick reboot uses the RAM copy
  of `.data` that `KernelEntry` makes.
- Quick reboot's fallback (no RAM copy, `.data` larger than 4 KB) maps PDE 0x3FF again as 0xFFC000FB (uncached)
  before it reads the flash copy.

## 2. Why
Until this change OXOS left PDE 0x3FF mapped write-back for the whole run. Real consoles then froze hard at
start-up: no exception, no machine check, the clock interrupt stopped, at a point that moved with the code layout.
On the no-DVD 1.4 console (Modxo, FATX drive, netboot image test):

| Kernel | Result |
|---|---|
| PDE 0x3FF kept (main before this change) | 0 of 22 boots reached the title |
| PDE 0x3FF cleared | 6 of 6 boots ran the title |
| Code-segment limit 0xBFFFFFFF only (stock value), PDE kept | 0 of 3 |

Interpretation (not proven): the MTRRs make the top 512 KB of flash write-protect, a memory type the CPU may read
speculatively. With a mapping present, speculative or prefetch reads can reach the flash over LPC at any time,
which fits a hang whose location depends on layout. With no mapping, the CPU cannot form those reads.
