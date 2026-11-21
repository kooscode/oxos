# Cerbios: all specs and references **fully** removed from OXOS

> [!IMPORTANT]
> ## Notice to the community
> **We have recently been made aware of reports that Cerbios, a closed-source "scene" BIOS for the original Xbox, has questionable and potentially illegal
> content: that it was built using leaked Microsoft Xbox source code, and that it may also use other private and open-source material without
> attribution.**
>
> **OXOS is a clean-room, fully open-source kernel, built by Team-Assembly, who brought you ConfigMagic, the TATX Debug BIOS, LiveInfo and the XKUtils API.
> With 25 years of goodwill and integrity on the line, we do not wish to taint our reputation through any inadvertent association with Cerbios's
> highly questionable practices, and we do not want to touch their work with a ten-foot pole!**
>
> **So we have spent a number of long nights removing every spec that had any reference to Cerbios, rebuilding our specs from scratch using only the stock
> Microsoft kernels, EvoX and publicly available source code, and re-implementing and validating our code, so that all of it is free of anything that
> may reference Cerbios's BIOS binaries.**

---

## What we changed, and where each feature now comes from
We now properly credit the people whose public and open-source work OXOS builds on: rmenhal (NKPatcher), Paul Bartholomew (oz_paulb, the LBA48 patch),
the Cromwell developers, Team Resurgent (Hermes, CCI) and the EvoX team, each for what they did first. Every feature below was re-specified from its
original source or from the retail Microsoft kernels, and re-implemented from that spec:
- **NV2A memory pad calibration:** the retail Microsoft boot loaders: the 5713 loader and its table (the same in the 4817, 5101, 5530 and 5713 BIOSes) on
  consoles 1.0–1.5, the 5838 loader on the 1.6 ([stock spec 21 §3](../stock/21_mcpx_bringup.md), [`oxos_internal/15`](../oxos_internal/15_pad_calibration.md)).
- **Large disks and LBA48:** Paul Bartholomew's (oz_paulb) LBA48 patch (2003, GPL), as EvoX M8+ carries it ([EvoX spec 03](../evox/03_disk.md),
  [`oxos_internal/04`](../oxos_internal/04_lba48.md)).
- **Extra FATX partitions (F:, G:, PARTINFO tables):** oz_paulb's LBA48 patch for the table format and the F:/G: split, with every layout confirmed by
  observation ([`oxos_internal/45`](../oxos_internal/45_fatx_disk_layouts.md)).
- **Title patches:** the titles' own code, observed in memory ([`oxos_internal/44`](../oxos_internal/44_title_patch_observation.md)).
- **In-game reset:** rmenhal's NKPatcher (2004, GPL-2.0) for the hook, the button combos and the quick reboot to the dashboard, with EvoX M8 as the
  reference ([EvoX spec 04 §2](../evox/04_runtime.md)); the full power cycle from xboxdevwiki's SMC ("PIC") page
  ([`oxos_internal/22`](../oxos_internal/22_in_game_reset.md)).
- **Virtual discs (ISO and CCI images):** rmenhal's NKPatcher virtual DVD drive (2004, GPL-2.0); the extended mount request as Team Resurgent's Hermes
  (GPL-3.0) sends it; CCI from Team Resurgent's libCCI format description (GPL-2.0) ([`oxos_internal/16`](../oxos_internal/16_virtual_discs.md)).
- **Video encoder detection:** Cromwell's `DetectVideoEncoder` (GPL-2.0).
- **Booting an open BIOS image:** Cromwell's X-codes and their "overflow trick" (GPL-2.0).
- **1.6 consoles:** today a separate BIOS image, built from the retail 5838 BIOS's behaviour; a single image for every console, which will detect a 1.6 at
  boot with EvoX M8+'s SMBus read at address 0x70, is planned ([`oxos_internal/28`](../oxos_internal/28_1_6_console.md)).

---

## Title and hardware interoperability
Two OXOS specs were written by observing, from outside, how titles' memory and a hard disk's partitions differ when running under Cerbios. They
**only** document what was observed in the titles' memory and in the partitions the running kernel reports for a disk, whichever kernel is running, to
allow hardware and software interoperability: **nothing** in them comes from any Cerbios binary whatsoever. They exist so that any console that previously
ran Cerbios can boot OXOS as it is and keep working, with its disks and titles unaffected.

### Some titles have different memory signatures when running Cerbios
- [`oxos_internal/44`](../oxos_internal/44_title_patch_observation.md): the memory of a few publicly named titles was compared while running under Cerbios
  and under a stock kernel. **Only** the title's memory was inspected, and the title itself was researched to understand how it behaves differently. This
  gives title interoperability: the named titles run the same way under OXOS.

### Microsoft FATX partitions remain functional
- [`oxos_internal/45`](../oxos_internal/45_fatx_disk_layouts.md): disks of chosen sizes, with and without a partition table, were given to an emulated
  console (xemu) running Cerbios and running a stock kernel. **Only** the partitions each running kernel reports for the disk were inspected, through a test title
  that asks the kernel where each partition lies. This gives disk interoperability: OXOS reports the same partitions in every case, so disks set up under
  Cerbios work unchanged under OXOS.
