# Specifications

Behavioural specifications, written clean-room: the `stock` and `cerbios` specs describe behaviour, data formats and
constants observed in a reference binary, never its code; `oxos-internal` holds OXOS's own designs for features that
have no reference to study. The OXOS kernel is written from these documents only.

| Folder | Studied from | Content |
|---|---|---|
| [`stock/`](stock/) | the retail kernel, xboxkrnl.exe 1.0.4627.1 (spec 19: 1.0.5838.1, the 1.6 kernel) | what OXOS implements today: boot, executive, memory, objects, I/O, file systems, AV, crypto, debug serial; and the Xcalibur video encoder of 1.6 consoles ([`19-xcalibur-encoder.md`](stock/19-xcalibur-encoder.md)) |
| [`cerbios/`](cerbios/) | the Cerbios 3.1.0 BIOS (its kernel, 1.0.8008.135, and its extension code) | features OXOS may add: 128 MB, large disks and partitions, a second hard disk, virtual disc images, IGR, video options, fan/LED/LCD/RTC, clocks, and more. Start at [`cerbios/00-overview.md`](cerbios/00-overview.md) |
| [`oxos-internal/`](oxos-internal/) | OXOS's own designs (traced on OXOS, not studied from another binary) | features with no reference implementation: [`lba48.md`](oxos-internal/lba48.md) (48-bit disk addressing, implemented), [`exfat-storage-spec.md`](oxos-internal/exfat-storage-spec.md) (an exFAT hard disk with the Xbox partitions as folders, GPT and MBR) and its [`plan`](oxos-internal/exfat-storage-plan.md), [`tv-encoders.md`](oxos-internal/tv-encoders.md) (Conexant and Focus register values computed from their datasheets) |

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
