# Specifications

Behavioural specifications, written clean-room: the `stock`, `cerbios` and `evox` specs describe behaviour, data
formats and constants observed in a reference binary, never its code (no source code, machine code, disassembly or
decompiled code). The one kind of data taken from Microsoft images is the video encoder and NV2A pad calibration
tables (stock specs 18–21), recorded as plain register maps ([`dmca-compliance.md`](../dmca-compliance.md) §5). The
OXOS kernel is written from these documents only.

| Folder | Studied from | Content |
|---|---|---|
| [`stock/`](stock/) | the retail kernel, xboxkrnl.exe 1.0.4627.1 (specs 17–20 also 1.0.5838.1, the 1.6 kernel) | what OXOS implements: boot, executive, memory, objects, I/O, file systems, AV, crypto; the video encoders with their register tables ([`18` Conexant](stock/18-conexant-encoder.md) 1.0–1.3, [`19` Focus](stock/19-focus-encoder.md) 1.4–1.5, [`20` Xcalibur](stock/20-xcalibur-encoder.md) 1.6, shared parts in [`17`](stock/17-av-display-mode.md)); MCPX and NV2A bring-up ([`21`](stock/21-mcpx-bringup.md), [`22`](stock/22-nv2a-display.md)) |
| [`cerbios/`](cerbios/) | the Cerbios 3.1.0 BIOS (its kernel, 1.0.8008.135, and its extension code) | features OXOS may add: 128 MB, large disks and partitions, a second hard disk, virtual disc images, IGR, video options, fan/LED/LCD/RTC, clocks, and more. Start at [`cerbios/00-overview.md`](cerbios/00-overview.md) |
| [`evox/`](evox/) | the EvoX M8+ BIOS (a patched retail 5838 kernel, no-DVD / fan-control / 137 GB build) | how a scene BIOS patches the retail kernel: no-DVD boot, EEPROM key autodetection, unsigned XBEs, a resident module installed at title launch; split by concern (start-up, title launch, disk, runtime). Start at [`evox/00-overview.md`](evox/00-overview.md) |
| [`oxos-internal/`](oxos-internal/) | OXOS's own designs (traced on OXOS, not studied from another binary) | features with no reference implementation, numbered in the order they were implemented: boot splash, call tracer, display modes, debug serial port, LBA48, exFAT disks, no-DVD boot, netboot, MCPX dump, CPU set-up and pad calibration on hardware, virtual discs, boot jingle, boot-only memory, dashboard choice, DRAM refresh, in-game reset, trainer hook and more |

## Conventions
- Code comments cite a spec as `spec/stock/NN §M` (or `spec/cerbios/NN §M`).
- Inside a folder, "spec NN" means a spec in the same folder. Specs in other folders are named with theirs:
  "stock spec NN", "Cerbios spec NN".
- Cerbios spec 00 has a clearly marked "Public information" section (release notes, tool docs, the shipped ini);
  everything else in the Cerbios specs comes from the code.
- Each Cerbios spec separates Cerbios behaviour from "For OXOS" implementation notes and ends with an **Open** list.
- Compatibility data in the Cerbios specs (per-game fixes, error texts) is described, not copied. OXOS derives its
  own fixes by testing each game.
- Reference binaries and other copyrighted inputs (`xbox-files/`, BIOS images, disc images) are never committed.
