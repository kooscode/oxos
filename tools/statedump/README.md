# statedump: machine-state probe

`statedump.h` prints the CPU, chipset and GPU set-up a kernel leaves behind (control registers, descriptor tables,
a safe list of Pentium III MSRs, PCI configuration spaces, NV2A PFB / clock / CRTC registers, PIC / ELCR, the ACPI
I/O block, page-directory entries), one `sd ...` line at a time, through a print function the caller supplies.
Header-only and freestanding: port I/O, MSR reads and plain memory reads, no kernel imports.

- **OXOS**: build with `EXTRA_FLAGS=-DXK_EXPERIMENT_STATEDUMP`; the lines go to the network log (after the memory
  manager and before the title launcher).
- **A reference kernel**: compile it into a small blob linked at a free address, inject it into the kernel image and
  call it at the end of start-up (2026-10-24: done for a 5838-based kernel through `experimental/`, untracked
  because the image is not ours).

Compare two dumps line by line; spec/stock/01 §4.1 records the first comparison.
