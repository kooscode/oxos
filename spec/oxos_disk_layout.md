# OXOS disk layout: every file and folder OXOS uses

One page for every file and folder OXOS reads or writes, today and planned, each with its spec:

✅  = exists today.

⬜  = planned.

[  Keep it current: a feature that adds a file adds it here, with its spec. ]

## The rules
- **Everything OXOS keeps on the hard disk is in `C:\OXOS`** (`\Device\Harddisk0\Partition2\OXOS`; `/C/OXOS/` on an exFAT disk): the same path on FATX and
  exFAT, the path a title would use. OXOS never makes a folder at the root of an exFAT disk (spec [30](oxos_internal/30_disk_format.md)).
- **OXOS's own files start with `oxos_`**; files without it are the user's or other people's (docs/cpp_style.md §2).
- **The root of `C:\OXOS` holds only the kernel's own files.** An extension's settings and data are in a file of its name next to it:
  `extensions\NAME.ini` for `extensions\NAME.xos` (spec [34 §3.1](oxos_internal/34_kernel_extensions.md)).
- **Ini files:** sections, `key = value`, comments on lines of their own (a `;` after a value is part of it), read with the kernel's ini reader (spec
  [33 §1](oxos_internal/33_launch_redirect.md)).
- **Safe boot** (B held at a cold boot) reads none of OXOS's ini files and no OXOS EEPROM setting (spec [39 §1.1](oxos_internal/39_oxos_setup.md)).

## `C:\OXOS` on the hard disk

```
C:\OXOS\                                  ✅ made at every cold boot
├── oxos_settings.ini                     ✅ the kernel's options: [dashboard] ✅ (19), [launch_redirect] ✅ (33), [boot_video] ✅ (38), [toasts] ✅ (36); in-game reset ⬜ (39); shipped
├── oxos_extensions.ini                   ✅ which extensions load, in which order, enabled (34)
├── oxos_title_patches.ini                ✅ per-title patches (25) / Will move to extensions (34 §10)
├── oxos_eeprom_backup.bin                ⬜ all 256 EEPROM bytes, saved by OXOS Setup before it changes one (39)
│
├── apps\                                 ✅ OXOS's apps (from the release zip)
│   ├── oxos_ftp.xbe                      ✅ the OXOS FTP server (apps/oxos_ftp)
│   ├── oxos_boot_video.xbe               ✅ the boot video player, started by the kernel (38)
│   └── oxos_settings.xbe                 ⬜ OXOS Setup: hold A at boot (39)
│
├── extensions\                           ✅ made at every cold boot; each extension with its own .ini of the same name
│   ├── <anyone's>.xos + <same>.ini       ✅ the extension and its own settings (top) and data ([sections]) (34 §3.1)
│   ├── oxos_title_patches.xos            ⬜ title patches (34 §10)
│   ├── oxos_nettitle.xos                 ⬜ netboot titles (34 §10)
│   ├── oxos_tracer.xos                   ⬜ kernel call tracer (34 §10)
│   ├── oxos_igs.xos                      ✅ in-game shortcuts (48)
│   ├── oxos_igs.ini                      ✅ the shortcuts: buttons, action, target, toast
│   ├── oxos_trainers.xos                 ✅ Universal Trainers (37)
│   ├── oxos_trainers.ini                 ✅ trainer and cheats per title ID
│   ├── oxos_trainers.xbe                 ✅ the trainer menu, before a game that has a trainer
│   ├── oxos_screenshots.xos              ⬜ screenshots (40)
│   ├── oxos_screenshots.ini              ⬜ buttons, format
│   ├── oxos_debugger.xos                 ⬜ homebrew debugging on a real console (41)
│   ├── oxos_playtime.xos                 ⬜ play time (42)
│   └── oxos_playtime.ini                 ⬜ save_every, skip
│
├── trainers\                             ✅ .etm trainers for Universal Trainers (37)
├── boot_videos\                          ✅ boot videos (38): oxos_boot_video.mpg from the release zip, and the user's own, converted on a PC
├── screenshots\<titleID>\<datetime>.png  ⬜ (40)
├── playtime\                             ⬜ (42)
│   ├── oxos_playtime.txt                 totals per title, never cleared (oxos_playtime.new while being replaced)
│   └── oxos_playtime.log                 sessions to send (oxos_playtime.sending while the XBL Dashboard uploads)
│
├── log\                                  ✅ made at every cold boot; each file rotated at 10 MB (35)
│   ├── extensions.log                    ✅ (+ extensions_1.log, _2, … older parts)
│   └── selftest.log                      ✅ self-test builds only
│
└── cache\                                ✅ made at every cold boot; the kernel's own, safe to delete
    ├── oxos_kernel.cache                 ✅ the kernel modules, rewritten at every cold boot (43)
    └── oxos_cache_partitions.cache       ✅ the X/Y/Z cache formats on exFAT (07)
```

| Spec | |
|---|---|
| 07 | [exFAT driver](oxos_internal/07_exfat_driver.md) |
| 25 | [Title compatibility and title patches](oxos_internal/25_title_compatibility.md) |
| 33 | [Launch redirects](oxos_internal/33_launch_redirect.md) |
| 34 | [Kernel extensions](oxos_internal/34_kernel_extensions.md) |
| 35 | [Log files](oxos_internal/35_log_files.md) |
| 37 | [Universal Trainers](oxos_internal/37_universal_trainers.md) |
| 38 | [Boot video](oxos_internal/38_boot_video.md) |
| 39 | [OXOS Setup, boot buttons, safe boot](oxos_internal/39_oxos_setup.md) |
| 40 | [Screenshots](oxos_internal/40_screenshots.md) |
| 41 | [Homebrew debugging on a real console](oxos_internal/41_remote_debugging.md) |
| 42 | [Play time](oxos_internal/42_play_time.md) |
| 43 | [Kernel modules](oxos_internal/43_kernel_modules.md) |

## The release zip
`oxos_vX.Y.Z.zip` (`make -C kernel release VERSION=X.Y.Z`, the `release_zip` target) holds `C:\OXOS` as it is installed: copy its `OXOS` folder to `C:\`
(`/C/` on an exFAT disk). When updating, keep your own `.ini` files (`oxos_settings.ini`, `oxos_extensions.ini`, `extensions\oxos_trainers.ini`): the zip's
are the defaults.

```
oxos_vX.Y.Z.zip
├── README.txt                            how to install: flash the image, copy the OXOS folder as it is to C:\ (kernel/settings/release_readme.txt)
├── OXOS\
│   ├── bios\                             bios_vX.Y.Z.bin (1 MB), bios_vX.Y.Z_256k.bin, bios_1.6_vX.Y.Z_256k.bin: to flash, not used from the disk
│   ├── apps\                             oxos_ftp.xbe (the OXOS FTP server), oxos_boot_video.xbe (the boot video player)
│   ├── oxos_settings.ini                 the dashboard order, launch redirects (none), the boot video on (kernel/settings/)
│   ├── oxos_extensions.ini               the shipped extensions, listed, off (kernel/extensions/oxos_extensions.ini)
│   ├── oxos_title_patches.ini            per-game fixes (kernel/settings/)
│   ├── boot_videos\                      oxos_boot_video.mpg, the OXOS boot video (kernel/splash/)
│   ├── extensions\                       oxos_trainers.xos, oxos_trainers.xbe, oxos_trainers.ini (every trainer off)
│   ├── trainers\                         the trainer pack's 1,084 .etm files
│   ├── log\                              empty, as the kernel makes it
│   └── cache\                            empty, as the kernel makes it
└── THIRD_PARTY_NOTICES.md
```

## On a USB stick used as a memory unit ⬜
Everything about a memory unit is on the unit itself; nothing is kept on the console's hard disk (spec [05 §9](oxos_internal/05_exfat_storage_spec.md)). A
stick formatted exFAT, FAT32 or FAT16 is shown to titles as a memory unit rooted at its `/OXOS` folder; the rest of the stick is the user's. Standard FATX
memory units are unchanged.

```
/ (the stick)
├── OXOS\                                 the memory unit, as titles see it
│   ├── oxos_memory_unit.ini              its name, serial and the size titles are told; hidden from titles
│   ├── UDATA\                            saves
│   └── TDATA\                            title data
└── … anything else on the stick          not seen by the console
```

## Not files
| What | Where | |
|---|---|---|
| Skip the splash | one OXOS byte in the EEPROM (0xC0–0xFF, which byte to check) | ⬜ spec 39 §1.1: read before the splash, set by OXOS Setup |
| Extension state pages | memory, 4 KB per extension, until the next cold boot | ✅ spec 34 §6 |
| Start-up marks, last words | the CMOS RAM | ✅ `kernel/src/diagnostics/boot_marks.cpp`, `last_words.cpp` |
