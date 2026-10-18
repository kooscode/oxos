# Cerbios spec 00: overview and inventory

Clean-room: behaviour, formats and constants only; no decompiled code or copied tables. The section "Public
information" collects what release notes and tool documentation say; everything else comes from the code. Sources:
`xbox-files/xboxkrnl-cerbios.exe` (sha256 bb221ea6...), `xbox-files/xboxkrnl-cerbios-extension.bin` (21 664 bytes),
both from the Cerbios 3.1.0 Beta suite (unencrypted, user-supplied); studied with REA/Ghidra and objdump; plus the
image's X-code program, bootloader and LDI streams as separate files (`xbox-files/cerbios-unpacked/`), disassembled
and decoded here.

## Kernel PE (xboxkrnl-cerbios.exe)
- Base 0x80010000, 1 427 functions, 378 export ordinals (4627: 366). PDB name xboxkrnl.pdb (NB10, sig 693700d7).
- Sections: Headers, .text 0x80010300–0x8003E940, **KDCODE** 0x8003E940–0x800420C0 (kernel-debugger code, as in
  debug kernels: the "hybrid retail/debug"), .data 0x800420C0–0x80047E60, STICKY 0x80047E60–0x80048DE0,
  IDEXPRDT 0x80048E00–0x80048F20, INIT 0x80048F20–0x80066440.
- Strings show these features in the kernel itself:
  - virtual disc images: `\Device\Virtual0\Image%d` (ISO/CCI mounting)
  - extended partition table `****PARTINFO****` with names XBOX MUSIC / XBOX SYSTEM / XBOX GAME SWAP 1–3 (two copies)
  - screenshots: `\Device\Harddisk%u\Partition%u\Cerbios\Screenshots\%08X\%04d-%02d-%02d %02d-%02d-%02d.bmp`
    (title ID folder, date-time BMP)
  - debug mode: `\Device\Harddisk0\Partition2\xshell.xbe`
  - error screens: a "Problem / Cause/Solution" text per fatal error code; "Error %i ... bad UDMA settings,
    rebooting into Safe Mode"
  - boot options `/DBBOOT`, `/CDBOOT`, `/HDBOOT`, `SHADOW`
  - an SVG parser and an embedded SVG logo (viewBox 0 0 4608 3456): the splash is vector-drawn
  - default boot animation `E:\Cerbios\BootAnims\Xbox\BootAnim.xbe`; `y:\xboxdash.xbe`

## Extension blob (xboxkrnl-cerbios-extension.bin)
- Headerless x86 code + data, **loaded at 0x80067000** (first page after the kernel image; 92 pointers resolve to
  its strings at that base, next best 18). For decompiling, wrap it in an ELF together with the kernel image:
  the blob as .text at 0x80067000 with a function symbol at every call target, the kernel image (sections placed by
  RVA) at 0x80010000 with a function symbol per export. Ghidra then finds all 75 functions and names every kernel
  call. Kernel-internal routines are read in the kernel PE itself.
- Holds the configuration: ini path `\Device\Harddisk0\Partition1\Cerbios\cerbios.ini`, every ini key (upper case:
  DRIVESETUP AVCHECK DEBUG DASHPATH DASHPATH1 BOOTANIMPATH FRONTLED OVERRIDEFAN FANSPEED UDMAMODEMASTER UDMAMODESLAVE
  FORCE480P FORCEVGA FORCEFFILTER INAPPLCDENABLE RTCENABLE BLOCKDASHUPDATE XONLINEDASHREDIR RESETONEJECT ADVCPUSUPPORT
  DISABLELIMITMEM APPLYTITLEPATCHES READONLYC ENABLESCREENSHOTS SCREENSHOTHDD SCREENSHOTPART LCDI2CADDR LCDPROTO LCDBUS
  XTALFREQ OVERCLOCKING CPUMPLLCOEFF NVPLLCOEFF TUDATAREDIR TUDATAREDIRHDD TUDATAREDIRPART IGRMASTERPORT IGRDASH
  IGRGAME IGRFULL IGRSHUTDOWN IGRCYCLE IGRSCREEN), values FALSE/TRUE.
- Drive letters: `C2E1F6G7X3Y4Z5` (letter, partition: C 2, E 1, F 6, G 7, X 3, Y 4, Z 5) and for each disk
  `<hdd>J10\ K11\ L12\ M13\ N14\ X3\` (J–N = partitions 10–14); path forms `HDD?-?:\`, `DVD-ROM:\`,
  `\Device\Harddisk?\Partition???`; `\Device\RealHD0`.
- Library and section names the extension looks for: BINK2X, BINK, XNET, XMVDEC, WMADEC, D3D, D3D8, D3D8LTCG
  (spec 09 §3, spec 12 §5, spec 13 §5); `XBEH`, `KDBG` markers.
- Paths: recovery `Partition1\Cerbios\Recovery\default.xbe`, boot anim, `Partition2\XODash\xonlinedash(_original).xbe`,
  `xbdm.dll` on CdRom0 or Partition1; LCD drive labels `LCD-E:\`, `LCD-F:\` and "XBOX F"/"XBOX G" names; banner
  "Welcome to Cerbios v3.1.0".

## Flash image and loading
Kernel version reported: **1.0.8008.135**. No step of the boot path decrypts or verifies anything: the bootloader
copies and decompresses, the kernel's stored PE checksum (0x6127F) differs from the computed one (0x60470), and it
boots.

Offsets in the 256 KiB image (flash address = 0xFF0C0000 + offset), each derived from a constant in the code or the
stream lengths:

| Image offset | Content | Where the offset comes from |
|---|---|---|
| 0x00080 | X-code program (554 ops) | |
| 0x0EDC2 | logo SVG, LDI stream (0x9E8 → 0x1BDF bytes) | kernel start − logo compressed size |
| 0x0F7AA | kernel, LDI stream (0x29048 → 0x57000 bytes, 11 blocks) | extension start − kernel compressed size |
| 0x387F2 | extension, LDI stream (0x3596 → 0x54A0 bytes) | hidden section header (below) |
| 0x3BD88 | kernel .data initializer (0x1050; starts with the CERB record, spec 01) | kernel DOS header +0x30 |
| 0x3CDD8 | bootloader (0x3028 bytes, entry 0x90208); its last 0x24 bytes are the parameter block | parameter block +0x1C |
| 0x3FE00 | top of the components; the area above is not studied | |

- **X-codes → stub → bootloader:** the last eight X-code ops are memory writes that place a 32-byte x86 stub at
  physical address 0, then the program exits. The stub loads the bootloader size from 0xFF0FFDFC and its flash
  address from 0xFF0FFDF8, copies it (dwords) to 0x90000 and jumps to the address held in the first dword there.
- **Bootloader:** moves itself to 0x80400000, decompresses the kernel to 0x80010000, copies the .data initializer
  (DOS header +0x2C bytes, from the flash address in +0x30 to the address in +0x34) and zero-fills the following
  +0x28 bytes, then calls the kernel entry point. Values in 3.1.0: +0x28 0x4D50, +0x2C 0x1050, +0x30 0xFF0FBD88,
  +0x34 0x800420C0.
- **Parameter block** (last 0x24 bytes of the bootloader, image 0x3FDDC, nine dwords): 0x1535 (not used by the code
  studied), logo offset 0xEDC2, logo compressed size, logo size, kernel compressed size, extension compressed size,
  .data size, bootloader flash address, bootloader size. Components are located **top-down** from 0xFF0FFE00:
  minus bootloader size, minus .data size = .data start; minus extension compressed size = extension start; minus
  kernel compressed size = kernel start. The **kernel locates the logo the same way** (kernel start minus logo
  compressed size), not through the stored offset; it decompresses the logo into pool memory and NUL-terminates it
  (the SVG text the splash draws).
- **LDI stream:** blocks of {u16 compressed size, u16 uncompressed size, data}; each block decompresses to 0x8000
  bytes except the last; the decoder is LZX with a 0x20000-byte window and 0x8000-byte frames, run as one stream;
  decoding stops once the given compressed size is consumed.
- **How the extension is loaded:** the kernel PE has a **hidden section header** right after the last counted one
  (NumberOfSections = 6; the 7th header is unnamed): virtual size 0x54A0, RVA 0x57000, and in the next two dwords
  the **flash address of the compressed stream (0xFF0F87F2) and its compressed size (0x3596)**. At the end of
  kernel initialisation the kernel takes the physical address of its own base, adds that RVA, allocates contiguous
  memory at exactly that physical range (so it lands at 0x80067000), decompresses the stream there, sets an
  "extension present" flag and calls into it (ini, partitions, launch decisions). A rebuilt image must update those
  two dwords as well as the parameter block.
- **Lifetime:** the memory manager's start-up reloads the extension from flash on **every kernel start** — the cold
  boot and each quick reboot (the kernel's in-place restart used between titles, which re-copies .data from flash
  and sets a "quick reboot" flag). Kernel start-up then calls into it (phase-0 init, then phase-1 init: spec 06).
  In the title-launch thread, just before jumping to the XBE entry point, the kernel calls the extension once more,
  then **overwrites the extension's memory with unrelated kernel bytes and frees it**. Everything a running title
  needs (IGR, screenshots, redirects, LCD) is kernel code; the extension only installs it.

## Public information (release notes, tool docs, the shipped cerbios.ini)
Everything above and in specs 01–14 comes from the code. This section is different: it collects what the public
sources say, to explain intent and history. Where it and the code disagree, the code describes 3.1.0 and the
difference is noted.

**Sources:** the Cerbios release NFO on xbins (`xboxnfo2572.nfo`, every release from Alpha V1 to 3.1.0 Beta; the
same text ships as `Readme.nfo` in the download); the ConsoleMods "Xbox: BIOS Features" page (Cerbios 2.3.1);
CerbiosTool by Team Resurgent (GPL-3.0; setting names and options, Cerbios 2.x); Xbox-Scene articles on 2.3.2 HD
Alpha and 2.4.2 Beta; FATXplorer 3.0 beta 19 notes (formatting disks over 2 TB); the `cerbios.ini` shipped in the
3.1.0 download. Cerbios is by Team Cerbios: a 256 KB image for console revisions 1.0–1.6 that also runs in xemu.
Latest release: 3.1.0 Beta, 2025-12-09.

### What the shipped cerbios.ini says each setting does
| Setting | Public description | Code (spec) |
|---|---|---|
| DashPath | one path; `HDD0-`/`HDD1-` prefixes; falls back to `E:\Cerbios\Recovery\default.xbe`, then `C:\xboxdash.xbe` | matches (09 §2) |
| BootAnimPath | falls back to `E:\Cerbios\BootAnims\Xbox\bootanim.xbe` | matches (09 §2) |
| AVCheck | checks the AV cable at boot; halts with flashing green/amber LEDs if none | code: powers off after 300 s without a pack (14 §4); the LED part was not traced |
| ForceFFilter | 0 off, 1–5 strength, 6 system/game default | matches (10 §4) |
| Force480p | 480i titles in 480p (component, 480p enabled in the dashboard; PAL unaffected) | matches (10 §2) |
| ForceVGA | VGA through RGsB (sync on green); enables Force480p and NTSC; not on 1.6 | matches (10 §3, 01 §3) |
| DriveSetup | 0 HDD + DVD (stock), 1 HDD only (legacy), 2 HDD only (modern: DVD code removed), 3 dual HDD with the slave in the DVD bay | 2 and 3 match (05); 0 and 1 take the same start-up path in the code studied, but with 1 a test disc was never read in xemu (05 Open) |
| UdmaModeMaster/Slave | 0 auto (StarTech adapter), 1 auto (generic), 2–6 UDMA mode (80-wire cable; the slave needs a jumper) | the value goes to SET FEATURES (06 §3); the auto modes were not traced |
| FanSpeed / OverrideFan | 0 auto, 10–100 % in steps of 2; Cerbios sets the fan at boot, reboot and title launch | matches (11 §1) |
| XtalFreq | 0 auto from the CPU's MSR, 1 16.66 MHz, 2 13.5 MHz; full reboot to change | matches (12 §2) |
| CPUMPLLCoeff | memory divider, FSB multiplier, PLL_N, PLL_M; stock 0x230801: FSB = 16.66 / 1 × 8 = 133.33 MHz, CPU = FSB × 5.5 = 733 MHz, VCO = FSB × 3 × 2 = 800 MHz, RAM = VCO / (2 × 2) = 200 MHz | N = bits 0–7, M = bits 8–15 (12 §3) |
| NVPLLCoeff | PLL_P, PLL_N, PLL_M; stock 0x011C01: GPU = 28 × 16.66 / 2¹ / 1 = 233 MHz | matches (12 §3) |
| InAppLCDEnable | FPS, temperatures and RAM use on a front-panel LCD | layout in 11 §3 |
| LCDBus / LCDProto / LCDI2CAddr | bus 0 SMBus, 1 X3LCD; protocol 0 HD44780, 1 US2066; address 0x3C (0x20–0x27, 0x38–0x3F) | matches (11 §3) |
| BlockDashUpdate | games can't update the Microsoft dashboard (softmods, boot from media) | mechanism in 13 §1 |
| XonlineDashRedir | Xbox Live dashboard calls go to `xonlinedash_original.xbe` (softmods with a dummy xonlinedash) | matches (09 §3) |
| AdvCPUSupport | run-time XBE patches for upgraded or overclocked CPUs (no effect on stock CPUs) | matches (12 §5) |
| DisableLimitMem | ignores the XBE's "limit to 64 MB" flag to give titles the full 128 MB; only on 128 MB consoles; may cause problems | matches (02 §4) |
| ApplyTitlePatches | run-time patches for known problem games | 13 §7 |
| ReadOnlyC | no writes to C: | matches (13 §2) |
| ResetOnEject | the original reset-when-the-tray-opens behaviour, back to the dashboard | 13 §6 |
| RTCEnable | time from an RTC module on the SMBus | matches (11 §4) |
| TUDATARedir, …HDD, …Part | HDD 0 master, 1 slave; partition 1 E:, 6 F:, 7 G: | matches (13 §3) |
| EnableScreenshots | framebuffer to `E:\Cerbios\Screenshots\<title ID>` on left thumb + right thumb | matches (13 §5, 08) |
| IGR combos | IGRDash keeps a mounted ISO/CCI; IGRFull unmounts it and returns to the dashboard; IGRCycle power-cycles; IGRShutdown; IGRScreen | matches (08: IGRDash is a quick reboot, which keeps mounts; IGRFull a full reboot, which drops them, 07 §1) |
| Debug | boots `C:\xshell.xbe` and loads `E:\xbdm.dll` (the devkit debug monitor) for network debugging | 14 §1 (the dashboard comes from `E:\dashboard.xbx`) |

IGR key codes in the ini (also in CerbiosTool's README): A 0, B 1, X 2, Y 3, BLACK 4, WHITE 5, LT 6, RT 7, D-pad up 8,
down 9, left A, right B, START C, BACK D, LS E, RS F (01 §2 derives the same mapping from the parser).
CerbiosTool also lists splash theming fields kept in the image (background, "Cerbios" and safe-mode text colours,
four logo colours, scale); in 3.1.0 field 0x105 is a splash factor (01 §1), and the theme's storage was not traced.

### Release history (from the NFO)
- **Alpha V1:** custom LBA "v4" (up to 8 TB), auto-detected custom partition, native XISO, custom error messages,
  128 MB RAM.
- **2.00:** retail and hybrid debug builds, two no-DVD modes, invalid-clock (Y2K-style) fix, dual HDD, extended
  partitions beyond 998 GB, IGR and IGS, fan control, external boot animations (`Bootanim.ini`).
- **2.02:** XMV boot animations, widescreen and up to 720p boot animations; experimental UDMA 5 builds.
- **2.03 repack:** unified retail/debug build, parental control removal, Eject safe mode (UDMA 5 back to UDMA 2).
- **2.1.0:** 1.6 support, Insignia (Xbox Live revival) account fix, the hybrid 1.0–1.6 build with 128 MB.
- **2.2.0:** CCI disc images (Cerbios Compressed Image, made with Repackinator); 2.2.1 split ISOs.
- **2.3.0:** automatic safe mode on errors 9 and 13. **2.3.1:** boot from media (BFM/PBL, through the Phoenix BIOS
  Loader). **2.3.2 HD Alpha:** HD+ mod support (removed in 3.0).
- **2.4.0:** 1.6 Xcalibur encoder video modes, 1.6 image without XBE patching, fan speed on 1.6 and CPU-upgraded
  consoles (default 60 %), 480i to 480p, IGPC, Force PAL, RTCEnable, ForceVGA (experimental), dashboard update
  blocking. **2.4.2:** dual HDD (DriveSetup 3), UdmaModeMaster/UdmaModeSlave.
- **3.0.0 Beta:** removed all XBOXHD+ products, custom CD paths and dashboard paths 2–3 (2.x had three dashboard
  paths, defaults evoxdash/avalaunch/nexgen, and three CD paths). Added: recovery menu (copy the Cerbios folder to
  E:\), AdvCPUSupport, overclocking (PLL coefficients; "risk of instability or damage") with a calculator script,
  ResetOnEject, the ForceFFilter range, front-panel LCD in games, XonlineDashRedir, DisableLimitMem,
  ApplyTitlePatches, TDATA/UDATA redirection, ReadOnlyC, screenshots, IGRMasterPort, OverrideFan, 256 MB RAM
  (experimental), Redump ISOs.
- **3.1.0 Beta:** Team Resurgent HDMI firmware support; safe mode through SMC scratch bit 5 (for modchip OSes; 01 §4);
  XtalFreq; automatic recovery XBE in safe mode; 128 MB on the 1.6 with the BGA RAM mod; boot-animation recovery with
  the White button; JPG/BMP/PNG brand logo (180 × 50 `brand.*` in the boot-animation folder); fixes for specific
  titles (Robot Wars NTSC, Riddick, Forza and Wolfenstein TDATA/UDATA, CPU frequency reporting for XBMC and LithiumX,
  Incredible Hulk, Ultra Bust-A-Move, Crash Tag Team Racing, disc games with a boot animation). All but the last appear in the
  per-title code in specs 10, 12 and 13.

### Install layout and known issues
- `E:\Cerbios\cerbios.ini`; boot animations in `E:\Cerbios\BootAnims\`; recovery from the Cerbios folder on E:.
  Boot from media: the Phoenix loader folder, the renamed `Cerbios.bin`, BootAnims and cerbios.ini on C:, then
  launch its default.xbe (a full IGR reloads the flashed BIOS).
- The 3.1.0 download contains the 256 KB hybrid image, a 256 MB-only build, the Phoenix BIOS Loader (`boot.cfg` with
  `Romfile Cerbios.bin`), the `Cerbios/` folder (cerbios.ini, `Recovery/default.xbe`, boot animations: Xbox, Modern,
  Nyan Cat, XMV Player), LithiumX-DualHDD, the overclock calculator and `Readme.nfo`.
- Known issues: composite video on the 1.6 (use component or HDMI); some IDE-to-SATA adapters can't do the higher
  UDMA modes; 256 MB is experimental. FATXplorer 3.0 beta 32 or later is needed to format dual-HDD set-ups (beta 19
  added disks over 2 TB, default largest partition 927.78 GB: compare spec 03 §3's 0x74000000-sector cap).

## For OXOS: features compared and a suggested order
| Feature | Cerbios | OXOS today (v0.1.6) | Spec |
|---|---|---|---|
| 128 MB RAM | full 128 MB, the upper half for titles without the 64 MB flag | first 64 MB only, as retail | 02 |
| Large disks, extra partitions | 48-bit LBA, F:/G:, on-disk table | 28-bit LBA, C E X Y Z only | 03 |
| Dual HDD | slave disk in the DVD bay | slave is the DVD drive | 05 |
| UDMA modes 2–6 | configurable per drive | UDMA 2 fixed, as retail | 06 §3 |
| Safe mode, recovery XBE | Eject at boot, error 9/13, SMC scratch bit | no | 01 §4, 09 §2, 14 §3 |
| No DVD drive | DRIVESETUP 2 | a missing drive is fatal error 10, as retail | 05 |
| Virtual discs | ISO, CCI, split images | no | 07 |
| Configurable dashboard, IGR | yes | fixed `C:\xboxdash.xbe`, no IGR | 08, 09 |
| Boot animation, splash theme | XBE, XMV, brand logo | one built-in PNG splash | 09 §2 |
| Front LED, fan, LCD, RTC | yes | retail LED only | 11 |
| Video overrides, region-free | yes | no | 10 |
| TDATA/UDATA redirect, read-only C:, screenshots | yes | no | 13 |
| Overclocking, CPU upgrades | yes (with a damage warning) | no | 12 |
| Revisions | 1.0–1.6b, 256 KB image | 1.0–1.5 (1.6 untested), 1 MB image | |

Suggested order: 128 MB RAM first (the most requested, testable in xemu with `mem_limit` 128); then large disks and
the extended partitions; then configurable dashboard paths and IGR (an everyday BIOS); then UDMA modes and the
second disk, no-DVD mode and disc images from the hard disk. Smaller ones fit in between: TDATA/UDATA redirect and
read-only C: (I/O layer), screenshots (framebuffer capture already exists in our test tools), recovery/safe mode
(today a missing dashboard is fatal error 13, as retail). Overclocking needs real hardware and care. Per-game patches
are a last resort: OXOS fixes the kernel instead, and derives any game-specific fix by its own testing.

## Specs
| File | Topic |
|---|---|
| 01-configuration.md | the built-in record (layout, defaults, run-time fields), cerbios.ini parsing and value types, clamps, safe mode |
| 02-memory-128mb.md | RAM size detection (PCI 0:0 reg 0x87), page database and mappings, the two page regions, the "limit to 64 MB" unlock rule |
| 03-partitions-large-disks.md | 48-bit addressing per drive, the 512-byte PARTINFO table (format, defaults, the three on-disk locations), automatic F:/G:, drive letters, the 0xCAFEBABE partition query |
| 04-extra-exports.md | ordinals 7 and 11 (debugger), 374–378 MmDbg* (debug monitor memory); 367–373 absent |
| 05-dual-hdd-drive-setup.md | DRIVESETUP 0–3, slave probe, the second disk and its spin-up reboot, the placeholder CD device |
| 06-boot-sequence.md | boot flags (command line, SMC scratch), phase-0/1 start-up order, parental-control reset, UDMA modes |
| 07-virtual-discs.md | ISO/CCI/HDD/MU image mounting: devices, control codes and request layout, full-dump offset, CCI format, LZ4 |
| 08-igr.md | button mask, port filter, the six combos and their actions |
| 09-boot-animation-title-launch.md | path translation, boot animation, launch data, disc, dashboard fallback chain, the XBE loader (no signature check, media/region unlock, XOnline redirect) |
| 10-video.md | AV pack codes, FORCE480P, FORCEVGA, FORCEFFILTER, per-title video fixes, region-free reporting |
| 11-fan-led-lcd-rtc.md | fan override, FRONTLED pattern, HD44780 and I²C OLED LCD drivers and the in-app display, DS1307-style RTC |
| 12-overclock-cpu.md | GPU/CPU PLL programming, crystal selection, CPU speed measurement, microcode, faster-CPU title fixes |
| 13-misc.md | dashboard-update block, read-only C:, TDATA/UDATA redirect, graphics hooks and screenshots, reset-on-eject, title patches |
| 14-debug-recovery-errors.md | xbdm loading and dashboard.xbx, recovery XBE, fatal-error screens and the UDMA safety net, AV pack check |

Every spec ends with an **Open** list of what the code studied did not settle. Compatibility data (per-title
patches, title-ID lists, error texts) is described, not copied: an implementation builds its own.
