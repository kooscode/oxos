# Specifications

Behavioural specifications, written clean-room: the `stock` and `cerbios` specs describe behaviour, data formats and
constants observed in a reference binary, never its code (no source code, machine code, disassembly or decompiled
code). Stock specs 18–20 (one per video encoder) record the encoder register tables as plain register maps: functional hardware
constants, the one kind of data taken from Microsoft images ([`dmca-compliance.md`](../dmca-compliance.md) §5); `oxos-internal` holds OXOS's own designs for features that
have no reference to study. The OXOS kernel is written from these documents only.

| Folder | Studied from | Content |
|---|---|---|
| [`stock/`](stock/) | the retail kernel, xboxkrnl.exe 1.0.4627.1 (specs 17–20 also 1.0.5838.1, the 1.6 kernel) | what OXOS implements today: boot, executive, memory, objects, I/O, file systems, AV, crypto; the video encoders, one spec each with their register tables: [`18` Conexant](stock/18-conexant-encoder.md) (1.0–1.3), [`19` Focus](stock/19-focus-encoder.md) (1.4–1.5), [`20` Xcalibur](stock/20-xcalibur-encoder.md) (1.6); the parts they share are [`17`](stock/17-av-display-mode.md) |
| [`cerbios/`](cerbios/) | the Cerbios 3.1.0 BIOS (its kernel, 1.0.8008.135, and its extension code) | features OXOS may add: 128 MB, large disks and partitions, a second hard disk, virtual disc images, IGR, video options, fan/LED/LCD/RTC, clocks, and more. Start at [`cerbios/00-overview.md`](cerbios/00-overview.md) |
| [`oxos-internal/`](oxos-internal/) | OXOS's own designs (traced on OXOS, not studied from another binary) | features with no reference implementation, numbered in the order they were implemented: [`00-boot-splash.md`](oxos-internal/00-boot-splash.md) (a picture from flash on cold boot), [`01-kernel-call-tracer.md`](oxos-internal/01-kernel-call-tracer.md) (a record of a title's last kernel calls, for debugging), [`02-display-modes.md`](oxos-internal/02-display-modes.md) (OXOS's display choices: the NV2A timing per encoder, the widescreen line, encoder detection and programming), [`03-debug-serial.md`](oxos-internal/03-debug-serial.md) (boot log, DbgPrint and crash reports on a SuperIO serial port), [`04-lba48.md`](oxos-internal/04-lba48.md) (48-bit disk addressing), [`05-exfat-storage-spec.md`](oxos-internal/05-exfat-storage-spec.md) (an exFAT hard disk with the Xbox partitions as folders, GPT and MBR), its [`06-exfat-storage-plan.md`](oxos-internal/06-exfat-storage-plan.md) and the [`07-exfat-driver.md`](oxos-internal/07-exfat-driver.md) |

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
